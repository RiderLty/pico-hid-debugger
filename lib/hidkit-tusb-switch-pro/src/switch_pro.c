/*
 * Switch Pro 手柄 USB 握手/初始化/校准状态机（见 switch_pro.h 模块说明）。
 *
 * 实现取向（与 XInput 适配器阻塞式握手的取舍）：
 *   - XInput 的握手在 set_config 回调里同步跑完，包间节奏靠
 *     "重入 tuh_task 自旋等 OUT 完成"——因为 Xbox 握手报文不吃间隔；
 *   - USB 握手按时间步进；0x01 子命令则异步等匹配的 0x21 ACK，
 *     超时有限重发，避免把 OUT 入队成功当成 IMU 已开启。
 *   - SPI 读核对 ACK、地址与长度；超时用缺省值继续。
 *     所有等待都在 task 中完成，不阻塞同总线其他设备。
 */

#include "switch_pro.h"

#include <string.h>

// ---------------------------------------------------------------- 槽位表

// 按 USB 地址分槽；容量由消费方编译期配置（含地址 0）。
#define SP_ADDR_MAX SWITCH_PRO_ADDR_MAX

enum {
    SP_IDLE = 0,   // 空闲/未识别
    // —— 阶段 1：握手（顺序敏感，照 hid-nintendo）——
    SP_HS1,        // 80 02 握手
    SP_BAUD,       // 80 03 切手柄内部 UART 波特率
    SP_HS2,        // 80 02 再握手
    SP_NOTO,       // 80 04 关 USB 超时（无应答）
    SP_RUMBLE,     // 01 .. 48 01 使能震动
    SP_RPTMODE,    // 01 .. 03 30 设报告模式 → 0x30 流开始
    // —— 阶段 2：IMU + 校准 ——
    SP_IMU,        // 01 .. 40 01 使能 IMU
    SP_CAL_LFAC,   // SPI 0x603D 9B  左摇杆工厂校准
    SP_CAL_RFAC,   // SPI 0x6046 9B  右摇杆工厂校准
    SP_CAL_IMUFAC, // SPI 0x6020 24B IMU 工厂校准
    SP_CAL_HOFF,   // SPI 0x6080 6B  水平偏移
    SP_CAL_LMAG,   // SPI 0x8010 2B  左用户校准魔数
    SP_CAL_RMAG,   // SPI 0x801B 2B  右用户校准魔数
    SP_CAL_MMAG,   // SPI 0x8026 2B  IMU 用户校准魔数
    SP_CAL_LUSR,   // SPI 0x8012 9B  左用户校准（仅命中魔数时执行）
    SP_CAL_RUSR,   // SPI 0x801D 9B  右用户校准（同上）
    SP_CAL_MUSR,   // SPI 0x8028 24B IMU 用户校准（同上）
    SP_DELIVER,    // 汇总校准 → 弱符号交付 → RUNNING
    SP_RUNNING,    // 全部完成，不再发报文
};

typedef struct {
    uint8_t state;      // enum 值；SP_IDLE 即槽位空闲
    uint8_t instance;   // HID instance（发送目标）
    uint8_t retries;    // 当前步骤连续发送失败次数
    uint8_t waits;      // 子命令已发出后等待应答的拍数；0 表示未发送
    uint8_t attempts;   // 当前初始化子命令已发送次数
    uint8_t packet_num; // 子命令包序号，0..15 循环，每设备独立
    bool    got_reply;  // 当前子命令是否已收到匹配的成功应答
    uint32_t next_us;   // 下一步动作的到期时间
    // 校准暂存（SPI 读回的原始字节，SP_DELIVER 时统一解码）
    uint8_t raw_lfac[9];
    uint8_t raw_rfac[9];
    uint8_t raw_imu[24];
    uint8_t raw_hoff[6];
    uint8_t magic_l[2], magic_r[2], magic_m[2];
    uint8_t raw_lusr[9];
    uint8_t raw_rusr[9];
    uint8_t raw_musr[24];
} sp_slot_t;

static sp_slot_t s_sp[SP_ADDR_MAX];

static inline sp_slot_t *slot_of(uint8_t dev_addr)
{
    return (dev_addr < SP_ADDR_MAX) ? &s_sp[dev_addr] : NULL;
}

