#include "button_detector.h"
#include "button_detector_config.h"

#include <math.h>
#include <string.h>

#define DETECTOR_HISTORY_SIZE 16

#if defined(DEBUG) && defined(__GNUC__)
#define DETECTOR_SIZE_OPTIMIZED __attribute__((optimize("Os")))
#else
#define DETECTOR_SIZE_OPTIMIZED
#endif

static int max_int(int left, int right)
{
    return (left > right) ? left : right;
}

static float max_float(float left, float right)
{
    return (left > right) ? left : right;
}

static void a_clear_fast_pending(ButtonDetector *d)
{
    d->a_fast_pending = false;
    d->a_pending_peak = 0.0f;
}

static void a_accept_press(ButtonDetector *d, float peak)
{
    d->a_peak = peak;
    d->a_after_release = false;
    d->a_valley_valid = false;
    a_clear_fast_pending(d);
}

static int history_get(ButtonDetector *d, int frames_ago)
{
    if (d->history_count <= 0)
    {
        return 0;
    }

    int available_ago = frames_ago;
    if (available_ago > d->history_count - 1)
    {
        available_ago = d->history_count - 1;
    }

    int index = d->history_idx - 1 - available_ago;
    while (index < 0)
    {
        index += DETECTOR_HISTORY_SIZE;
    }

    return d->history[index];
}

static void history_push(ButtonDetector *d, int value)
{
    d->history[d->history_idx] = value;
    d->history_idx = (d->history_idx + 1) % DETECTOR_HISTORY_SIZE;

    if (d->history_count < DETECTOR_HISTORY_SIZE)
    {
        d->history_count++;
    }
}

void detector_reset(ButtonDetector *d)
{
    memset(d, 0, sizeof(*d));
}

