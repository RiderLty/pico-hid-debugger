/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2019 Ha Thach (tinyusb.org)
 *                    sekigon-gonnoc
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

// pico-hid-debugger：USB HID 设备调试器固件。
// PIO-USB Host（GPIO12/13）枚举插入的 HID 设备：挂载时 dump 描述符与
// 基本信息，运行时把原始报文按行 hexdump，全部经 UART0（GPIO2/3，
// 2Mbaud）输出。原生 USB Device 栈禁用（CFG_TUD_ENABLED=0），Pico
// 在上位机上不再是任何 USB 设备，仅作为独立的调试采集器。
// TinyUSB 中 roothub port0 是原生 USB 控制器（不初始化），port1 是 pico-pio-usb。
//
// 原始 hexdump 之外另叠一层语义事件（[HIDKIT]）与库内诊断（[HKDBG]）：
// 由 lib/hidkit（解析核心）与 lib/hidkit-tusb-xinput（Xbox 类驱动）提供，
// 接线见 src/hidkit_app.c。两者都是叠加层 —— 关掉不影响原始采集。

#include "hardware/clocks.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"

#include "pio_usb.h"
#include "tusb.h"

#include <stdio.h>

#include "uart_output.h"
#include "tusb_log.h"
#include "hidkit.h"        /* 自证行里的 HIDKIT_DEBUG / 容量宏 */
#include "hidkit_app.h"

// 字符串化宏：启动标记里显示编译期确定的波特率
#define PICO_STR2(x) #x
#define PICO_STR(x)  PICO_STR2(x)

// 补丁在位标志由 CMake 依据实际打补丁情况传入（顶层 CMakeLists.txt），
// 用于 [BOOT] 行自证版本：刷的到底是哪份固件，一眼可辨
#ifndef TUSB_HUB_PATCHED
#define TUSB_HUB_PATCHED 0
#endif
#ifndef PIO_USB_COMMIT
#define PIO_USB_COMMIT "unknown"   // PIO-USB 子模块当前提交（顶层 CMakeLists.txt 读取）
#endif

/*------------- 主程序 -------------*/

// 启动标记。必须在 core1 启动之前入队：它是本串第一条输出，此后才可能
// 出现任何 TinyUSB 日志——上位机见到它，即知本连接从系统启动起完整
// 抓取，枚举过程无缺失。这是唯一的 core0 生产者调用（仅发生在
// multicore_launch_core1 之前，SPSC 单生产者约束不被破坏）。
static void boot_banner(void) {
  // 版本号自建：TinyUSB 的 TUSB_VERSION_STRING 是单层 TU_STRING 展开，主版本
  // 会打印成宏名（"TUSB_VERSION_MAJOR.18.0"），用数值宏拼才可靠
  char ver[16];
  snprintf(ver, sizeof(ver), "%u.%u.%u", (unsigned)TUSB_VERSION_MAJOR,
           (unsigned)TUSB_VERSION_MINOR, (unsigned)TUSB_VERSION_REVISION);

  char buf[192];
  // tusb= 实际链接的 TinyUSB 版本；hubpatch= 是否打了 hub 驱动韧性补丁；
  // piousb= PIO-USB 子模块当前提交（本仓库固定为旧血脉顶端 9510f79）——
  // 排查前先核对这三个值，避免"日志一样其实是上一版固件"
  // hkdbg=/hkevt= hidkit 语义层的两个开关（库内诊断 / [HIDKIT] 事件行）：
  // "为什么没有 [HIDKIT] 行"先看这两个值，与"刷错固件"是同一类误判
  int n = snprintf(buf, sizeof(buf) - 2,
                   "[BOOT]  system init: pico-hid-debugger uart=%s 8N1"
                   " tusb=%s hubpatch=%u piousb=%s hkdbg=%u hkevt=%u\r\n",
                   PICO_STR(UARTO_BAUDRATE), ver, (unsigned)TUSB_HUB_PATCHED,
                   PIO_USB_COMMIT, (unsigned)HIDKIT_DEBUG,
                   (unsigned)HIDKIT_APP_EVENTS);
  if (n > 0) {
    if (n > (int)sizeof(buf) - 2) n = (int)sizeof(buf) - 2;
    uart_output_send(buf, (uint8_t)n);
  }
}

// core1: 处理 USB Host 事件
void core1_main() {
  sleep_ms(10);

  // TinyUSB 日志桥接先于 tuh_init 就绪：最早一条日志出现在栈初始化期间
  tusb_log_init();

  // hidkit 解析层上电清空（静态槽位表 + instance→槽位映射）。
  // 与栈无耦合，早于 tuh_init 即可；此后挂载/报文回调都会用到它
  hidkit_app_init();

  // 通过 tuh_configure() 将 PIO 配置传递给 Host 栈
  // 注意: tuh_configure() 必须在 tuh_init() 之前调用
  // D+/D- 引脚在应用侧显式配置（GPIO12/13，DM=DP+1）：
  // 上游库的 DP 默认值是 GPIO0，引脚属于应用配置，不依赖库内魔改默认值
  pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
  pio_cfg.pin_dp = 12;
  pio_cfg.pinout = PIO_USB_PINOUT_DPDM;
  tuh_configure(1, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);

  // Report 协议而非 Boot 协议：收到的是设备原生报文（Boot 协议下设备
  // 会改写成简化布局，丢失厂商扩展字段）
  tuh_hid_set_default_protocol(HID_PROTOCOL_REPORT);

  // 在 core1 上初始化 PIO-USB Host 栈 (roothub port1)，用于处理 USB SOF 中断
  tuh_init(1);

  while (true) {
    tuh_task(); // TinyUSB Host 任务循环
  }
}

// core0: UART 输出
int main(void) {
  // 默认 150MHz 不合适，系统时钟必须是 12MHz 的整数倍（PIO-USB 时序依赖）
  set_sys_clock_khz(120000, true);

  // 初始化 UART0 与跨核队列、临界区。必须先于 core1 启动：
  // 生产者随时可能入队，串口与自旋锁必须先就绪
  uart_output_init();

  // 等待数毫秒让 UART 线路与对端适配器就绪，随后发启动标记
  sleep_ms(10);
  boot_banner();

  multicore_reset_core1();
  // 所有 USB Host 任务在 core1 上运行
  multicore_launch_core1(core1_main);

  while (true) {
    uart_output_flush();  // 队列批量出队，写 UART0
  }

  return 0;
}
