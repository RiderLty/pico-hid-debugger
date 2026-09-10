/*
 * USB 设备信息采集模块实现。
 *
 * 描述符抓取用异步控制传输串成状态机（tuh_descriptor_get_* 完成回调里
 * 发起下一步）。每种描述符先打一行解析出的关键字段，再整块 hexdump；
 * 字符串描述符按 UTF-16LE 转可打印 ASCII 显示。设备级信息每设备只采集
 * 一次（tuh_mount_cb），HID 接口级信息每次挂载采集。
 */

#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include "pico/time.h"

#include "tusb.h"

#include "uart_output.h"
#include "hid_host_app.h"

//--------------------------------------------------------------------+
// 行输出
//--------------------------------------------------------------------+

// 行输出：格式化 + 追加 \r\n + 入队
static void emit(const char *fmt, ...)
{
    char buf[UARTO_REC_MAX];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n > (int)sizeof(buf) - 2) n = (int)sizeof(buf) - 2;
    buf[n++] = '\r';
    buf[n++] = '\n';
    uart_output_send(buf, (uint8_t)n);
}

static const char hex_table[] = "0123456789ABCDEF";

// 描述符抓取状态机的控制传输完成回调（前向声明，状态机互相引用）
static void desc_xfer_cb(tuh_xfer_t *xfer);

// 向当前行追加 " XX " 形式的十六进制（调用方保证空间）
static void append_hex(char **p, const uint8_t *data, uint16_t n)
{
    while (n--) {
        *(*p)++ = ' ';
        *(*p)++ = hex_table[(*data >> 4) & 0x0F];
        *(*p)++ = hex_table[(*data++) & 0x0F];
    }
}

// HEX 折行 dump：[TAG] 紧跟 TAG 本体（不补在括号内），其后用空格补齐到
// TAG_COL 再输出字段；每行至多 16 字节，前缀含 len（整块总长）与 off
// （本行起始偏移）。前缀用空格补齐到固定列后才输出十六进制，使跨行数据列
// 垂直对齐（如 64 字节手柄报告）。itf_num >= 0 时附带 itf= 字段（报文/
// 报告描述符），< 0 时省略（设备级描述符）。整块数据全部转储，不截断。
#define TAG_COL 8u    // [TAG] 之后内容（dev=...）的起始列
#define HEX_COL 40u   // 十六进制数据起始列

static void hexdump(const char *tag, uint8_t dev_addr, int itf_num,
                    const uint8_t *data, uint16_t len)
{
    char line[UARTO_REC_MAX];
    uint16_t off = 0;

    // [TAG] 后的补位空格数，使 dev=... 从 TAG_COL 列开始（至少 1 格分隔）
    int tag_pad = (int)TAG_COL - (int)strlen(tag) - 2;
    if (tag_pad < 1) tag_pad = 1;

    while (off < len) {
        uint16_t n = len - off;
        if (n > 16u) n = 16u;

        int used;
        if (itf_num >= 0) {
            used = snprintf(line, sizeof(line) - 2, "[%s]%*sdev=%u itf=%u len=%u off=%u:",
                            tag, tag_pad, "", dev_addr, (unsigned)itf_num, len, off);
        } else {
            used = snprintf(line, sizeof(line) - 2, "[%s]%*sdev=%u len=%u off=%u:",
                            tag, tag_pad, "", dev_addr, len, off);
        }
        if (used < 0) return;

        char *p = line + used;
        if (used < (int)HEX_COL) {
            while (p < line + HEX_COL) *p++ = ' ';   // 补齐到固定数据列
        } else {
            *p++ = ' ';   // 病理超长前缀：至少留一个空格分隔
        }
        append_hex(&p, data + off, n);
        *p++ = '\r';
        *p++ = '\n';
        uart_output_send(line, (uint8_t)(p - line));

        off = (uint16_t)(off + n);
    }
}

//--------------------------------------------------------------------+
// 描述符抓取状态机（每设备一份状态）
//--------------------------------------------------------------------+

enum {
    DS_IDLE = 0,
    DS_DEV,     // 设备描述符已发出，等待完成
    DS_CFG,     // 配置描述符
    DS_LANG,    // 语言 ID 字符串
    DS_MFG,     // 厂商字符串
    DS_PROD,    // 产品字符串
    DS_SER,     // 序列号字符串
};

