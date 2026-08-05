#include "button_detector.h"
#include "button_detector_config.h"

#include <string.h>

static const int a_edge_deriv_min[DETECTOR_A_EDGE_TRIGGER_COUNT] =
    DETECTOR_A_EDGE_DERIV_MIN_VALUES;
static const int a_edge_deriv_max[DETECTOR_A_EDGE_TRIGGER_COUNT] =
    DETECTOR_A_EDGE_DERIV_MAX_VALUES;
static const int a_edge_diff_min[DETECTOR_A_EDGE_TRIGGER_COUNT] =
    DETECTOR_A_EDGE_DIFF_MIN_VALUES;
static const int a_edge_diff_max[DETECTOR_A_EDGE_TRIGGER_COUNT] =
    DETECTOR_A_EDGE_DIFF_MAX_VALUES;

static int history_get(ButtonDetector *d, int frames_ago)
{
    if (!d->history_filled)
    {
        return d->history[0];
    }

    int index = (d->history_idx - 1 - frames_ago + 16) % 16;
    return d->history[index];
}

static void history_push(ButtonDetector *d, int value)
{
    d->history[d->history_idx] = value;
    d->history_idx = (d->history_idx + 1) % 16;
    if (d->history_idx == 0)
    {
        d->history_filled = true;
    }
}

void detector_reset(ButtonDetector *d)
{
    memset(d, 0, sizeof(*d));
    d->diff_deriv_down_count = -1;
    d->active_edge_min_diff = 300;
}

bool detector_process_frame(ButtonDetector *d,
                            uint8_t phys_ch,
                            int current_val,
                            int setup_raw)
{
    char block = detector_get_block(phys_ch);
    int diff = current_val - setup_raw;
    int diff_deriv = current_val - history_get(d, 0);
    int diff_deriv_2 = current_val - history_get(d, 1);
    int diff_deriv_3 = current_val - history_get(d, 2);
    bool on = false;

    if (block == 'A')
    {
        int on_default_diff = DETECTOR_A_TRIGGER_SENSITIVITY;
        int matched_min_diff = d->active_edge_min_diff;
        bool is_fast_edge_strike = false;

        on = d->is_pressed;

        for (uint32_t i = 0U; i < DETECTOR_A_EDGE_TRIGGER_COUNT; i++)
        {
            bool deriv_matches =
                (diff_deriv >= a_edge_deriv_min[i]) &&
                (diff_deriv <= a_edge_deriv_max[i]);
            bool diff_matches =
                (diff >= a_edge_diff_min[i]) &&
                (diff <= a_edge_diff_max[i]);

            if (deriv_matches && diff_matches)
            {
                is_fast_edge_strike = true;
                matched_min_diff = a_edge_diff_min[i];
                break;
            }
        }

        if (is_fast_edge_strike)
        {
            d->edge_holding = true;
            d->active_edge_min_diff = matched_min_diff;
        }
        else if ((diff < d->active_edge_min_diff - 50) || !d->is_pressed)
        {
            d->edge_holding = false;
        }

        if ((diff > on_default_diff + 400) ||
            (diff < on_default_diff - 400))
        {
            d->up = 0;
        }

        int on_diff = on_default_diff;
        int last_diff = history_get(d, 0) - setup_raw;

        if ((last_diff < on_default_diff) && (diff >= on_default_diff))
        {
            d->up = 1;
        }
        else if ((last_diff >= on_default_diff) && (diff < on_default_diff))
        {
            d->up = -1;
        }

        switch (d->up)
        {
            case 1:
                on_diff = DETECTOR_A_HOLD_THRESHOLD;
                break;
            case -1:
                on_diff = 800;
                break;
            default:
                on_diff = on_default_diff;
                break;
        }

        if (d->edge_holding && (d->active_edge_min_diff < on_diff))
        {
            on_diff = d->active_edge_min_diff;
        }

        if (diff < 200)
        {
            d->lock_releasing = false;
        }

        if ((d->lock_releasing && (diff_deriv > 150) && (diff > on_diff)) ||
            (diff > on_diff * 1.5f) || is_fast_edge_strike)
        {
            d->lock_releasing = false;
        }

        if (((diff_deriv > 150) && (diff > on_diff)) ||
            (diff > on_diff * 1.5f) || is_fast_edge_strike)
        {
            d->lock_releasing = false;
            d->diff_deriv_down_count = 0;
        }

        if ((diff > on_diff) || is_fast_edge_strike)
        {
            if (d->diff_deriv_down_count > 0)
            {
                d->diff_deriv_down_count--;
            }
            else if (!d->lock_releasing)
            {
                bool hovering =
                    !d->is_pressed &&
                    (diff_deriv < DETECTOR_A_HOVER_SPEED_MAX) &&
                    (diff < DETECTOR_A_HOVER_DIFF_MAX) &&
                    !is_fast_edge_strike;

                if (!hovering)
                {
                    on = true;
                }
            }
        }
        else
        {
            if (d->diff_deriv_down_count > 0)
            {
                d->diff_deriv_down_count--;
            }
            if (d->is_pressed && (diff > 200))
            {
                d->lock_releasing = true;
            }
            on = false;
        }

        int deriv_down = DETECTOR_A_FAST_LIFT_SPEED;
        int last3_diff = history_get(d, 2) - setup_raw;

        if (last3_diff > 2700)
        {
            deriv_down = -400;
        }

        if ((diff_deriv < deriv_down) ||
            (diff_deriv_2 < deriv_down * 1.5f) ||
            (diff_deriv_3 < deriv_down * 2))
        {
            if ((diff_deriv < -800) ||
                (diff_deriv_2 < -1200) ||
                (diff_deriv_3 < -1500))
            {
                if (diff < 1000)
                {
                    on = false;
                    d->diff_deriv_down_count = 3;
                    if (diff > 500)
                    {
                        d->lock_releasing = true;
                    }
                }
            }
            else if (diff < DETECTOR_A_QUICK_RELEASE_LINE)
            {
                on = false;
                d->diff_deriv_down_count = 3;
                if (diff > 200)
                {
                    d->lock_releasing = true;
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
        int default_diff = 15;
        int default_deriv_r = -16;

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
        else if (block == 'E')
        {
            default_diff = DETECTOR_E_DIFF_THRESHOLD;
            default_deriv_r = DETECTOR_E_DERIV_RELEASE;
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
