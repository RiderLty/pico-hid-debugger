#ifndef LOG_SWITCH_H
#define LOG_SWITCH_H

#include <stdint.h>
#include <stdbool.h>

/*
 * 运行期日志输出开关：UART0 下行**单字节**指令 → 位掩码。
 *
 * 一个字节的每一位控制一类输出，收到即生效（不需要重编译/重刷）。这是叠加在
 * 编译期开关（HIDKIT_APP_EVENTS / HIDKIT_DEBUG / CFG_TUSB_DEBUG）之上的一层：
 * 编译期决定"代码在不在固件里"，运行期决定"现在出不出"。
 *
 * 位定义是**固件与上位机的接口**，index.html 的勾选框按同一张表下发
 * （位号写在 DOM 的 data-sw 上），改这里必须同步改那边与 README「运行期输出开关」。
 *
 * 恒开、不设位的 TAG：[BOOT]（开机自证，先于任何指令）、[ERROR]、[DROP]
 * 与 [CTRL]（开关自身的回执）—— 这四个正是"其余全静音"时最需要看到的。
 */

#define LOG_SW_HIDKIT  (1u << 0)   // [HIDKIT] 语义事件行
#define LOG_SW_HKDBG   (1u << 1)   // [HKDBG]  库内诊断行
#define LOG_SW_TUSB    (1u << 2)   // [TUSB]   TinyUSB 栈日志
#define LOG_SW_HID     (1u << 3)   // [HID]    原始报文 hexdump
#define LOG_SW_INFO    (1u << 4)   // [MOUNT]/[DEVDS]/[CFGDS]/[STRDS]/[HIDMT]/
                                   // [RPTDS]/[UNHID]/[DEVRM] 挂载与描述符 dump

// 有效位与上电默认值。
//
// 默认**全开**（0x1F），这是刻意的"失败要响"取向：固件没法知道上位机想要什么，
// 而任何不是 index.html 的观察者（screen、tools/uart_monitor.py）根本没有下发
// 掩码的手段，只能吃默认值 —— 默认全开意味着它们永远不会遇到"某个类别静默
// 消失"。index.html 侧的记忆偏好默认是 0x13（安静），连上就会把掩码压下来，
// 所以用它的人并不会因此被淹没。
#define LOG_SW_MASK    0x1Fu
#define LOG_SW_DEFAULT (LOG_SW_MASK)

// 初始化：置默认掩码，并读丢弃 RX FIFO 里的残留字节。
// 必须在 multicore_launch_core1() 之前、且在 core0 调用（core1 一起来就可能读
// 掩码，晚于 launch 会让 core1 看到 BSS 的 0，把最早的栈日志全丢掉）。
// 本函数不输出任何东西 —— 否则 boot_banner() 不再是本次连接的第一行。
void log_switch_init(void);

// 某位是否放行。热路径：core1 每份报文、TinyUSB 每条日志都会调。
// 只读一个单字节 volatile，无锁（理由见 log_switch.c）。
bool log_switch_on(uint8_t bit);

// core0 主循环调用：非阻塞排空 UART0 RX，逐字节应用掩码并回一行 [CTRL]。
// 必须在 core0 调用 —— 它是 UART 的唯一读者与写者之一。
void log_switch_poll(void);

#endif // LOG_SWITCH_H
