/*
 * Switch Pro 手柄 USB 握手状态机（见 switch_pro.h 模块说明）。
 *
 * 实现取向（与 XInput 适配器阻塞式握手的取舍）：
 *   - XInput 的握手在 set_config 回调里同步跑完，包间节奏靠
 *     "重入 tuh_task 自旋等 OUT 完成"——因为 Xbox 握手报文不吃间隔；
 *   - Switch Pro 的应答（0x81/0x21）从 IN 端点异步回来，等 ACK 复杂
 *     且无收益；且在 tuh_hid_mount_cb 里阻塞会卡住同总线其他设备。
 *   - 故取其发送机构（发送失败下轮重试），弃其阻塞结构：
 *     时间步进 + 每步固定 15ms 间隔，不等 ACK（应答报文会自然出现在
 *     [HID] hexdump 里，供人工核对——本固件"所见即原始行为"的定位）。
 */

#include "switch_pro.h"

#include <string.h>

#include "pico/time.h"      // time_us_32 / time_reached
#include "tusb.h"

#include "hid_host_app.h"   // hid_app_emit / hid_app_emit_info（日志出口）

// ---------------------------------------------------------------- 槽位表

// 与 hid_host_app.c 的 desc_state_t 同一惯例：按 dev_addr 分槽，
// 容量 = 设备数 + hub 数 + 1
#define SP_ADDR_MAX (CFG_TUH_DEVICE_MAX + CFG_TUH_HUB + 1)

enum {
    SP_IDLE = 0,   // 空闲/未识别
    SP_HS1,        // 80 02 握手
    SP_BAUD,       // 80 03 波特率（USB 上无意义，失败也继续）
    SP_HS2,        // 80 02 再握手
    SP_NOTO,       // 80 04 关 USB 超时（无应答）
    SP_RUMBLE,     // 01 .. 48 01 使能震动
    SP_RPTMODE,    // 01 .. 03 30 设报告模式 → 0x30 流开始
    SP_RUNNING,    // 握手完成，不再发报文
};

typedef struct {
    uint8_t state;      // enum 值；SP_IDLE 即槽位空闲
    uint8_t instance;   // HID instance（发送目标）
    uint8_t retries;    // 当前步骤连续发送失败次数
    uint32_t next_us;   // 下一步动作的到期时间
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

// 子命令：01 + 16B 零 rumble + 子命令号 + 数据（19 字节）
static const uint8_t PKT_RUMBLE[19]  = {0x01, [17] = 0x48, [18] = 0x01};
static const uint8_t PKT_RPTMODE[19] = {0x01, [17] = 0x03, [18] = 0x30};

typedef struct {
    const uint8_t *pkt;
    uint16_t len;
    const char *name;
} sp_step_t;

static const sp_step_t s_steps[] = {
    [SP_HS1]     = {PKT_HS1,     sizeof(PKT_HS1),  "handshake"},
    [SP_BAUD]    = {PKT_BAUD,    sizeof(PKT_BAUD), "baudrate3m"},
    [SP_HS2]     = {PKT_HS2,     sizeof(PKT_HS2),  "handshake"},
    [SP_NOTO]    = {PKT_NOTO,    sizeof(PKT_NOTO), "no-timeout"},
    [SP_RUMBLE]  = {PKT_RUMBLE,  sizeof(PKT_RUMBLE),  "rumble-en"},
    [SP_RPTMODE] = {PKT_RPTMODE, sizeof(PKT_RPTMODE), "report-mode-30"},
};

#define SP_STEP_COUNT (sizeof(s_steps) / sizeof(s_steps[0]))

// ---------------------------------------------------------------- 参数

#define SP_STEP_INTERVAL_US 15000u  // 步间隔（应答异步回来，不看 ACK）
#define SP_RETRY_INTERVAL_US 2000u  // 发送失败重试间隔
#define SP_MAX_RETRIES 50u          // 连续失败上限（约 100ms）→ [ERROR] 放弃
#define SP_FIRST_DELAY_US 10000u    // 挂载到第一步的缓冲

// 任天堂：Pro Controller / 左右 Joy-Con（初始化序列相同）
#define SP_VID 0x057eu
static bool is_target(uint16_t vid, uint16_t pid)
{
    return vid == SP_VID &&
           (pid == 0x2009u || pid == 0x2006u || pid == 0x2007u);
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

    sp->state = SP_HS1;
    sp->instance = instance;
    sp->retries = 0;
    sp->next_us = time_us_32() + SP_FIRST_DELAY_US;
    hid_app_emit_info("[SWPRO] dev=%u itf=%u pid=%04x handshake start",
                      dev_addr, instance, pid);
}

void switch_pro_umount(uint8_t dev_addr)
{
    sp_slot_t *sp = slot_of(dev_addr);
    if (sp) {
        memset(sp, 0, sizeof(*sp)); // 回 IDLE；重枚举自然重跑握手
    }
}

void switch_pro_task(void)
{
    for (uint8_t addr = 0; addr < SP_ADDR_MAX; addr++) {
        sp_slot_t *sp = &s_sp[addr];
        if (sp->state == SP_IDLE || sp->state == SP_RUNNING) {
            continue;
        }

        uint32_t now = time_us_32();
        if (!time_reached(sp->next_us)) {
            continue;
        }

        const sp_step_t *step = &s_steps[sp->state];
        if (tuh_hid_send_report(addr, sp->instance, 0, step->pkt, step->len)) {
            hid_app_emit_info("[SWPRO] dev=%u send=%s len=%u",
                              addr, step->name, step->len);
            sp->retries = 0;
            sp->state++; // 最后一步自增后即 SP_RUNNING
            sp->next_us = now + SP_STEP_INTERVAL_US;
        } else {
            // busy/claim 失败：不推进，下一轮重试
            if (++sp->retries > SP_MAX_RETRIES) {
                hid_app_emit("[ERROR] dev=%u switchpro handshake failed at %s",
                             addr, step->name);
                memset(sp, 0, sizeof(*sp));
            } else {
                sp->next_us = now + SP_RETRY_INTERVAL_US;
            }
        }
    }
}
