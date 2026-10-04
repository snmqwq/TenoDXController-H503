#ifndef __TOUCH_PIPELINE_H__
#define __TOUCH_PIPELINE_H__

#include <stdbool.h>
#include <stdint.h>

/*
 * Initialize the detector pipeline. Mirrors TouchHost SerialReader +
 * SignalPipeline/detector.py: per-board dedup -> IIR -> 100-frame warm-up ->
 * 100-frame setup_raw collection -> detector.py decision.
 */
void touch_pipeline_init(void);

/*
 * Update the two-bit PSoC availability masks.
 *
 * A disconnected device always forces all regions mapped from its 17
 * channels. forced_mask can additionally keep a connected/recovering device
 * forced until its new software baseline is ready. A newly connected device
 * restarts the whole pipeline calibration (warmup + baseline collection).
 */
void touch_pipeline_set_device_masks(uint8_t connected_mask,
                                     uint8_t forced_mask);

/* Report whether warmup and baseline collection have completed. */
bool touch_pipeline_is_ready(void);

/* Current merged touch region mask (bit N = game area N). */
uint64_t touch_pipeline_get_touch_bits(void);

/*
 * Commit a batch of packets. Each packet is a validity mask followed by two
 * 34-byte PSoC payloads in board order (0 first). A board whose validity bit
 * is clear is absent, not a zero sample. This matches the 70B packet batch
 * TouchHost drains before spending each board's 180 Hz budget newest-first.
 */
#define TOUCH_PIPELINE_PACKET_MASK_BYTES 1U
#define TOUCH_PIPELINE_PACKET_PAYLOAD_BYTES 68U
#define TOUCH_PIPELINE_PACKET_BYTES \
    (TOUCH_PIPELINE_PACKET_MASK_BYTES + TOUCH_PIPELINE_PACKET_PAYLOAD_BYTES)

void touch_pipeline_commit_batch(const uint8_t *packets,
                                 uint16_t packet_count);

/* Commit one packet in the format above. */
void touch_pipeline_commit_packet(const uint8_t *packet);

#if defined(TOUCH_PIPELINE_TEST_PROBES)
/*
 * Read-only snapshots used by the host-side three-way parity driver. The
 * production firmware never defines TOUCH_PIPELINE_TEST_PROBES.
 */
typedef struct {
    int latest_value;
    double setup_raw;
    bool pressed;
    bool baseline_valid;
    double baseline;
    double effective_diff;
    double deriv_eff;
    double deriv2_eff;
    double algo_deriv;
    double algo_deriv2;
    int fsm_state;
    double touched_effective_diff;
    double released_min_diff;
    bool released_min_valid;
    int large_diff_latched;
    double pre_judge_deriv;
} TouchPipelineSnapshot;

bool touch_pipeline_snapshot(uint8_t channel, TouchPipelineSnapshot *snapshot);
#endif

#endif /* __TOUCH_PIPELINE_H__ */
