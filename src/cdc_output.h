#ifndef CDC_OUTPUT_H
#define CDC_OUTPUT_H

#include <stdint.h>
#include <stdbool.h>

/*
 * 跨核传输层：SPSC 字节块队列。
 *
 * 上层（out_text / out_binary / out_device）把统一事件格式化成字节块后
 * 打上 kind 标签入队；所有 tud_cdc_* 与 tud_hid_* 调用只发生在 core0 的
 * cdc_output_flush() 里——TinyUSB Device 栈只在 core0 上运行，
 * 从结构上消除双核直写 Device FIFO 的竞态。
 */

// 记录 kind 标签（每条记录第 1 字节）
#define CDCO_KIND_CDC_DATA  1u   // 其余字节 = 写往 CDC 的文本行/二进制帧
#define CDCO_KIND_HID_KB    2u   // 其余字节 = 模拟键盘的原始报文
#define CDCO_KIND_HID_MS    3u   // 其余字节 = 模拟鼠标的归一化报文

// 单条记录上限（字节）。文本行最长约 96；二进制帧最长 73；HID 报文 6/8 字节。
#define CDC_REC_MAX   96u

// 初始化队列与临界区。必须在 multicore_launch_core1() 之前调用：
// 生产者（core1）随时可能入队，自旋锁必须先就绪。
void cdc_output_init(void);

// 入队一块带 kind 标签的字节（负载 ≤ CDC_REC_MAX-1，超限截断）。
// 队列满或目的地不可达时丢弃（满则计数，排空后补报）。
// 仅允许 core1 调用。
void cdc_output_send(uint8_t kind, const void *data, uint8_t len);

// core0 主循环调用：批量出队，按 kind 分发到 CDC 或模拟 HID 设备。
void cdc_output_flush(void);

#endif // CDC_OUTPUT_H