// ---------------------------------------------------------------- 报文表

// USB 命令：2 字节 {0x80, cmd}，经中断 OUT 原样上线
static const uint8_t PKT_HS1[]   = {0x80, 0x02};
static const uint8_t PKT_BAUD[]  = {0x80, 0x03};
static const uint8_t PKT_HS2[]   = {0x80, 0x02};
static const uint8_t PKT_NOTO[]  = {0x80, 0x04};

// 子命令：[0]=01 [1]=序号 [2..9]=8B rumble [10]=子命令 [11..]=参数。
// USB 与蓝牙共用此布局；USB 的内部 UART 前缀由手柄固件添加。
static const uint8_t PKT_RUMBLE[12]  = {0x01, [10] = 0x48, [11] = 0x01};
static const uint8_t PKT_RPTMODE[12] = {0x01, [10] = 0x03, [11] = 0x30};
static const uint8_t PKT_IMUON[12]   = {0x01, [10] = 0x40, [11] = 0x01};

static bool send_subcmd(uint8_t addr, sp_slot_t *sp,
                        const uint8_t *pkt, uint16_t len)
{
    uint8_t buf[16]; // 最长请求：11B 头 + SPI 地址4B + 长度1B
    static const uint8_t neutral_rumble[8] = {
        0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40
    };
    if (len < 11 || len > sizeof(buf)) return false;
    memcpy(buf, pkt, len);
    buf[1] = sp->packet_num;
    memcpy(buf + 2, neutral_rumble, sizeof(neutral_rumble));
    // 缓冲含 Report ID；宿主必须在返回前复制报文，不能保留栈指针。
    if (!switch_pro_send_report(addr, sp->instance, buf, len)) return false;
    sp->packet_num = (sp->packet_num + 1u) & 0x0Fu;
    return true;
}

typedef struct {
    const uint8_t *pkt;
    uint16_t len;
    const char *name;
} sp_step_t;

// 阶段 1 + IMU 使能的固定报文表（下标 = 状态枚举值）
static const sp_step_t s_steps[] = {
    [SP_HS1]     = {PKT_HS1,     sizeof(PKT_HS1),  "handshake"},
    [SP_BAUD]    = {PKT_BAUD,    sizeof(PKT_BAUD), "baudrate3m"},
    [SP_HS2]     = {PKT_HS2,     sizeof(PKT_HS2),  "handshake"},
    [SP_NOTO]    = {PKT_NOTO,    sizeof(PKT_NOTO), "no-timeout"},
    [SP_RUMBLE]  = {PKT_RUMBLE,  sizeof(PKT_RUMBLE),  "rumble-en"},
    [SP_RPTMODE] = {PKT_RPTMODE, sizeof(PKT_RPTMODE), "report-mode-30"},
    [SP_IMU]     = {PKT_IMUON,   sizeof(PKT_IMUON),   "imu-on"},
};

// SPI 读步骤表（下标 = 状态枚举值 - SP_CAL_LFAC）
typedef struct {
    uint32_t addr;
    uint8_t  len;
    uint8_t *dest;      // 暂存区（sp_slot_t 内）
    const char *name;
} sp_read_t;

#define SP_READ_COUNT (SP_CAL_MUSR - SP_CAL_LFAC + 1)

