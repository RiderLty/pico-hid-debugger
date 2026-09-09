#ifndef UART_OUTPUT_H
#define UART_OUTPUT_H

#include <stdint.h>

/*
 * 跨核传输层：SPSC 字节块队列 → 硬件 UART。
 *
 * core1（USB Host 侧）把格式化好的文本行入队；所有 UART 写操作只发生在
 * core0 的 uart_output_flush() 里，从结构上避免双核并发写同一 UART。
 * UART0 固定在 GPIO2(TX)/GPIO3(RX)，921600bps。
 */

// 波特率可经编译定义覆盖（src/CMakeLists.txt 的 UART_BAUD 缓存变量，
// 如 cmake -DUART_BAUD=2000000），须与上位机选择值一致
#ifndef UARTO_BAUDRATE
#define UARTO_BAUDRATE 921600u
#endif
#define UARTO_TX_PIN   2u
#define UARTO_RX_PIN   3u

// 单条记录上限（字节）。最长行为 16 字节 HEX 折行行，约 90 字符。
#define UARTO_REC_MAX 128u

// 初始化 UART0 与队列、临界区。必须在 multicore_launch_core1() 之前调用：
// 生产者（core1）随时可能入队，串口与自旋锁必须先就绪。
void uart_output_init(void);

// 入队一块字节（负载 ≤ UARTO_REC_MAX，超限截断）。
// 队列满则丢弃并计数（921600bps 约 11.5KB/s，高流量设备可能瞬时超载），
// 排空后由 flush 补报丢弃量。仅允许 core1 调用。
void uart_output_send(const void *data, uint8_t len);

// core0 主循环调用：批量出队写 UART；队列排空时补报溢出丢弃量。
void uart_output_flush(void);

#endif // UART_OUTPUT_H
