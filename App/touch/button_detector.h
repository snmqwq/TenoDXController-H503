#ifndef __BUTTON_DETECTOR_H__
#define __BUTTON_DETECTOR_H__

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool is_pressed;

    /* A-zone per-channel state */
    float a_baseline;
    bool a_baseline_initialized;
    float a_peak;
    bool a_after_release;
    float a_valley_raw;
    bool a_valley_valid;
    bool a_quick_repress_armed;
    int a_frames_after_release;
    int a_settled_after_release_frames;
    bool a_fast_pending;
    float a_pending_peak;
    char a_pending_kind;         /* 'f' = fast, 'e' = edge, 0 = none */
    int a_pending_frames;
    float a_pending_max_deriv;

    /* Sixteen-frame raw-value history */
    int history[16];
    int history_idx;
    int history_count;
} ButtonDetector;

void detector_reset(ButtonDetector *d);
bool detector_process_frame(ButtonDetector *d, uint8_t phys_ch,
                            int current_val, int setup_raw);

#endif /* __BUTTON_DETECTOR_H__ */
