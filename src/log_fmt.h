#ifndef LOG_FMT_H
#define LOG_FMT_H

/*
 * 数值字段宽度约定（所有 [TAG] 行共用）。
 *
 * 规则：**固定长度 + 左侧补空格**。同一字段在不同行占据相同列数，纵向扫日志
 * 时数字位对齐（"dev= 3" 与 "dev=12" 的后续字段落在同一列）；写 "%-3u" 或不管
 * 宽度，"-1024" 与 "12" 就会把后面的列挤得参差，正是要避免的。
 *
 * 宽度取**该字段值域的上限**，不取类型上限 —— 按类型上限 len/off 要占 5 格
 * （uint16），hexdump 前缀会被撑长一截、每行白烧带宽。值域由本固件/栈的配置
 * 定死（见各条注释），超出时 printf 自然变宽一格，只是那一行不对齐，不会截断。
 *
 * **例外：十六进制标识符保持 0 补定宽**（vid/pid/bcdUSB/cls/code/langid/attr）。
 * 0x046d、0x0004 这类值是"身份"而不是"数量"，左边补空格会读成 0x 46d，语义
 * 就没了；它们本来就是定宽，对齐目标已由 0 补达成。
 *
 * 用宏而不是直接写字面量，是为了让调用点一眼看出"这是对齐字段"，并且改宽度
 * 只改这一处。格式检查（__attribute__((format))）照常生效：相邻字符串字面量
 * 在编译期拼接。
 */

/* 设备地址：栈内 dev_addr < CFG_TUH_DEVICE_MAX + CFG_TUH_HUB + 1 (= 6) */
#define LOG_W_DEV     "%2u"
/* HID 接口号：< CFG_TUH_HID (= 16) */
#define LOG_W_ITF     "%2u"
/* hidkit 槽位：< HIDKIT_MAX_KEYBOARDS + MICE + GAMEPADS (= 12)，-1 = 未接管 */
#define LOG_W_SLOT    "%2d"
/* 字节型描述符字段（0..255）：字符串索引、bNumConfigurations、
 * bNumInterfaces、bConfigurationValue、bNumEndpoints、bcdDevice 高位… */
#define LOG_W_BYTE    "%3u"
/* 长度与偏移：描述符 ≤ CFG_TUH_ENUMERATION_BUFSIZE (512)、HID 报文 ≤ 64 */
#define LOG_W_LEN     "%3u"
#define LOG_W_OFF     "%3u"
/* 配置描述符 wTotalLength（同上，受枚举缓冲限制 ≤ 512） */
#define LOG_W_TOTAL   "%3u"
/* 电流：bMaxPower × 2 ≤ 510 mA */
#define LOG_W_POWER   "%3u"
/* 描述符抓取状态机步骤（0..6）与传输结果码（0..3） */
#define LOG_W_STEP    "%1u"
#define LOG_W_RES     "%1d"
/* 鼠标位移 / 滚轮：hidkit 累加后的轴值（16 位字段 → ±32767） */
#define LOG_W_MOUSE   "%6d"
/* 手柄摇杆：-32767..32767 */
#define LOG_W_STICK   "%6d"
/* 手柄扳机：0..32767（无符号量，少一格符号位） */
#define LOG_W_TRIG    "%5d"
/* 六轴：±32767 */
#define LOG_W_MOTION  "%6d"
/* 队列溢出丢弃块数：uint32 累计计数，10 位是它的十进制上限 */
#define LOG_W_DROP    "%10lu"
/* 运行期开关位的布尔回显（[CTRL] 行）：0/1 单字符，天然定宽 */
#define LOG_W_BIT     "%1u"

#endif // LOG_FMT_H
