#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "tusb.h"
#include "hidkit_switch_pro_glue.h"
#include "gamepad.h"

typedef struct {
    uint8_t addr, instance, data[16];
    uint16_t len;
} packet_t;

static uint32_t now;
static packet_t packets[128];
static unsigned sent, errors, busy, delivered[6], sequence[6];
static hidkit_switchpro_calib_t calibration[6];
static unsigned key_events, pad_events, motion_events;
static uint16_t last_key;
static bool last_pressed;
static int16_t last_accel_z;

void hidkit_input_key(int8_t slot, uint16_t code, bool pressed)
{
    (void)slot;
    key_events++;
    last_key = code;
    last_pressed = pressed;
}

void hidkit_input_gamepad_abs(int8_t slot, int32_t lx, int32_t ly,
                              int32_t rx, int32_t ry, int32_t lt, int32_t rt,
                              uint32_t buttons)
{
    (void)slot; (void)buttons;
    assert(lx >= -32767 && lx <= 32767 && ly >= -32767 && ly <= 32767);
    assert(rx >= -32767 && rx <= 32767 && ry >= -32767 && ry <= 32767);
    assert(lt >= 0 && lt <= 32767 && rt >= 0 && rt <= 32767);
    pad_events++;
}

void hidkit_input_gamepad_motion(int8_t slot, int16_t gx, int16_t gy, int16_t gz,
                                 int16_t ax, int16_t ay, int16_t az)
{
    (void)slot; (void)gx; (void)gy; (void)gz; (void)ax; (void)ay;
    motion_events++;
    last_accel_z = az;
}

uint32_t switch_pro_time_us(void) { return now; }
void switch_pro_log(bool error, const char *fmt, ...)
{
    (void)fmt;
    if (error) errors++;
}

bool tuh_hid_send_report(uint8_t addr, uint8_t instance, uint8_t report_id,
                         const void *report, uint16_t len)
{
    const uint8_t *p = report;
    assert(report_id == 0); // Report ID 已在 payload 中，不能再加一次
    if (busy) { busy--; return false; }
    assert(sent < 128 && len <= sizeof(packets[0].data));
    if (p[0] == 0x01) {
        static const uint8_t neutral[] = {0, 1, 0x40, 0x40, 0, 1, 0x40, 0x40};
        assert(p[1] == (sequence[addr]++ & 15));
        assert(memcmp(p + 2, neutral, 8) == 0);
        assert(p[10] == 0x48 || p[10] == 0x03 || p[10] == 0x40 || p[10] == 0x10);
        assert(len == (p[10] == 0x10 ? 16 : 12));
        if (p[10] != 0x10) assert(p[11] == (p[10] == 0x03 ? 0x30 : 1));
    }
    packets[sent] = (packet_t){.addr = addr, .instance = instance, .len = len};
    memcpy(packets[sent++].data, p, len);
    return true;
}

void switch_pro_calibration(uint8_t addr, uint8_t instance,
                            const switch_pro_calib_t *cal)
{
    assert(instance == 0);
    delivered[addr]++;
    calibration[addr] = hidkit_switch_pro_calibration(cal);
}

static void reset(void)
{
    for (uint8_t addr = 0; addr < 6; addr++) switch_pro_umount(addr);
    memset(delivered, 0, sizeof(delivered));
    memset(sequence, 0, sizeof(sequence));
    memset(calibration, 0, sizeof(calibration));
    // 同时覆盖 32bit 微秒计时器回绕。
    now = UINT32_MAX - 90000u;
    sent = errors = busy = 0;
    switch_pro_maybe_mount(1, 0, 0x057e, 0x2009);
}

static void tick(void) { now += 15000; switch_pro_task(); }
static void ack(const packet_t *p, uint8_t id, uint8_t status)
{
    uint8_t report[15] = {0x21};
    report[13] = status;
    report[14] = id;
    switch_pro_report(p->addr, p->instance, report, sizeof(report));
}

static uint32_t spi_addr(const packet_t *p)
{
    return (uint32_t)p->data[11] | ((uint32_t)p->data[12] << 8) |
           ((uint32_t)p->data[13] << 16) | ((uint32_t)p->data[14] << 24);
}

