/*
 * hidkit 按键 code → 可读名称（[HIDKIT] key 行的 name= 字段）。
 *
 * code 是 hidkit 的统一按键 code 空间（lib/hidkit/src/hidkit_codes.h），用段前缀
 * 区分设备类型：0x00xx 键盘 HID Usage ID / 0x01xx 鼠标按键序号 / 0x02xx 手柄 BTN_*。
 *
 * 两条设计取舍：
 *   ① 名称**就是 hidkit 的宏名**（KEY_A / MOUSE_BUTTON_LEFT / BTN_DPAD_UP），不另造
 *      一套词表 —— 日志里看到 name= 可以原样回 grep hidkit_codes.h 的定义，也不会
 *      出现"库里改了词表、这边名字对不上"的漂移；
 *   ② 只做查表，**不改动 code 本身**：[HIDKIT] key 行仍旧打印 code=0x%04X，
 *      name= 是叠加的可读视图（本固件首先是"所见即原始"的采集器）。
 *
 * 纯查表：无状态、无动态内存，表在 flash（.rodata）。调用点只在按键**边沿**事件
 * （hidkit_input_key），不在每报文路径上，所以查表开销与报文速率无关。
 *
 * 表由 lib/hidkit/src/hidkit_codes.h 的宏逐条生成（下标即 HID Usage ID / 按键序号 /
 * BTN_*），库里新增按键宏时这里同步补一行；未列出的 code 返回 NULL，打印为 "?"。
 */

#include <stddef.h>  /* NULL */

#include "key_names.h"

#include "hidkit.h"      /* 段前缀 HIDKIT_CODE_ 与按键词表宏（KEY_ / BTN_ / MOUSE_BUTTON_） */
#include "hidkit_app.h"  /* HIDKIT_APP_EVENTS：与调用点同一个编译期开关 */

#if HIDKIT_APP_EVENTS

