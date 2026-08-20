#include "button_detector.h"
#include "button_detector_config.h"

#include <math.h>
#include <string.h>

#define DETECTOR_HISTORY_SIZE 16

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static float max_float(float a, float b)
{
    return (a > b) ? a : b;
}

static int history_get(ButtonDetector *d, int frames_ago)
{
    if (d->history_count <= 0) return 0;

    int avail = frames_ago;
    if (avail > d->history_count - 1) avail = d->history_count - 1;

    int idx = d->history_idx - 1 - avail;
    while (idx < 0) idx += DETECTOR_HISTORY_SIZE;
    return d->history[idx];
}

static void history_push(ButtonDetector *d, int value)
{
    d->history[d->history_idx] = value;
    d->history_idx = (d->history_idx + 1) % DETECTOR_HISTORY_SIZE;
    if (d->history_count < DETECTOR_HISTORY_SIZE) d->history_count++;
}

/* ------------------------------------------------------------------ */
/* A-zone pending helpers                                             */
/* ------------------------------------------------------------------ */

static void a_start_pending(ButtonDetector *d, char kind,
                            float signal, float deriv)
{
    d->a_fast_pending = true;
    d->a_pending_kind = kind;
    d->a_pending_start_signal = signal;
    d->a_pending_peak = signal;
    d->a_pending_frames = 0;
    d->a_pending_max_deriv = (deriv > 0.0f) ? deriv : 0.0f;
    d->a_pending_last_deriv = deriv;
}

static void a_clear_pending(ButtonDetector *d)
{
    d->a_fast_pending = false;
    d->a_pending_kind = 0;
    d->a_pending_start_signal = 0.0f;
    d->a_pending_peak = 0.0f;
    d->a_pending_frames = 0;
    d->a_pending_max_deriv = 0.0f;
    d->a_pending_last_deriv = 0.0f;
}

static void a_clear_side_edge(ButtonDetector *d)
{
    d->a_side_edge_rise_start_valid = false;
    d->a_side_edge_rise_start = 0.0f;
    d->a_side_edge_peak = 0.0f;
    d->a_side_edge_stable_frames = 0;
}

static void a_update_side_edge(ButtonDetector *d, float setup_signal, float deriv)
{
    if (d->a_after_release ||
        d->frames_seen <= DETECTOR_A_SIDE_EDGE_ARM_FRAMES ||
        setup_signal < DETECTOR_A_SIDE_EDGE_RESET_ON ||
        setup_signal > DETECTOR_A_SIDE_EDGE_MAX_ON ||
        deriv < DETECTOR_A_SIDE_EDGE_FALL_DERIV)
    {
        a_clear_side_edge(d);
        return;
    }

    if (!d->a_side_edge_rise_start_valid) {
        d->a_side_edge_rise_start = setup_signal;
        d->a_side_edge_peak = setup_signal;
        d->a_side_edge_stable_frames = 0;
        d->a_side_edge_rise_start_valid = true;
    } else {
        if (setup_signal < d->a_side_edge_rise_start)
            d->a_side_edge_rise_start = setup_signal;
        if (setup_signal > d->a_side_edge_peak)
            d->a_side_edge_peak = setup_signal;
    }

    if (setup_signal >= DETECTOR_A_SIDE_EDGE_ON &&
        fabsf(deriv) <= DETECTOR_A_SIDE_EDGE_MAX_DERIV)
    {
        d->a_side_edge_stable_frames++;
    } else {
        d->a_side_edge_stable_frames = 0;
    }
}

