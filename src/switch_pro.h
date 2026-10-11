#ifndef SWITCH_PRO_H
#define SWITCH_PRO_H

/*
 * Switch Pro 手柄（任天堂 057e:2009/2006/2007）USB 握手模块。
 *
 * 这些手柄枚举为标准 HID 接口，但在主机完成握手前**完全不发输入报文**
 * （订阅中断 IN 端点也收不到任何东西）。本模块在 HID 接口挂载后按
 * hid-nintendo（Linux 内核驱动）的顺序发一组中断 OUT 报文：
 *
 *   80 02   握手（应答 0x81 0x02）
 *   80 03   切 3M 波特率（BT 专属，USB 上无意义，失败也继续）
 *   80 02   再握手
 *   80 04   关 USB 超时（无应答）
 *   01 [16B 零 rumble] 48 01   使能震动（部分固件不使能不出全量报文）
 *   01 [16B 零 rumble] 03 30   设输入报告模式 = 0x30 标准全量 → 报文流开始
 *
 * 全部等待/延时都在 switch_pro_task() 里：按 time_us_32 步进的**非阻塞**
 * 状态机（每步 15ms，发送失败不推进、下轮重试），适配"大循环 + tuh_task"
 * 的宿主结构，不阻塞同总线其他设备。
 *
 * 移植到其他工程只需重接五处：
 *   - 三个 API 的调用位置（mount 回调 / umount 回调 / 大循环）
 *   - 两类日志出口（emit_info → 本工程 LOG_SW_INFO 门控行；
 *     emit_error → 恒开错误行）——见 switch_pro.c 顶部的 include 与调用
 */

#include <stdint.h>
#include <stdbool.h>

// 挂载时调用：模块内部自判 vid/pid（0x057e:2009 Pro / 2006/2007 Joy-Con），
// 非目标设备直接返回，调用方无须预判
void switch_pro_maybe_mount(uint8_t dev_addr, uint8_t instance,
                            uint16_t vid, uint16_t pid);

// 拔出时调用：清对应槽位（重新枚举会自然重跑一遍握手）
void switch_pro_umount(uint8_t dev_addr);

// 周期任务：在 tuh_task() 之后的同一循环里每轮调用（与 USB 栈同核）
void switch_pro_task(void);

#endif // SWITCH_PRO_H
