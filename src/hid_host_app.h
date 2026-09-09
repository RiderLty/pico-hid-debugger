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
 * 不做任何 HID 语义解析。全部回调运行在 core1 的 tuh_task() 上下文，
 * 单线程无并发问题。
 */

#include <stdint.h>

void tuh_mount_cb(uint8_t dev_addr);
void tuh_umount_cb(uint8_t dev_addr);
void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance,
                      uint8_t const *desc_report, uint16_t desc_len);
void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance);
void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance,
                                uint8_t const *report, uint16_t len);

#endif // HID_HOST_APP_H
