/*
 * 文本输出实现（core_input 风格）：把统一事件格式化成可读文本行。
 *
 * 键盘/鼠标按键的边沿检测在本模块内完成（按 instance 分槽）：
 * 同一份归一化数据，文本输出需要"按下/松开"语义；二进制输出则直接发帧、
 * 由上位机自行比对。设备拔出时在 umount 里补发松开。
 */

#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include "cdc_output.h"
#include "hid_output.h"

//--------------------------------------------------------------------+
// 按 instance 分槽的边沿检测状态（仅 core1 访问，无需加锁）
//--------------------------------------------------------------------+

#define HID_MAX_ITF   16    // 须 ≥ CFG_TUH_HID；越界的 instance 安全丢弃
#define KBD_MAX_KEYS  12
#define KBD_MAX_MODS  2

typedef struct {
    uint8_t kbd_mods[KBD_MAX_MODS];   // 两个修饰键字节
    uint8_t kbd_keys[KBD_MAX_KEYS];   // 普通键数组
    uint8_t mouse_btns;               // 上一帧鼠标按键掩码
} edge_state_t;

static edge_state_t s_edge[HID_MAX_ITF];

static inline edge_state_t *edge_of(uint8_t instance)
{
    return instance < HID_MAX_ITF ? &s_edge[instance] : NULL;
}

// 行输出：格式化 + 追加 \r\n + 入队
static void emit(const char *fmt, ...)
{
    char buf[CDC_REC_MAX];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n > (int)sizeof(buf) - 2) n = (int)sizeof(buf) - 2;
    buf[n++] = '\r';
    buf[n++] = '\n';
    cdc_output_send(CDCO_KIND_CDC_DATA, buf, (uint8_t)n);
}

//--------------------------------------------------------------------+
// 键盘事件
//--------------------------------------------------------------------+

// 修饰键名表（0xE0-0xE7）。普通键只打码值，保持输出精简；
// 0xE8-0xEF 为扩展修饰键字节（罕见），无惯用名，同样只打码值。
static const char *const s_mod_names[8] = {
    "LCTRL", "LSHIFT", "LALT", "LGUI",
    "RCTRL", "RSHIFT", "RALT", "RGUI",
};

static void key_line(uint8_t keycode, bool down)
{
    if (keycode == 0) return;

    if (keycode >= 0xE0 && keycode < 0xE8) {
        emit("KEY:%02X %s %s", keycode, down ? "DN" : "UP",
             s_mod_names[keycode - 0xE0]);
    } else {
        emit("KEY:%02X %s", keycode, down ? "DN" : "UP");
    }
}

//--------------------------------------------------------------------+
// 鼠标事件
//--------------------------------------------------------------------+

static const char s_btn_letters[8] = { 'L', 'R', 'M', 'B', 'F', '5', '6', '7' };

static void btn_line(uint8_t button, bool down)
{
    if (button >= 8) return;
    emit("MOUSE:BTN:%c %s", s_btn_letters[button], down ? "DN" : "UP");
}

// 位掩码 → "L|R" 形式；无按键时 "---"
static void format_btn_mask(char out[16], uint8_t mask)
{
    if (!mask) {
        strcpy(out, "---");
        return;
    }
    size_t pos = 0;
    for (uint8_t b = 0; b < 8; b++) {
        if (mask & (1u << b)) {
            if (pos) out[pos++] = '|';
            out[pos++] = s_btn_letters[b];
        }
    }
    out[pos] = '\0';
}

//--------------------------------------------------------------------+
// 挂载/卸载/错误/原始 HEX 兜底
//--------------------------------------------------------------------+

static const char *const s_proto_str[3] = { "None", "Keyboard", "Mouse" };

static void text_error(const char *msg)
{
    emit("[ERROR ] %s", msg ? msg : "");
}

// HEX 兜底行：HID:[A1 02 3C FF]，最多转储前 20 字节，超出以 ".. +N" 标注剩余长度
static void text_generic(uint8_t instance, const hid_dev_info_t *info,
                         const uint8_t *report, uint16_t len)
{
    (void)instance;
    (void)info;

    static const char hex_table[] = "0123456789ABCDEF";
    const uint16_t dump_max = 20;

    char line[CDC_REC_MAX];
    char *p = line;
    char *end = line + CDC_REC_MAX - 3u;   // 给 ']' 和 \r\n 留位

    memcpy(p, "HID:[", 5); p += 5;

    uint16_t n = (len > dump_max) ? dump_max : len;
    for (uint16_t i = 0; i < n && p + 3 <= end; i++) {
        if (i) *p++ = ' ';
        *p++ = hex_table[(report[i] >> 4) & 0x0F];
        *p++ = hex_table[report[i] & 0x0F];
    }
    if (len > dump_max && p + 12 <= end) {
        p += snprintf(p, (size_t)(end - p), " .. +%u", (unsigned)(len - dump_max));
    }
    *p++ = ']';

    emit("%s", line);
}

