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

#include "tusb.h"

// 复合设备：CDC 串口 + 模拟 HID 键盘 + 模拟 HID 鼠标。
// 新增键鼠接口后升 PID，使主机视为全新设备、干净重枚举。
#define USB_VID   0xCafe
#define USB_PID   0x4002
#define USB_BCD   0x0200

//--------------------------------------------------------------------+
// String Descriptor Index
//--------------------------------------------------------------------+
enum
{
  STRID_LANGID = 0,
  STRID_MANUFACTURER,
  STRID_PRODUCT,
  STRID_SERIAL,
  STRID_CDC,       // CDC 接口
  STRID_HID_KB,    // 模拟键盘接口
  STRID_HID_MS,    // 模拟鼠标接口
};

//--------------------------------------------------------------------+
// Interface Numbers
//--------------------------------------------------------------------+
enum
{
  ITF_NUM_CDC = 0,
  ITF_NUM_CDC_DATA,
  ITF_NUM_HID_KB,
  ITF_NUM_HID_MS,
  ITF_NUM_TOTAL
};

//--------------------------------------------------------------------+
// Endpoint Numbers
//--------------------------------------------------------------------+
#define EPNUM_CDC_NOTIF   0x81
#define EPNUM_CDC_OUT     0x02
#define EPNUM_CDC_IN      0x82

#define EPNUM_HID_KB_IN   0x83
#define EPNUM_HID_MS_IN   0x84

//--------------------------------------------------------------------+
// HID Report Descriptors（长度一律用 sizeof，勿手写常量）
//--------------------------------------------------------------------+

// 标准启动协议键盘：mod + reserved + keys[6]（8 字节报文）
static const uint8_t desc_hid_report_keyboard[] = {
    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x06,       // Usage (Keyboard)
    0xA1, 0x01,       // Collection (Application)
    0x05, 0x07,       //   Usage Page (Key Codes)
    0x19, 0xE0,       //   Usage Minimum (E0)
    0x29, 0xE7,       //   Usage Maximum (E7)
    0x15, 0x00,       //   Logical Minimum (0)
    0x25, 0x01,       //   Logical Maximum (1)
    0x75, 0x01,       //   Report Size (1)
    0x95, 0x08,       //   Report Count (8)
    0x81, 0x02,       //   Input (Data, Var, Abs) —— 修饰键
    0x95, 0x01,       //   Report Count (1)
    0x75, 0x08,       //   Report Size (8)
    0x81, 0x01,       //   Input (Const) —— 保留字节
    0x95, 0x06,       //   Report Count (6)
    0x75, 0x08,       //   Report Size (8)
    0x15, 0x00,       //   Logical Minimum (0)
    0x26, 0xFF, 0x00, //   Logical Maximum (255)
    0x05, 0x07,       //   Usage Page (Key Codes)
    0x19, 0x00,       //   Usage Minimum (0)
    0x29, 0x65,       //   Usage Maximum (0x65)
    0x81, 0x00,       //   Input (Data, Array) —— 按键数组
    0x05, 0x08,       //   Usage Page (LEDs)
    0x19, 0x01,       //   Usage Minimum (1)
    0x29, 0x05,       //   Usage Maximum (5)
    0x95, 0x05,       //   Report Count (5)
    0x75, 0x01,       //   Report Size (1)
    0x91, 0x02,       //   Output (Data, Var, Abs) —— 键盘灯
    0x95, 0x03,       //   Report Count (3)
    0x75, 0x01,       //   Report Size (1)
    0x91, 0x01,       //   Output (Const) —— 填充
    0xC0              // End Collection
};

// 模拟鼠标：buttons u8 | wheel i8 | x i16 | y i16（6 字节报文，
// 位布局与 hid_mouse_frame_t 归一化帧一致，转发零转换）。
// 不声明 Boot 子类——16bit 轴不符合 boot 布局，现代主机按报告协议解析。
static const uint8_t desc_hid_report_mouse[] = {
    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x02,       // Usage (Mouse)
    0xA1, 0x01,       // Collection (Application)
    0x09, 0x01,       //   Usage (Pointer)
    0xA1, 0x00,       //   Collection (Physical)
    0x05, 0x09,       //     Usage Page (Buttons)
    0x19, 0x01,       //     Usage Minimum (1)
    0x29, 0x08,       //     Usage Maximum (8)
    0x15, 0x00,       //     Logical Minimum (0)
    0x25, 0x01,       //     Logical Maximum (1)
    0x75, 0x01,       //     Report Size (1)
    0x95, 0x08,       //     Report Count (8)
    0x81, 0x02,       //     Input (Data, Var, Abs) —— 按键位掩码
    0x05, 0x01,       //     Usage Page (Generic Desktop)
    0x09, 0x38,       //     Usage (Wheel)
    0x15, 0x81,       //     Logical Minimum (-127)
    0x25, 0x7F,       //     Logical Maximum (127)
    0x75, 0x08,       //     Report Size (8)
    0x95, 0x01,       //     Report Count (1)
    0x81, 0x06,       //     Input (Data, Var, Rel) —— 滚轮
    0x09, 0x30,       //     Usage (X)
    0x09, 0x31,       //     Usage (Y)
    0x16, 0x01, 0x80, //     Logical Minimum (-32767)
    0x26, 0xFF, 0x7F, //     Logical Maximum (32767)
    0x75, 0x10,       //     Report Size (16)
    0x95, 0x02,       //     Report Count (2)
    0x81, 0x06,       //     Input (Data, Var, Rel) —— X/Y
    0xC0,             //   End Collection
    0xC0              // End Collection
};

