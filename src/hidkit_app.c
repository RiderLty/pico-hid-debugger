/*
 * hidkit 接入层实现。
 *
 * 三件事：
 *   ① 覆盖 hidkit 的四个弱符号出口函数（hidkit_input_*）与库内诊断出口
 *      （hidkit_debug_printf）—— 这是本固件"打印到日志"的全部实现；
 *   ② HID 侧的三个转发（mount/report/umount）：TinyUSB 回调 → hidkit 标准入口，
 *      并维护 instance → 槽位 的映射（未接管的 instance 记 -1）；
 *   ③ 定义 usbh_app_driver_get_cb()，把 XInput 适配器的类驱动注册进 Host 栈 ——
 *      这是**唯一**允许占用该钩子的地方（适配器约定：它自己不定义，避免抢符号）。
 *
 * 事件行 / 诊断行两个 TAG 分开：前者是"设备做了什么"，后者是"库怎么想的"。
 * 排查"设备动作为什么没出来"看 [HKDBG]；看输入本身看 [HIDKIT]。
 *
 * 两族行各自受一个**运行期**开关位控制（LOG_SW_HIDKIT / LOG_SW_HKDBG，
 * 见 log_switch.h）—— 这是叠加在上面那两个编译期开关之上的一层：编译期决定
 * 代码在不在固件里，运行期决定现在出不出。
 */

#include <stdio.h>
#include <stdarg.h>

#include "tusb.h"

#include "hidkit.h"
#include "hidkit_debug.h"        /* hidkit_debug_printf 原型 + HIDKIT_LOG 开关 */
#include "hidkit_xinput_glue.h"  /* hidkit_tusb_xinput_driver() */

#include "uart_output.h"
#include "log_switch.h"
#include "log_fmt.h"
#include "hidkit_app.h"
#include "hid_host_app.h"   /* hid_app_emit_info（校准交付行走 INFO 位） */
#include "hidkit_switch_pro_glue.h" /* 独立握手库 → hidkit 校准转换 */
#include "key_names.h"

/* instance → hidkit 槽位；-1 = 本库没接管这个接口（[HKDBG] 里能看到原因） */
static int8_t s_slot[CFG_TUH_HID];

/*--------------------------------------------------------------------+
 * 行输出
 *--------------------------------------------------------------------*/

// TAG 之后内容起始列：[HIDKIT] 比 [HKDBG] 长两格，补齐到同一列，两族行对齐
// （与 hid_host_app.c 的 TAG_COL 同一套做法，只是这里全是非 hexdump 行）
#define HK_TAG_COL 9u

// 两个开关都关掉时本文件不再输出任何行，helper 一并裁掉（否则 -Wunused-function）
#if HIDKIT_APP_EVENTS || HIDKIT_DEBUG
// bit 是运行期开关位（log_switch.h），放首参：漏改调用点会编译报错而非静默变义。
// 与上面的 #if 是两层正交的门：编译期决定"代码在不在固件里"，
// 运行期决定"现在出不出"。format 属性是补上的 —— 此前五个格式串完全没被检查
__attribute__((format(printf, 3, 4)))
static void emit_tagged(uint8_t bit, const char *tag, const char *fmt, ...)
{
    if (!log_switch_on(bit)) return;

    char line[UARTO_REC_MAX];

    int n = snprintf(line, sizeof(line), "[%s]", tag);
    if (n <= 0) return;

    size_t p = (size_t)n;
    while (p < HK_TAG_COL && p < sizeof(line) - 3u) line[p++] = ' ';

    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(line + p, sizeof(line) - p - 2u, fmt, ap);
    va_end(ap);
    if (m <= 0) return;
    if (m > (int)(sizeof(line) - p - 2u)) m = (int)(sizeof(line) - p - 2u);
    p += (size_t)m;

    line[p++] = '\r';
    line[p++] = '\n';
    uart_output_send(line, (uint8_t)p);
}
#endif /* HIDKIT_APP_EVENTS || HIDKIT_DEBUG */

/*--------------------------------------------------------------------+
 * hidkit 出口：事件（弱符号覆盖，仅状态变化 / 非零时被调用）
 *--------------------------------------------------------------------*/

// 键盘 + 鼠标按键 + 手柄按键统一出口，code 的段前缀区分类型（hidkit_codes.h）：
//   0x00xx 键盘 HID Usage ID / 0x01xx 鼠标按键序号 / 0x02xx 手柄 BTN_*
// name= 是 code 的可读名（key_names.c 的词表），**code 原值照旧保留**：本固件首先
// 是"所见即原始"的采集器，语义层只叠加可读性。词表外的 code 显示 "?"
void hidkit_input_key(int8_t slot, uint16_t code, bool pressed)
{
#if HIDKIT_APP_EVENTS
    const char *const name = key_name_lookup(code);
    emit_tagged(LOG_SW_HIDKIT, "HIDKIT", "key slot=" LOG_W_SLOT " code=0x%04X name=%s %s",
                (int)slot, (unsigned)code, name ? name : "?", pressed ? "down" : "up");
#else
    (void)slot; (void)code; (void)pressed;
#endif
}

