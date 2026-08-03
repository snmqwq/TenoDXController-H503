#include "touch_pipeline.h"
#include "button_detector.h"
#include "button_detector_config.h"
#include "mai2touch.h"
#include "tenodata_config.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// ================= 校准常量 =================
#define CALIB_SKIP_FRAMES      10U   // 跳过前 N 帧 (PSoC 已做硬件校准, 只需短暂稳定)
#define CALIB_COLLECT_FRAMES   30U   // 收集 N 帧取平均值作为基线

// ================= 管线状态 =================
typedef enum {
    PIPELINE_STATE_CALIB_SKIP,
    PIPELINE_STATE_CALIB_COLLECT,
    PIPELINE_STATE_READY,
} PipelineState;

// ================= PSoC 数据缓存 (34字节 × 2) =================
static uint8_t last_block[2][34];
static bool block_cache_valid[2];

// ================= 34 通道判定器 + 校准 =================
static ButtonDetector detectors[TENODATA_TOTAL_CHANNELS];
static int32_t calib_accum[TENODATA_TOTAL_CHANNELS];
static uint16_t calib_feed_count;          // 喂入次数 (skip用)
static uint16_t calib_per_ch_count[TENODATA_TOTAL_CHANNELS];
static bool calib_ready[TENODATA_TOTAL_CHANNELS];
static int32_t setup_raw[TENODATA_TOTAL_CHANNELS];
static PipelineState state;

static void reset_detection_state(void)
{
    uint64_t *touch_bits = mai2touch_get_touch_bits();

    if (touch_bits != NULL)
    {
        *touch_bits = 0U;
    }

    memset(last_block, 0, sizeof(last_block));
    memset(block_cache_valid, 0, sizeof(block_cache_valid));

    for (uint8_t channel = 0U; channel < TENODATA_TOTAL_CHANNELS; channel++)
    {
        detector_reset(&detectors[channel]);
    }
}

// ================= 公开 API =================

void touch_pipeline_init(void)
{
    state = PIPELINE_STATE_CALIB_SKIP;
    calib_feed_count = 0;
    memset(calib_per_ch_count, 0, sizeof(calib_per_ch_count));
    memset(calib_ready, 0, sizeof(calib_ready));
    memset(calib_accum, 0, sizeof(calib_accum));
    memset(setup_raw, 0, sizeof(setup_raw));
    reset_detection_state();
}

void touch_pipeline_reset_detection_state(void)
{
    reset_detection_state();
}

void touch_pipeline_feed(int psoc_index, const uint8_t *raw_35)
{
    if (psoc_index < 0 || psoc_index > 1) return;

    int base_ch = psoc_index * 17;

    // ---- 阶段1: 字节级变化检测 (防重复数据污染) ----
    if ((state == PIPELINE_STATE_READY) && block_cache_valid[psoc_index]) {
        bool changed = false;
        for (int i = 0; i < TENODATA_TOTAL_CHANNELS; i++) {
            if (last_block[psoc_index][i] != raw_35[i + 1]) { // +1 跳过 status 字节
                changed = true;
                break;
            }
        }
        if (!changed) return; // 无变化，跳过这片 PSoC
    }

    // 更新缓存
    memcpy(last_block[psoc_index], &raw_35[1], 34);
    block_cache_valid[psoc_index] = true;

    // ---- 阶段2: 提取 17 通道 ushort 值 ----
    uint16_t channel_vals[17];
    for (int i = 0; i < 17; i++) {
        channel_vals[i] = raw_35[1 + i * 2] | ((uint16_t)raw_35[2 + i * 2] << 8);
    }

    // ---- 阶段3: 校准 或 判定 ----
    uint64_t *touch_bits = mai2touch_get_touch_bits();

    switch (state) {
        case PIPELINE_STATE_CALIB_SKIP:
            calib_feed_count++;
            if (calib_feed_count >= CALIB_SKIP_FRAMES) {
                calib_feed_count = 0;
                state = PIPELINE_STATE_CALIB_COLLECT;
            }
            break;

        case PIPELINE_STATE_CALIB_COLLECT:
            // 每通道独立累积, 避免另一片 PSoC 未到达时提前退出校准
            for (int i = 0; i < 17; i++) {
                int ch = base_ch + i;
                if (calib_per_ch_count[ch] < CALIB_COLLECT_FRAMES) {
                    calib_accum[ch] += channel_vals[i];
                    calib_per_ch_count[ch]++;
                    if (calib_per_ch_count[ch] >= CALIB_COLLECT_FRAMES) {
                        setup_raw[ch] = calib_accum[ch] / CALIB_COLLECT_FRAMES;
                        calib_ready[ch] = true;
                    }
                }
            }
            // 全部 34 通道就绪才进入判定
            for (int i = 0; i < TENODATA_TOTAL_CHANNELS; i++) {
                if (!calib_ready[i]) break;
                if (i == (TENODATA_TOTAL_CHANNELS - 1U)) state = PIPELINE_STATE_READY;
            }
            break;

        case PIPELINE_STATE_READY: {
            uint64_t mapped_bits = 0U;

            for (int i = 0; i < 17; i++) {
                int phys_ch = base_ch + i;
                if (!calib_ready[phys_ch]) continue; // 该通道尚未校准完毕

                (void)detector_process_frame(&detectors[phys_ch],
                                             (uint8_t)phys_ch,
                                             channel_vals[i],
                                             setup_raw[phys_ch]);
            }

            /* One physical channel may drive many regions; overlapping
             * channels are merged with OR and therefore cannot clear each
             * other's active output.
             */
            for (uint8_t channel = 0U;
                 channel < TENODATA_TOTAL_CHANNELS;
                 channel++)
            {
                if (detectors[channel].is_pressed)
                {
                    mapped_bits |=
                        tenodata_config_get_mai2touch_mask(channel);
                }
            }

            *touch_bits = mapped_bits & TENODATA_MAI2TOUCH_VALID_MASK;
            break;
        }
    }
}
