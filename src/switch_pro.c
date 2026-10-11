/* Pico clock and debugger logging ports for lib/hidkit-tusb-switch-pro.
 * Protocol state, packets and SPI calibration belong to the reusable library. */
#include "switch_pro.h"

#include <stdarg.h>
#include <stdio.h>

#include "pico/time.h"
#include "hid_host_app.h"
#include "uart_output.h"

uint32_t switch_pro_time_us(void)
{
    return time_us_32();
}

void switch_pro_log(bool error, const char *fmt, ...)
{
    char message[UARTO_REC_MAX];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (error) hid_app_emit("%s", message);
    else hid_app_emit_info("%s", message);
}
