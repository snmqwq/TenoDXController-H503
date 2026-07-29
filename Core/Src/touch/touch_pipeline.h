#ifndef __TOUCH_PIPELINE_H__
#define __TOUCH_PIPELINE_H__

#include <stdbool.h>
#include <stdint.h>

// 初始化判定管线 (校准阶段)
void touch_pipeline_init(void);

// 喂入一片 PSoC 的数据 (17ch)
// psoc_index: 0 或 1
// raw_35: 指向 35 字节 PSoC 数据 (Status + 17ch×2byte)
void touch_pipeline_feed(int psoc_index, const uint8_t *raw_35);

#endif /* __TOUCH_PIPELINE_H__ */