static sp_read_t read_of(sp_slot_t *sp, uint8_t state)
{
    static sp_read_t table[] = {
        [SP_CAL_LFAC - SP_CAL_LFAC]   = {0x603D, 9,  NULL, "l-factory"},
        [SP_CAL_RFAC - SP_CAL_LFAC]   = {0x6046, 9,  NULL, "r-factory"},
        [SP_CAL_IMUFAC - SP_CAL_LFAC] = {0x6020, 24, NULL, "imu-factory"},
        [SP_CAL_HOFF - SP_CAL_LFAC]   = {0x6080, 6,  NULL, "horiz-offset"},
        [SP_CAL_LMAG - SP_CAL_LFAC]   = {0x8010, 2,  NULL, "l-user-magic"},
        [SP_CAL_RMAG - SP_CAL_LFAC]   = {0x801B, 2,  NULL, "r-user-magic"},
        [SP_CAL_MMAG - SP_CAL_LFAC]   = {0x8026, 2,  NULL, "m-user-magic"},
        [SP_CAL_LUSR - SP_CAL_LFAC]   = {0x8012, 9,  NULL, "l-user"},
        [SP_CAL_RUSR - SP_CAL_LFAC]   = {0x801D, 9,  NULL, "r-user"},
        [SP_CAL_MUSR - SP_CAL_LFAC]   = {0x8028, 24, NULL, "m-user"},
    };
    sp_read_t r = table[state - SP_CAL_LFAC];
    switch (state) {   // dest 指向各槽自己的暂存（表里放不了相对指针）
    case SP_CAL_LFAC:   r.dest = sp->raw_lfac;  break;
    case SP_CAL_RFAC:   r.dest = sp->raw_rfac;  break;
    case SP_CAL_IMUFAC: r.dest = sp->raw_imu;   break;
    case SP_CAL_HOFF:   r.dest = sp->raw_hoff;  break;
    case SP_CAL_LMAG:   r.dest = sp->magic_l;   break;
    case SP_CAL_RMAG:   r.dest = sp->magic_r;   break;
    case SP_CAL_MMAG:   r.dest = sp->magic_m;   break;
    case SP_CAL_LUSR:   r.dest = sp->raw_lusr;  break;
    case SP_CAL_RUSR:   r.dest = sp->raw_rusr;  break;
    case SP_CAL_MUSR:   r.dest = sp->raw_musr;  break;
    default:            r.dest = NULL;          break;
    }
    return r;
}

#define SP_STEP_COUNT (sizeof(s_steps) / sizeof(s_steps[0]))

// ---------------------------------------------------------------- 参数

#define SP_STEP_INTERVAL_US 15000u  // 步间隔（握手应答异步回来，不看 ACK）
#define SP_RETRY_INTERVAL_US 2000u  // 发送失败重试间隔
#define SP_MAX_RETRIES 50u          // 连续失败上限（约 100ms）→ [ERROR] 放弃
#define SP_FIRST_DELAY_US 10000u    // 挂载到第一步的缓冲
#define SP_REPLY_EXTRA_WAITS 7u     // 应答超时约 105ms（15ms/拍）
#define SP_MAX_CMD_ATTEMPTS 3u      // 初始化子命令 ACK 超时的发送次数上限

// 用户校准区魔数（B2 A1 = 有效）
static inline bool magic_ok(const uint8_t *m)
{
    return m[0] == 0xB2 && m[1] == 0xA1;
}

// 任天堂：Pro Controller / 左右 Joy-Con（初始化序列相同）
#define SP_VID 0x057eu
static bool is_target(uint16_t vid, uint16_t pid)
{
    return vid == SP_VID &&
           (pid == 0x2009u || pid == 0x2006u || pid == 0x2007u);
}

// ---------------------------------------------------------------- 校准汇总

// 12bit LSB-first 打包字段提取：9B 校准块的第 idx 个字段（idx 0..5）。
// 字段跨 2 字节（bit 偏移只有 0/4），不需要第 3 字节 —— 不越界读取
static uint16_t cal_field(const uint8_t *raw, unsigned idx)
{
    unsigned bit = idx * 12u;
    uint32_t acc = (uint32_t)raw[bit >> 3] | ((uint32_t)raw[(bit >> 3) + 1] << 8);
    return (uint16_t)((acc >> (bit & 7u)) & 0x0FFFu);
}

static inline int16_t le16(const uint8_t *p)
{
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

// 一支摇杆的 9B 校准块 → 中心 + 满偏。左/右的存储顺序不同（hid-nintendo）：
//   左：X满偏, Y满偏, X中心, Y中心, X满偏, Y满偏（前段=max 后段=min）
//   右：X中心, Y中心, X满偏(min), Y满偏(min), X满偏(max), Y满偏(max)
static void decode_stick(const uint8_t *raw, bool right,
                         uint16_t *xc, uint16_t *yc,
                         uint16_t *xmax, uint16_t *xmin,
                         uint16_t *ymax, uint16_t *ymin)
{
    if (!right) {
        *xmax = cal_field(raw, 0); *ymax = cal_field(raw, 1);
        *xc   = cal_field(raw, 2); *yc   = cal_field(raw, 3);
        *xmin = cal_field(raw, 4); *ymin = cal_field(raw, 5);
    } else {
        *xc   = cal_field(raw, 0); *yc   = cal_field(raw, 1);
        *xmin = cal_field(raw, 2); *ymin = cal_field(raw, 3);
        *xmax = cal_field(raw, 4); *ymax = cal_field(raw, 5);
    }
}

static inline bool buf_nonzero(const uint8_t *p, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        if (p[i]) return true;
    }
    return false;
}

