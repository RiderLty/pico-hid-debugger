#ifndef TUSB_LOG_H
#define TUSB_LOG_H

/*
 * TinyUSB 内部日志桥接（CFG_TUSB_DEBUG 开启时被引用，见 tusb_config.h）。
 *
 * TinyUSB 的 TU_LOG 全部经 tu_printf → tusb_log_printf() 进入本模块：
 * 日志片段按行组装（TinyUSB 的日志片段不保证行完整），整行打上 [TUSB]
 * 前缀送入 uart_output，便于上位机按头筛选出栈内部日志。
 *
 * 仅允许 core1（tuh_task 所在核）调用链触碰本模块。
 *
 * 输出另受运行期开关的 LOG_SW_TUSB 位门控（见 log_switch.h）：关掉时
 * tusb_log_printf() 在格式化之前早退，[TUSB] 一行不出、CPU 也不花。
 * 默认就是关的（TinyUSB 级别 3 的日志量大，只在排查枚举/传输问题时才需要）。
 */

// TinyUSB 日志出口（经 tusb_config.h 的 CFG_TUSB_DEBUG_PRINTF 挂接为
// tu_printf）。printf 风格、返回 int（与 TinyUSB 期望的 printf 签名一致，
// 返回值无实际用途），由 TinyUSB 在 tuh_task / core1 中断上下文调用。
int tusb_log_printf(const char *format, ...) __attribute__((format(printf, 1, 2)));

// 初始化行组装缓冲与临界区。必须在 tuh_init() 之前、即任何 TinyUSB
// 日志产生之前调用（core1_main 开头）。
void tusb_log_init(void);

#endif // TUSB_LOG_H
