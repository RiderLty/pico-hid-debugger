#ifndef KEY_NAMES_H
#define KEY_NAMES_H

/*
 * hidkit 按键 code → 可读名称，供 [HIDKIT] key 行的 name= 字段使用。
 *
 * code 是 hidkit 的统一按键 code 空间（lib/hidkit/src/hidkit_codes.h），段前缀区分
 * 类型：0x00xx 键盘 HID Usage ID / 0x01xx 鼠标按键序号 / 0x02xx 手柄 BTN_*。
 *
 * 名称就是 hidkit 的宏名（KEY_A / MOUSE_BUTTON_LEFT / BTN_DPAD_UP）—— 日志里看到
 * name=… 可原样回 grep 词表定义。**code 原值照旧打印**：name 只是叠加的可读视图，
 * 语义层不改变"所见即原始"。
 *
 * 只在按键边沿事件（hidkit_input_key）上调用；纯查表、无状态、无动态内存。
 */

#include <stdint.h>

/* 查到返回静态字符串（永不释放）；hidkit 词表外的 code 返回 NULL，由调用方决定
 * 显示成什么（本固件显示 "?"）。 */
const char *key_name_lookup(uint16_t code);

#endif // KEY_NAMES_H