static void sp_deliver(uint8_t dev_addr, sp_slot_t *sp)
{
    switch_pro_calib_t cal;
    memset(&cal, 0, sizeof(cal));

    // 摇杆：工厂读数到位才标 valid（全零 = 该步超时没拿到）；用户校准
    // 优先（魔数命中且已读到），否则工厂值
    const uint8_t *lsrc = magic_ok(sp->magic_l) && buf_nonzero(sp->raw_lusr, 9)
                          ? sp->raw_lusr : sp->raw_lfac;
    const uint8_t *rsrc = magic_ok(sp->magic_r) && buf_nonzero(sp->raw_rusr, 9)
                          ? sp->raw_rusr : sp->raw_rfac;
    const bool sticks = buf_nonzero(sp->raw_lfac, 9) &&
                        buf_nonzero(sp->raw_rfac, 9);
    decode_stick(lsrc, false, &cal.ls_xc, &cal.ls_yc,
                 &cal.ls_xmax, &cal.ls_xmin, &cal.ls_ymax, &cal.ls_ymin);
    decode_stick(rsrc, true,  &cal.rs_xc, &cal.rs_yc,
                 &cal.rs_xmax, &cal.rs_xmin, &cal.rs_ymax, &cal.rs_ymin);

    // IMU：用户校准（0x8028，24B 同工厂布局）优先
    const uint8_t *isrc = magic_ok(sp->magic_m) && buf_nonzero(sp->raw_musr, 24)
                          ? sp->raw_musr : sp->raw_imu;
    cal.acc_origin[0]  = le16(isrc + 0);  cal.acc_origin[1]  = le16(isrc + 2);
    cal.acc_origin[2]  = le16(isrc + 4);
    cal.gyro_origin[0] = le16(isrc + 12); cal.gyro_origin[1] = le16(isrc + 14);
    cal.gyro_origin[2] = le16(isrc + 16);
    cal.acc_hoffset[0] = le16(sp->raw_hoff + 0);
    cal.acc_hoffset[1] = le16(sp->raw_hoff + 2);
    cal.acc_hoffset[2] = le16(sp->raw_hoff + 4);
    for (unsigned i = 0; i < 3; i++) {
        cal.acc_scale[i] = le16(isrc + 6 + i * 2);
        cal.gyro_scale[i] = le16(isrc + 18 + i * 2);
    }

    const bool motion = buf_nonzero(isrc, 24);
    if (sticks) cal.valid |= SWITCH_PRO_CAL_STICKS;
    if (motion) cal.valid |= SWITCH_PRO_CAL_MOTION;

    switch_pro_log(false, "[SWPRO] dev=%u calib %s (valid=0x%02x)",
                      dev_addr, cal.valid ? "ready" : "empty", cal.valid);
    switch_pro_calibration(dev_addr, sp->instance, &cal);
}

// ---------------------------------------------------------------- API

void switch_pro_maybe_mount(uint8_t dev_addr, uint8_t instance,
                            uint16_t vid, uint16_t pid)
{
    sp_slot_t *sp = slot_of(dev_addr);
    if (!sp || !is_target(vid, pid)) {
        return;
    }
    if (sp->state != SP_IDLE) {
        return; // 同地址已接管（如复合接口的第二实例），忽略
    }

    memset(sp, 0, sizeof(*sp));
    sp->state = SP_HS1;
    sp->instance = instance;
    sp->next_us = switch_pro_time_us() + SP_FIRST_DELAY_US;
    switch_pro_log(false, "[SWPRO] dev=%u itf=%u pid=%04x handshake start",
                      dev_addr, instance, pid);
}

