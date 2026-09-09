#ifndef HID_OUTPUT_H
#define HID_OUTPUT_H

#include <stdint.h>
#include <stdbool.h>
#include "hid_parser.h"

/*
 * 输出中间层：hid_host_app 产出的统一事件按当前模式转发到
 * 文本（out_text，core_input 风格）或二进制（out_binary，0x55AA 帧）实现。
 *
 * 二进制帧格式（多字节字段均为小端），解析规则按 type 二分：
 *   [0]   0x55 帧头
 *   [1]   0xAA 帧头
 *   [2]   len = 自本字段之后至帧尾的字节数
 *   [3]   type
 *
 *   键盘/鼠标帧（无来源字段，紧凑）——
 *   [4..] payload：
 *           0x01 键盘 = 原始键盘报文（mod + keys）
 *           0x02 鼠标 = 归一化帧 6 字节：
 *                buttons u8（bit0..7 = L R M B F 5 6 7）、wheel i8、x i16、y i16
 *
 *   其余帧在 type 后紧跟来源身份（PID u16、VID u16、port u8）再跟 payload ——
 *   [4-5] PID (u16)
 *   [6-7] VID (u16)
 *   [8]   port —— 报文来源 USB 设备地址 dev_addr（hub 下每设备唯一）
 *   [9..] payload：
 *           0x00 其他/未识别 HID 设备，payload = 原始报文
 *           0x10 挂载，payload = { itf u8, proto u8 }
 *           0x11 拔出，payload = { itf u8 }
 *           0x12 行队列溢出补报，payload = lost u32（无设备来源，PID/VID/port 填 0）
 *           0x13 错误，payload = ASCII 文本（同上填 0）
 *   payload 最长 64B，等于 Host EPIN 缓冲上限
 */

// 报文来源设备信息
typedef struct {
    uint16_t vid, pid;
    uint8_t  dev_addr;   // 端口（USB 设备地址）
} hid_dev_info_t;

// 帧 type 常量
#define HIDOUT_TYPE_NONE      0x00
#define HIDOUT_TYPE_KEYBOARD  0x01
#define HIDOUT_TYPE_MOUSE     0x02
#define HIDOUT_TYPE_MOUNT     0x10
#define HIDOUT_TYPE_UMOUNT    0x11
#define HIDOUT_TYPE_DROP      0x12
#define HIDOUT_TYPE_ERROR     0x13

// 输出实现的统一接口（同一份归一化数据，两种呈现方式）
typedef struct {
    void (*mount)(uint8_t instance, const hid_dev_info_t *info, uint8_t proto);
    void (*umount)(uint8_t instance, const hid_dev_info_t *info);
    void (*error)(const char *msg);
    void (*keyboard)(uint8_t instance, const hid_dev_info_t *info,
                     const uint8_t *report, uint8_t len);
    void (*mouse)(uint8_t instance, const hid_dev_info_t *info,
                  const hid_mouse_frame_t *frame);
    void (*generic)(uint8_t instance, const hid_dev_info_t *info,
                    const uint8_t *report, uint16_t len);
} hid_output_ops_t;

extern const hid_output_ops_t hid_output_text_ops;    // out_text.c
extern const hid_output_ops_t hid_output_binary_ops;  // out_binary.c
extern const hid_output_ops_t hid_output_device_ops;  // out_device.c（模拟键鼠）

// 统一事件入口（hid_host_app 在 core1 上调用，内部按当前模式转发；
// 全部运行于 core1 单线程，无并发问题）
void hid_output_mount(uint8_t instance, const hid_dev_info_t *info, uint8_t proto);
void hid_output_umount(uint8_t instance, const hid_dev_info_t *info);
void hid_output_error(const char *msg);
void hid_output_keyboard(uint8_t instance, const hid_dev_info_t *info,
                         const uint8_t *report, uint8_t len);
void hid_output_mouse(uint8_t instance, const hid_dev_info_t *info,
                      const hid_mouse_frame_t *frame);
void hid_output_generic(uint8_t instance, const hid_dev_info_t *info,
                        const uint8_t *report, uint16_t len);

// 输出模式开关（static 变量存于 hid_output.c，非宏，可运行时切换；
// core0 经 CDC 下行命令 'T'/'B' 写入，事件转发侧读取，故用原子访问）
bool hid_output_is_binary(void);
void hid_output_set_binary(bool binary);

// 输出目的地开关：设备转发（模拟键鼠，开机默认）与串口输出二选一。
// 'D' 切设备模式，'S'/'T'/'B' 切串口模式（T/B 同时选定格式）。
void hid_output_set_dest_device(bool device);
bool hid_output_dest_is_device(void);

// Mac 模式修饰键交换开关（开机默认开启）：
// 键盘报文的修饰键区 LALT↔LGUI、RALT↔RGUI 对调（Ctrl 不变）后再生效于所有输出
// （串口文本/二进制与模拟键鼠）。'M' 开启（Mac），'W' 关闭（Windows 原生布局）。
void hid_output_set_mac_swap(bool enable);
bool hid_output_mac_swap_enabled(void);

// 构造无设备来源的元信息帧（DROP 等场景，vid/pid/port 填 0），返回帧长。
// 由 out_binary.c 实现、cdc_output.c 的溢出补报使用。
uint8_t hid_output_binary_meta(uint8_t *dst, uint8_t type,
                               const void *payload, uint8_t paylen);

#endif // HID_OUTPUT_H
