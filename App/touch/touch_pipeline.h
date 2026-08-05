#ifndef __TOUCH_PIPELINE_H__
#define __TOUCH_PIPELINE_H__

#include <stdbool.h>
#include <stdint.h>

/* Initialize the detector pipeline and begin software baseline calibration. */
void touch_pipeline_init(void);

/*
 * Update the two-bit PSoC availability masks.
 *
 * A disconnected device always forces all regions mapped from its 17
 * channels. forced_mask can additionally keep a connected/recovering device
 * forced until its new software baseline is ready.
 */
void touch_pipeline_set_device_masks(uint8_t connected_mask,
                                     uint8_t forced_mask);

/* Report whether every currently connected device has a software baseline. */
bool touch_pipeline_is_ready(void);

/* Clear detector history without discarding baselines or forced outputs. */
void touch_pipeline_reset_detection_state(void);

/* Feed one PSoC frame: status byte followed by 17 little-endian channels. */
void touch_pipeline_feed(int psoc_index, const uint8_t *raw_35);

#endif /* __TOUCH_PIPELINE_H__ */
