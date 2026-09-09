/*
 * 二进制输出实现：0x55 0xAA 帧头定界的紧凑帧。
 *
 * 帧格式见 hid_output.h 头部注释。解析规则按 type 二分：
 *   键盘/鼠标帧 = 帧头 + len + type + payload（无来源字段，最紧凑）；
 *   其余帧     = 帧头 + len + type + PID/VID/port + payload。
 * 与文本模式不同，键盘发原始报文、鼠标发归一化帧，
 * 按键边沿由上位机自行比对——固件侧不做边沿检测。
 */

#include <string.h>

#include "cdc_output.h"
#include "hid_output.h"

#define BIN_PAYLOAD_MAX  64u    // 与 CFG_TUH_HID_EPIN_BUFSIZE 一致，无需截断
#define BIN_HDR_MAX      9u     // 固定区最长：55 AA | len | type | PID | VID | port
#define BIN_ID_LEN       5u     // 来源身份区：PID2 + VID2 + port1

// 无设备来源的元信息帧（DROP/ERROR）使用
static const hid_dev_info_t s_zero_info = { .vid = 0, .pid = 0, .dev_addr = 0 };

// 写入固定区。identified=true 时在 type 后附 PID/VID/port，
// 返回固定区总长（payload 起始偏移）
static uint8_t put_header(uint8_t *p, const hid_dev_info_t *info,
                          bool identified, uint8_t type, uint8_t payload_len)
{
    // len = type(1) [+ 身份区5] + payload
    uint8_t len = (uint8_t)(1u + (identified ? BIN_ID_LEN : 0u) + payload_len);

    p[0] = 0x55;
    p[1] = 0xAA;
    p[2] = len;
    p[3] = type;
    if (!identified) return 4;

    p[4] = (uint8_t)(info->pid & 0xFF);        // PID 小端
    p[5] = (uint8_t)(info->pid >> 8);
    p[6] = (uint8_t)(info->vid & 0xFF);        // VID 小端
    p[7] = (uint8_t)(info->vid >> 8);
    p[8] = info->dev_addr;                     // 端口（USB 设备地址）
    return 4 + BIN_ID_LEN;
}

static void send_frame(const hid_dev_info_t *info, bool identified, uint8_t type,
                       const void *payload, uint8_t n)
{
    if (n > BIN_PAYLOAD_MAX) n = BIN_PAYLOAD_MAX;

    uint8_t buf[BIN_HDR_MAX + BIN_PAYLOAD_MAX];
    uint8_t off = put_header(buf, identified ? info : &s_zero_info,
                             identified, type, n);
    if (n && payload) memcpy(buf + off, payload, n);

    cdc_output_send(CDCO_KIND_CDC_DATA, buf, (uint8_t)(off + n));
}

//--------------------------------------------------------------------+
// 统一事件入口实现
//--------------------------------------------------------------------+

static void binary_keyboard(uint8_t instance, const hid_dev_info_t *info,
                            const uint8_t *report, uint8_t len)
{
    (void)instance;
    (void)info;
    // 键盘：原始报文（mod + keys），紧凑帧无来源字段
    send_frame(info, false, HIDOUT_TYPE_KEYBOARD, report, len);
}

static void binary_mouse(uint8_t instance, const hid_dev_info_t *info,
                         const hid_mouse_frame_t *frame)
{
    (void)instance;
    (void)info;
    // 鼠标：归一化解析结果重新打包 buttons u8 | wheel i8 | x i16 | y i16，紧凑帧
    uint8_t payload[6];
    payload[0] = frame->buttons;
    payload[1] = (uint8_t)frame->wheel;
    payload[2] = (uint8_t)(frame->x & 0xFF);   // x 小端
    payload[3] = (uint8_t)((frame->x >> 8) & 0xFF);
    payload[4] = (uint8_t)(frame->y & 0xFF);   // y 小端
    payload[5] = (uint8_t)((frame->y >> 8) & 0xFF);
    send_frame(info, false, HIDOUT_TYPE_MOUSE, payload, sizeof(payload));
}

static void binary_generic(uint8_t instance, const hid_dev_info_t *info,
                           const uint8_t *report, uint16_t len)
{
    (void)instance;
    // 其他 HID 设备：带 PID/VID/端口身份的原始报文
    if (len > 0xFF) len = 0xFF;
    send_frame(info, true, HIDOUT_TYPE_NONE, report, (uint8_t)len);
}

static void binary_mount(uint8_t instance, const hid_dev_info_t *info, uint8_t proto)
{
    const uint8_t payload[2] = { instance, proto };
    send_frame(info, true, HIDOUT_TYPE_MOUNT, payload, sizeof(payload));
}

static void binary_umount(uint8_t instance, const hid_dev_info_t *info)
{
    const uint8_t payload[1] = { instance };
    send_frame(info, true, HIDOUT_TYPE_UMOUNT, payload, sizeof(payload));
}

static void binary_error(const char *msg)
{
    uint8_t n = msg ? (uint8_t)strlen(msg) : 0;
    send_frame(&s_zero_info, true, HIDOUT_TYPE_ERROR, msg, n);
}

// 元信息帧构造（无设备来源，PID/VID/port 填 0）；cdc_output 的溢出补报使用
uint8_t hid_output_binary_meta(uint8_t *dst, uint8_t type,
                               const void *payload, uint8_t paylen)
{
    if (!dst) return 0;
    if (paylen > BIN_PAYLOAD_MAX) paylen = BIN_PAYLOAD_MAX;

    uint8_t off = put_header(dst, &s_zero_info, true, type, paylen);
    if (paylen && payload) memcpy(dst + off, payload, paylen);
    return (uint8_t)(off + paylen);
}

const hid_output_ops_t hid_output_binary_ops = {
    .mount    = binary_mount,
    .umount   = binary_umount,
    .error    = binary_error,
    .keyboard = binary_keyboard,
    .mouse    = binary_mouse,
    .generic  = binary_generic,
};