static void spi_reply(const packet_t *p, bool user, bool truncated)
{
    uint8_t report[64] = {0x21};
    uint8_t len = p->data[15];
    uint32_t addr = spi_addr(p);
    report[13] = 0x90;
    report[14] = 0x10;
    memcpy(report + 15, p->data + 11, 5);
    if (addr == 0x8010 || addr == 0x801B || addr == 0x8026) {
        if (user) { report[20] = 0xB2; report[21] = 0xA1; }
    } else if (addr != 0x6080) {
        // 工厂/用户 IMU 原点各为 10/100；摇杆区提供非零工厂数据。
        for (unsigned i = 0; i < len; i += 2) report[20 + i] = addr >= 0x8000 ? 100 : 10;
        if (addr == 0x6020 || addr == 0x8028) {
            for (unsigned i = 0; i < 3; i++) {
                report[26 + i * 2] = 0; report[27 + i * 2] = 0x40; // 16384
                report[38 + i * 2] = 0x3B; report[39 + i * 2] = 0x34; // 13371
            }
        }
    }
    switch_pro_report(p->addr, p->instance, report,
                      truncated ? (uint16_t)(19 + len) : sizeof(report));
}

// 完整初始化；可让每条初始化命令前两次丢 ACK，验证有限重发和序号回绕。
static void complete(bool retry, bool user, bool drop_user_imu)
{
    unsigned handled = sent, counts[6][256] = {{0}};
    for (unsigned i = 0; i < 250; i++) {
        tick();
        while (handled < sent) {
            packet_t p = packets[handled++];
            if (p.data[0] != 1) continue;
            uint8_t cmd = p.data[10];
            if (cmd == 0x10) {
                if (!(drop_user_imu && spi_addr(&p) == 0x8028)) spi_reply(&p, user, false);
            } else if (!retry || ++counts[p.addr][cmd] == 3) {
                ack(&p, cmd, 0x80);
            }
        }
    }
}

static void test_initialization(void)
{
    reset();
    busy = 2;
    switch_pro_maybe_mount(2, 0, 0x057e, 0x2009);
    complete(false, false, false);
    assert(errors == 0 && delivered[1] == 1 && delivered[2] == 1);
    assert(calibration[1].valid == (HIDKIT_SWITCHPRO_CAL_STICKS | HIDKIT_SWITCHPRO_CAL_MOTION));
    assert(calibration[1].acc_origin[0] == 10);
    assert(calibration[1].acc_scale[2] == 16384 && calibration[1].gyro_scale[2] == 13371);
    unsigned before = sent;
    tick();
    assert(sent == before); // RUNNING 不再发送
    switch_pro_umount(1);
    sequence[1] = 0;
    switch_pro_maybe_mount(1, 0, 0x057e, 0x2009);
    complete(false, false, false);
    assert(delivered[1] == 2 && delivered[2] == 1);
}

static void test_retries_and_user_calibration(void)
{
    reset();
    complete(true, true, false);
    assert(errors == 0 && delivered[1] == 1 && sequence[1] == 19);
    assert(calibration[1].acc_origin[0] == 100);
    reset();
    complete(false, true, true);
    assert(delivered[1] == 1 && calibration[1].acc_origin[0] == 10);
}

static void test_subcmd_busy(void)
{
    reset();
    for (unsigned i = 0; i < 4; i++) tick(); // 四条 USB 握手命令
    assert(sent == 4 && sequence[1] == 0);
    busy = 2;
    tick(); tick();
    assert(sent == 4 && sequence[1] == 0); // 入队失败不消耗子命令序号
    tick();
    assert(sent == 5 && packets[4].data[10] == 0x48 && sequence[1] == 1);
    packet_t wrong_instance = packets[4];
    wrong_instance.instance = 1;
    ack(&wrong_instance, 0x48, 0x80);
    switch_pro_report(1, 0, NULL, 0);
    tick(); tick();
    assert(sent == 5); // 非目标接口的 ACK 不能推进
    ack(&packets[4], 0x48, 0x80);
    tick(); tick();
    assert(sent == 6 && packets[5].data[10] == 0x03);
}