static void a_confirm_press(ButtonDetector *d, float signal)
{
    d->is_pressed = true;
    d->a_peak = max_float(signal, (float)DETECTOR_A_REPRESS_SIGNAL_MIN);
    d->a_after_release = false;
    d->a_valley_valid = false;
    d->a_quick_repress_armed = false;
    d->a_frames_after_release = 0;
    d->a_settled_after_release_frames = 0;
    d->a_soft_edge_frames = 0;
    a_clear_side_edge(d);
    a_clear_pending(d);
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

void detector_reset(ButtonDetector *d)
{
    memset(d, 0, sizeof(*d));
}

bool detector_process_frame(ButtonDetector *d,
                            uint8_t phys_ch,
                            int current_val,
                            int setup_raw)
{
    char block = detector_get_block(phys_ch);
    bool on = false;

    int deriv = (d->history_count > 0)
        ? current_val - history_get(d, 0)
        : 0;

    /* ================================================================
     *  A-zone
     * ================================================================ */
    if (block == 'A') {
        d->frames_seen++;

        float raw = (float)current_val;
        float d_val = (float)deriv;

        /* --- baseline init --- */
        if (!d->a_baseline_initialized) {
            d->a_baseline = (float)setup_raw;
            d->a_setup_baseline = d->a_baseline;
            d->a_baseline_initialized = true;
            d->a_peak = 0.0f;
            d->a_after_release = false;
            d->a_valley_raw = raw;
            d->a_valley_valid = true;
            a_clear_pending(d);
            a_clear_side_edge(d);
        }

        float signal = raw - d->a_baseline;
        float setup_signal = raw - d->a_setup_baseline;

        /* ============================================================
         *  PRESSED
         * ============================================================ */
        if (d->is_pressed) {
            if (signal > d->a_peak) d->a_peak = signal;

            float peak_drop = d->a_peak - signal;
            float required_drop = max_float(
                (float)DETECTOR_A_RELEASE_MIN_DROP,
                d->a_peak * DETECTOR_A_RELEASE_DROP_RATIO);

            bool gesture_release =
                (d_val <= (float)DETECTOR_A_RELEASE_DERIV) &&
                (peak_drop >= required_drop) &&
                (signal <= d->a_peak * DETECTOR_A_RELEASE_PEAK_RATIO);

            bool fast_release =
                (d_val <= (float)DETECTOR_A_FAST_RELEASE_DERIV) &&
                (peak_drop >= (float)DETECTOR_A_FAST_RELEASE_MIN_DROP) &&
                (signal <= (float)DETECTOR_A_FAST_RELEASE_SIGNAL);

            bool slide_release =
                (d_val <= (float)DETECTOR_A_SLIDE_RELEASE_DERIV) &&
                (peak_drop >= max_float(
                    (float)DETECTOR_A_RELEASE_MIN_DROP,
                    d->a_peak * DETECTOR_A_SLIDE_RELEASE_DROP_RATIO)) &&
                (signal <= d->a_peak * DETECTOR_A_SLIDE_RELEASE_PEAK_RATIO);

            bool clean_release =
                (signal <= (float)DETECTOR_A_CLEAN_RELEASE);

            if (clean_release || gesture_release ||
                fast_release || slide_release) {
                on = false;
                d->a_after_release = true;
                d->a_valley_raw = raw;
                d->a_valley_valid = true;
                d->a_quick_repress_armed = !clean_release;
                d->a_frames_after_release = 0;
                d->a_settled_after_release_frames = 0;
                d->a_peak = 0.0f;
                d->a_soft_edge_frames = 0;
                a_clear_side_edge(d);
                a_clear_pending(d);
            } else {
                on = true;
            }
        }
        /* ============================================================
         *  NOT PRESSED
         * ============================================================ */
        else {
            /* --- valley tracking --- */
            if (!d->a_valley_valid) {
                d->a_valley_raw = raw;
                d->a_valley_valid = true;
            }
            if (raw < d->a_valley_raw) {
                d->a_valley_raw = raw;
            }
            float rise_from_valley = raw - d->a_valley_raw;

            /* --- after-release frame counting --- */
            if (d->a_after_release) {
                d->a_frames_after_release++;

                bool settled =
                    (signal <= (float)DETECTOR_A_CLEAN_RELEASE) ||
                    ((fabsf(signal) <= (float)DETECTOR_A_BASE_TRACK_RANGE) &&
                     (fabsf(d_val) <= (float)DETECTOR_A_BASE_QUIET_DERIV));

                if (settled) {
                    d->a_settled_after_release_frames++;
                } else {
                    d->a_settled_after_release_frames = 0;
                }

                if (d->a_frames_after_release >
                        DETECTOR_A_REPRESS_WINDOW_FRAMES ||
                    d->a_settled_after_release_frames >=
                        DETECTOR_A_REPRESS_SETTLE_FRAMES ||
                    d_val < -40.0f) {
                    d->a_quick_repress_armed = false;
                }
            }

            /* --- baseline update (quiet zone only) --- */
            if (fabsf(signal) <= (float)DETECTOR_A_BASE_TRACK_RANGE &&
                fabsf(d_val) <= (float)DETECTOR_A_BASE_QUIET_DERIV) {
                float step = signal * DETECTOR_A_BASE_ALPHA;
                if (step > DETECTOR_A_BASE_MAX_STEP)
                    step = DETECTOR_A_BASE_MAX_STEP;
                else if (step < -DETECTOR_A_BASE_MAX_STEP)
                    step = -DETECTOR_A_BASE_MAX_STEP;
                d->a_baseline += step;
                signal = raw - d->a_baseline;
            }

            /* --- quick re-press --- */
            bool quick_repress =
                d->a_after_release &&
                d->a_quick_repress_armed &&
                d->a_frames_after_release <=
                    DETECTOR_A_REPRESS_WINDOW_FRAMES &&
                signal >= (float)DETECTOR_A_REPRESS_SIGNAL_MIN &&
                rise_from_valley >= (float)DETECTOR_A_REPRESS_RISE &&
                d_val >= (float)DETECTOR_A_REPRESS_DERIV;

            if (quick_repress) {
                on = true;
                a_confirm_press(d, signal);
            }
            /* --- solid high: direct confirm --- */
            else if (signal >= (float)DETECTOR_A_EDGE_SOLID_ON &&
                     d_val >= -10.0f &&
                     d_val <= (float)DETECTOR_A_LARGE_CONFIRM_DERIV) {
                on = true;
                a_confirm_press(d, signal);
            }
            /* --- pending state machine --- */
            else if (d->a_fast_pending) {
                d->a_pending_frames++;

                float previous_pending_peak = d->a_pending_peak;
                float previous_pending_max_deriv = d->a_pending_max_deriv;
                float previous_pending_deriv = d->a_pending_last_deriv;

                if (signal > d->a_pending_peak)
                    d->a_pending_peak = signal;
                if (d_val > d->a_pending_max_deriv)
                    d->a_pending_max_deriv = d_val;
                d->a_pending_last_deriv = d_val;

                /* fast_confirm */
                bool fast_confirm =
                    (d->a_pending_kind == 'f') &&
                    (d->a_pending_frames >= 1) &&
                    (signal >= (float)DETECTOR_A_APPROACH_CONFIRM_ON) &&
                    (d_val >= -10.0f) &&
                    (d_val <= max_float(120.0f,
                        d->a_pending_max_deriv *
                            DETECTOR_A_APPROACH_DERIV_RATIO));

                /* fast_medium_confirm */
                bool fast_medium_confirm =
                    (d->a_pending_kind == 'f') &&
                    (d->a_pending_frames >= 1) &&
                    (d->a_pending_max_deriv >= (float)DETECTOR_A_FAST_MEDIUM_PEAK_DERIV) &&
                    (signal >= (float)DETECTOR_A_FAST_MEDIUM_ON) &&
                    (signal <= (float)DETECTOR_A_FAST_MEDIUM_MAX_ON) &&
                    (d_val >= (float)DETECTOR_A_FAST_MEDIUM_MIN_DERIV) &&
                    (d_val <= (float)DETECTOR_A_FAST_MEDIUM_MAX_DERIV) &&
                    ((signal - previous_pending_peak >= (float)DETECTOR_A_FAST_MEDIUM_RISE) ||
                     (signal - d->a_pending_start_signal >= (float)DETECTOR_A_FAST_MEDIUM_TOTAL_RISE));

                /* fast_glance_confirm */
                bool fast_glance_confirm =
                    d->a_after_release &&
                    (d->a_pending_kind == 'f') &&
                    (d->a_pending_frames >= 1) &&
                    (d->a_pending_frames <= DETECTOR_A_FAST_GLANCE_MAX_FRAMES) &&
                    (previous_pending_max_deriv >= (float)DETECTOR_A_FAST_GLANCE_PEAK_DERIV) &&
                    (signal >= (float)DETECTOR_A_FAST_GLANCE_ON) &&
                    (signal - d->a_pending_start_signal >= (float)DETECTOR_A_FAST_GLANCE_RISE) &&
                    (d_val >= (float)DETECTOR_A_FAST_GLANCE_DERIV) &&
                    (d_val <= previous_pending_max_deriv * DETECTOR_A_GLANCE_DERIV_RATIO);

                /* fast_sweep_confirm */
                bool fast_sweep_confirm =
                    d->a_after_release &&
                    (d->a_pending_kind == 'f') &&
                    (d->a_pending_frames >= 2) &&
                    (d->a_pending_frames <= DETECTOR_A_FAST_SWEEP_MAX_FRAMES) &&
                    (rise_from_valley >= (float)DETECTOR_A_FAST_SWEEP_REARM_RISE) &&
                    (previous_pending_max_deriv >= (float)DETECTOR_A_FAST_SWEEP_PEAK_DERIV) &&
                    (previous_pending_peak >= (float)DETECTOR_A_FAST_SWEEP_ON) &&
                    (signal >= (float)DETECTOR_A_FAST_SWEEP_FALL_SIGNAL) &&
                    (d_val <= (float)DETECTOR_A_FAST_SWEEP_FALL_DERIV);

                /* fast_sweep_accelerate */
                bool fast_sweep_accelerate =
                    (d->a_pending_kind == 'f') &&
                    (d->a_pending_frames >= 1) &&
                    (previous_pending_deriv >= (float)DETECTOR_A_FAST_SWEEP_ACCELERATE_PEAK_DERIV) &&
                    (d_val >= 0) &&
                    (d_val <= previous_pending_deriv * DETECTOR_A_FAST_SWEEP_ACCELERATE_DERIV_RATIO) &&
                    (signal >= (d->a_after_release
                        ? (float)DETECTOR_A_POST_RELEASE_FAST_SWEEP_ACCELERATE_ON
                        : (float)DETECTOR_A_FAST_SWEEP_ACCELERATE_ON));

                /* edge_confirm */
                bool edge_confirm =
                    (d->a_pending_kind == 'e') &&
                    (d->a_pending_frames >= DETECTOR_A_EDGE_CONFIRM_FRAMES) &&
                    (signal >= (float)DETECTOR_A_EDGE_SOLID_ON) &&
                    (d_val >= -10.0f) &&
                    (d_val <= max_float(
                        (float)DETECTOR_A_LARGE_CONFIRM_DERIV,
                        d->a_pending_max_deriv *
                            DETECTOR_A_APPROACH_DERIV_RATIO));

                /* edge_glance_confirm */
                bool edge_glance_confirm =
                    d->a_after_release &&
                    (d->a_pending_kind == 'e') &&
                    (d->a_pending_frames >= 1) &&
                    (d->a_pending_frames <= DETECTOR_A_EDGE_GLANCE_MAX_FRAMES) &&
                    (previous_pending_max_deriv >= (float)DETECTOR_A_EDGE_GLANCE_PEAK_DERIV) &&
                    (signal >= (float)DETECTOR_A_EDGE_GLANCE_ON) &&
                    (signal - d->a_pending_start_signal >= (float)DETECTOR_A_EDGE_GLANCE_RISE) &&
                    (d_val >= (float)DETECTOR_A_EDGE_GLANCE_DERIV) &&
                    (d_val <= previous_pending_max_deriv * DETECTOR_A_GLANCE_DERIV_RATIO);

                /* edge_ramp_confirm */
                bool edge_ramp_confirm =
                    d->a_after_release &&
                    (d->a_pending_kind == 'e') &&
                    (d->a_pending_frames >= DETECTOR_A_EDGE_RAMP_MIN_FRAMES) &&
                    (previous_pending_max_deriv >= (float)DETECTOR_A_EDGE_RAMP_PEAK_DERIV) &&
                    (previous_pending_max_deriv <= (float)DETECTOR_A_EDGE_RAMP_MAX_PEAK_DERIV) &&
                    (setup_signal >= (float)DETECTOR_A_EDGE_RAMP_ON) &&
                    (setup_signal <= (float)DETECTOR_A_EDGE_RAMP_MAX_ON) &&
                    (d_val >= 0) &&
                    (d_val <= (float)DETECTOR_A_EDGE_RAMP_MAX_DERIV) &&
                    (d_val <= previous_pending_max_deriv * DETECTOR_A_EDGE_RAMP_DERIV_RATIO);

                /* side_edge_confirm */
                bool side_edge_confirm =
                    (d->a_pending_kind == 's') &&
                    (d->a_pending_frames >= 1) &&
                    (d->a_pending_peak >= DETECTOR_A_SIDE_EDGE_ON) &&
                    (DETECTOR_A_SIDE_EDGE_ON <= setup_signal) &&
                    (setup_signal <= DETECTOR_A_SIDE_EDGE_MAX_ON) &&
                    ((setup_signal <= previous_pending_peak) || (d_val <= 0)) &&
                    ((fabsf(d_val) <= DETECTOR_A_SIDE_EDGE_MAX_DERIV) ||
                     ((d_val >= DETECTOR_A_SIDE_EDGE_FALL_DERIV) && (d_val <= 0)));

                /* side_micro_confirm */
                bool side_micro_confirm =
                    (d->a_pending_kind == 'm') &&
                    (d->a_pending_frames >= 1) &&
                    (d->a_pending_peak <= DETECTOR_A_SIDE_EDGE_MICRO_MAX_ON) &&
                    (DETECTOR_A_SIDE_EDGE_ON <= setup_signal) &&
                    (setup_signal <= DETECTOR_A_SIDE_EDGE_MICRO_MAX_ON) &&
                    ((fabsf(d_val) <= DETECTOR_A_SIDE_EDGE_MAX_DERIV) ||
                     ((d_val >= DETECTOR_A_SIDE_EDGE_FALL_DERIV) && (d_val <= 0)));

                /* edge_fall_confirm */
                float edge_tap_peak =
                    (d->a_pending_kind == 'e')
                        ? (float)DETECTOR_A_EDGE_TAP_PEAK
                        : (float)DETECTOR_A_SHORT_TAP_PEAK;

                bool edge_fall_confirm =
                    (d->a_pending_kind == 'e') &&
                    (d->a_pending_frames >= 1) &&
                    (d->a_pending_peak >= edge_tap_peak) &&
                    (signal >= (float)DETECTOR_A_EDGE_FALL_SIGNAL) &&
                    (d_val >= (float)DETECTOR_A_EDGE_FALL_DERIV) &&
                    (d_val <= 0.0f);

                /* impulse_confirm */
                bool impulse_confirm =
                    (d->a_pending_kind == 'i') &&
                    (d->a_pending_frames >= 1) &&
                    ((signal - previous_pending_peak >= (float)DETECTOR_A_IMPULSE_CONFIRM_RISE) ||
                     (signal >= (float)DETECTOR_A_IMPULSE_CONFIRM_ON) ||
                     (d_val >= (float)DETECTOR_A_IMPULSE_CONFIRM_DERIV));

                if (fast_confirm || fast_medium_confirm || fast_glance_confirm ||
                    fast_sweep_confirm || fast_sweep_accelerate ||
                    edge_confirm || edge_glance_confirm || edge_ramp_confirm ||
                    side_edge_confirm || side_micro_confirm ||
                    edge_fall_confirm || impulse_confirm)
                {
                    on = true;
                    a_confirm_press(d, max_float(d->a_pending_peak, signal));
                }
                else if (d_val >= -40.0f && d_val <= 0.0f) {
                    if (d->a_pending_peak >= edge_tap_peak) {
                        on = true;
                        a_confirm_press(d, max_float(d->a_pending_peak, signal));
                    } else {
                        on = false;
                    }
                    a_clear_pending(d);
                }
                else if (d_val < (float)DETECTOR_A_EDGE_FALL_DERIV) {
                    on = false;
                    a_clear_pending(d);
                }
                else if (signal < (float)DETECTOR_A_FAST_PENDING_CANCEL) {
                    on = false;
                    a_clear_pending(d);
                }
                else {
                    on = false;
                }
            }
            /* --- entry to pending --- */
            else {
                a_update_side_edge(d, setup_signal, d_val);

                bool edge_candidate =
                    (signal >= (float)DETECTOR_A_EDGE_CANDIDATE_ON) &&
                    (d_val >= (float)DETECTOR_A_EDGE_MIN_DERIV);

                bool impulse_candidate =
                    (signal >= (float)DETECTOR_A_IMPULSE_ON) &&
                    (d_val >= (float)DETECTOR_A_IMPULSE_DERIV);

                bool release_tail =
                    d->a_after_release &&
                    (d_val <= 0) &&
                    (rise_from_valley < (float)DETECTOR_A_RELEASE_TAIL_REARM_RISE);

                bool falling_edge_candidate =
                    !release_tail &&
                    (signal >= (float)DETECTOR_A_EDGE_TAP_PEAK) &&
                    (d_val >= (float)DETECTOR_A_EDGE_FALL_DERIV) &&
                    (d_val <= 0.0f);

                bool soft_edge_hold =
                    !d->a_after_release &&
                    (setup_signal >= (float)DETECTOR_A_SOFT_EDGE_ON) &&
                    (setup_signal <= DETECTOR_A_SOFT_EDGE_MAX_ON) &&
                    (fabsf(d_val) <= (float)DETECTOR_A_SOFT_EDGE_MAX_DERIV);

                bool has_side_edge_rise = d->a_side_edge_rise_start_valid;

                bool micro_side_edge =
                    has_side_edge_rise &&
                    (d->a_setup_baseline >= (float)DETECTOR_A_SIDE_EDGE_BASELINE_MIN) &&
                    (d->a_side_edge_peak <= (float)DETECTOR_A_SIDE_EDGE_MICRO_MAX_ON) &&
                    (d->a_side_edge_rise_start <= (float)DETECTOR_A_SIDE_EDGE_MICRO_START_MAX_ON) &&
                    (d->a_side_edge_peak - d->a_side_edge_rise_start >= (float)DETECTOR_A_SIDE_EDGE_MICRO_MIN_RISE);

                bool high_side_edge =
                    has_side_edge_rise &&
                    (d->a_setup_baseline >= (float)DETECTOR_A_SIDE_EDGE_BASELINE_MIN) &&
                    (d->a_side_edge_peak >= (float)DETECTOR_A_SIDE_EDGE_HIGH_ON) &&
                    (d->a_side_edge_peak - d->a_side_edge_rise_start >= (float)DETECTOR_A_SIDE_EDGE_HIGH_MIN_RISE);

                bool side_edge_candidate =
                    !d->a_after_release &&
                    has_side_edge_rise &&
                    (micro_side_edge || high_side_edge) &&
                    (d->a_side_edge_stable_frames >= DETECTOR_A_SIDE_EDGE_MIN_STABLE_FRAMES) &&
                    (DETECTOR_A_SIDE_EDGE_ON <= setup_signal) &&
                    (setup_signal <= DETECTOR_A_SIDE_EDGE_MAX_ON) &&
                    (fabsf(d_val) <= DETECTOR_A_SIDE_EDGE_MAX_DERIV);

                if (impulse_candidate || edge_candidate || falling_edge_candidate) {
                    d->a_soft_edge_frames = 0;
                    a_clear_side_edge(d);
                    char kind;
                    if (impulse_candidate)
                        kind = 'i';
                    else if (signal >= (float)DETECTOR_A_LARGE_ON || d_val >= (float)DETECTOR_A_FAST_RISE_DERIV)
                        kind = 'f';
                    else
                        kind = 'e';
                    a_start_pending(d, kind, signal, d_val);
                    on = false;
                }
                else if (side_edge_candidate) {
                    d->a_soft_edge_frames = 0;
                    float side_signal = max_float(signal, setup_signal);
                    char side_kind = (micro_side_edge && !high_side_edge) ? 'm' : 's';
                    a_start_pending(d, side_kind, side_signal, d_val);
                    on = false;
                }
                else if (soft_edge_hold) {
                    d->a_soft_edge_frames++;
                    if (d->a_soft_edge_frames >= DETECTOR_A_SOFT_EDGE_CONFIRM_FRAMES) {
                        on = true;
                        a_confirm_press(d, max_float(signal, setup_signal));
                    } else {
                        on = false;
                    }
                }
                else {
                    d->a_soft_edge_frames = 0;
                    on = false;
                }
            }
        }
    }
    /* ================================================================
     *  C-zone
     * ================================================================ */
    else if (block == 'C') {
        int diff = current_val - setup_raw;

        if (diff > 40 || deriv > 35)
            on = true;
        if (deriv < -20 || diff < 15)
            on = false;
    }
    /* ================================================================
     *  B-zone
     * ================================================================ */
    else if (block == 'B') {
        int diff = current_val - setup_raw;

        if (diff > 16) on = true;
        if (deriv < -15) on = false;
    }
    /* ================================================================
     *  D-zone
     * ================================================================ */
    else if (block == 'D') {
        int diff = current_val - setup_raw;

        if (diff > 20) on = true;
        if (deriv < -20) on = false;
    }
    /* ================================================================
     *  E-zone
     * ================================================================ */
    else if (block == 'E') {
        int diff = current_val - setup_raw;

        if (diff > 10) on = true;
        if (deriv < -16) on = false;
    }
    /* ================================================================
     *  Unknown block
     * ================================================================ */
    else {
        on = false;
    }

    d->is_pressed = on;
    history_push(d, current_val);
    return on;
}
