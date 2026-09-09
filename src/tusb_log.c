/*
 * TinyUSB 内部日志桥接实现。
 *
 * 数据流：TinyUSB TU_LOG（tuh_task / core1 上下文）→ tusb_log_printf()
 *   → 行组装缓冲（critical_section 保护：单核上等效关中断，
 *     防止 task 上下文与 core1 中断上下文的日志片段在缓冲内穿插）
 *   → 整行加 [TUSB] 头 → uart_output SPSC 队列 → core0 写 UART。
 *
 * TinyUSB 的日志以 \r\n/\n 收尾的片段居多，但不保证行完整（如
 * tu_print_buf 逐字节打印后再单独输出 "\r\n"），本模块把片段组装成
 * 完整行；超长内容强制断行（不出栈），只丢换行符不丢内容。
 */

#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include "pico/sync.h"

#include "uart_output.h"
#include "tusb_log.h"

// 输出行形如 "[TUSB] <内容>\r\n"，总长与 uart_output 单记录上限对齐
#define LOG_PREFIX       "[TUSB] "
#define LOG_PREFIX_LEN   7u
#define LOG_CONTENT_MAX  (UARTO_REC_MAX - LOG_PREFIX_LEN - 2u)

static critical_section_t s_cs;
static char     s_line[LOG_CONTENT_MAX];
static uint16_t s_pos;

void tusb_log_init(void)
{
    critical_section_init(&s_cs);
    s_pos = 0;
}

// 把当前组装中的行整体输出（须持有临界区）。空行直接丢弃。
static void flush_line_locked(void)
{
    if (s_pos == 0) return;

    char out[UARTO_REC_MAX];
    memcpy(out, LOG_PREFIX, LOG_PREFIX_LEN);
    memcpy(out + LOG_PREFIX_LEN, s_line, s_pos);
    out[LOG_PREFIX_LEN + s_pos] = '\r';
    out[LOG_PREFIX_LEN + s_pos + 1] = '\n';
    uart_output_send(out, (uint8_t)(LOG_PREFIX_LEN + s_pos + 2u));
    s_pos = 0;
}

// TinyUSB 日志入口：把 printf 片段按行组装后加头输出。
// 返回 int 仅为匹配 tu_printf 期望的 printf 签名，无实际用途。
int tusb_log_printf(const char *format, ...)
{
    char chunk[128];
    va_list ap;
    va_start(ap, format);
    int n = vsnprintf(chunk, sizeof(chunk), format, ap);
    va_end(ap);
    if (n <= 0) return 0;
    if (n > (int)sizeof(chunk) - 1) n = (int)sizeof(chunk) - 1;

    critical_section_enter_blocking(&s_cs);
    for (int i = 0; i < n; i++) {
        char c = chunk[i];
        if (c == '\n') {
            flush_line_locked();
        } else if (c != '\r') {
            if (s_pos >= LOG_CONTENT_MAX) flush_line_locked();  // 超长强制断行
            s_line[s_pos++] = c;
        }
    }
    critical_section_exit(&s_cs);
    return n;
}