static void test_imu_failure(void)
{
    reset();
    unsigned handled = 0, imu_attempts = 0;
    for (unsigned i = 0; i < 100; i++) {
        tick();
        while (handled < sent) {
            packet_t p = packets[handled++];
            if (p.data[0] != 1) continue;
            assert(p.data[10] != 0x10); // 未确认 IMU 时不能推进到校准
            if (p.data[10] == 0x40) {
                imu_attempts++;
                ack(&p, 0x03, 0x80); // 迟到的其他命令 ACK
                ack(&p, 0x40, 0x00); // NACK 不能当作成功
            } else {
                ack(&p, p.data[10], 0x80);
            }
        }
    }
    assert(imu_attempts == 3 && delivered[1] == 0 && errors == 4);
}

static void test_spi_reply_matching(void)
{
    reset();
    unsigned handled = 0;
    packet_t left = {0}, right = {0};
    for (unsigned i = 0; i < 50 && !right.len; i++) {
        tick();
        while (handled < sent) {
            packet_t p = packets[handled++];
            if (p.data[0] != 1) continue;
            if (p.data[10] != 0x10) ack(&p, p.data[10], 0x80);
            else if (spi_addr(&p) == 0x603D) left = p; // 故意不答，等超时
            else { assert(spi_addr(&p) == 0x6046); right = p; }
        }
    }
    assert(left.len && right.len);
    unsigned before = sent;
    spi_reply(&left, false, false); // 迟到旧地址
    tick(); tick();
    assert(sent == before);
    spi_reply(&right, false, true); // 截断数据不能标记完成
    tick(); tick();
    assert(sent == before);
    packet_t wrong_instance = right;
    wrong_instance.instance = 1;
    spi_reply(&wrong_instance, false, false);
    tick();
    assert(sent == before);
    spi_reply(&right, false, false);
    tick(); tick();
    assert(sent == before + 1 && spi_addr(&packets[sent - 1]) == 0x6020);
}

static void put16(uint8_t *p, int16_t value)
{
    p[0] = (uint16_t)value & 0xFF;
    p[1] = (uint16_t)value >> 8;
}

static void test_motion_parser(void)
{
    // 使用固件当前链接的真实 hidkit，确认偏移 37 的最新 IMU 样本输出非零。
    uint8_t report[64] = {0x30};
    gamepad_state_t state;
    assert(gamepad_hid_mount(0, 0x057e, 0x2009, NULL, 0));
    put16(report + 37, 100);
    put16(report + 39, -200);
    put16(report + 41, 4096); // 静止时重力约 1g
    put16(report + 43, 300);
    put16(report + 45, -400);
    put16(report + 47, 500);
    assert(gamepad_hid_dispatch(0, report, sizeof(report), &state));
    assert(state.has_motion && state.accel_x == 400 && state.accel_y == 8192);
    assert(state.accel_z == -200 && state.gyro_x == 459 && state.gyro_y == 573 && state.gyro_z == -344);
    gamepad_hid_umount(0);
}

