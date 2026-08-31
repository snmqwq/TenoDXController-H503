#include "button_detector.h"
#include "button_detector_config.h"

#include <string.h>

/* ---------------------------------------------------------------- */
/* 简单区 (B/C/D/E) 抽象算法配置 — 对齐 run3 detector.py ZONE_CONFIGS */
/* ---------------------------------------------------------------- */
typedef struct {
    float diff_trig;
    float deriv_rel;
    bool has_deriv_trig;
    float deriv_trig;
    bool has_diff_rel;
    float diff_rel;
} SimpleZoneConfig;

static const SimpleZoneConfig simple_zones[4] = {
    /* B */ { DETECTOR_B_DIFF_TRIG, DETECTOR_B_DERIV_REL, false, 0.0f, false, 0.0f },
    /* C */ { DETECTOR_C_DIFF_TRIG, DETECTOR_C_DERIV_REL, true, DETECTOR_C_DERIV_TRIG, true, DETECTOR_C_DIFF_REL },
    /* D */ { DETECTOR_D_DIFF_TRIG, DETECTOR_D_DERIV_REL, false, 0.0f, false, 0.0f },
    /* E */ { DETECTOR_E_DIFF_TRIG, DETECTOR_E_DERIV_REL, false, 0.0f, false, 0.0f },
};

/* ---------------------------------------------------------------- */
/* A-zone: 动态导数预触发线 (分段线性, 对齐 detector.py)              */
/* ---------------------------------------------------------------- */
static float a_deriv_pre_trig(float effective_diff)
{
    if (effective_diff < DETECTOR_A_PRE_RAMP1_MIN)
    {
        return DETECTOR_A_PRE_BASE;
    }
    if (effective_diff <= DETECTOR_A_PRE_RAMP1_MAX)
    {
        return DETECTOR_A_PRE_RAMP1_LO +
               (effective_diff - DETECTOR_A_PRE_RAMP1_MIN) *
                   (DETECTOR_A_PRE_RAMP1_HI - DETECTOR_A_PRE_RAMP1_LO) /
                   (DETECTOR_A_PRE_RAMP1_MAX - DETECTOR_A_PRE_RAMP1_MIN);
    }
    if (effective_diff <= DETECTOR_A_PRE_RAMP2_MAX)
    {
        return DETECTOR_A_PRE_RAMP2_LO +
               (effective_diff - DETECTOR_A_PRE_RAMP2_MIN) *
                   (DETECTOR_A_PRE_RAMP2_HI - DETECTOR_A_PRE_RAMP2_LO) /
                   (DETECTOR_A_PRE_RAMP2_MAX - DETECTOR_A_PRE_RAMP2_MIN);
    }
    return DETECTOR_A_PRE_RAMP2_HI;
}

/* ---------------------------------------------------------------- */
/* 公开 API                                                          */
/* ---------------------------------------------------------------- */

void detector_reset(ButtonDetector *d)
{
    memset(d, 0, sizeof(*d));
}

static bool evaluate_simple_zone(float diff, float deriv,
                                 const SimpleZoneConfig *cfg)
{
    bool on = false;

    /* 触发逻辑 */
    if ((diff > cfg->diff_trig) ||
        (cfg->has_deriv_trig && (deriv > cfg->deriv_trig)))
    {
        on = true;
    }

    /* 抑制/松开逻辑 (放后面优先级更高) */
    if ((deriv < cfg->deriv_rel) ||
        (cfg->has_diff_rel && (diff < cfg->diff_rel)))
    {
        on = false;
    }

    return on;
}

