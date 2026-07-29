#include "touch_pipeline.h"
#include "button_detector.h"
#include "button_detector_config.h"
#include "mai2touch.h"
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
static ButtonDetector detectors[34];
static int32_t calib_accum[34];
static uint16_t calib_feed_count;          // 喂入次数 (skip用)
static uint16_t calib_per_ch_count[34];    // 每通道已校准样本数
static bool calib_ready[34];               // 每通道校准是否完成
static int32_t setup_raw[34];
static PipelineState state;

// ================= 物理通道 → Mai2Touch bit 映射 =================
// PhysicalToLogicalMap: E4,D4,B3,A3,C1,E3,D3,B2,A2,E2,D2,B1,A1,E1,D1,B8,A8,E8,D8,B7,A7,C2,E7,D7,B6,A6,E6,D6,B5,A5,E5,D5,B4,A4
// Game internal: A1=bit0, B1=bit8, C1=bit16, D1=bit18, E1=bit26
static const uint8_t phys_to_mai2touch_bit[34] = {
    29, // ch0:  E4 → 26+3=29
    21, // ch1:  D4 → 18+3=21
    10, // ch2:  B3 →  8+2=10
    2,  // ch3:  A3 →  0+2=2
    16, // ch4:  C1 → 16+0=16
    28, // ch5:  E3 → 26+2=28
    20, // ch6:  D3 → 18+2=20
    9,  // ch7:  B2 →  8+1=9
    1,  // ch8:  A2 →  0+1=1
    27, // ch9:  E2 → 26+1=27
    19, // ch10: D2 → 18+1=19
    8,  // ch11: B1 →  8+0=8
    0,  // ch12: A1 →  0+0=0
    26, // ch13: E1 → 26+0=26
    18, // ch14: D1 → 18+0=18
    15, // ch15: B8 →  8+7=15
    7,  // ch16: A8 →  0+7=7
    33, // ch17: E8 → 26+7=33
    25, // ch18: D8 → 18+7=25
    14, // ch19: B7 →  8+6=14
    6,  // ch20: A7 →  0+6=6
    17, // ch21: C2 → 16+1=17
    32, // ch22: E7 → 26+6=32
    24, // ch23: D7 → 18+6=24
    13, // ch24: B6 →  8+5=13
    5,  // ch25: A6 →  0+5=5
    31, // ch26: E6 → 26+5=31
    23, // ch27: D6 → 18+5=23
    12, // ch28: B5 →  8+4=12
    4,  // ch29: A5 →  0+4=4
    30, // ch30: E5 → 26+4=30
    22, // ch31: D5 → 18+4=22
    11, // ch32: B4 →  8+3=11
    3,  // ch33: A4 →  0+3=3
};

// ================= 公开 API =================

void touch_pipeline_init(void)
{
    state = PIPELINE_STATE_CALIB_SKIP;
    calib_feed_count = 0;
    memset(calib_per_ch_count, 0, sizeof(calib_per_ch_count));
    memset(calib_ready, 0, sizeof(calib_ready));
    memset(calib_accum, 0, sizeof(calib_accum));
    memset(setup_raw, 0, sizeof(setup_raw));
    memset(last_block, 0, sizeof(last_block));
    block_cache_valid[0] = false;
    block_cache_valid[1] = false;

    for (int i = 0; i < 34; i++) detector_reset(&detectors[i]);
}

void touch_pipeline_feed(int psoc_index, const uint8_t *raw_35)
{
    if (psoc_index < 0 || psoc_index > 1) return;

    int base_ch = psoc_index * 17;

    // ---- 阶段1: 字节级变化检测 (防重复数据污染) ----
    if (block_cache_valid[psoc_index]) {
        bool changed = false;
        for (int i = 0; i < 34; i++) {
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
            for (int i = 0; i < 34; i++) {
                if (!calib_ready[i]) break;
                if (i == 33) state = PIPELINE_STATE_READY;
            }
            break;

        case PIPELINE_STATE_READY:
            for (int i = 0; i < 17; i++) {
                int phys_ch = base_ch + i;
                if (!calib_ready[phys_ch]) continue; // 该通道尚未校准完毕

                bool pressed = detector_process_frame(&detectors[phys_ch], (uint8_t)phys_ch, channel_vals[i], setup_raw[phys_ch]);

                // 更新 mai2touch 触摸位
                uint8_t mt_bit = phys_to_mai2touch_bit[phys_ch];
                if (pressed)
                    *touch_bits |= (1ULL << mt_bit);
                else
                    *touch_bits &= ~(1ULL << mt_bit);
            }
            break;
    }
}
