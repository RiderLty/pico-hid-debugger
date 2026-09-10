/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2019 Ha Thach (tinyusb.org)
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

#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
 extern "C" {
#endif

// 原型随 tusb_config.h 进入所有 TinyUSB 翻译单元，
// 使 CFG_TUSB_DEBUG_PRINTF 的宏替换点都能拿到声明
#include "tusb_log.h"

//--------------------------------------------------------------------
// COMMON CONFIGURATION
//--------------------------------------------------------------------

#define CFG_TUSB_OS               OPT_OS_PICO

// Enable device stack —— 调试器仅作 Host，禁用原生 USB Device 栈，
// 避免 Pico 在上位机上枚举出设备造成角色混淆
#define CFG_TUD_ENABLED     0

// Enable host stack with pio-usb if Pico-PIO-USB library is available
#define CFG_TUH_ENABLED     1
#define CFG_TUH_RPI_PIO_USB 1

// TinyUSB 内部日志级别：1=错误 2=警告+错误（含枚举过程） 3=信息（最啰嗦）。
// 日志经 CFG_TUSB_DEBUG_PRINTF 挂接的 tusb_log_printf() 按行组装，
// 以 [TUSB] 头从 UART 输出；级别 3 在高流量设备下会挤占带宽，按需调低
//
// SDK 的 tinyusb_host 默认带 -DCFG_TUSB_DEBUG=0（命令行定义），这里要 3 级：
// 先 undef 再定义，否则每份包含本头的 TU 都会报 "CFG_TUSB_DEBUG redefined"
#undef  CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG           3

// 把 TinyUSB 的 tu_printf 重定向到本工程的日志桥接（src/tusb_log.c）
#define CFG_TUSB_DEBUG_PRINTF    tusb_log_printf

/* USB DMA on some MCUs can only access a specific SRAM region with restriction on alignment.
 * Tinyusb use follows macros to declare transferring memory so that they can be put
 * into those specific section.
 * e.g
 * - CFG_TUSB_MEM SECTION : __attribute__ (( section(".usb_ram") ))
 * - CFG_TUSB_MEM_ALIGN   : __attribute__ ((aligned(4)))
 */
#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN          __attribute__ ((aligned(4)))
#endif

//--------------------------------------------------------------------
// HOST CONFIGURATION
//--------------------------------------------------------------------

// Size of buffer to hold descriptors and other data used for enumeration.
// 复杂鼠标描述符常超 256 字节，超限时 desc_report=NULL、无法解析
#define CFG_TUH_ENUMERATION_BUFSIZE 512

#define CFG_TUH_HUB                 1
// max device support (excluding hub device)
#define CFG_TUH_DEVICE_MAX          (CFG_TUH_HUB ? 4 : 1) // hub typically has 4 ports

#define CFG_TUH_HID                  16
#define CFG_TUH_HID_EPIN_BUFSIZE    64
#define CFG_TUH_HID_EPOUT_BUFSIZE   64

// XInput 类驱动（lib/hidkit-tusb-xinput）：Xbox 手柄走厂商接口而非 HID，
// 由 usbh_app_driver_get_cb()（src/hidkit_app.c）注册。0 = 整个驱动编译掉，
// 此时 Xbox 手柄枚举不到类驱动，只在 [MOUNT]/[DEVDS] 里露个脸。
#define CFG_TUH_XINPUT 1

#ifdef __cplusplus
 }
#endif

#endif /* _TUSB_CONFIG_H_ */