// 鼠标位移与滚轮（不做横向滚轮）
void hidkit_input_mouse_abs(int8_t slot, int32_t dx, int32_t dy, int32_t wheel)
{
#if HIDKIT_APP_EVENTS
    emit_tagged(LOG_SW_HIDKIT, "HIDKIT",
                "mouse slot=" LOG_W_SLOT " dx=" LOG_W_MOUSE " dy=" LOG_W_MOUSE
                " wheel=" LOG_W_MOUSE, (int)slot, (int)dx, (int)dy, (int)wheel);
#else
    (void)slot; (void)dx; (void)dy; (void)wheel;
#endif
}

// 手柄绝对状态：每份解析成功的报文都回调（不做去重），所以 1kHz 手柄下
// 这行是持续的 —— 想看"只在变化时报"，得靠上位机过滤或关掉事件行。
// btn= 是 32 位原生按钮位图（bit31 在左），整体包在一对中括号里：■=按下
// □=抬起。位序 = 手柄报文原生位序（Switch Pro：bit0..7 右组 Y X B A SR SL
// R ZR / bit8..15 共享 − + RS LS Home Capture / bit16..23 左组 下 上 右 左
// SR SL L ZL，bit24..31 恒 0；DS5/XInput 见各自布局的 bit 定义）。
//
// 为什么整体包一对而不是逐键 [■][□]：32 键逐键包裹是 128 字节，一行逼近
// UARTO_REC_MAX 且 1kHz 下白烧带宽；整体包裹只多 2 字节，方格数与位一一对应，
// 靠列位即可定位是第几位。■/□ 是 East Asian Ambiguous 宽度的字符（西文终端
// 算 1 列、CJK 环境算 2 列），但 32 格宽度彼此一致，位图内部始终逐列对齐。
void hidkit_input_gamepad_abs(int8_t slot, int32_t ls_x, int32_t ls_y,
                              int32_t rs_x, int32_t rs_y, int32_t lt, int32_t rt,
                              uint32_t buttons)
{
#if HIDKIT_APP_EVENTS
    char btn[2 + 32 * 3 + 1];   // '[' + 32 × 3 字节 UTF-8 方格 + ']' + NUL
    char *p = btn;
    *p++ = '[';
    for (int i = 31; i >= 0; i--) {
        memcpy(p, (buttons >> i) & 1u ? "\xE2\x96\xA0"  // ■ U+25A0
                                      : "\xE2\x96\xA1", // □ U+25A1
               3);
        p += 3;
    }
    *p++ = ']';
    *p = '\0';
    emit_tagged(LOG_SW_HIDKIT, "HIDKIT",
                "pad slot=" LOG_W_SLOT " ls=" LOG_W_STICK ", " LOG_W_STICK
                " rs=" LOG_W_STICK ", " LOG_W_STICK
                " lt=" LOG_W_TRIG " rt=" LOG_W_TRIG " btn=%s",
                (int)slot, (int)ls_x, (int)ls_y, (int)rs_x, (int)rs_y,
                (int)lt, (int)rt, btn);
#else
    (void)slot; (void)ls_x; (void)ls_y; (void)rs_x; (void)rs_y;
    (void)lt; (void)rt; (void)buttons;
#endif
}

// 手柄六轴运动数据（带 IMU 的手柄每份报文回调；卸载补发全零帧）。
// 量程约定（hidkit.h）：±32767 = ±2000 dps / ±4 g
void hidkit_input_gamepad_motion(int8_t slot, int16_t gx, int16_t gy, int16_t gz,
                                 int16_t ax, int16_t ay, int16_t az)
{
#if HIDKIT_APP_EVENTS
    emit_tagged(LOG_SW_HIDKIT, "HIDKIT",
                "motion slot=" LOG_W_SLOT " g=" LOG_W_MOTION ", " LOG_W_MOTION
                ", " LOG_W_MOTION " a=" LOG_W_MOTION ", " LOG_W_MOTION
                ", " LOG_W_MOTION,
                (int)slot, (int)gx, (int)gy, (int)gz, (int)ax, (int)ay, (int)az);
#else
    (void)slot; (void)gx; (void)gy; (void)gz; (void)ax; (void)ay; (void)az;
#endif
}

// 槽位耗尽且策略为 DROP_NEW 时的一次性通知（默认策略是 EVICT_IDLE，走不到这儿）
void hidkit_input_dropped(int8_t slot, uint16_t vid, uint16_t pid)
{
#if HIDKIT_APP_EVENTS
    emit_tagged(LOG_SW_HIDKIT, "HIDKIT", "dropped slot=" LOG_W_SLOT " vid=%04X pid=%04X",
                (int)slot, (unsigned)vid, (unsigned)pid);
#else
    (void)slot; (void)vid; (void)pid;
#endif
}

/*--------------------------------------------------------------------+
 * hidkit 出口：库内诊断（弱符号覆盖）
 *
 * 调用点只在冷路径（挂载/卸载/认领判定/槽位挤出/首次未被消费），不在每报文
 * 解析里 —— 所以这行不随报文速率增长。HIDKIT_DEBUG=0 时库内连调用点都没有。
 *--------------------------------------------------------------------*/

