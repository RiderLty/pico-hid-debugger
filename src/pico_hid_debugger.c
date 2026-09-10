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

#include "hardware/clocks.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"

#include "pio_usb.h"
#include "tusb.h"

#include <stdio.h>

#include "uart_output.h"
#include "tusb_log.h"

// 字符串化宏：启动标记里显示编译期确定的波特率
#define PICO_STR2(x) #x
#define PICO_STR(x)  PICO_STR2(x)

/*------------- 主程序 -------------*/

// 启动标记。必须在 core1 启动之前入队：它是本串第一条输出，此后才可能
// 出现任何 TinyUSB 日志——上位机见到它，即知本连接从系统启动起完整
// 抓取，枚举过程无缺失。这是唯一的 core0 生产者调用（仅发生在
// multicore_launch_core1 之前，SPSC 单生产者约束不被破坏）。
static void boot_banner(void) {
  char buf[96];
  int n = snprintf(buf, sizeof(buf) - 2,
                   "[BOOT]  system init: pico-hid-debugger uart=%s 8N1\r\n",
                   PICO_STR(UARTO_BAUDRATE));
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

  // 通过 tuh_configure() 将 PIO 配置传递给 Host 栈
  // 注意: tuh_configure() 必须在 tuh_init() 之前调用
  pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
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
