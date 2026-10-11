#include "tusb.h"
#include "switch_pro.h"

bool switch_pro_send_report(uint8_t dev_addr, uint8_t instance,
                            const uint8_t *report, uint16_t len)
{
    /* Report ID is already in the payload; TinyUSB copies to its OUT buffer. */
    return tuh_hid_send_report(dev_addr, instance, 0, report, len);
}
