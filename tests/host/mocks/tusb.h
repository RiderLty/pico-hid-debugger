#ifndef TEST_TUSB_H
#define TEST_TUSB_H
#include <stdbool.h>
#include <stdint.h>
#define CFG_TUH_DEVICE_MAX 4
#define CFG_TUH_HUB 1
bool tuh_hid_send_report(uint8_t addr, uint8_t instance, uint8_t report_id,
                         const void *report, uint16_t len);
#endif
