#ifndef __TOUCH_PIPELINE_H__
#define __TOUCH_PIPELINE_H__

#include <stdbool.h>
#include <stdint.h>

/* Initialize the detector pipeline and begin software baseline calibration. */
void touch_pipeline_init(void);

/* Clear mapped output and detector history without discarding baselines. */
void touch_pipeline_reset_detection_state(void);

/* Feed one PSoC frame: status byte followed by 17 little-endian channels. */
void touch_pipeline_feed(int psoc_index, const uint8_t *raw_35);

#endif /* __TOUCH_PIPELINE_H__ */
