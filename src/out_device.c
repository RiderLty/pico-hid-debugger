/*
 * 设备转发实现：把统一事件写入原生 USB 上模拟的键鼠设备。
 *
 * 键盘转发原始 8 字节报文；鼠标转发归一化 6 字节报文——模拟鼠标描述符
 * 的位布局（buttons u8 / wheel i8 / x i16 / y i16）与其完全一致，零转换。
 * 报文经跨核队列交给 core0 写入端点（与 CDC 输出同一约束：
 * tud_hid_* 只能在 core0 调用）。
 *
 * 挂载/拔出/错误与其他 HID 设备在设备模式下无对应输出，静默忽略；
 * 拔出时补发全零报告松开模拟设备上的残留按键。
 */

#include <string.h>

#include "tusb.h"

#include "cdc_output.h"
#include "hid_output.h"
#include "out_device.h"

// 模拟键盘为标准 8 字节布局（mod + reserved + keys[6]），超长截断
#define DEV_KB_REPORT_LEN 8u

static void dev_mount(uint8_t instance, const hid_dev_info_t *info, uint8_t proto)
{
    (void)instance; (void)info; (void)proto;
    // 模拟设备侧无挂载通知概念
}

static void dev_error(const char *msg)
{
    (void)msg;
}

static void dev_generic(uint8_t instance, const hid_dev_info_t *info,
                        const uint8_t *report, uint16_t len)
{
    (void)instance; (void)info; (void)report; (void)len;
    // 未识别设备无法透传为标准键鼠，丢弃
}

// 键盘报文 → 模拟键盘
static void dev_keyboard(uint8_t instance, const hid_dev_info_t *info,
                         const uint8_t *report, uint8_t len)
{
    (void)instance; (void)info;
    if (len > DEV_KB_REPORT_LEN) len = (uint8_t)DEV_KB_REPORT_LEN;
    cdc_output_send(CDCO_KIND_HID_KB, report, len);
}

// 归一化鼠标帧 → 模拟鼠标（位布局一致，直接打包）
static void dev_mouse(uint8_t instance, const hid_dev_info_t *info,
                      const hid_mouse_frame_t *frame)
{
    (void)instance; (void)info;
    uint8_t payload[6];
    payload[0] = frame->buttons;
    payload[1] = (uint8_t)frame->wheel;
    payload[2] = (uint8_t)(frame->x & 0xFF);   // x 小端
    payload[3] = (uint8_t)((frame->x >> 8) & 0xFF);
    payload[4] = (uint8_t)(frame->y & 0xFF);   // y 小端
    payload[5] = (uint8_t)((frame->y >> 8) & 0xFF);
    cdc_output_send(CDCO_KIND_HID_MS, payload, sizeof(payload));
}

// 设备拔出：补发全零报告，避免模拟键鼠上的按键残留"按住"状态。
// 注：hub 下多键盘同插时，一个拔出会连带清掉其他键盘的按住状态——
// 罕见场景，换取实现简单。
static void dev_umount(uint8_t instance, const hid_dev_info_t *info)
{
    (void)instance; (void)info;
    static const uint8_t zero_kb[DEV_KB_REPORT_LEN] = { 0 };
    static const uint8_t zero_ms[6] = { 0 };
    cdc_output_send(CDCO_KIND_HID_KB, zero_kb, sizeof(zero_kb));
    cdc_output_send(CDCO_KIND_HID_MS, zero_ms, sizeof(zero_ms));
}

const hid_output_ops_t hid_output_device_ops = {
    .mount    = dev_mount,
    .umount   = dev_umount,
    .error    = dev_error,
    .keyboard = dev_keyboard,
    .mouse    = dev_mouse,
    .generic  = dev_generic,
};

void out_device_release_all(void)
{
    static const uint8_t zero_kb[DEV_KB_REPORT_LEN] = { 0 };
    static const uint8_t zero_ms[6] = { 0 };

    // core0 上下文（CDC RX 命令处理），可直接操作 Device 栈
    if (tud_hid_n_ready(0)) tud_hid_n_report(0, 0, zero_kb, sizeof(zero_kb));
    if (tud_hid_n_ready(1)) tud_hid_n_report(1, 0, zero_ms, sizeof(zero_ms));
}
