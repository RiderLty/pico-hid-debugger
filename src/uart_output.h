#ifndef UART_OUTPUT_H
#define UART_OUTPUT_H

#include <stddef.h>
#include <stdint.h>

/*
 * 跨核传输层：SPSC 字节块队列 → 硬件 UART。
 *
 * core1（USB Host 侧）把格式化好的文本行入队；UART 写操作**只在 core0**，
 * 出口有二：队列消费口 uart_output_flush()，与绕过队列的
 * uart_output_write_direct()（core0 自己要输出的行）。core1 绝不写 UART ——
 * 这样从结构上避免双核并发写同一 UART。
 * UART0 固定在 GPIO2(TX)/GPIO3(RX)，2000000bps。
 *
 * RX（GPIO3）用于接收上位机下行的单字节日志开关指令，由 core0 的
 * log_switch_poll() 排空；本模块只负责把它上拉并初始化，不解析内容。
 */

// 波特率可经编译定义覆盖（src/CMakeLists.txt 的 UART_BAUD 缓存变量）。
// 2M @ 240MHz 系统时钟分频恰为整数（零误差），容量 200KB/s，
// 可承载 1kHz 鼠标全量报文 + 级别 3 日志；须与上位机选择值一致
#ifndef UARTO_BAUDRATE
#define UARTO_BAUDRATE 2000000
#endif
#define UARTO_TX_PIN   2u
#define UARTO_RX_PIN   3u

// 单条记录上限（字节）。最长行为 16 字节 HEX 折行行，约 90 字符。
#define UARTO_REC_MAX 128u

// 初始化 UART0 与队列、临界区。必须在 multicore_launch_core1() 之前调用：
// 生产者（core1）随时可能入队，串口与自旋锁必须先就绪。
void uart_output_init(void);

// 入队一块字节（负载 ≤ UARTO_REC_MAX，超限截断）。
// 队列满则丢弃并计数，排空后由 flush 补报丢弃量。仅允许 core1 调用。
void uart_output_send(const void *data, uint8_t len);

// core0 主循环调用：批量出队写 UART；队列排空时补报溢出丢弃量。
void uart_output_flush(void);

// core0 专用：绕过队列直接写 UART（阻塞，须是完整的一行，含 \r\n）。
// 队列是 core1 单向的生产者，core0 入队会破坏 SPSC 单生产者约定 ——
// core0 自己要输出的行（[DROP] 补报、[CTRL] 回执）只能走这里。
// 与 uart_output_flush() 同核同线程顺序执行，core0 没有会写 UART 的 ISR，
// 因此不会与 flush 的输出穿插；但**可能排在 core1 更早入队的行之前**
// （直写不经过队列），这是外观问题，不是丢行。
void uart_output_write_direct(const void *data, size_t len);

#endif // UART_OUTPUT_H
