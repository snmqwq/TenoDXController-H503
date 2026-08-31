#ifndef __TOUCH_PIPELINE_H__
#define __TOUCH_PIPELINE_H__

#include <stdbool.h>
#include <stdint.h>

/*
 * Initialize the detector pipeline. Mirrors run3 SignalPipeline:
 * IIR 滤波 -> 100 帧预热 -> 100 帧 setup_raw 基线收集 -> 正常检测。
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

/* Feed one PSoC frame: status byte followed by 17 little-endian channels. */
void touch_pipeline_feed(int psoc_index, const uint8_t *raw_35);

#endif /* __TOUCH_PIPELINE_H__ */
