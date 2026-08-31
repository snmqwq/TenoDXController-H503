#ifndef __BUTTON_DETECTOR_CONFIG_H__
#define __BUTTON_DETECTOR_CONFIG_H__

#include <stdint.h>

/* Return the configured A-E block for a physical channel, or 0xff. */
char detector_get_block(uint8_t physical_channel);

/* ============================================================
 * 判定参数对齐 Python run3 上位机 (detector.py)
 * ============================================================ */

/* ------------------------------------------------------------
 * A-zone: 基线追踪
 * ------------------------------------------------------------ */
#define DETECTOR_A_BASE_FREEZE_DIFF     320.0f  /* diff 超过此值冻结基线 */
#define DETECTOR_A_BASE_ALPHA           0.05f   /* 基线收敛速度 */

/* ------------------------------------------------------------
 * A-zone: 状态机触发阈值
 * ------------------------------------------------------------ */
#define DETECTOR_A_TRIG_DIFF            620.0f  /* 位移超过此值且导数>0 立即触发 */
#define DETECTOR_A_TRIG_MID_MIN         320.0f  /* 中位移区间下限 */
#define DETECTOR_A_TRIG_MID_DERIV       100.0f  /* 中位移区间触发导数 */
#define DETECTOR_A_TRIG_DERIV_HARD      250.0f  /* 极速重击: 延迟一帧触发 */
#define DETECTOR_A_TRIG_DERIV_MAX       400.0f  /* 进阶拦截: 导数超大但位移未达上限 */
#define DETECTOR_A_TRIG_DERIV_MAX_DIFF  1250.0f
#define DETECTOR_A_PRE_MIN_DIFF         100.0f  /* 进入预判定所需最小位移 */
#define DETECTOR_A_PRE_FALL_MIN_DIFF    150.0f  /* 预判定拐点触发所需最小位移 */
#define DETECTOR_A_STATE1_TRIG_DERIV_MIN (-30.0f) /* 预判期间强制触发的导数下限 */

/* ------------------------------------------------------------
 * A-zone: 动态导数预触发线 (分段线性)
 * ------------------------------------------------------------ */
#define DETECTOR_A_PRE_BASE             50.0f
#define DETECTOR_A_PRE_RAMP1_MIN        380.0f
#define DETECTOR_A_PRE_RAMP1_MAX        900.0f
#define DETECTOR_A_PRE_RAMP1_LO         65.0f
#define DETECTOR_A_PRE_RAMP1_HI         300.0f
#define DETECTOR_A_PRE_RAMP2_MIN        900.0f
#define DETECTOR_A_PRE_RAMP2_MAX        1600.0f
#define DETECTOR_A_PRE_RAMP2_LO         300.0f
#define DETECTOR_A_PRE_RAMP2_HI         200.0f

/* ------------------------------------------------------------
 * A-zone: 释放条件
 * ------------------------------------------------------------ */
#define DETECTOR_A_RELEASE_RATIO        0.5f    /* 有效位移低于触发值的一半 */
#define DETECTOR_A_RELEASE_DERIV        (-200.0f) /* 或导数骤降触发快放 */

/* ------------------------------------------------------------
 * B/C/D/E 简单区 (对齐 run3 ZONE_CONFIGS)
 * ------------------------------------------------------------ */
#define DETECTOR_B_DIFF_TRIG            16.0f
#define DETECTOR_B_DERIV_REL            (-15.0f)

#define DETECTOR_C_DIFF_TRIG            40.0f
#define DETECTOR_C_DERIV_TRIG           35.0f
#define DETECTOR_C_DERIV_REL            (-20.0f)
#define DETECTOR_C_DIFF_REL             15.0f

#define DETECTOR_D_DIFF_TRIG            22.0f
#define DETECTOR_D_DERIV_REL            (-8.0f)

#define DETECTOR_E_DIFF_TRIG            10.0f
#define DETECTOR_E_DERIV_REL            (-16.0f)

#endif /* __BUTTON_DETECTOR_CONFIG_H__ */