/* ---- 键盘段 0x0000：下标 = HID Usage ID（0x00..0xFF） ---------------- */
static const char *const s_keyboard[256] = {
    /* 0x00  无键 / Roll Over 错误（不出边沿事件，占位以便回查） */
    [KEY_NONE]               = "KEY_NONE",
    [KEY_ERR_OVF]            = "KEY_ERR_OVF",

    /* 0x04  字母 */
    [KEY_A]                  = "KEY_A",
    [KEY_B]                  = "KEY_B",
    [KEY_C]                  = "KEY_C",
    [KEY_D]                  = "KEY_D",
    [KEY_E]                  = "KEY_E",
    [KEY_F]                  = "KEY_F",
    [KEY_G]                  = "KEY_G",
    [KEY_H]                  = "KEY_H",
    [KEY_I]                  = "KEY_I",
    [KEY_J]                  = "KEY_J",
    [KEY_K]                  = "KEY_K",
    [KEY_L]                  = "KEY_L",
    [KEY_M]                  = "KEY_M",
    [KEY_N]                  = "KEY_N",
    [KEY_O]                  = "KEY_O",
    [KEY_P]                  = "KEY_P",
    [KEY_Q]                  = "KEY_Q",
    [KEY_R]                  = "KEY_R",
    [KEY_S]                  = "KEY_S",
    [KEY_T]                  = "KEY_T",
    [KEY_U]                  = "KEY_U",
    [KEY_V]                  = "KEY_V",
    [KEY_W]                  = "KEY_W",
    [KEY_X]                  = "KEY_X",
    [KEY_Y]                  = "KEY_Y",
    [KEY_Z]                  = "KEY_Z",

    /* 0x1e  数字行（与 Shift 组合是 !@#$…） */
    [KEY_1]                  = "KEY_1",
    [KEY_2]                  = "KEY_2",
    [KEY_3]                  = "KEY_3",
    [KEY_4]                  = "KEY_4",
    [KEY_5]                  = "KEY_5",
    [KEY_6]                  = "KEY_6",
    [KEY_7]                  = "KEY_7",
    [KEY_8]                  = "KEY_8",
    [KEY_9]                  = "KEY_9",
    [KEY_0]                  = "KEY_0",

    /* 0x28  回车 / 编辑 / 空白 / 常用符号 */
    [KEY_ENTER]              = "KEY_ENTER",
    [KEY_ESC]                = "KEY_ESC",
    [KEY_BACKSPACE]          = "KEY_BACKSPACE",
    [KEY_TAB]                = "KEY_TAB",
    [KEY_SPACE]              = "KEY_SPACE",
    [KEY_MINUS]              = "KEY_MINUS",
    [KEY_EQUAL]              = "KEY_EQUAL",
    [KEY_LEFTBRACE]          = "KEY_LEFTBRACE",
    [KEY_RIGHTBRACE]         = "KEY_RIGHTBRACE",
    [KEY_BACKSLASH]          = "KEY_BACKSLASH",
    [KEY_HASHTILDE]          = "KEY_HASHTILDE",
    [KEY_SEMICOLON]          = "KEY_SEMICOLON",
    [KEY_APOSTROPHE]         = "KEY_APOSTROPHE",
    [KEY_GRAVE]              = "KEY_GRAVE",
    [KEY_COMMA]              = "KEY_COMMA",
    [KEY_DOT]                = "KEY_DOT",
    [KEY_SLASH]              = "KEY_SLASH",
    [KEY_CAPSLOCK]           = "KEY_CAPSLOCK",

    /* 0x3a  F1..F12 */
    [KEY_F1]                 = "KEY_F1",
    [KEY_F2]                 = "KEY_F2",
    [KEY_F3]                 = "KEY_F3",
    [KEY_F4]                 = "KEY_F4",
    [KEY_F5]                 = "KEY_F5",
    [KEY_F6]                 = "KEY_F6",
    [KEY_F7]                 = "KEY_F7",
    [KEY_F8]                 = "KEY_F8",
    [KEY_F9]                 = "KEY_F9",
    [KEY_F10]                = "KEY_F10",
    [KEY_F11]                = "KEY_F11",
    [KEY_F12]                = "KEY_F12",

    /* 0x46  系统键、编辑键、方向键 */
    [KEY_SYSRQ]              = "KEY_SYSRQ",
    [KEY_SCROLLLOCK]         = "KEY_SCROLLLOCK",
    [KEY_PAUSE]              = "KEY_PAUSE",
    [KEY_INSERT]             = "KEY_INSERT",
    [KEY_HOME]               = "KEY_HOME",
    [KEY_PAGEUP]             = "KEY_PAGEUP",
    [KEY_DELETE]             = "KEY_DELETE",
    [KEY_END]                = "KEY_END",
    [KEY_PAGEDOWN]           = "KEY_PAGEDOWN",
    [KEY_RIGHT]              = "KEY_RIGHT",
    [KEY_LEFT]               = "KEY_LEFT",
    [KEY_DOWN]               = "KEY_DOWN",
    [KEY_UP]                 = "KEY_UP",

    /* 0x53  小键盘（Num Lock 区） */
    [KEY_NUMLOCK]            = "KEY_NUMLOCK",
    [KEY_KPSLASH]            = "KEY_KPSLASH",
    [KEY_KPASTERISK]         = "KEY_KPASTERISK",
    [KEY_KPMINUS]            = "KEY_KPMINUS",
    [KEY_KPPLUS]             = "KEY_KPPLUS",
    [KEY_KPENTER]            = "KEY_KPENTER",
    [KEY_KP1]                = "KEY_KP1",
    [KEY_KP2]                = "KEY_KP2",
    [KEY_KP3]                = "KEY_KP3",
    [KEY_KP4]                = "KEY_KP4",
    [KEY_KP5]                = "KEY_KP5",
    [KEY_KP6]                = "KEY_KP6",
    [KEY_KP7]                = "KEY_KP7",
    [KEY_KP8]                = "KEY_KP8",
    [KEY_KP9]                = "KEY_KP9",
    [KEY_KP0]                = "KEY_KP0",
    [KEY_KPDOT]              = "KEY_KPDOT",

    /* 0x64  杂项：102 键、Compose、Power、F13..F24、执行/编辑类 */
    [KEY_102ND]              = "KEY_102ND",
    [KEY_COMPOSE]            = "KEY_COMPOSE",
    [KEY_POWER]              = "KEY_POWER",
    [KEY_KPEQUAL]            = "KEY_KPEQUAL",
    [KEY_F13]                = "KEY_F13",
    [KEY_F14]                = "KEY_F14",
    [KEY_F15]                = "KEY_F15",
    [KEY_F16]                = "KEY_F16",
    [KEY_F17]                = "KEY_F17",
    [KEY_F18]                = "KEY_F18",
    [KEY_F19]                = "KEY_F19",
    [KEY_F20]                = "KEY_F20",
    [KEY_F21]                = "KEY_F21",
    [KEY_F22]                = "KEY_F22",
    [KEY_F23]                = "KEY_F23",
    [KEY_F24]                = "KEY_F24",
    [KEY_OPEN]               = "KEY_OPEN",
    [KEY_HELP]               = "KEY_HELP",
    [KEY_PROPS]              = "KEY_PROPS",
    [KEY_FRONT]              = "KEY_FRONT",
    [KEY_STOP]               = "KEY_STOP",
    [KEY_AGAIN]              = "KEY_AGAIN",
    [KEY_UNDO]               = "KEY_UNDO",
    [KEY_CUT]                = "KEY_CUT",
    [KEY_COPY]               = "KEY_COPY",
    [KEY_PASTE]              = "KEY_PASTE",
    [KEY_FIND]               = "KEY_FIND",
    [KEY_MUTE]               = "KEY_MUTE",
    [KEY_VOLUMEUP]           = "KEY_VOLUMEUP",
    [KEY_VOLUMEDOWN]         = "KEY_VOLUMEDOWN",

    /* 0x85  国际键（日/韩） */
    [KEY_KPCOMMA]            = "KEY_KPCOMMA",
    [KEY_RO]                 = "KEY_RO",
    [KEY_KATAKANAHIRAGANA]   = "KEY_KATAKANAHIRAGANA",
    [KEY_YEN]                = "KEY_YEN",
    [KEY_HENKAN]             = "KEY_HENKAN",
    [KEY_MUHENKAN]           = "KEY_MUHENKAN",
    [KEY_KPJPCOMMA]          = "KEY_KPJPCOMMA",
    [KEY_HANGEUL]            = "KEY_HANGEUL",
    [KEY_HANJA]              = "KEY_HANJA",
    [KEY_KATAKANA]           = "KEY_KATAKANA",
    [KEY_HIRAGANA]           = "KEY_HIRAGANA",
    [KEY_ZENKAKUHANKAKU]     = "KEY_ZENKAKUHANKAKU",

    /* 0xb6  小键盘括号 */
    [KEY_KPLEFTPAREN]        = "KEY_KPLEFTPAREN",
    [KEY_KPRIGHTPAREN]       = "KEY_KPRIGHTPAREN",

    /* 0xe0  修饰键（左右 Ctrl/Shift/Alt/GUI） */
    [KEY_LEFTCTRL]           = "KEY_LEFTCTRL",
    [KEY_LEFTSHIFT]          = "KEY_LEFTSHIFT",
    [KEY_LEFTALT]            = "KEY_LEFTALT",
    [KEY_LEFTMETA]           = "KEY_LEFTMETA",
    [KEY_RIGHTCTRL]          = "KEY_RIGHTCTRL",
    [KEY_RIGHTSHIFT]         = "KEY_RIGHTSHIFT",
    [KEY_RIGHTALT]           = "KEY_RIGHTALT",
    [KEY_RIGHTMETA]          = "KEY_RIGHTMETA",

    /* 0xe8  媒体键（hidkit 词表扩展；HID 键盘段本身到 0xE7） */
    [KEY_MEDIA_PLAYPAUSE]    = "KEY_MEDIA_PLAYPAUSE",
    [KEY_MEDIA_STOPCD]       = "KEY_MEDIA_STOPCD",
    [KEY_MEDIA_PREVIOUSSONG] = "KEY_MEDIA_PREVIOUSSONG",
    [KEY_MEDIA_NEXTSONG]     = "KEY_MEDIA_NEXTSONG",
    [KEY_MEDIA_EJECTCD]      = "KEY_MEDIA_EJECTCD",
    [KEY_MEDIA_VOLUMEUP]     = "KEY_MEDIA_VOLUMEUP",
    [KEY_MEDIA_VOLUMEDOWN]   = "KEY_MEDIA_VOLUMEDOWN",
    [KEY_MEDIA_MUTE]         = "KEY_MEDIA_MUTE",
    [KEY_MEDIA_WWW]          = "KEY_MEDIA_WWW",
    [KEY_MEDIA_BACK]         = "KEY_MEDIA_BACK",
    [KEY_MEDIA_FORWARD]      = "KEY_MEDIA_FORWARD",
    [KEY_MEDIA_STOP]         = "KEY_MEDIA_STOP",
    [KEY_MEDIA_FIND]         = "KEY_MEDIA_FIND",
    [KEY_MEDIA_SCROLLUP]     = "KEY_MEDIA_SCROLLUP",
    [KEY_MEDIA_SCROLLDOWN]   = "KEY_MEDIA_SCROLLDOWN",
    [KEY_MEDIA_EDIT]         = "KEY_MEDIA_EDIT",
    [KEY_MEDIA_SLEEP]        = "KEY_MEDIA_SLEEP",
    [KEY_MEDIA_COFFEE]       = "KEY_MEDIA_COFFEE",
    [KEY_MEDIA_REFRESH]      = "KEY_MEDIA_REFRESH",
    [KEY_MEDIA_CALC]         = "KEY_MEDIA_CALC",
};

