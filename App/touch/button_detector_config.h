#ifndef __BUTTON_DETECTOR_CONFIG_H__
#define __BUTTON_DETECTOR_CONFIG_H__

#include <stdint.h>

/* Return the configured A-E block for a physical channel, or 0xff. */
char detector_get_block(uint8_t physical_channel);

/* ============================================================
 * A-zone: press detection
 * ============================================================ */

#define DETECTOR_A_EDGE_ON                 320
#define DETECTOR_A_EDGE_CANDIDATE_ON       190
#define DETECTOR_A_EDGE_TAP_PEAK           220
#define DETECTOR_A_EDGE_SOLID_ON           520
#define DETECTOR_A_EDGE_CONFIRM_FRAMES     1
#define DETECTOR_A_LARGE_ON                850
#define DETECTOR_A_LARGE_CONFIRM_DERIV     40
#define DETECTOR_A_FAST_RISE_DERIV         90
#define DETECTOR_A_EDGE_MIN_DERIV          2
#define DETECTOR_A_EDGE_FALL_DERIV         (-65)
#define DETECTOR_A_EDGE_FALL_SIGNAL        180
#define DETECTOR_A_APPROACH_CONFIRM_ON     720
#define DETECTOR_A_APPROACH_DERIV_RATIO    0.75f

/* ============================================================
 * A-zone: quick re-press
 * ============================================================ */

#define DETECTOR_A_REPRESS_RISE            70
#define DETECTOR_A_REPRESS_DERIV           12
#define DETECTOR_A_REPRESS_SIGNAL_MIN      250
#define DETECTOR_A_REPRESS_WINDOW_FRAMES   2
#define DETECTOR_A_REPRESS_SETTLE_FRAMES   1

/* ============================================================
 * A-zone: short tap compensation
 * ============================================================ */

#define DETECTOR_A_SHORT_TAP_PEAK          390
#define DETECTOR_A_FAST_PENDING_CANCEL     180

/* ============================================================
 * A-zone: release detection
 * ============================================================ */

#define DETECTOR_A_CLEAN_RELEASE           105
#define DETECTOR_A_RELEASE_PEAK_RATIO      0.52f
#define DETECTOR_A_RELEASE_MIN_DROP        180
#define DETECTOR_A_RELEASE_DROP_RATIO      0.25f
#define DETECTOR_A_RELEASE_DERIV           (-12)
#define DETECTOR_A_FAST_RELEASE_DERIV      (-18)
#define DETECTOR_A_FAST_RELEASE_MIN_DROP   100
#define DETECTOR_A_FAST_RELEASE_SIGNAL     320
#define DETECTOR_A_SLIDE_RELEASE_DERIV     (-55)
#define DETECTOR_A_SLIDE_RELEASE_DROP_RATIO 0.18f
#define DETECTOR_A_SLIDE_RELEASE_PEAK_RATIO 0.82f

/* ============================================================
 * A-zone: baseline tracking
 * ============================================================ */

#define DETECTOR_A_BASE_TRACK_RANGE        120
#define DETECTOR_A_BASE_QUIET_DERIV        6
#define DETECTOR_A_BASE_ALPHA              0.02f
#define DETECTOR_A_BASE_MAX_STEP           0.5f

/* ============================================================
 * C-zone
 * ============================================================ */

#define DETECTOR_C_DIFF_THRESHOLD          25
#define DETECTOR_C_DERIV_THRESHOLD         25
#define DETECTOR_C_DERIV_RELEASE           (-20)
#define DETECTOR_C_DIFF_RELEASE            15

/* ============================================================
 * B/D/E-zone
 * ============================================================ */

#define DETECTOR_B_DIFF_THRESHOLD          20
#define DETECTOR_B_DERIV_RELEASE           (-15)
#define DETECTOR_D_DIFF_THRESHOLD          20
#define DETECTOR_D_DERIV_RELEASE           (-20)
#define DETECTOR_E_DIFF_THRESHOLD          15
#define DETECTOR_E_DERIV_RELEASE           (-16)

#endif /* __BUTTON_DETECTOR_CONFIG_H__ */