static void test_motion_units(void)
{
    uint8_t report[64] = {0x30};
    gamepad_state_t state;
    assert(gamepad_hid_mount(0, 0x057e, 0x2009, NULL, 0));
    // 未校准：±1g；4g 输出满量程；超过 4g 的原始硬件数据也限幅。
    put16(report + 37, 4096); put16(report + 39, -4096); put16(report + 41, 16384);
    // 默认灵敏度下 13371 counts = 936°/s；约 28571 counts = 2000°/s。
    put16(report + 43, 13371); put16(report + 45, -13371); put16(report + 47, 28571);
    assert(gamepad_hid_dispatch(0, report, sizeof(report), &state));
    assert(state.accel_x == 8192 && state.accel_y == 32767 && state.accel_z == -8192);
    assert(state.gyro_x == 15335 && state.gyro_y == 32767 && state.gyro_z == -15335);
    put16(report + 41, -32768); put16(report + 47, -32768);
    assert(gamepad_hid_dispatch(0, report, sizeof(report), &state));
    assert(state.accel_y == -32767 && state.gyro_y == -32767);

    // 独立校准基准点：raw = acc divisor/4 就是 1g，raw = gyro scale 就是 936°/s。
    hidkit_switchpro_calib_t cal = {.valid = HIDKIT_SWITCHPRO_CAL_MOTION,
        .acc_origin = {100, -200, 300}, .acc_scale = {16500, 16600, 17500},
        .gyro_origin = {-10, 20, -30}, .gyro_scale = {14000, 15000, 16000},
        .acc_hoffset = {-688, 0, 4038}};
    assert(hidkit_switchpro_set_calib(0, &cal));
    put16(report + 37, 4100); put16(report + 39, 4200); put16(report + 41, 4300);
    put16(report + 43, 14000); put16(report + 45, 15000); put16(report + 47, 16000);
    assert(gamepad_hid_dispatch(0, report, sizeof(report), &state));
    assert(state.accel_x == -8192 && state.accel_y == 8192 && state.accel_z == -8192);
    assert(state.gyro_x == -15335 && state.gyro_y == 15335 && state.gyro_z == -15335);
    put16(report + 43, -10); put16(report + 45, 20); put16(report + 47, -30);
    assert(gamepad_hid_dispatch(0, report, sizeof(report), &state));
    assert(state.gyro_x == 0 && state.gyro_y == 0 && state.gyro_z == 0);

    // 用户日志中的真实工厂校准：按 SDL 的物理单位公式核对结果。
    cal = (hidkit_switchpro_calib_t){.valid = HIDKIT_SWITCHPRO_CAL_MOTION,
        .acc_origin = {-57, 37, 585}, .acc_scale = {16384, 16384, 16384},
        .gyro_origin = {-18, 0, 6}, .gyro_scale = {15335, 15335, 15335},
        .acc_hoffset = {-688, 0, 4038}};
    assert(hidkit_switchpro_set_calib(0, &cal));
    put16(report + 37, -777); put16(report + 39, 5); put16(report + 41, 4044);
    put16(report + 43, -20); put16(report + 45, -2); put16(report + 47, 9);
    assert(gamepad_hid_dispatch(0, report, sizeof(report), &state));
    assert(state.accel_x == -10 && state.accel_y == 8387 && state.accel_z == 1549);
    assert(state.gyro_x == 2 && state.gyro_y == 3 && state.gyro_z == 2);

    // 无效的灵敏度或分母回退缺省，不能除零或使符号翻转。
    cal.acc_scale[0] = cal.acc_origin[0];
    cal.gyro_scale[0] = -1;
    assert(hidkit_switchpro_set_calib(0, &cal));
    put16(report + 37, 4096); put16(report + 43, 13371);
    assert(gamepad_hid_dispatch(0, report, sizeof(report), &state));
    assert(state.accel_z == -8192 && state.gyro_z == -15335);
    gamepad_hid_umount(0);
}

static void test_control_reports_preserve_input(void)
{
    hidkit_init();
    static const uint8_t desc[] = {0x05, 0x01, 0x09, 0x05, 0xA1, 0x01, 0xC0};
    hidkit_dev_info_t info = {.vid = 0x057e, .pid = 0x2009, .dev_addr = 1,
                             .report_desc = desc, .report_desc_len = sizeof(desc)};
    int8_t slot = hidkit_mount(&info);
    assert(slot >= 0);
    key_events = pad_events = motion_events = 0;
    // 用户日志最后一份 0x30 的原始报文头与最新 IMU 样本。
    uint8_t report[64] = {0x30, 0xA9, 0x91, 0, 0x80, 0, 0x5D, 0x98, 0x77,
                         0xFA, 0xD7, 0x7B, 0x0B};
    put16(report + 37, -777); put16(report + 39, 5); put16(report + 41, 4044);
    put16(report + 43, -20); put16(report + 45, -2); put16(report + 47, 9);
    assert(hidkit_report(slot, report, sizeof(report)));
    assert(pad_events == 1 && motion_events == 1 && last_accel_z == 1554);
    unsigned keys_before = key_events;
    assert(keys_before == 0); // 日志中的共享 bit15 为保留位，不应产生 0x0200

    // 用户日志停在此处：0x8010 的 SPI 回包，魔数 FF FF（无用户校准）。
    uint8_t reply[64] = {0x21, 0xAD, 0x91, 0, 0x80, 0, 0x5D, 0xB8, 0x77,
                        2, 0xB8, 0x7B, 0x0B, 0x90, 0x10, 0x10, 0x80,
                        0, 0, 2, 0xFF, 0xFF};
    gamepad_state_t state;
    assert(!gamepad_hid_dispatch(slot, reply, sizeof(reply), &state));
    // ACK 不能当作输入状态；重复处理也不能伪造按键/运动事件。
    for (unsigned i = 0; i < 100; i++) {
        assert(hidkit_report(slot, reply, sizeof(reply)));
        assert(key_events == keys_before && pad_events == 1 && motion_events == 1);
    }
    uint8_t usb_reply[] = {0x81, 0x02};
    assert(!gamepad_hid_dispatch(slot, usb_reply, sizeof(usb_reply), &state));
    assert(hidkit_report(slot, usb_reply, sizeof(usb_reply)));
    assert(key_events == keys_before && pad_events == 1 && motion_events == 1);
    assert(hidkit_report(slot, report, sizeof(report)));
    assert(key_events == keys_before && pad_events == 2 && motion_events == 2);

    report[3] = 0x04; // B 按下
    assert(hidkit_report(slot, report, sizeof(report)));
    assert(key_events == keys_before + 1 && last_key == (HIDKIT_CODE_GAMEPAD | BTN_A) && last_pressed);
    assert(hidkit_report(slot, reply, sizeof(reply)));
    assert(key_events == keys_before + 1 && pad_events == 3 && motion_events == 3);
    hidkit_umount(slot);
    assert(key_events == keys_before + 2 && last_key == (HIDKIT_CODE_GAMEPAD | BTN_A) && !last_pressed);
    assert(pad_events == 4 && motion_events == 4 && last_accel_z == 0);
}

