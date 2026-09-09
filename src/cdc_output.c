/*
 * 跨核传输层实现：SPSC 字节块队列，按 kind 分发。
 *
 * 数据通路：core1 格式化字节块（打 kind 标签）→ SPSC 队列（critical_section 保护）
 *           → core0 主循环 cdc_output_flush() 批量出队，
 *             CDC 数据写 CDC FIFO，HID 报文写入模拟键鼠设备端点。
 */

#include <string.h>
#include <stdio.h>

#include "pico/sync.h"
#include "tusb.h"

#include "cdc_output.h"
#include "hid_output.h"

#define CDC_QUEUE_DEPTH  128u   // 2 的幂（掩码索引）；约 12.5KB 静态 RAM
#define CDC_FLUSH_BUDGET 32u    // core0 每轮最多出队块数，防止饿死 tud_task()

// 模拟 HID 设备的 TinyUSB instance 号（与配置描述符中 HID 接口顺序一致）
#define DEV_ITF_KEYBOARD 0u
#define DEV_ITF_MOUSE    1u

typedef struct {
    uint8_t len;                    // 含 kind 标签的总长
    uint8_t data[CDC_REC_MAX];
} cdc_rec_t;

static cdc_rec_t      s_queue[CDC_QUEUE_DEPTH];
static uint16_t       s_head, s_tail, s_count;
static critical_section_t s_cs;
static uint32_t       s_dropped;   // 溢出丢弃块数（临界区内读写）

void cdc_output_init(void)
{
    critical_section_init(&s_cs);
    s_head = s_tail = s_count = 0;
    s_dropped = 0;
}

// 入队核心（须持有临界区）。队列满则丢最新：保住更早的事件，
// 且被丢的多为重复率最高的移动帧。
static void queue_push_locked(uint8_t kind, const uint8_t *data, uint8_t len)
{
    if (s_count == CDC_QUEUE_DEPTH) {
        s_dropped++;
        return;
    }
    cdc_rec_t *r = &s_queue[s_tail];
    r->len = (uint8_t)(len + 1u);
    r->data[0] = kind;
    memcpy(&r->data[1], data, len);
    s_tail = (uint16_t)((s_tail + 1u) & (CDC_QUEUE_DEPTH - 1u));
    s_count++;
}

void cdc_output_send(uint8_t kind, const void *data, uint8_t len)
{
    if (!data || !len) return;

    if (kind == CDCO_KIND_CDC_DATA && !tud_cdc_connected()) {
        return;  // 串口未连接即产即弃，不缓存回放
    }

    if (len > CDC_REC_MAX - 1u) len = (uint8_t)(CDC_REC_MAX - 1u);

    critical_section_enter_blocking(&s_cs);
    queue_push_locked(kind, (const uint8_t *)data, len);
    critical_section_exit(&s_cs);
}

void cdc_output_flush(void)
{
    for (uint8_t budget = CDC_FLUSH_BUDGET; budget; budget--) {
        critical_section_enter_blocking(&s_cs);
        if (s_count == 0) {
            uint32_t dropped = s_dropped;
            s_dropped = 0;
            critical_section_exit(&s_cs);

            // 队列排空后一次性补报溢出量。溢出提示只走串口：
            // 设备模式下没有可承载它的通道，继续累计即可
            if (dropped && !hid_output_dest_is_device() && tud_cdc_connected()) {
                char buf[64];
                int n;
                if (hid_output_is_binary()) {
                    n = (int)hid_output_binary_meta((uint8_t *)buf, HIDOUT_TYPE_DROP,
                                                    &(uint32_t){ dropped }, sizeof(uint32_t));
                } else {
                    n = snprintf(buf, sizeof(buf), "[DROP ] lost_events=%lu\r\n",
                                 (unsigned long)dropped);
                }
                if (n > 0) tud_cdc_write(buf, (uint32_t)n);
            }
            break;
        }

        cdc_rec_t *r = &s_queue[s_head];
        uint8_t len = (uint8_t)(r->len - 1u);
        uint8_t kind = r->data[0];

        // 先确认目的地放得下整条再出队：出队后再失败会把记录拆丢
        if (kind == CDCO_KIND_HID_KB || kind == CDCO_KIND_HID_MS) {
            uint8_t itf = (kind == CDCO_KIND_HID_KB) ? DEV_ITF_KEYBOARD : DEV_ITF_MOUSE;
            if (!tud_hid_n_ready(itf)) {
                critical_section_exit(&s_cs);
                break;  // 设备未枚举或上一份报告还在发送，等下一轮
            }
        } else {
            if (!tud_cdc_connected() || tud_cdc_write_available() < len) {
                critical_section_exit(&s_cs);
                break;  // 本轮 CDC FIFO 已满或串口断开，等 tud_task() 后再来
            }
        }

        uint8_t data[CDC_REC_MAX];
        memcpy(data, &r->data[1], len);
        s_head = (uint16_t)((s_head + 1u) & (CDC_QUEUE_DEPTH - 1u));
        s_count--;
        critical_section_exit(&s_cs);

        if (kind == CDCO_KIND_HID_KB) {
            tud_hid_n_report(DEV_ITF_KEYBOARD, 0, data, len);
        } else if (kind == CDCO_KIND_HID_MS) {
            tud_hid_n_report(DEV_ITF_MOUSE, 0, data, len);
        } else {
            tud_cdc_write(data, len);
        }
    }

    tud_cdc_write_flush();
}