#define DESC_DEV_MAX   18u                          // tusb_desc_device_t
#define DESC_CFG_MAX   CFG_TUH_ENUMERATION_BUFSIZE  // 512，与枚举缓冲同级
#define DESC_STR_MAX   128u
// daddr 上界：下游设备 + hub 自身（CFG_TUH_DEVICE_MAX 不含 hub）
#define DESC_MAX_DEV   (CFG_TUH_DEVICE_MAX + CFG_TUH_HUB + 1u)

typedef struct {
    uint8_t  step;
    // 传输缓冲按 TinyUSB 惯例 4 字节对齐（CFG_TUSB_MEM_ALIGN）
    uint8_t  dev_buf[DESC_DEV_MAX] CFG_TUSB_MEM_ALIGN;
    uint8_t  cfg_buf[DESC_CFG_MAX] CFG_TUSB_MEM_ALIGN;
    uint8_t  str_buf[DESC_STR_MAX] CFG_TUSB_MEM_ALIGN;
    uint16_t langid;
    uint8_t  str_seq[3];   // 待抓取的字符串索引队列（iMfg/iProd/iSer）
    uint8_t  str_pos;      // str_seq 游标
} desc_state_t;

static desc_state_t s_desc[DESC_MAX_DEV];

// 各 HID instance 对应的接口号（报文行显示用；umount 时清零）
static uint8_t s_itf_num[CFG_TUH_HID];

// ---------------------------------------------------------------- 报文失败熔断
// HID 中断 IN 不会发送 0 长度报文：0 字节完成 = 传输失败（设备已拔出/异常）。
// 新版 PIO-USB 对失败事务内部重试 3 次，若固件继续无条件重入队，
// 死设备的端点会把每帧调度带宽耗尽在超时事务上，Hub 的状态轮询被饿死，
// 拔出事件永远无法上报（卸载流程卡死、日志死循环）。
// 因此连续 0 字节达到阈值即停止该接口的重入队，让出调度带宽给 Hub；
// 停止后仅以 500ms 慢速探测保活，卸载/重挂时计数复位。
#define XFER_FAIL_STOP     8u     // 连续失败次数阈值
#define XFER_PROBE_US (500 * 1000u)

static uint8_t  s_fail_cnt[CFG_TUH_HID];
static uint32_t s_fail_last_us[CFG_TUH_HID];

static desc_state_t *state_of(uint8_t dev_addr)
{
    return dev_addr < DESC_MAX_DEV ? &s_desc[dev_addr] : NULL;
}

static void desc_fetch_fail(desc_state_t *st, uint8_t dev_addr)
{
    emit("[ERROR] dev=%u desc fetch busy/failed (step=%u)", dev_addr, st->step);
    st->step = DS_IDLE;
}

// 发起下一个字符串请求；没有可抓的字符串时结束
static void fetch_next_string(desc_state_t *st, uint8_t dev_addr)
{
    while (st->str_pos < 3u) {
        uint8_t idx = st->str_seq[st->str_pos];
        if (!idx) { st->str_pos++; continue; }

        st->step = (uint8_t)(DS_MFG + st->str_pos);
        if (tuh_descriptor_get_string(dev_addr, idx, st->langid,
                                      st->str_buf, DESC_STR_MAX, desc_xfer_cb, 0)) {
            return;
        }
        desc_fetch_fail(st, dev_addr);
        return;
    }
    st->step = DS_IDLE;
}

// 字符串描述符 → "Mfg(1)=\"Logitech\"" 行（UTF-16LE 转可打印 ASCII）
static void emit_string(desc_state_t *st, uint8_t dev_addr, const char *label, uint8_t idx)
{
    uint8_t len = st->str_buf[0];
    if (len < 4u || st->str_buf[1] != TUSB_DESC_STRING) {
        emit("[STRDS] dev=%u %s(%u) <invalid>", dev_addr, label, idx);
        return;
    }

    char text[64];
    uint8_t n = 0;
    for (uint8_t i = 2u; i < len && n < sizeof(text) - 1u; i += 2u) {
        uint8_t c = st->str_buf[i];
        text[n++] = (c >= 0x20u && c < 0x7Fu) ? (char)c : '?';
    }
    text[n] = '\0';
    emit("[STRDS] dev=%u %s(%u)=\"%s\"", dev_addr, label, idx, text);
}