void switch_pro_umount(uint8_t dev_addr)
{
    sp_slot_t *sp = slot_of(dev_addr);
    if (sp) {
        memset(sp, 0, sizeof(*sp)); // 回 IDLE；重枚举自然重跑握手
    }
}

void switch_pro_report(uint8_t dev_addr, uint8_t instance,
                       const uint8_t *report, uint16_t len)
{
    sp_slot_t *sp = slot_of(dev_addr);
    if (!sp || !report || instance != sp->instance || !sp->waits) return;

    // 只看 0x21 子命令应答（预期布局：[13]=ack [14]=echo id [15..18]=地址 LE
    // [19]=长度 [20..]=数据，依据 hid-nintendo；读不到就按缺省值继续）
    if (len < 15 || report[0] != 0x21) return;

    if (sp->state >= SP_RUMBLE && sp->state <= SP_IMU) {
        const sp_step_t *step = &s_steps[sp->state];
        if (report[14] != step->pkt[10]) return;
        if (report[13] & 0x80) {
            sp->got_reply = true;
            switch_pro_log(false, "[SWPRO] dev=%u ack=%s", dev_addr, step->name);
        } else {
            switch_pro_log(true, "[ERROR] dev=%u switchpro %s NACK=%02x",
                         dev_addr, step->name, report[13]);
        }
        return;
    }

    if (sp->state < SP_CAL_LFAC || sp->state > SP_CAL_MUSR) return;
    if (report[14] != 0x10 || !(report[13] & 0x80) || len < 20) return;

    // SPI 所有应答 id 都是 0x10，必须核对地址，避免上一笔迟到应答污染下一笔。
    sp_read_t r = read_of(sp, sp->state);
    uint32_t addr = (uint32_t)report[15] | ((uint32_t)report[16] << 8) |
                    ((uint32_t)report[17] << 16) | ((uint32_t)report[18] << 24);
    if (addr != r.addr || report[19] != r.len || len < 20u + r.len) return;
    if (r.dest) memcpy(r.dest, report + 20, r.len);
    sp->got_reply = true;
}