void hidkit_debug_printf(const char *fmt, ...)
{
#if HIDKIT_DEBUG
    char msg[UARTO_REC_MAX];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n > (int)sizeof(msg) - 1) n = (int)sizeof(msg) - 1;

    // 库内消息自带换行（"…\n"），这里统一由 emit_tagged 收尾
    while (n > 0 && (msg[n - 1] == '\n' || msg[n - 1] == '\r')) n--;
    msg[n] = '\0';

    emit_tagged(LOG_SW_HKDBG, "HKDBG", "%s", msg);
#else
    (void)fmt;
#endif
}

/*--------------------------------------------------------------------+
 * HID 侧转发
 *--------------------------------------------------------------------*/

void hidkit_app_init(void)
{
    for (uint8_t i = 0; i < CFG_TUH_HID; i++) s_slot[i] = -1;
    hidkit_init();
}

void hidkit_app_mount(uint8_t dev_addr, uint8_t instance, uint8_t itf_num,
                      uint8_t itf_proto, uint8_t const *desc_report,
                      uint16_t desc_len)
{
    if (instance >= CFG_TUH_HID) return;
    s_slot[instance] = -1;

    uint16_t vid = 0, pid = 0;
    tuh_vid_pid_get(dev_addr, &vid, &pid);

    hidkit_dev_info_t info;
    info.vid = vid;
    info.pid = pid;
    info.dev_addr = dev_addr;
    info.itf = itf_num;
    // TinyUSB 的 hid_interface_protocol_enum_t 与 HIDKIT_PROTO_* 数值一致，
    // 但仍显式映射：本库不依赖任何栈的头文件，这层对应关系归宿主
    info.proto = (itf_proto == HID_ITF_PROTOCOL_KEYBOARD) ? HIDKIT_PROTO_KEYBOARD
               : (itf_proto == HID_ITF_PROTOCOL_MOUSE)    ? HIDKIT_PROTO_MOUSE
                                                          : HIDKIT_PROTO_NONE;
    info.report_desc = desc_report;
    info.report_desc_len = desc_len;

    // <0 = 库不认这个设备（普通 HID 手柄以外的东西、或描述符没抓全）：
    // 保持 -1，后续报文不再打扰库。原始 hexdump 不受影响 —— 采集照旧
    int8_t slot = hidkit_mount(&info);
    if (slot >= 0) s_slot[instance] = slot;
}

void hidkit_app_report(uint8_t instance, uint8_t const *report, uint16_t len)
{
    if (instance >= CFG_TUH_HID) return;
    int8_t const slot = s_slot[instance];
    if (slot < 0) return;

    // 返回值忽略：false = 这份报文本库不认（如已认领的手柄收到了非手柄报文）。
    // 库里对这种"认了设备但吃不下的报文"已经自证过一次（[HKDBG]），不必逐帧刷屏
    (void)hidkit_report(slot, report, len);
}

void hidkit_app_umount(uint8_t instance)
{
    if (instance >= CFG_TUH_HID) return;
    if (s_slot[instance] >= 0) hidkit_umount(s_slot[instance]);
    s_slot[instance] = -1;
}

/*--------------------------------------------------------------------+
 * Switch Pro 校准交付（覆盖独立握手库的弱符号）
 *
 * 握手模块经 SPI 读回手柄校准后回调到这里：映射到 hidkit 槽位注入，
 * 布局解析（gamepad_switch.h）此后按校准量程换算摇杆与六轴
 *--------------------------------------------------------------------*/

void switch_pro_calibration(uint8_t dev_addr, uint8_t instance,
                            const switch_pro_calib_t *cal)
{
    if (instance >= CFG_TUH_HID) return;
    int8_t slot = s_slot[instance];
    if (slot < 0) return;   // hidkit 尚未接管（罕见：校准先于挂载完成）

    hidkit_switchpro_calib_t converted = hidkit_switch_pro_calibration(cal);
    if (hidkit_switchpro_set_calib(slot, &converted)) {
        hid_app_emit_info("[SWPRO] dev=" LOG_W_DEV " calib applied slot=" LOG_W_SLOT,
                          dev_addr, (int)slot);
    }
}

/*--------------------------------------------------------------------+
 * XInput：注册适配器的类驱动
 *
 * usbh_app_driver_get_cb() 是 Host 栈里唯一的应用类驱动注册钩子，全工程只能有
 * 一个定义 —— 所以由本文件（而不是适配器）来定义，把适配器的驱动转发出去。
 * 以后再加别的类驱动（如自定义 HID 类），在这里一起返回、driver_count 加一。
 *--------------------------------------------------------------------*/

usbh_class_driver_t const *usbh_app_driver_get_cb(uint8_t *driver_count)
{
#if CFG_TUH_XINPUT
    *driver_count = 1;
    return hidkit_tusb_xinput_driver();
#else
    (void)driver_count;
    return NULL;
#endif
}