/* ---- 鼠标段 0x0100：下标 = 按键序号（滚轮也占两个码位） -------------- */
static const char *const s_mouse[] = {
    [MOUSE_BUTTON_LEFT]    = "MOUSE_BUTTON_LEFT",
    [MOUSE_BUTTON_RIGHT]   = "MOUSE_BUTTON_RIGHT",
    [MOUSE_BUTTON_MIDDLE]  = "MOUSE_BUTTON_MIDDLE",
    [MOUSE_BUTTON_BACK]    = "MOUSE_BUTTON_BACK",
    [MOUSE_BUTTON_FORWARD] = "MOUSE_BUTTON_FORWARD",
    [REL_WHEEL_UP]         = "REL_WHEEL_UP",
    [REL_WHEEL_DOWN]       = "REL_WHEEL_DOWN",
};

/* ---- 手柄段 0x0200：下标 = BTN_*（布局表 btn_map 的全部取值） -------- */
static const char *const s_gamepad[] = {
    [BTN_A]          = "BTN_A",
    [BTN_B]          = "BTN_B",
    [BTN_X]          = "BTN_X",
    [BTN_Y]          = "BTN_Y",
    [BTN_LB]         = "BTN_LB",
    [BTN_RB]         = "BTN_RB",
    [BTN_LT]         = "BTN_LT",
    [BTN_RT]         = "BTN_RT",
    [BTN_SELECT]     = "BTN_SELECT",
    [BTN_START]      = "BTN_START",
    [BTN_LS]         = "BTN_LS",
    [BTN_RS]         = "BTN_RS",
    [BTN_HOME]       = "BTN_HOME",
    [BTN_MISC]       = "BTN_MISC",
    [BTN_DPAD_UP]    = "BTN_DPAD_UP",
    [BTN_DPAD_DOWN]  = "BTN_DPAD_DOWN",
    [BTN_DPAD_LEFT]  = "BTN_DPAD_LEFT",
    [BTN_DPAD_RIGHT] = "BTN_DPAD_RIGHT",
    [BTN_EXTRA_1]    = "BTN_EXTRA_1",
    [BTN_EXTRA_2]    = "BTN_EXTRA_2",
    [BTN_EXTRA_3]    = "BTN_EXTRA_3",
    [BTN_EXTRA_4]    = "BTN_EXTRA_4",
    [BTN_EXTRA_5]    = "BTN_EXTRA_5",
    [BTN_EXTRA_6]    = "BTN_EXTRA_6",
    [BTN_EXTRA_7]    = "BTN_EXTRA_7",
    [BTN_EXTRA_8]    = "BTN_EXTRA_8",
};

#define ARRAY_CNT(a) (sizeof(a) / sizeof((a)[0]))

const char *key_name_lookup(uint16_t code)
{
    uint8_t const sub = (uint8_t)(code & 0x00FFu);

    switch (HIDKIT_CODE_SEG(code)) {
        case HIDKIT_CODE_KEYBOARD: return s_keyboard[sub];
        case HIDKIT_CODE_MOUSE:    return (sub < ARRAY_CNT(s_mouse))   ? s_mouse[sub]   : NULL;
        case HIDKIT_CODE_GAMEPAD:  return (sub < ARRAY_CNT(s_gamepad)) ? s_gamepad[sub] : NULL;
        default:                   return NULL;
    }
}

#else /* !HIDKIT_APP_EVENTS：事件行整条不在固件里，词表也不进 flash */

const char *key_name_lookup(uint16_t code)
{
    (void)code;
    return NULL;
}

#endif
