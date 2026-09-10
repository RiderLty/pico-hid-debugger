/*
 * 跨核传输层实现：SPSC 字节块队列 + 硬件 UART 输出。
 *
 * 数据通路：core1 格式化文本行 → SPSC 队列（critical_section 保护）
 *           → core0 主循环 uart_output_flush() 批量出队，阻塞写 UART0。
 * UART 侧没有流控回压手段，队列满即丢：被丢的多为重复率最高的
 * 移动报文行，排空后一次性补报丢弃量。
 *
 * core0 自己要输出的行（[DROP] 补报、log_switch 的 [CTRL] 回执）不走队列 ——
 * 队列是 core1 单向的生产者，core0 入队会破坏 SPSC 单生产者约定 ——
 * 改走 uart_output_write_direct() 直写。UART 写操作因此只在 core0 发生，
 * 出口是本文件的 flush 与 write_direct 两个。
 */

#include <string.h>
#include <stdio.h>

#include "pico/sync.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"

#include "uart_output.h"

#define UARTO_QUEUE_DEPTH  1024u  // 2 的幂（掩码索引）；约 131KB 静态 RAM，
                                  // 可平滑枚举/多设备挂载等日志突发
                                  // （持续超带宽仍会溢出，[DROP] 补报）
#define UARTO_FLUSH_BUDGET 32u    // core0 每轮最多出队块数

typedef struct {
    uint8_t len;
    uint8_t data[UARTO_REC_MAX];
} uart_rec_t;

static uart_rec_t      s_queue[UARTO_QUEUE_DEPTH];
static uint16_t        s_head, s_tail, s_count;
static critical_section_t s_cs;
static uint32_t        s_dropped;   // 溢出丢弃块数（临界区内读写）

void uart_output_init(void)
{
    critical_section_init(&s_cs);
    s_head = s_tail = s_count = 0;
    s_dropped = 0;

    // UART0 @ GPIO2(TX)/GPIO3(RX)。RP2350 上这两个引脚的 UART 功能
    // 在 FUNCSEL 11（GPIO_FUNC_UART_AUX），非 RP2040 时代的 FUNC2
    uart_init(uart0, UARTO_BAUDRATE);
    gpio_set_function(UARTO_TX_PIN, GPIO_FUNC_UART_AUX);
    gpio_set_function(UARTO_RX_PIN, GPIO_FUNC_UART_AUX);

    // RX 上拉：uart_init() 会置 RXE，但 gpio_set_function() 只开输入缓冲、
    // **不动上下拉**，而 RP2350 的 pad 复位是 PUE=PDE=0 —— 也就是说在
    // USB-UART 适配器接上并打开之前，GPIO3 是**悬空**的。悬空输入在 2Mbaud
    // 下会产生随机起始位，每个随机字节都会被当成一发下行指令（见
    // log_switch.c），后果是日志被凑出来的掩码静默关掉。
    // 上拉让空闲线保持高电平，从根上消掉这类噪声
    gpio_pull_up(UARTO_RX_PIN);
}

// 入队核心（须持有临界区）。队列满则丢最新：保住更早的行。
static void queue_push_locked(const uint8_t *data, uint8_t len)
{
    if (s_count == UARTO_QUEUE_DEPTH) {
        s_dropped++;
        return;
    }
    uart_rec_t *r = &s_queue[s_tail];
    r->len = len;
    memcpy(r->data, data, len);
    s_tail = (uint16_t)((s_tail + 1u) & (UARTO_QUEUE_DEPTH - 1u));
    s_count++;
}

void uart_output_send(const void *data, uint8_t len)
{
    if (!data || !len) return;
    if (len > UARTO_REC_MAX) len = UARTO_REC_MAX;

    critical_section_enter_blocking(&s_cs);
    queue_push_locked((const uint8_t *)data, len);
    critical_section_exit(&s_cs);
}

void uart_output_write_direct(const void *data, size_t len)
{
    if (!data || !len) return;
    uart_write_blocking(uart0, (const uint8_t *)data, len);
}

void uart_output_flush(void)
{
    for (uint8_t budget = UARTO_FLUSH_BUDGET; budget; budget--) {
        critical_section_enter_blocking(&s_cs);
        if (s_count == 0) {
            uint32_t dropped = s_dropped;
            s_dropped = 0;
            critical_section_exit(&s_cs);

            // 队列排空后一次性补报丢弃量；直写 UART（core0 独占），
            // 不回队列，避免消费者自我拥塞
            if (dropped) {
                char buf[64];
                int n = snprintf(buf, sizeof(buf) - 2,
                                 "[DROP]  lost_lines=%lu\r\n",
                                 (unsigned long)dropped);
                if (n > 0) uart_output_write_direct(buf, (size_t)n);
            }
            break;
        }

        uint8_t len = s_queue[s_head].len;
        uint8_t data[UARTO_REC_MAX];
        memcpy(data, s_queue[s_head].data, len);
        s_head = (uint16_t)((s_head + 1u) & (UARTO_QUEUE_DEPTH - 1u));
        s_count--;
        critical_section_exit(&s_cs);

        uart_write_blocking(uart0, data, len);
    }
}