bool detector_process_frame(ButtonDetector *d, uint8_t phys_ch,
                            float val, float diff, float setup_raw,
                            float deriv, float deriv2)
{
    char block = detector_get_block(phys_ch);
    bool on = false;

    /* ------------------------------------------------------------ */
    /* A-zone: 基线追踪 + 四状态机判定 (对齐 run3 detector.py)        */
    /* ------------------------------------------------------------ */
    if (block == 'A')
    {
        float raw = val;
        float current_diff = diff;

        /* 基础量初始化 (Raw 坐标系) */
        if (!d->baseline_valid)
        {
            d->baseline = setup_raw;
            d->baseline_valid = true;
        }

        /* 基线追踪: diff > 320 冻结, 否则以 0.05 慢速向当前值收敛 */
        if (current_diff <= DETECTOR_A_BASE_FREEZE_DIFF)
        {
            d->baseline += (raw - d->baseline) * DETECTOR_A_BASE_ALPHA;
        }

        /* 有效位移 = 当前位移 - 基线漂移 */
        float baseline_diff = d->baseline - setup_raw;
        float effective_diff = current_diff - baseline_diff;

        /* 三阶导数 = 二阶导数的帧间差分 */
        float deriv3 = deriv2 - d->prev_deriv2;
        d->prev_deriv2 = deriv2;

        switch (d->fsm_state)
        {
        case 0U:
        {
            /* 状态0: 未触发 */
            float pre_trig = a_deriv_pre_trig(effective_diff);

            if ((deriv > DETECTOR_A_TRIG_DERIV_MAX) &&
                (effective_diff < DETECTOR_A_TRIG_DERIV_MAX_DIFF))
            {
                /* 极速重击进阶拦截: 导数超大且位移未达上限, 延迟到下一帧触发 */
                d->fsm_state = 3U;
            }
            else if ((effective_diff > DETECTOR_A_TRIG_DIFF) &&
                     (deriv > 0.0f))
            {
                /* 位移超 620 且导数向上, 跳过预判定立即触发 */
                d->touched_effective_diff = effective_diff;
                d->fsm_state = 2U;
                d->is_pressed = true;
                d->pre_judge_deriv = 0.0f;
            }
            else if ((effective_diff >= DETECTOR_A_TRIG_MID_MIN) &&
                     (effective_diff <= DETECTOR_A_TRIG_DIFF) &&
                     (deriv > DETECTOR_A_TRIG_MID_DERIV) &&
                     (deriv3 < 0.0f))
            {
                /* 中位移区间: 导数>100 且三阶导数<0, 立即触发 */
                d->touched_effective_diff = effective_diff;
                d->fsm_state = 2U;
                d->is_pressed = true;
                d->pre_judge_deriv = 0.0f;
            }
            else if (deriv >= DETECTOR_A_TRIG_DERIV_HARD)
            {
                /* 极速重击: 导数到达 250, 延迟到下一帧触发 */
                d->fsm_state = 3U;
            }
            else if ((deriv > pre_trig) &&
                     (effective_diff >= DETECTOR_A_PRE_MIN_DIFF))
            {
                /* 常规/缓慢触发: 使用动态导数预触发线寻找拐点 */
                d->fsm_state = 1U;
                d->pre_judge_deriv = deriv;
            }
            break;
        }

        case 1U:
        {
            /* 状态1: 预判定 */
            if ((deriv > DETECTOR_A_TRIG_DERIV_MAX) &&
                (effective_diff < DETECTOR_A_TRIG_DERIV_MAX_DIFF))
            {
                d->fsm_state = 3U;
            }
            else if ((effective_diff > DETECTOR_A_TRIG_DIFF) &&
                     (deriv > DETECTOR_A_STATE1_TRIG_DERIV_MIN))
            {
                /* 预判期间位移超 620, 立即触发 */
                d->touched_effective_diff = effective_diff;
                d->fsm_state = 2U;
                d->is_pressed = true;
                d->pre_judge_deriv = 0.0f;
            }
            else if ((effective_diff >= DETECTOR_A_TRIG_MID_MIN) &&
                     (effective_diff <= DETECTOR_A_TRIG_DIFF) &&
                     (deriv > DETECTOR_A_TRIG_MID_DERIV) &&
                     (deriv3 < 0.0f))
            {
                d->touched_effective_diff = effective_diff;
                d->fsm_state = 2U;
                d->is_pressed = true;
                d->pre_judge_deriv = 0.0f;
            }
            else if (deriv >= DETECTOR_A_TRIG_DERIV_HARD)
            {
                /* 预判期间突增: 导数冲到 250, 延迟到下一帧触发 */
                d->fsm_state = 3U;
            }
            else if (deriv > d->pre_judge_deriv)
            {
                d->pre_judge_deriv = deriv;
            }
            else if ((deriv < d->pre_judge_deriv) &&
                     (effective_diff >= DETECTOR_A_PRE_FALL_MIN_DIFF))
            {
                /* 找到导数下降拐点, 常规触发 */
                d->touched_effective_diff = effective_diff;
                d->fsm_state = 2U;
                d->is_pressed = true;
                d->pre_judge_deriv = 0.0f;
            }
            break;
        }

        case 3U:
            /* 状态3: 极速重击延迟一帧触发 */
            d->touched_effective_diff = effective_diff;
            d->fsm_state = 2U;
            d->is_pressed = true;
            d->pre_judge_deriv = 0.0f;
            break;

        case 2U:
            /* 状态2: 已触发, 判定释放 */
            if ((effective_diff <
                 (d->touched_effective_diff * DETECTOR_A_RELEASE_RATIO)) ||
                (deriv < DETECTOR_A_RELEASE_DERIV))
            {
                d->fsm_state = 0U;
                d->is_pressed = false;
                d->touched_effective_diff = 0.0f;
            }
            break;

        default:
            d->fsm_state = 0U;
            d->is_pressed = false;
            break;
        }

        on = d->is_pressed;
    }
    /* ------------------------------------------------------------ */
    /* 简单区 (B/C/D/E): 电平式判定, 非粘滞                          */
    /* ------------------------------------------------------------ */
    else if ((block >= 'B') && (block <= 'E'))
    {
        const SimpleZoneConfig *cfg =
            &simple_zones[(unsigned)(block - 'B')];

        on = evaluate_simple_zone(diff, deriv, cfg);
    }
    /* ------------------------------------------------------------ */
    /* 未知区块                                                       */
    /* ------------------------------------------------------------ */
    else
    {
        on = false;
    }

    d->is_pressed = on;
    return on;
}
