#ifndef __BUTTON_DETECTOR_H__
#define __BUTTON_DETECTOR_H__

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool is_pressed;

    /* AquaMai A-zone detector state. */
    int diff_deriv_down_count;
    int up;
    bool lock_releasing;
    bool edge_holding;
    int active_edge_min_diff;

    /* Sixteen-frame raw-value history. */
    int history[16];
    int history_idx;
    bool history_filled;
} ButtonDetector;

void detector_reset(ButtonDetector *d);
bool detector_process_frame(ButtonDetector *d, uint8_t phys_ch, int current_val, int setup_raw);

#endif /* __BUTTON_DETECTOR_H__ */
