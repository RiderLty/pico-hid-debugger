#ifndef HID_HOST_APP_H
#define HID_HOST_APP_H

/*
 * USB 设备信息采集模块。
 *
 * 实现 TinyUSB Host 的回调（tuh_*_cb），完成调试信息采集：
 *   - 设备挂载（tuh_mount_cb）：异步抓取设备描述符、配置描述符、
 *     字符串描述符（语言 ID/厂商/产品/序列号）并逐行 dump
 *   - HID 接口挂载（tuh_hid_mount_cb）：dump 接口信息与报告描述符
 *   - 报文接收（tuh_hid_report_received_cb）：按行 hexdump 原始报文
 *
 * 本模块只做"原始采集"：不做任何 HID 语义解析 —— 语义层在 hidkit_app.c，
 * 由这里在 dump 之后叠加调用（关掉它，固件行为与从前完全一致）。
 * 全部回调运行在 core1 的 tuh_task() 上下文，单线程无并发问题。
 */

#include <stdint.h>

// 行输出：格式化 + 追加 \r\n + 入队。**恒开** —— [ERROR] 这类异常行走它：
// 其余全静音时不能连错误也哑掉。（语义层的 [HIDKIT]/[HKDBG] 行有自己的出口
// emit_tagged，在 hidkit_app.c 里，与本函数无关。）
void hid_app_emit(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// 冷路径信息行出口：挂载与描述符 dump（[MOUNT]/[DEVDS]/[CFGDS]/[STRDS]/
// [HIDMT]/[RPTDS]/[UNHID]/[DEVRM]）。受运行期开关的 LOG_SW_INFO 门控
// （见 log_switch.h）—— 关掉只抑制打印，描述符抓取状态机照常跑。
void hid_app_emit_info(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

void tuh_mount_cb(uint8_t dev_addr);
void tuh_umount_cb(uint8_t dev_addr);
void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance,
                      uint8_t const *desc_report, uint16_t desc_len);
void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance);
void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance,
                                uint8_t const *report, uint16_t len);

#endif // HID_HOST_APP_H