DETECTOR_SIZE_OPTIMIZED
bool detector_process_frame(ButtonDetector *d,
                            uint8_t phys_ch,
                            int current_val,
                            int setup_raw)
{
    char block = detector_get_block(phys_ch);
    int diff = current_val - setup_raw;
    int diff_deriv = (d->history_count > 0)
        ? current_val - history_get(d, 0)
        : 0;
    bool on = false;

    if (block == 'A')
    {
        float raw = (float)current_val;
        int deriv = diff_deriv;

        if (!d->a_baseline_initialized)
        {
            d->a_baseline = (float)setup_raw;
            d->a_baseline_initialized = true;
            d->a_peak = 0.0f;
            d->a_after_release = false;
            d->a_valley_raw = raw;
            d->a_valley_valid = true;
            a_clear_fast_pending(d);
        }

        float signal = raw - d->a_baseline;
        int edge_on = DETECTOR_A_EDGE_ON;
        int large_on = max_int(edge_on + 1, DETECTOR_A_LARGE_ON);

        if (d->is_pressed)
        {
            if (signal > d->a_peak)
            {
                d->a_peak = signal;
            }

            float peak_drop = d->a_peak - signal;
            float required_drop = max_float(
                (float)DETECTOR_A_RELEASE_MIN_DROP,
                d->a_peak * DETECTOR_A_RELEASE_DROP_RATIO);
            bool gesture_release =
                (deriv <= DETECTOR_A_RELEASE_DERIV) &&
                (peak_drop >= required_drop) &&
                (signal <= d->a_peak * DETECTOR_A_RELEASE_PEAK_RATIO);
            bool clean_release = signal <= DETECTOR_A_CLEAN_RELEASE;

            if (clean_release || gesture_release)
            {
                on = false;
                d->a_after_release = true;
                d->a_valley_raw = raw;
                d->a_valley_valid = true;
                d->a_peak = 0.0f;
                a_clear_fast_pending(d);
            }
            else
            {
                on = true;
            }
        }
        else
        {
            if (!d->a_valley_valid)
            {
                d->a_valley_raw = raw;
                d->a_valley_valid = true;
            }
            else if (raw < d->a_valley_raw)
            {
                d->a_valley_raw = raw;
            }

            float rise_from_valley = raw - d->a_valley_raw;
            bool can_track_baseline =
                (fabsf(signal) <= DETECTOR_A_BASELINE_TRACK_RANGE) &&
                (fabsf((float)deriv) <= DETECTOR_A_BASELINE_QUIET_DERIV) &&
                !d->a_fast_pending;

            if (can_track_baseline)
            {
                float baseline_step = signal * DETECTOR_A_BASELINE_ALPHA;
                float max_step = max_float(0.0f, DETECTOR_A_BASELINE_MAX_STEP);

                if (baseline_step > max_step)
                {
                    baseline_step = max_step;
                }
                else if (baseline_step < -max_step)
                {
                    baseline_step = -max_step;
                }

                d->a_baseline += baseline_step;
                signal = raw - d->a_baseline;
            }

            if (d->a_after_release &&
                (fabsf(signal) <= DETECTOR_A_BASELINE_TRACK_RANGE) &&
                (fabsf((float)deriv) <= DETECTOR_A_BASELINE_QUIET_DERIV))
            {
                d->a_after_release = false;
                d->a_valley_raw = raw;
                d->a_valley_valid = true;
                rise_from_valley = 0.0f;
            }

            bool fast_repress =
                d->a_after_release &&
                (signal >= DETECTOR_A_REPRESS_SIGNAL_MIN) &&
                (rise_from_valley >= DETECTOR_A_REPRESS_RISE) &&
                (deriv >= DETECTOR_A_REPRESS_DERIV);
            bool slow_repress =
                d->a_after_release &&
                (signal >= DETECTOR_A_REPRESS_SIGNAL_MIN) &&
                (rise_from_valley >= DETECTOR_A_REPRESS_SLOW_RISE) &&
                (deriv >= DETECTOR_A_EDGE_MIN_DERIV);

            if (fast_repress || slow_repress)
            {
                on = true;
                a_accept_press(
                    d,
                    max_float(signal, (float)DETECTOR_A_REPRESS_SIGNAL_MIN));
            }
            else if (d->a_after_release)
            {
                on = false;
                a_clear_fast_pending(d);
            }
            else if (d->a_fast_pending)
            {
                if (signal > d->a_pending_peak)
                {
                    d->a_pending_peak = signal;
                }

                if ((signal >= large_on) && (deriv >= 0))
                {
                    on = true;
                    a_accept_press(d, max_float(signal, d->a_pending_peak));
                }
                else if ((deriv <= DETECTOR_A_PENDING_SETTLE_DERIV) &&
                         (d->a_pending_peak >= DETECTOR_A_SHORT_TAP_PEAK))
                {
                    on = true;
                    a_accept_press(d, max_float(signal, d->a_pending_peak));
                }
                else if ((signal < DETECTOR_A_FAST_PENDING_CANCEL) ||
                         ((deriv < 0) &&
                          (d->a_pending_peak < DETECTOR_A_SHORT_TAP_PEAK)))
                {
                    on = false;
                    a_clear_fast_pending(d);
                }
                else
                {
                    on = false;
                }
            }
            else
            {
                if ((signal >= large_on) && (deriv >= 0))
                {
                    on = true;
                }
                else if ((signal >= edge_on) &&
                         (deriv >= DETECTOR_A_EDGE_MIN_DERIV))
                {
                    if (deriv >= DETECTOR_A_FAST_RISE_DERIV)
                    {
                        d->a_fast_pending = true;
                        d->a_pending_peak = signal;
                        on = false;
                    }
                    else
                    {
                        on = true;
                    }
                }
                else
                {
                    on = false;
                }

                if (on)
                {
                    a_accept_press(d, max_float(signal, (float)edge_on));
                }
            }
        }
    }
    else if (block == 'C')
    {
        int c_diff = DETECTOR_C_DIFF_THRESHOLD;
        int c_deriv_t = DETECTOR_C_DERIV_THRESHOLD;
        int c_deriv_r = DETECTOR_C_DERIV_RELEASE;
        int c_diff_r = DETECTOR_C_DIFF_RELEASE;

        if ((diff > c_diff) || (diff_deriv > c_deriv_t))
        {
            if ((diff_deriv < c_deriv_r) && (diff < c_diff * 1.5f))
            {
                on = false;
            }
            else
            {
                on = true;
            }
        }
        else if (diff < c_diff_r)
        {
            on = false;
        }
    }
    else
    {
        int default_diff = DETECTOR_E_DIFF_THRESHOLD;
        int default_deriv_r = DETECTOR_E_DERIV_RELEASE;

        if (block == 'B')
        {
            default_diff = DETECTOR_B_DIFF_THRESHOLD;
            default_deriv_r = DETECTOR_B_DERIV_RELEASE;
        }
        else if (block == 'D')
        {
            default_diff = DETECTOR_D_DIFF_THRESHOLD;
            default_deriv_r = DETECTOR_D_DERIV_RELEASE;
        }

        int last_diff = history_get(d, 0) - setup_raw;

        if (diff > default_diff * 1.5f)
        {
            on = true;
        }
        else if ((diff > default_diff) &&
                 (last_diff > default_diff / 2))
        {
            on = true;
        }
        else if (d->is_pressed && (diff > default_diff))
        {
            on = true;
        }

        if ((diff_deriv < default_deriv_r) &&
            (diff < default_diff * 1.5f))
        {
            on = false;
        }

        if (diff <= default_diff / 2)
        {
            on = false;
        }
    }

    d->is_pressed = on;
    history_push(d, current_val);
    return on;
}
