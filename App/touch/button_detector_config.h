#ifndef __BUTTON_DETECTOR_CONFIG_H__
#define __BUTTON_DETECTOR_CONFIG_H__

#include <stdint.h>

/* Return the configured A-E block for a physical channel, or 0xff. */
char detector_get_block(uint8_t physical_channel);

/* AquaMai A-zone defaults. */
#define DETECTOR_A_TRIGGER_SENSITIVITY    650
#define DETECTOR_A_HOLD_THRESHOLD         450
#define DETECTOR_A_QUICK_RELEASE_LINE     1200
#define DETECTOR_A_HOVER_DIFF_MAX         1000
#define DETECTOR_A_HOVER_SPEED_MAX        15
#define DETECTOR_A_FAST_LIFT_SPEED        (-250)

/* Paired Deriv and MinDiff ranges, evaluated by array index. */
#define DETECTOR_A_EDGE_TRIGGER_COUNT     3U
#define DETECTOR_A_EDGE_DERIV_MIN_VALUES  { 180, 120, 50 }
#define DETECTOR_A_EDGE_DERIV_MAX_VALUES  { 9999, 9999, 9999 }
#define DETECTOR_A_EDGE_DIFF_MIN_VALUES   { 200, 300, 400 }
#define DETECTOR_A_EDGE_DIFF_MAX_VALUES   { 9999, 9999, 9999 }

/* AquaMai C-zone defaults. */
#define DETECTOR_C_DIFF_THRESHOLD         25
#define DETECTOR_C_DERIV_THRESHOLD        25
#define DETECTOR_C_DERIV_RELEASE          (-20)
#define DETECTOR_C_DIFF_RELEASE           15

/* AquaMai B/D/E-zone defaults. */
#define DETECTOR_B_DIFF_THRESHOLD         8
#define DETECTOR_B_DERIV_RELEASE          (-15)
#define DETECTOR_D_DIFF_THRESHOLD         3
#define DETECTOR_D_DERIV_RELEASE          (-4)
#define DETECTOR_E_DIFF_THRESHOLD         15
#define DETECTOR_E_DERIV_RELEASE          (-16)

#endif /* __BUTTON_DETECTOR_CONFIG_H__ */
