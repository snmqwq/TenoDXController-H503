#ifndef __BUTTON_DETECTOR_H__
#define __BUTTON_DETECTOR_H__

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool is_pressed;

    /* A-zone state (对齐 run3 detector.py _AChannelState) */
    bool baseline_valid;
    float baseline;              /* 追踪基线 (Raw 坐标系) */
    uint8_t fsm_state;           /* 0:未触发 1:预判定 2:触发 3:极速重击延迟一帧 */
    float pre_judge_deriv;       /* 预判定记录的导数, 用于寻找拐点 */
    float touched_effective_diff;/* 触发瞬间的有效位移, 用于释放判定 */
    float prev_deriv2;           /* 上一帧二阶导数, 用于计算三阶导数 */
} ButtonDetector;

void detector_reset(ButtonDetector *d);
bool detector_process_frame(ButtonDetector *d, uint8_t phys_ch,
                            float val, float diff, float setup_raw,
                            float deriv, float deriv2);

#endif /* __BUTTON_DETECTOR_H__ */
