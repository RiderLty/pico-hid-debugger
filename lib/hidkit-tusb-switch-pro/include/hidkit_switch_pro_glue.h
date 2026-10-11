#ifndef HIDKIT_SWITCH_PRO_GLUE_H
#define HIDKIT_SWITCH_PRO_GLUE_H

/* Optional hidkit integration. Include tusb.h first if also using TinyUSB:
 * hidkit's MOUSE_BUTTON_* macros overlap TinyUSB's HID enum identifiers. */
#include "switch_pro.h"
#include "hidkit.h"

/* Convert explicitly instead of relying on the two structures' ABI layout. */
static inline hidkit_switchpro_calib_t
hidkit_switch_pro_calibration(const switch_pro_calib_t *src)
{
    hidkit_switchpro_calib_t dst = {0};
    if (src->valid & SWITCH_PRO_CAL_STICKS) dst.valid |= HIDKIT_SWITCHPRO_CAL_STICKS;
    if (src->valid & SWITCH_PRO_CAL_MOTION) dst.valid |= HIDKIT_SWITCHPRO_CAL_MOTION;
    dst.ls_xc = src->ls_xc; dst.ls_yc = src->ls_yc;
    dst.ls_xmax = src->ls_xmax; dst.ls_xmin = src->ls_xmin;
    dst.ls_ymax = src->ls_ymax; dst.ls_ymin = src->ls_ymin;
    dst.rs_xc = src->rs_xc; dst.rs_yc = src->rs_yc;
    dst.rs_xmax = src->rs_xmax; dst.rs_xmin = src->rs_xmin;
    dst.rs_ymax = src->rs_ymax; dst.rs_ymin = src->rs_ymin;
    for (unsigned i = 0; i < 3; i++) {
        dst.acc_origin[i] = src->acc_origin[i];
        dst.gyro_origin[i] = src->gyro_origin[i];
        dst.acc_hoffset[i] = src->acc_hoffset[i];
        dst.acc_scale[i] = src->acc_scale[i];
        dst.gyro_scale[i] = src->gyro_scale[i];
    }
    return dst;
}
#endif
