/*
 * 输出中间层：按当前目的地（模拟键鼠 / 串口）与串口格式（文本 / 二进制）
 * 把统一事件转发到对应实现。
 */

#include <string.h>

#include "cdc_output.h"
#include "hid_output.h"

// 当前串口输出格式：0 = 文本，1 = 二进制。static 变量而非宏，可运行时切换。
static volatile uint32_t s_binary_mode = 0;

// 当前输出目的地：false = 模拟键鼠设备转发（开机默认），true = 串口输出。
// core0 写（CDC 下行命令）、core1 读（事件转发），跨核访问用原子操作。
static volatile uint32_t s_dest_device = 1;

// Mac 模式修饰键交换（开机默认开启）。core0 写、core1 读，原子访问。
static volatile uint32_t s_mac_swap = 1;

bool hid_output_is_binary(void)
{
    return __atomic_load_n(&s_binary_mode, __ATOMIC_RELAXED) != 0;
}

void hid_output_set_binary(bool binary)
{
    __atomic_store_n(&s_binary_mode, binary ? 1u : 0u, __ATOMIC_RELAXED);
}

bool hid_output_dest_is_device(void)
{
    return __atomic_load_n(&s_dest_device, __ATOMIC_RELAXED) != 0;
}

void hid_output_set_dest_device(bool device)
{
    __atomic_store_n(&s_dest_device, device ? 1u : 0u, __ATOMIC_RELAXED);
}

bool hid_output_mac_swap_enabled(void)
{
    return __atomic_load_n(&s_mac_swap, __ATOMIC_RELAXED) != 0;
}

void hid_output_set_mac_swap(bool enable)
{
    __atomic_store_n(&s_mac_swap, enable ? 1u : 0u, __ATOMIC_RELAXED);
}

// 按目的地选实现：设备模式固定走 out_device；串口模式再按格式二分
static const hid_output_ops_t *route(void)
{
    if (hid_output_dest_is_device()) {
        return &hid_output_device_ops;
    }
    return hid_output_is_binary() ? &hid_output_binary_ops : &hid_output_text_ops;
}

void hid_output_mount(uint8_t instance, const hid_dev_info_t *info, uint8_t proto)
{
    route()->mount(instance, info, proto);
}

void hid_output_umount(uint8_t instance, const hid_dev_info_t *info)
{
    route()->umount(instance, info);
}

void hid_output_error(const char *msg)
{
    route()->error(msg);
}

// Mac 模式修饰键交换：单个修饰键字节内 LALT(bit2)↔LGUI(bit3)、
// RALT(bit6)↔RGUI(bit7)，Ctrl 保持不变。对两个修饰键字节都适用。
static uint8_t swap_mod_byte(uint8_t m)
{
    uint8_t r = m;
    r = (uint8_t)((r & ~0x0Cu) | ((m & 0x04u) << 1) | ((m & 0x08u) >> 1));
    r = (uint8_t)((r & ~0xC0u) | ((m & 0x40u) << 1) | ((m & 0x80u) >> 1));
    return r;
}

void hid_output_keyboard(uint8_t instance, const hid_dev_info_t *info,
                         const uint8_t *report, uint8_t len)
{
    // 单点修饰键转换：所有输出（文本/二进制/模拟设备）共用此入口，
    // 在路由之前完成，各实现无需感知 Mac/Windows 布局差异
    uint8_t buf[CDC_REC_MAX - 1];
    if (len > sizeof(buf)) len = (uint8_t)sizeof(buf);
    memcpy(buf, report, len);

    if (hid_output_mac_swap_enabled()) {
        if (len >= 1) buf[0] = swap_mod_byte(buf[0]);
        if (len >= 2) buf[1] = swap_mod_byte(buf[1]);
    }

    route()->keyboard(instance, info, buf, len);
}

void hid_output_mouse(uint8_t instance, const hid_dev_info_t *info,
                      const hid_mouse_frame_t *frame)
{
    route()->mouse(instance, info, frame);
}

void hid_output_generic(uint8_t instance, const hid_dev_info_t *info,
                        const uint8_t *report, uint16_t len)
{
    route()->generic(instance, info, report, len);
}
