/*
 * 运行期日志输出开关实现：单字节掩码 + UART0 下行指令解析。
 *
 * 数据通路：上位机写一个字节 → UART0 RX（GPIO3）
 *           → core0 的 log_switch_poll() 逐字节取出、过滤、置掩码
 *           → 回一行 [CTRL]（直写 UART，见 uart_output_write_direct）
 *           → core1 侧各输出点用 log_switch_on() 判定放行。
 *
 * 状态就是一个字节，core0 写、core1（含中断上下文）读，**刻意不加临界区**：
 * ARMv8-M 保证对齐字节访问的单拷贝原子性，RP2350 的 SRAM 无 cache，且这一个
 * 字节不伴随任何其它数据（没有 release/acquire 配对可错），最坏结果只是多出
 * 或少出一行。与 uart_output.c 的 s_dropped、tusb_log.c 的组装缓冲那种
 * "读-改-写多字结构"是两回事 —— 那里必须加锁。会打破本结论的改动：把掩码
 * 拆成多字段结构，或给它配一个计数器/游标。
 */

#include <stdio.h>

#include "hardware/uart.h"

#include "uart_output.h"
#include "log_switch.h"

// 单字节 volatile：见文件头注释，这里不需要临界区
static volatile uint8_t s_mask = LOG_SW_DEFAULT;

// 单次 poll 最多处理的字节数：下行洪泛不能把 flush 循环饿死。
// RX FIFO 仅 32 字节深，正常远用不满。
#define LOG_SW_POLL_MAX 64u

void log_switch_init(void)
{
    s_mask = LOG_SW_DEFAULT;

    // 读并**丢弃**上电窗口内积压的 RX 字节。两点：
    //   ① 不能走 apply_mask()：一个噪声字节会在用户连接之前就把默认掩码改掉；
    //   ② 放在 uart_output_init() 之后隔了 10ms（见 pico_hid_debugger.c 的
    //      sleep_ms），那正是线路最不稳的窗口，drain 要覆盖它 ——
    //      uart_init() 自带 reset/unreset，紧随其后的 FIFO 本来就是空的。
    // 读 dr 顺带清掉溢出位，RX 不会因溢出卡死（PL011 溢出只覆盖移位寄存器）。
    while (uart_is_readable(uart0)) {
        (void)uart_get_hw(uart0)->dr;
    }
}

bool log_switch_on(uint8_t bit)
{
    return (s_mask & bit) != 0u;
}

// 应用一个字节并回执。每个（无错的）字节都回 [CTRL]，不是"仅变化时"：
// 这样"我发的字节到了吗"在日志里直接可答，与 [BOOT] 的自证思路一致。
static void apply_mask(uint8_t byte)
{
    uint8_t const mask = (uint8_t)(byte & LOG_SW_MASK);   // 保留位忽略
    s_mask = mask;

    char buf[80];
    int n = snprintf(buf, sizeof(buf) - 2,
                     "[CTRL]  sw=0x%02X hidkit=%u hkdbg=%u tusb=%u hid=%u info=%u\r\n",
                     mask,
                     (unsigned)((mask & LOG_SW_HIDKIT) != 0u),
                     (unsigned)((mask & LOG_SW_HKDBG) != 0u),
                     (unsigned)((mask & LOG_SW_TUSB) != 0u),
                     (unsigned)((mask & LOG_SW_HID) != 0u),
                     (unsigned)((mask & LOG_SW_INFO) != 0u));
    if (n > 0) {
        // core0 直写（不是入队）：队列是 core1 单向的生产者，core0 入队会破坏
        // SPSC 单生产者约定。core0 自己是 UART 的唯一写者，直写安全
        uart_output_write_direct(buf, (size_t)n);
    }
}

void log_switch_poll(void)
{
    for (uint32_t i = 0; i < LOG_SW_POLL_MAX && uart_is_readable(uart0); i++) {
        // 一次读入：PL011 的帧/奇偶/断线错误位与数据同在 dr 里，读走即清。
        // 不能用 uart_getc() —— 它是 uart_read_blocking()，会阻塞。
        uint32_t const dr = uart_get_hw(uart0)->dr;

        // 有错的字节一律丢弃：波特率不匹配或线路噪声下，与其拿一个凑巧拼出来的
        // 掩码把日志静音，不如什么都不做。空闲线由 RX 上拉保持高电平
        // （见 uart_output_init），正常情况下根本不会读到这些错误位。
        if (dr & (UART_UARTDR_FE_BITS | UART_UARTDR_PE_BITS | UART_UARTDR_BE_BITS)) {
            continue;
        }
        apply_mask((uint8_t)dr);
    }
}
