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

// pico-hid-debugger：USB HID Host 输入调试/转发固件。
// PIO-USB Host（GPIO12/13）接收 HID 报文，hid_host_app 处理成统一事件，
// 经 hid_output 中间层按当前目的地输出：模拟键鼠设备转发，或 CDC 串口。
// TinyUSB 中 roothub port0 是原生 USB 控制器，roothub port1 是 pico-pio-usb。

#include "hardware/clocks.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"

#include "pio_usb.h"
#include "tusb.h"

#include "cdc_output.h"
#include "hid_output.h"
#include "out_device.h"
#include "hid_host_app.h"

/*------------- 主程序 -------------*/

// core1: 处理 USB Host 事件
void core1_main() {
  sleep_ms(10);

  // 通过 tuh_configure() 将 PIO 配置传递给 Host 栈
  // 注意: tuh_configure() 必须在 tuh_init() 之前调用
  pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
  tuh_configure(1, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);

  // Report 协议而非 Boot 协议：配合描述符级鼠标解析，
  // 才能拿到完整按键数/12bit 滚轮等扩展字段
  tuh_hid_set_default_protocol(HID_PROTOCOL_REPORT);

  // 在 core1 上初始化 PIO-USB Host 栈 (roothub port1)，用于处理 USB SOF 中断
  tuh_init(1);

  while (true) {
    tuh_task(); // TinyUSB Host 任务循环
  }
}

// core0: 处理 USB Device 事件
int main(void) {
  // 默认 125MHz 不合适，系统时钟必须是 12MHz 的整数倍（PIO-USB 时序依赖）
  set_sys_clock_khz(120000, true);

  // 初始化跨核队列与临界区。必须先于 core1 启动：
  // 生产者随时可能入队，自旋锁必须先就绪
  cdc_output_init();

  multicore_reset_core1();
  // 所有 USB Host 任务在 core1 上运行
  multicore_launch_core1(core1_main);

  sleep_ms(100);
  // 在原生 USB 上初始化 Device 栈 (roothub port0)
  tud_init(0);

  while (true) {
    tud_task();          // TinyUSB Device 任务循环
    cdc_output_flush();  // 队列批量出队，分发到 CDC / 模拟 HID
  }

  return 0;
}

//--------------------------------------------------------------------+
// USB Device CDC
//--------------------------------------------------------------------+

// 切换输出目的地。从设备模式切走时补发全零报告，
// 松开模拟键鼠上仍按着的键（core0 上下文，可直接操作 Device 栈）
static void cmd_set_dest(bool device)
{
  bool was_device = hid_output_dest_is_device();
  hid_output_set_dest_device(device);
  if (was_device && !device) {
    out_device_release_all();
  }
}

// CDC 接口收到上位机数据时的回调：单字符命令控制输出
//   'D' = 设备转发模式（模拟键鼠，开机默认）
//   'S' = 串口输出模式
//   'T' = 文本格式并切到串口   'B' = 二进制格式并切到串口
//   'M' = Mac 模式修饰键交换 LALT↔LGUI、RALT↔RGUI（默认）
//   'W' = 不交换（Windows 原生布局）
void tud_cdc_rx_cb(uint8_t itf)
{
  (void) itf;

  while (tud_cdc_available()) {
    char c = (char)tud_cdc_read_char();
    if (c == 'D' || c == 'd')      cmd_set_dest(true);
    else if (c == 'S' || c == 's') cmd_set_dest(false);
    else if (c == 'T' || c == 't') { hid_output_set_binary(false); cmd_set_dest(false); }
    else if (c == 'B' || c == 'b') { hid_output_set_binary(true);  cmd_set_dest(false); }
    else if (c == 'M' || c == 'm') hid_output_set_mac_swap(true);
    else if (c == 'W' || c == 'w') hid_output_set_mac_swap(false);
    // 其余字符忽略
  }
}