// 控制传输完成回调：dump 当前步结果并发起下一步
static void desc_xfer_cb(tuh_xfer_t *xfer)
{
    desc_state_t *st = state_of(xfer->daddr);
    // 设备可能已拔出：状态复位后迟到的回调直接丢弃
    if (!st || st->step == DS_IDLE) return;

    if (xfer->result != XFER_RESULT_SUCCESS) {
        emit("[ERROR] dev=%u desc fetch result=%d (step=%u)",
             xfer->daddr, (int)xfer->result, st->step);
        st->step = DS_IDLE;
        return;
    }

    switch (st->step) {
    case DS_DEV: {
        tusb_desc_device_t const *d = (tusb_desc_device_t const *)st->dev_buf;
        emit("[DEVDS] dev=%u vid=%04x pid=%04x bcdUSB=%04x cls=%02x/%02x/%02x "
             "pkt0=%u bcdDev=%04x cfgs=%u",
             xfer->daddr, d->idVendor, d->idProduct, d->bcdUSB,
             d->bDeviceClass, d->bDeviceSubClass, d->bDeviceProtocol,
             d->bMaxPacketSize0, d->bcdDevice, d->bNumConfigurations);
        emit("[DEVDS] dev=%u iMfg=%u iProd=%u iSer=%u",
             xfer->daddr, d->iManufacturer, d->iProduct, d->iSerialNumber);
        hexdump("DEVDS", xfer->daddr, -1, st->dev_buf, (uint16_t)xfer->actual_len);

        st->str_seq[0] = d->iManufacturer;
        st->str_seq[1] = d->iProduct;
        st->str_seq[2] = d->iSerialNumber;
        st->str_pos = 0;

        st->step = DS_CFG;
        if (!tuh_descriptor_get_configuration(xfer->daddr, 1, st->cfg_buf, DESC_CFG_MAX,
                                              desc_xfer_cb, 0)) {
            desc_fetch_fail(st, xfer->daddr);
        }
        break;
    }

    case DS_CFG: {
        tusb_desc_configuration_t const *c = (tusb_desc_configuration_t const *)st->cfg_buf;
        emit("[CFGDS] dev=%u total=%u itfs=%u cfg=%u attr=0x%02x power=%umA",
             xfer->daddr, c->wTotalLength, c->bNumInterfaces, c->bConfigurationValue,
             c->bmAttributes, c->bMaxPower * 2u);
        hexdump("CFGDS", xfer->daddr, -1, st->cfg_buf, (uint16_t)xfer->actual_len);

        st->step = DS_LANG;
        if (!tuh_descriptor_get_string(xfer->daddr, 0, 0, st->str_buf, DESC_STR_MAX,
                                       desc_xfer_cb, 0)) {
            // 拿不到语言 ID 就放弃全部字符串，不算致命
            st->step = DS_IDLE;
        }
        break;
    }

    case DS_LANG: {
        if (xfer->actual_len >= 4u) {
            st->langid = (uint16_t)(st->str_buf[2] | (st->str_buf[3] << 8));
            emit("[STRDS] dev=%u langid=0x%04x", xfer->daddr, st->langid);
        }
        st->step = DS_IDLE;   // fetch_next_string 内部会推进到具体字符串
        fetch_next_string(st, xfer->daddr);
        break;
    }

    case DS_MFG:
    case DS_PROD:
    case DS_SER: {
        static const char *const labels[3] = { "Mfg", "Prod", "Ser" };
        uint8_t pos = (uint8_t)(st->step - DS_MFG);
        emit_string(st, xfer->daddr, labels[pos], st->str_seq[pos]);
        st->str_pos = (uint8_t)(pos + 1u);
        fetch_next_string(st, xfer->daddr);
        break;
    }

    default:
        st->step = DS_IDLE;
        break;
    }
}

//--------------------------------------------------------------------+
// TinyUSB Host 回调
//--------------------------------------------------------------------+

