#ifndef __BUTTON_DETECTOR_H__
#define __BUTTON_DETECTOR_H__

#include <stdbool.h>
#include <stdint.h>

// A区累积-导数双鉴算法状态机
typedef struct {
    // 通用
    bool is_pressed;

    // A区状态
    int a_max_diff;
    int a_ring_buf[16];     // 滑动窗口环形缓冲
    int a_ring_head;
    int a_ring_tail;
    int a_ring_count;
    int a_ring_sum;
    bool a_pending;
    int a_confirm_cnt;
    bool a_observing;
    int a_observe_cnt;
    int a_large_gate;

    // 历史记录 (16帧环形缓冲)
    int history[16];
    int history_idx;
    bool history_filled;
} ButtonDetector;

void detector_reset(ButtonDetector *d);
bool detector_process_frame(ButtonDetector *d, uint8_t phys_ch, int current_val, int setup_raw);

#endif /* __BUTTON_DETECTOR_H__ */