static void test_pro_ds5_axes(void)
{
    assert(gamepad_hid_mount(0, 0x057e, 0x2009, NULL, 0));
    assert(gamepad_hid_mount(1, 0x054c, 0x0ce6, NULL, 0));
    hidkit_switchpro_calib_t cal = {.valid = HIDKIT_SWITCHPRO_CAL_MOTION,
        .acc_scale = {16384, 16384, 16384}, .gyro_scale = {15335, 15335, 15335}};
    assert(hidkit_switchpro_set_calib(0, &cal));
    // SDL 的 PlayStation 对齐映射：native X→-Z，Y→-X，Z→+Y。
    static const unsigned target_axis[] = {2, 0, 1};
    static const int target_sign[] = {-1, -1, 1};
    for (unsigned axis = 0; axis < 3; axis++) {
        for (int sign = -1; sign <= 1; sign += 2) {
            uint8_t pro[64] = {0x30}, ds5[64] = {0x01};
            put16(pro + 37 + axis * 2, sign * 4096);
            put16(pro + 43 + axis * 2, sign * 15335);
            put16(ds5 + 22 + target_axis[axis] * 2, sign * target_sign[axis] * 8192);
            put16(ds5 + 16 + target_axis[axis] * 2, sign * target_sign[axis] * 15335);
            gamepad_state_t pro_state, ds5_state;
            assert(gamepad_hid_dispatch(0, pro, sizeof(pro), &pro_state));
            assert(gamepad_hid_dispatch(1, ds5, sizeof(ds5), &ds5_state));
            assert(pro_state.gyro_x == ds5_state.gyro_x && pro_state.gyro_y == ds5_state.gyro_y &&
                   pro_state.gyro_z == ds5_state.gyro_z);
            assert(pro_state.accel_x == ds5_state.accel_x && pro_state.accel_y == ds5_state.accel_y &&
                   pro_state.accel_z == ds5_state.accel_z);
        }
    }
    gamepad_hid_umount(0);
    gamepad_hid_umount(1);
    // Joy-Con 没有 Pro 的持握基准，此次保持其原生轴序。
    assert(gamepad_hid_mount(0, 0x057e, 0x2006, NULL, 0));
    uint8_t joycon[64] = {0x30};
    put16(joycon + 37, 4096);
    gamepad_state_t state;
    assert(gamepad_hid_dispatch(0, joycon, sizeof(joycon), &state));
    assert(state.accel_x == 8192 && state.accel_y == 0 && state.accel_z == 0);
    gamepad_hid_umount(0);
}

int main(void)
{
    test_control_reports_preserve_input();
    test_initialization();
    test_retries_and_user_calibration();
    test_subcmd_busy();
    test_imu_failure();
    test_spi_reply_matching();
    test_motion_parser();
    test_motion_units();
    test_pro_ds5_axes();
    puts("Switch Pro protocol and motion tests passed");
    return 0;
}