void switch_pro_task(void)
{
    for (unsigned addr = 0; addr < SP_ADDR_MAX; addr++) {
        sp_slot_t *sp = &s_sp[addr];
        if (sp->state == SP_IDLE || sp->state == SP_RUNNING) {
            continue;
        }

        // 校准交付：无报文，进位即做
        if (sp->state == SP_DELIVER) {
            sp_deliver(addr, sp);
            sp->state = SP_RUNNING;
            continue;
        }

        uint32_t now = switch_pro_time_us();
        if ((uint32_t)(now - sp->next_us) >= 0x80000000u) {
            continue;
        }

        // 初始化子命令：OUT 入队成功后留在当前步骤，收到成功 ACK 才推进。
        if (sp->state >= SP_RUMBLE && sp->state <= SP_IMU) {
            if (sp->got_reply) {
                sp->got_reply = false;
                sp->waits = 0;
                sp->attempts = 0;
                sp->retries = 0;
                sp->state++;
                sp->next_us = now + SP_STEP_INTERVAL_US;
                continue;
            }
            if (sp->waits > 0) {
                if (sp->waits < SP_REPLY_EXTRA_WAITS) {
                    sp->waits++;
                    sp->next_us = now + SP_STEP_INTERVAL_US;
                    continue;
                }
                if (sp->attempts >= SP_MAX_CMD_ATTEMPTS) {
                    switch_pro_log(true, "[ERROR] dev=%u switchpro %s ACK timeout",
                                 addr, s_steps[sp->state].name);
                    memset(sp, 0, sizeof(*sp));
                    continue;
                }
                sp->waits = 0; // 到期重发；仍在同一步，下一份 ACK 仍可配对
            }
        }

        // SPI 读步骤：发出后等应答（0x21 回包核对地址）。
        // 等不到就带缺省值继续 —— 状态机绝不阻塞大循环
        if (sp->state >= SP_CAL_LFAC && sp->state <= SP_CAL_MUSR) {
            if (sp->got_reply) {
                sp->got_reply = false;
                sp->waits = 0;
                sp->retries = 0;
                sp->state++;
                sp->next_us = now + SP_STEP_INTERVAL_US;
                continue;
            }
            if (sp->waits > 0 && sp->waits <= SP_REPLY_EXTRA_WAITS) {
                // 已发出、还在等：续等（发过就不重发，避免命令重叠）
                if (sp->waits < SP_REPLY_EXTRA_WAITS) {
                    sp->waits++;
                    sp->next_us = now + SP_STEP_INTERVAL_US;
                } else {
                    switch_pro_log(false, "[SWPRO] dev=%u spi %s timeout, default used",
                                      addr, read_of(sp, sp->state).name);
                    sp->waits = 0;
                    sp->got_reply = false;   // 迟到的应答不许污染下一步
                    sp->state++;
                    sp->next_us = now + SP_STEP_INTERVAL_US;
                }
                continue;
            }
            // 首拍到：条件不满足的步骤直接跳过（用户区魔数未命中）
            if ((sp->state == SP_CAL_LUSR && !magic_ok(sp->magic_l)) ||
                (sp->state == SP_CAL_RUSR && !magic_ok(sp->magic_r)) ||
                (sp->state == SP_CAL_MUSR && !magic_ok(sp->magic_m))) {
                sp->state++;
                continue;
            }
            // 首拍到且条件满足：发 SPI 读命令
            uint8_t pkt[16];
            sp_read_t r = read_of(sp, sp->state);
            memset(pkt, 0, sizeof(pkt));
            pkt[0] = 0x01;
            pkt[10] = 0x10;
            pkt[11] = (uint8_t)(r.addr & 0xFF);
            pkt[12] = (uint8_t)((r.addr >> 8) & 0xFF);
            pkt[13] = (uint8_t)((r.addr >> 16) & 0xFF);
            pkt[14] = (uint8_t)((r.addr >> 24) & 0xFF);
            pkt[15] = r.len;
            if (send_subcmd(addr, sp, pkt, sizeof(pkt))) {
                switch_pro_log(false, "[SWPRO] dev=%u spi %s addr=0x%04x len=%u",
                                  addr, r.name, (unsigned)r.addr, r.len);
                sp->waits = 1;
                sp->next_us = now + SP_STEP_INTERVAL_US;
            } else {
                if (++sp->retries > SP_MAX_RETRIES) {
                    switch_pro_log(true, "[ERROR] dev=%u switchpro spi %s failed",
                                 addr, r.name);
                    memset(sp, 0, sizeof(*sp));
                } else {
                    sp->next_us = now + SP_RETRY_INTERVAL_US;
                }
            }
            continue;
        }

        // 固定报文步骤（阶段 1 + IMU 使能）
        const sp_step_t *step = (sp->state < SP_STEP_COUNT)
                                ? &s_steps[sp->state] : NULL;
        if (!step || !step->pkt) {   // 状态空洞防护：直接进位
            sp->state++;
            continue;
        }
        bool subcmd = sp->state >= SP_RUMBLE;
        bool sent = subcmd ? send_subcmd(addr, sp, step->pkt, step->len)
                           : switch_pro_send_report(addr, sp->instance, step->pkt, step->len);
        if (sent) {
            switch_pro_log(false, "[SWPRO] dev=%u send=%s len=%u",
                              addr, step->name, step->len);
            sp->retries = 0;
            if (subcmd) {
                sp->waits = 1;
                sp->attempts++;
            } else {
                sp->state++;
            }
            sp->next_us = now + SP_STEP_INTERVAL_US;
        } else {
            // busy/claim 失败：不推进，下一轮重试
            if (++sp->retries > SP_MAX_RETRIES) {
                switch_pro_log(true, "[ERROR] dev=%u switchpro handshake failed at %s",
                             addr, step->name);
                memset(sp, 0, sizeof(*sp));
            } else {
                sp->next_us = now + SP_RETRY_INTERVAL_US;
            }
        }
    }
}

// 可选出口的弱默认实现；宿主按需覆盖。
__attribute__((weak))
void switch_pro_calibration(uint8_t dev_addr, uint8_t instance,
                            const switch_pro_calib_t *cal)
{
    (void)dev_addr;
    (void)instance;
    (void)cal;
}

__attribute__((weak))
void switch_pro_log(bool error, const char *fmt, ...)
{
    (void)error;
    (void)fmt;
}