static void text_mount(uint8_t instance, const hid_dev_info_t *info, uint8_t proto)
{
    emit("[MOUNT ] vid=%04x pid=%04x dev=%u itf=%u proto=%s",
         info->vid, info->pid, info->dev_addr, instance,
         proto < 3 ? s_proto_str[proto] : "?");
}

static void text_umount(uint8_t instance, const hid_dev_info_t *info)
{
    // 补发该 instance 仍按着的修饰键/普通键/鼠标键，否则上位机残留"按住"事件
    edge_state_t *st = edge_of(instance);
    if (st) {
        for (uint8_t m = 0; m < KBD_MAX_MODS; m++) {
            for (uint8_t bit = 0; bit < 8; bit++) {
                if (st->kbd_mods[m] & (1u << bit)) {
                    key_line((uint8_t)(0xE0 + m * 8 + bit), false);
                }
            }
        }
        for (uint8_t i = 0; i < KBD_MAX_KEYS && st->kbd_keys[i]; i++) {
            key_line(st->kbd_keys[i], false);
        }
        for (uint8_t bit = 0; bit < 8; bit++) {
            if (st->mouse_btns & (1u << bit)) {
                btn_line(bit, false);
            }
        }
        memset(st, 0, sizeof(*st));
    }

    emit("[UMOUNT] vid=%04x pid=%04x dev=%u itf=%u",
         info->vid, info->pid, info->dev_addr, instance);
}

//--------------------------------------------------------------------+
// 统一事件入口实现
//--------------------------------------------------------------------+

// 键盘报文（mod + keys 布局）→ 边沿检测 → 逐键文本行
static void text_keyboard(uint8_t instance, const hid_dev_info_t *info,
                          const uint8_t *report, uint8_t len)
{
    (void)info;
    if (len < 2) return;   // 至少需要两个修饰键字节

    edge_state_t *st = edge_of(instance);
    if (!st) return;

    const uint8_t *mods = report;                 // 前两个字节为修饰键
    const uint8_t *keys = report + 2;             // 后续为按键数组
    uint8_t key_count = len - 2;                  // 按键数据字节数
    if (key_count > KBD_MAX_KEYS) {
        key_count = KBD_MAX_KEYS;                 // 最多处理 12 个
    }

    // --- 修饰键处理（两个字节，每个字节 8 位）---
    for (uint8_t m = 0; m < KBD_MAX_MODS; m++) {
        uint8_t changed = mods[m] ^ st->kbd_mods[m];
        for (uint8_t bit = 0; bit < 8; bit++) {
            if (changed & (1u << bit)) {
                bool down = (mods[m] >> bit) & 1;
                // 映射：第一个字节 → 0xE0+bit，第二个字节 → 0xE8+bit
                key_line((uint8_t)(0xE0 + m * 8 + bit), down);
            }
        }
        st->kbd_mods[m] = mods[m];
    }

    // --- 普通键按下检测（当前帧有、上帧没有）---
    for (uint8_t i = 0; i < key_count && keys[i]; i++) {
        bool found = false;
        for (uint8_t j = 0; j < KBD_MAX_KEYS && st->kbd_keys[j]; j++) {
            if (st->kbd_keys[j] == keys[i]) {
                found = true;
                break;
            }
        }
        if (!found) {
            key_line(keys[i], true);
        }
    }

    // --- 普通键释放检测（上帧有、当前帧没有）---
    for (uint8_t i = 0; i < KBD_MAX_KEYS && st->kbd_keys[i]; i++) {
        bool found = false;
        for (uint8_t j = 0; j < key_count && keys[j]; j++) {
            if (keys[j] == st->kbd_keys[i]) {
                found = true;
                break;
            }
        }
        if (!found) {
            key_line(st->kbd_keys[i], false);
        }
    }

    // --- 更新按键状态（清零后复制当前按键，剩余自然为 0）---
    memset(st->kbd_keys, 0, sizeof(st->kbd_keys));
    memcpy(st->kbd_keys, keys, key_count);
}

// 归一化鼠标帧 → 按键边沿行 + 移动行
static void text_mouse(uint8_t instance, const hid_dev_info_t *info,
                       const hid_mouse_frame_t *frame)
{
    (void)info;
    edge_state_t *st = edge_of(instance);
    if (!st) return;

    // 按键边沿检测
    uint8_t changed = frame->buttons ^ st->mouse_btns;
    for (uint8_t b = 0; b < 8; b++) {
        if (changed & (1u << b)) {
            btn_line(b, (frame->buttons >> b) & 1);
        }
    }
    st->mouse_btns = frame->buttons;

    // 移动行（BTN= 反映本设备当前按住的键）
    if (frame->x != 0 || frame->y != 0 || frame->wheel != 0) {
        char btn[16];
        format_btn_mask(btn, st->mouse_btns);
        emit("MOUSE:DX=%+d DY=%+d WH=%+d BTN=%s",
             frame->x, frame->y, frame->wheel, btn);
    }
}

const hid_output_ops_t hid_output_text_ops = {
    .mount    = text_mount,
    .umount   = text_umount,
    .error    = text_error,
    .keyboard = text_keyboard,
    .mouse    = text_mouse,
    .generic  = text_generic,
};