//--------------------------------------------------------------------+
// Device Descriptors
//--------------------------------------------------------------------+
tusb_desc_device_t const desc_device =
{
  .bLength            = sizeof(tusb_desc_device_t),
  .bDescriptorType    = TUSB_DESC_DEVICE,
  .bcdUSB             = USB_BCD,

  // Use Interface Association Descriptor (IAD) for CDC
  .bDeviceClass       = TUSB_CLASS_MISC,
  .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
  .bDeviceProtocol    = MISC_PROTOCOL_IAD,

  .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,

  .idVendor           = USB_VID,
  .idProduct          = USB_PID,
  .bcdDevice          = 0x0100,

  .iManufacturer      = STRID_MANUFACTURER,
  .iProduct           = STRID_PRODUCT,
  .iSerialNumber      = STRID_SERIAL,

  .bNumConfigurations = 0x01
};

// Invoked when received GET DEVICE DESCRIPTOR
uint8_t const * tud_descriptor_device_cb(void)
{
  return (uint8_t const *) &desc_device;
}

//--------------------------------------------------------------------+
// Configuration Descriptor
//--------------------------------------------------------------------+

#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN \
                           + TUD_HID_DESC_LEN + TUD_HID_DESC_LEN)

uint8_t const desc_fs_configuration[] =
{
  // Config number, interface count, string index, total length, attribute, power in mA
  TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),

  // CDC: Interface number, string index, EP notification address and size, EP data address (out, in) and size
  TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, STRID_CDC, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),

  // 模拟键盘: Interface number, string index, boot protocol, report descriptor length, EP IN address, EP size, polling interval (1ms)
  TUD_HID_DESCRIPTOR(ITF_NUM_HID_KB, STRID_HID_KB, HID_ITF_PROTOCOL_KEYBOARD,
                     sizeof(desc_hid_report_keyboard), EPNUM_HID_KB_IN,
                     CFG_TUD_HID_EP_BUFSIZE, 1),

  // 模拟鼠标: 同上（报告协议，非 boot 布局）
  TUD_HID_DESCRIPTOR(ITF_NUM_HID_MS, STRID_HID_MS, HID_ITF_PROTOCOL_NONE,
                     sizeof(desc_hid_report_mouse), EPNUM_HID_MS_IN,
                     CFG_TUD_HID_EP_BUFSIZE, 1),
};

// Invoked when received GET CONFIGURATION DESCRIPTOR
uint8_t const * tud_descriptor_configuration_cb(uint8_t index)
{
  (void) index;
  return desc_fs_configuration;
}

//--------------------------------------------------------------------+
// HID Descriptors（TinyUSB Device HID 回调；instance 按配置顺序：0=键盘 1=鼠标）
//--------------------------------------------------------------------+

// Invoked when received GET HID REPORT DESCRIPTOR
uint8_t const * tud_hid_descriptor_report_cb(uint8_t instance)
{
  if (instance == 0) return desc_hid_report_keyboard;
  return desc_hid_report_mouse;
}

// Invoked when received GET_REPORT control request（本设备不提供存储的报告，返回 0）
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t* buffer,
                               uint16_t reqlen)
{
  (void) instance; (void) report_id; (void) report_type;
  (void) buffer; (void) reqlen;
  return 0;
}

// Invoked when received SET_REPORT control request（如键盘灯状态，暂不处理）
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type, uint8_t const* buffer,
                           uint16_t bufsize)
{
  (void) instance; (void) report_id; (void) report_type;
  (void) buffer; (void) bufsize;
}

//--------------------------------------------------------------------+
// String Descriptors
//--------------------------------------------------------------------+

char const* string_desc_arr [] =
{
  (const char[]) { 0x09, 0x04 }, // 0: English (0x0409)
  "TinyUSB",                     // 1: Manufacturer
  "Pico HID Bridge",             // 2: Product
  "123456789012",                // 3: Serials
  "TinyUSB CDC",                 // 4: CDC Interface
  "TinyUSB Keyboard",            // 5: 模拟键盘
  "TinyUSB Mouse",               // 6: 模拟鼠标
};

static uint16_t _desc_str[32];

uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
  (void) langid;

  uint8_t chr_count;

  if ( index == 0)
  {
    memcpy(&_desc_str[1], string_desc_arr[0], 2);
    chr_count = 1;
  }
  else
  {
    if ( !(index < sizeof(string_desc_arr)/sizeof(string_desc_arr[0])) ) return NULL;

    const char* str = string_desc_arr[index];

    chr_count = (uint8_t) strlen(str);
    if ( chr_count > 31 ) chr_count = 31;

    for(uint8_t i=0; i<chr_count; i++)
    {
      _desc_str[1+i] = str[i];
    }
  }

  _desc_str[0] = (TUSB_DESC_STRING << 8 ) | (2*chr_count + 2);

  return _desc_str;
}
