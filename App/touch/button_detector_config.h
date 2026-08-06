#ifndef __BUTTON_DETECTOR_CONFIG_H__
#define __BUTTON_DETECTOR_CONFIG_H__

#include <stdint.h>

/* Return the configured A-E block for a physical channel, or 0xff. */
char detector_get_block(uint8_t physical_channel);

/* AquaMai A-zone dynamic-baseline defaults. */
#define DETECTOR_A_EDGE_ON                 320
#define DETECTOR_A_LARGE_ON                850
#define DETECTOR_A_FAST_RISE_DERIV         90
#define DETECTOR_A_EDGE_MIN_DERIV          2

#define DETECTOR_A_SHORT_TAP_PEAK          390
#define DETECTOR_A_PENDING_SETTLE_DERIV    3
#define DETECTOR_A_FAST_PENDING_CANCEL     180

#define DETECTOR_A_CLEAN_RELEASE           105
#define DETECTOR_A_RELEASE_PEAK_RATIO      0.52f
#define DETECTOR_A_RELEASE_MIN_DROP        180
#define DETECTOR_A_RELEASE_DROP_RATIO      0.25f
#define DETECTOR_A_RELEASE_DERIV           (-12)

#define DETECTOR_A_REPRESS_RISE            120
#define DETECTOR_A_REPRESS_DERIV           20
#define DETECTOR_A_REPRESS_SLOW_RISE       280
#define DETECTOR_A_REPRESS_SIGNAL_MIN      250

#define DETECTOR_A_BASELINE_TRACK_RANGE    120
#define DETECTOR_A_BASELINE_QUIET_DERIV    6
#define DETECTOR_A_BASELINE_ALPHA          0.02f
#define DETECTOR_A_BASELINE_MAX_STEP       0.5f

/* AquaMai C-zone defaults. */
#define DETECTOR_C_DIFF_THRESHOLD          25
#define DETECTOR_C_DERIV_THRESHOLD         25
#define DETECTOR_C_DERIV_RELEASE           (-20)
#define DETECTOR_C_DIFF_RELEASE            15

/* AquaMai B/D/E-zone defaults. */
#define DETECTOR_B_DIFF_THRESHOLD          20
#define DETECTOR_B_DERIV_RELEASE           (-20)
#define DETECTOR_D_DIFF_THRESHOLD          20
#define DETECTOR_D_DERIV_RELEASE           (-18)
#define DETECTOR_E_DIFF_THRESHOLD          15
#define DETECTOR_E_DERIV_RELEASE           (-16)

#endif /* __BUTTON_DETECTOR_CONFIG_H__ */
