#ifndef SWITCH_PRO_H
#define SWITCH_PRO_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* USB addresses include hubs and reserved address 0. Override on the consumer
 * target; no USB-stack or platform headers are required by the protocol core. */
#ifndef SWITCH_PRO_ADDR_MAX
#define SWITCH_PRO_ADDR_MAX 16
#endif
#if SWITCH_PRO_ADDR_MAX < 2 || SWITCH_PRO_ADDR_MAX > 256
#error "SWITCH_PRO_ADDR_MAX must be between 2 and 256"
#endif

#define SWITCH_PRO_CAL_STICKS 0x01u
#define SWITCH_PRO_CAL_MOTION 0x02u

/* Native-axis SPI data. Stick min/max are distances from the center, not
 * absolute coordinates. Horizontal reference contains gravity; do not
 * subtract it from accelerometer readings. No normalization is performed. */
typedef struct {
    uint16_t valid;
    uint16_t ls_xc, ls_yc, ls_xmax, ls_xmin, ls_ymax, ls_ymin;
    uint16_t rs_xc, rs_yc, rs_xmax, rs_xmin, rs_ymax, rs_ymin;
    int16_t acc_origin[3], gyro_origin[3], acc_hoffset[3];
    int16_t acc_scale[3], gyro_scale[3];
} switch_pro_calib_t;

/* Call from HID mount, unmount, main loop and input callbacks respectively.
 * Only Nintendo 057e:2009/2006/2007 is handled. Input buffers include Report ID.
 * All calls and port callbacks must run on the same USB task/core; no blocking,
 * allocation, USB enumeration or IN polling is performed by this library.
 * USB handshake uses timed steps; initialization subcommands require ACKs.
 * Unmount clears the whole device address, including any claimed interface. */
void switch_pro_maybe_mount(uint8_t dev_addr, uint8_t instance,
                            uint16_t vid, uint16_t pid);
void switch_pro_umount(uint8_t dev_addr);
void switch_pro_task(void);
void switch_pro_report(uint8_t dev_addr, uint8_t instance,
                       const uint8_t *report, uint16_t len);

/* Required port functions. Time is a wrapping, monotonic 32-bit microsecond
 * clock. Call task regularly (well within half the timer's wrap period).
 * Send returns true when queued; false means busy/failure, retried later.
 * The packet includes Report ID and MUST be copied before send returns;
 * its buffer can be stack storage. The TinyUSB adapter implements send. */
uint32_t switch_pro_time_us(void);
bool switch_pro_send_report(uint8_t dev_addr, uint8_t instance,
                            const uint8_t *report, uint16_t len);

/* Optional weak callbacks (GCC/Clang). Calibration is delivered once when SPI
 * reads finish, even if valid=0. Copy it during the callback if retaining it.
 * SPI timeouts fall back to factory/default calibration. Fatal initialization
 * failure is logged with error=true and stops that device until remounted. */
void switch_pro_calibration(uint8_t dev_addr, uint8_t instance,
                            const switch_pro_calib_t *cal);
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 2, 3)))
#endif
void switch_pro_log(bool error, const char *fmt, ...);

#ifdef __cplusplus
}
#endif
#endif
