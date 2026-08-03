#include "button_detector.h"
#include "button_detector_config.h"
#include <string.h>

// ================= 历史记录 =================

static int history_get(ButtonDetector *d, int frames_ago)
{
    if (!d->history_filled) return d->history[0];
    int idx = (d->history_idx - 1 - frames_ago + 16) % 16;
    return d->history[idx];
}

static void history_push(ButtonDetector *d, int val)
{
    d->history[d->history_idx] = val;
    d->history_idx = (d->history_idx + 1) % 16;
    if (d->history_idx == 0) d->history_filled = true;
}

// ================= A区环形队列 =================

static void a_ring_enqueue(ButtonDetector *d, int val)
{
    d->a_ring_buf[d->a_ring_tail] = val;
    d->a_ring_tail = (d->a_ring_tail + 1) % 16;
    d->a_ring_sum += val;
    d->a_ring_count++;
}

static int a_ring_dequeue(ButtonDetector *d)
{
    int val = d->a_ring_buf[d->a_ring_head];
    d->a_ring_head = (d->a_ring_head + 1) % 16;
    d->a_ring_sum -= val;
    d->a_ring_count--;
    return val;
}

static void a_ring_clear(ButtonDetector *d)
{
    d->a_ring_head = 0;
    d->a_ring_tail = 0;
    d->a_ring_count = 0;
    d->a_ring_sum = 0;
}

// ================= 公开 API =================

void detector_reset(ButtonDetector *d)
{
    memset(d, 0, sizeof(ButtonDetector));
}

bool detector_process_frame(ButtonDetector *d, uint8_t phys_ch, int current_val, int setup_raw)
{
    char block = detector_get_block(phys_ch);
    int diff = current_val - setup_raw;
    int diff_deriv = current_val - history_get(d, 0);

    bool on = false;

    if (block == 'A')
    {
        // ==========================================
        // A区：累积-导数双鉴算法
        // ==========================================
        int large_diff_thresh = DETECTOR_A_TRIGGER_SENSITIVITY;

        if (diff >= large_diff_thresh)
        {
            if (!d->is_pressed && d->a_large_gate < DETECTOR_A_LARGE_SIGNAL_GATE)
            {
                d->a_large_gate++;
                on = false;
            }
            else
            {
                on = true;
                d->a_max_diff = (diff > d->a_max_diff) ? diff : d->a_max_diff;
                d->a_observing = false;
                d->a_large_gate = 0;
            }
        }
        else if (d->a_large_gate > 0 && diff > 200)
        {
            d->a_large_gate++;
            if (d->a_large_gate > DETECTOR_A_LARGE_SIGNAL_GATE)
            {
                on = true;
                d->a_max_diff = (diff > d->a_max_diff) ? diff : d->a_max_diff;
                d->a_large_gate = 0;
            }
            else
            {
                on = false;
            }
        }
        else if (d->is_pressed)
        {
            if (diff > d->a_max_diff) d->a_max_diff = diff;

            int release_thresh = (int)(d->a_max_diff * DETECTOR_A_RELEASE_RATIO);
            if (release_thresh < DETECTOR_A_RELEASE_FLOOR) release_thresh = DETECTOR_A_RELEASE_FLOOR;

            if (diff < release_thresh || diff_deriv < DETECTOR_A_SHARP_RELEASE_DERIV)
            {
                on = false;
                d->a_max_diff = 0;
                a_ring_clear(d);
                d->a_pending = false;
                d->a_observing = false;
            }
            else
            {
                on = true;
            }
        }
        else if (d->a_pending)
        {
            d->a_confirm_cnt++;
            if (diff > d->a_max_diff) d->a_max_diff = diff;

            if (d->a_observing)
            {
                d->a_observe_cnt++;
                if (diff_deriv < DETECTOR_A_CRASH_DERIV_THRESHOLD && diff < DETECTOR_A_CRASH_DIFF_THRESHOLD)
                {
                    d->a_pending = false;
                    d->a_observing = false;
                    d->a_max_diff = 0;
                    a_ring_clear(d);
                    on = false;
                }
                else if (d->a_observe_cnt >= DETECTOR_A_CRASH_WINDOW)
                {
                    on = true;
                    d->a_pending = false;
                    d->a_observing = false;
                }
                else
                {
                    on = false;
                }
            }
            else if (diff > DETECTOR_A_CONFIRM_DIFF)
            {
                d->a_observing = true;
                d->a_observe_cnt = 0;
                on = false;
            }
            else if (d->a_confirm_cnt >= DETECTOR_A_CONFIRM_FRAMES)
            {
                d->a_pending = false;
                d->a_observing = false;
                d->a_max_diff = 0;
                a_ring_clear(d);
                on = false;
            }
            else
            {
                on = false;
            }
        }
        else
        {
            d->a_large_gate = 0;
            a_ring_enqueue(d, diff);
            if (d->a_ring_count > DETECTOR_A_WINDOW_SIZE)
                a_ring_dequeue(d);

            if (d->a_ring_count > 0)
            {
                float cum_avg = (float)d->a_ring_sum / d->a_ring_count;
                if (cum_avg > 0.1f)
                {
                    float spike_ratio = diff / cum_avg;
                    if (spike_ratio > DETECTOR_A_TRIGGER_RATIO
                        && diff_deriv > DETECTOR_A_TRIGGER_DERIV
                        && diff > DETECTOR_A_TRIGGER_DIFF_MIN)
                    {
                        d->a_pending = true;
                        d->a_confirm_cnt = 0;
                        d->a_observing = false;
                        d->a_max_diff = diff;
                    }
                }
            }
            on = false;
        }
    }
    else if (block == 'C')
    {
        int c_diff   = DETECTOR_C_DIFF_THRESHOLD;
        int c_deriv_t = DETECTOR_C_DERIV_THRESHOLD;
        int c_deriv_r = DETECTOR_C_DERIV_RELEASE;
        int c_diff_r  = DETECTOR_C_DIFF_RELEASE;

        if (diff > c_diff || diff_deriv > c_deriv_t)
        {
            if (diff_deriv < c_deriv_r && diff < c_diff * 1.5f)
                on = false;
            else
                on = true;
        }
        else if (diff < c_diff_r)
        {
            on = false;
        }
    }
    else // B/D/E
    {
        int default_diff = 15;
        int default_deriv_r = -16;

        if (block == 'B') {
            default_diff = DETECTOR_B_DIFF_THRESHOLD;
            default_deriv_r = DETECTOR_B_DERIV_RELEASE;
        } else if (block == 'D') {
            default_diff = DETECTOR_D_DIFF_THRESHOLD;
            default_deriv_r = DETECTOR_D_DERIV_RELEASE;
        } else if (block == 'E') {
            default_diff = DETECTOR_E_DIFF_THRESHOLD;
            default_deriv_r = DETECTOR_E_DERIV_RELEASE;
        }

        int bde_diff = default_diff;
        int bde_deriv_r = default_deriv_r;

        int last_diff = history_get(d, 0) - setup_raw;

        if (diff > bde_diff * 1.5f)
            on = true;
        else if (diff > bde_diff && last_diff > bde_diff / 2)
            on = true;
        else if (d->is_pressed && diff > bde_diff)
            on = true;

        if (diff_deriv < bde_deriv_r)
            if (diff < bde_diff * 1.5f)
                on = false;

        if (diff <= bde_diff / 2)
            on = false;
    }

    d->is_pressed = on;
    history_push(d, current_val);

    return on;
}