// 设备枚举完成（含 hub 设备自身）
void tuh_mount_cb(uint8_t dev_addr)
{
    uint16_t vid, pid;
    tuh_vid_pid_get(dev_addr, &vid, &pid);
    emit("[MOUNT] dev=%u vid=%04x pid=%04x", dev_addr, vid, pid);

    desc_state_t *st = state_of(dev_addr);
    if (!st) {
        emit("[ERROR] dev=%u exceeds dev table (%u)", dev_addr, DESC_MAX_DEV);
        return;
    }
    memset(st, 0, sizeof(*st));

    st->step = DS_DEV;
    if (!tuh_descriptor_get_device(dev_addr, st->dev_buf, DESC_DEV_MAX,
                                   desc_xfer_cb, 0)) {
        desc_fetch_fail(st, dev_addr);
    }
}

// 设备移除（掉线或拔出）
void tuh_umount_cb(uint8_t dev_addr)
{
    emit("[DEVRM] dev=%u", dev_addr);
    desc_state_t *st = state_of(dev_addr);
    if (st) st->step = DS_IDLE;
}

static const char *const s_proto_str[3] = { "None", "Keyboard", "Mouse" };

// HID 接口挂载：dump 接口信息 + 报告描述符，然后订阅报文
void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance,
                      uint8_t const *desc_report, uint16_t desc_len)
{
    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);
    uint16_t vid, pid;
    tuh_vid_pid_get(dev_addr, &vid, &pid);

    if (instance < CFG_TUH_HID) s_fail_cnt[instance] = 0;   // 新挂载：清失败计数

    uint8_t itf_num = 0, cls = 0, sub = 0, eps = 0;
    tuh_itf_info_t itf_info;
    if (instance < CFG_TUH_HID && tuh_hid_itf_get_info(dev_addr, instance, &itf_info)) {
        itf_num = itf_info.desc.bInterfaceNumber;
        cls = itf_info.desc.bInterfaceClass;
        sub = itf_info.desc.bInterfaceSubClass;
        eps = itf_info.desc.bNumEndpoints;
        s_itf_num[instance] = itf_num;
    }

    emit("[HIDMT] dev=%u vid=%04x pid=%04x itf=%u proto=%s cls=%02x sub=%02x eps=%u",
         dev_addr, vid, pid, itf_num,
         itf_protocol < 3u ? s_proto_str[itf_protocol] : "?", cls, sub, eps);

    // 报告描述符由 TinyUSB 枚举时抓取；超枚举缓冲时为 NULL
    if (desc_report && desc_len) {
        hexdump("RPTDS", dev_addr, (int)itf_num, desc_report, desc_len);
    } else {
        emit("[RPTDS] dev=%u itf=%u not captured (>enum buf)", dev_addr, itf_num);
    }

    if (!tuh_hid_receive_report(dev_addr, instance)) {
        emit("[ERROR] dev=%u cannot request report", dev_addr);
    }
}

// HID 接口拔出
void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance)
{
    uint8_t itf_num = (instance < CFG_TUH_HID) ? s_itf_num[instance] : 0;
    emit("[UNHID] dev=%u itf=%u", dev_addr, itf_num);
    if (instance < CFG_TUH_HID) {
        s_itf_num[instance] = 0;
        s_fail_cnt[instance] = 0;      // 卸载：复位失败计数，供下一个设备复用
    }
}

// 收到 HID 中断端点报文：整包 hexdump，不做语义解析
void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance,
                                uint8_t const *report, uint16_t len)
{
  if (instance >= CFG_TUH_HID) return;

  if (len != 0) {
    s_fail_cnt[instance] = 0;
    hexdump("HID", dev_addr, (int)s_itf_num[instance], report, len);
  } else {
    // 0 字节完成 = 传输失败。连续失败达到阈值后熔断重入队，
    // 避免死设备端点拖垮 PIO-USB 调度、饿死 Hub 的拔出检测
    if (s_fail_cnt[instance] < XFER_FAIL_STOP) {
      s_fail_cnt[instance]++;
      s_fail_last_us[instance] = time_us_32();
      if (s_fail_cnt[instance] == XFER_FAIL_STOP) {
        emit("[ERROR] dev=%u itf=%u xfer failed x%u, polling paused",
             dev_addr, s_itf_num[instance], XFER_FAIL_STOP);
        return;
      }
    } else if (time_us_32() - s_fail_last_us[instance] < XFER_PROBE_US) {
      return;                            // 熔断后的慢速探测间隔
    } else {
      s_fail_last_us[instance] = time_us_32();
    }
  }

  if (!tuh_hid_receive_report(dev_addr, instance)) {
    emit("[ERROR] dev=%u cannot request report", dev_addr);
  }
}
