#ifndef HIDKIT_APP_H
#define HIDKIT_APP_H

/*
 * hidkit 接入层：把解析库（lib/hidkit）与 XInput 适配器（lib/hidkit-tusb-xinput）
 * 接到本固件的 TinyUSB 回调上。
 *
 * 输出两族行（详见 README「输出格式」）：
 *   [HIDKIT] —— 语义事件（键/鼠标位移/手柄状态），来自库的四个弱符号出口函数
 *   [HKDBG]  —— 库内诊断（认领判定、槽位分配/挤出、卸载、报文未被消费），
 *               来自弱符号 hidkit_debug_printf，仅当 HIDKIT_DEBUG=1 时有调用点
 *
 * 与原始采集的分工：**原始报文 hexdump 照旧由 hid_host_app.c 输出**，本模块只在其后
 * 叠加语义层。两者互不依赖 —— 关掉本模块，固件仍是原来那台"所见即原始"的采集器。
 *
 * 线程：全部回调跑在 core1 的 tuh_task() 上下文（与库同核，库内无锁、静态槽位），
 * 只做格式化入队，绝不直接写 UART（见 uart_output.h 的单生产者约束）。
 */

#include <stdint.h>

/* [HIDKIT] 事件行开关（src/CMakeLists.txt 的 HIDKIT_APP_EVENTS 可覆盖）。
 * 1kHz 鼠标这类高流量设备下语义事件行会吃掉可观的 UART 带宽（已知限制第 1 条），
 * 置 0 只留原始 hexdump 与库内诊断。 */
#ifndef HIDKIT_APP_EVENTS
#define HIDKIT_APP_EVENTS 1
#endif

/* 初始化：清空库内槽位与本文的 instance→槽位映射。
 * 必须在 tuh_init() 之前、且只在 core1 调用一次（core1_main 开头）。 */
void hidkit_app_init(void);

/* HID 接口挂载：交给 hidkit 判定是否接管（<0 = 不认，本固件不做别的处理）。
 * itf_proto 传 TinyUSB 的 hid_interface_protocol_enum_t 值。 */
void hidkit_app_mount(uint8_t dev_addr, uint8_t instance, uint8_t itf_num,
                      uint8_t itf_proto, uint8_t const *desc_report,
                      uint16_t desc_len);

/* HID 报文：转交 hidkit 解析（未接管的 instance 静默跳过）。 */
void hidkit_app_report(uint8_t instance, uint8_t const *report, uint16_t len);

/* HID 接口拔出：释放槽位（库内会先补发"全部抬起"）。 */
void hidkit_app_umount(uint8_t instance);

#endif // HIDKIT_APP_H
