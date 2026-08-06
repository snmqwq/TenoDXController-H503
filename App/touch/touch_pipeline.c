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
#define PIPELINE_DEVICE_COUNT  2U
#define PIPELINE_DEVICE_MASK   ((1U << PIPELINE_DEVICE_COUNT) - 1U)

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
static uint16_t calib_skip_count[PIPELINE_DEVICE_COUNT];
static uint16_t calib_per_ch_count[TENODATA_TOTAL_CHANNELS];
static bool calib_ready[TENODATA_TOTAL_CHANNELS];
static int32_t setup_raw[TENODATA_TOTAL_CHANNELS];
static PipelineState state;
static uint8_t connected_device_mask;
static uint8_t forced_device_mask;

static uint8_t device_bit(uint8_t device_index)
{
    return (uint8_t)(1U << device_index);
}

static uint8_t channel_device(uint8_t channel)
{
    return (uint8_t)(channel / TENODATA_CHANNELS_PER_DEVICE);
}

static void add_channel_region(uint64_t *mapped_bits, uint8_t channel)
{
    uint8_t region = tenodata_config_get_mai2touch_region(channel);

    if ((mapped_bits != NULL) &&
        (region < TENODATA_MAI2TOUCH_REGION_COUNT))
    {
        *mapped_bits |= 1ULL << region;
    }
}

static uint64_t get_forced_mapped_bits(void)
{
    uint8_t unavailable_mask =
        (uint8_t)((~connected_device_mask) & PIPELINE_DEVICE_MASK);
    uint8_t output_mask = (uint8_t)(unavailable_mask | forced_device_mask);
    uint64_t mapped_bits = 0U;

    for (uint8_t channel = 0U;
         channel < TENODATA_TOTAL_CHANNELS;
         channel++)
    {
        uint8_t owner_mask = device_bit(channel_device(channel));

        if ((output_mask & owner_mask) != 0U)
        {
            add_channel_region(&mapped_bits, channel);
        }
    }

    return mapped_bits;
}

static void update_touch_output(void)
{
    uint64_t mapped_bits = get_forced_mapped_bits();
    uint64_t *touch_bits = mai2touch_get_touch_bits();

    if (state == PIPELINE_STATE_READY)
    {
        for (uint8_t channel = 0U;
             channel < TENODATA_TOTAL_CHANNELS;
             channel++)
        {
            uint8_t owner_mask = device_bit(channel_device(channel));

            if (((connected_device_mask & owner_mask) != 0U) &&
                detectors[channel].is_pressed)
            {
                add_channel_region(&mapped_bits, channel);
            }
        }
    }

    if (touch_bits != NULL)
    {
        *touch_bits = mapped_bits & TENODATA_MAI2TOUCH_VALID_MASK;
    }
}

static void reset_device_calibration(uint8_t device_index)
{
    uint8_t first_channel =
        (uint8_t)(device_index * TENODATA_CHANNELS_PER_DEVICE);
    uint8_t end_channel =
        (uint8_t)(first_channel + TENODATA_CHANNELS_PER_DEVICE);

    for (uint8_t channel = first_channel; channel < end_channel; channel++)
    {
        calib_accum[channel] = 0;
        calib_per_ch_count[channel] = 0U;
        calib_ready[channel] = false;
        setup_raw[channel] = 0;
        detector_reset(&detectors[channel]);
    }

    memset(last_block[device_index], 0, sizeof(last_block[device_index]));
    block_cache_valid[device_index] = false;
    calib_skip_count[device_index] = 0U;
}

static bool connected_channels_are_ready(void)
{
    for (uint8_t channel = 0U;
         channel < TENODATA_TOTAL_CHANNELS;
         channel++)
    {
        uint8_t owner_mask = device_bit(channel_device(channel));

        if (((connected_device_mask & owner_mask) != 0U) &&
            !calib_ready[channel])
        {
            return false;
        }
    }

    return true;
}

static void enter_ready_if_possible(void)
{
    if (!connected_channels_are_ready())
    {
        return;
    }

    if (state != PIPELINE_STATE_READY)
    {
        state = PIPELINE_STATE_READY;

        /* A recovered device stops being forced only after all of its
         * connected channels have a valid software baseline. Disconnected
         * devices remain forced through get_forced_mapped_bits().
         */
        forced_device_mask &= (uint8_t)~connected_device_mask;
    }

    update_touch_output();
}

static void reset_detection_state(void)
{
    memset(last_block, 0, sizeof(last_block));
    memset(block_cache_valid, 0, sizeof(block_cache_valid));

    for (uint8_t channel = 0U; channel < TENODATA_TOTAL_CHANNELS; channel++)
    {
        detector_reset(&detectors[channel]);
    }

    update_touch_output();
}

// ================= 公开 API =================

void touch_pipeline_init(void)
{
    state = PIPELINE_STATE_CALIB_SKIP;
    memset(calib_skip_count, 0, sizeof(calib_skip_count));
    memset(calib_per_ch_count, 0, sizeof(calib_per_ch_count));
    memset(calib_ready, 0, sizeof(calib_ready));
    memset(calib_accum, 0, sizeof(calib_accum));
    memset(setup_raw, 0, sizeof(setup_raw));
    reset_detection_state();
    enter_ready_if_possible();
}

void touch_pipeline_set_device_masks(uint8_t connected_mask,
                                     uint8_t forced_mask)
{
    uint8_t previous_connected_mask = connected_device_mask;
    uint8_t newly_connected_mask;
    uint8_t disconnected_mask;

    connected_mask &= PIPELINE_DEVICE_MASK;
    forced_mask &= PIPELINE_DEVICE_MASK;
    newly_connected_mask =
        (uint8_t)(connected_mask & (uint8_t)~previous_connected_mask);
    disconnected_mask =
        (uint8_t)(previous_connected_mask & (uint8_t)~connected_mask);

    connected_device_mask = connected_mask;
    forced_device_mask = forced_mask;

    for (uint8_t device_index = 0U;
         device_index < PIPELINE_DEVICE_COUNT;
         device_index++)
    {
        uint8_t mask = device_bit(device_index);

        if (((newly_connected_mask | disconnected_mask) & mask) != 0U)
        {
            reset_device_calibration(device_index);
        }
    }

    if (newly_connected_mask != 0U)
    {
        state = PIPELINE_STATE_CALIB_SKIP;
    }

    enter_ready_if_possible();
    update_touch_output();
}

bool touch_pipeline_is_ready(void)
{
    return state == PIPELINE_STATE_READY;
}

void touch_pipeline_reset_detection_state(void)
{
    reset_detection_state();
}

void touch_pipeline_feed(int psoc_index, const uint8_t *raw_35)
{
    uint8_t psoc_mask;

    if ((psoc_index < 0) ||
        (psoc_index >= (int)PIPELINE_DEVICE_COUNT) ||
        (raw_35 == NULL))
    {
        return;
    }

    psoc_mask = device_bit((uint8_t)psoc_index);
    if ((connected_device_mask & psoc_mask) == 0U)
    {
        return;
    }

    int base_ch = psoc_index * 17;

    // ---- 阶段1: 提取数值并记录每个通道是否变化 ----
    uint16_t channel_vals[TENODATA_CHANNELS_PER_DEVICE];
    bool channel_changed[TENODATA_CHANNELS_PER_DEVICE];
    bool any_channel_changed = !block_cache_valid[psoc_index];

    for (int i = 0; i < TENODATA_CHANNELS_PER_DEVICE; i++) {
        int byte_offset = 1 + i * 2;
        channel_vals[i] = raw_35[byte_offset] |
            ((uint16_t)raw_35[byte_offset + 1] << 8);
        channel_changed[i] =
            !block_cache_valid[psoc_index] ||
            (last_block[psoc_index][i * 2] != raw_35[byte_offset]) ||
            (last_block[psoc_index][i * 2 + 1] != raw_35[byte_offset + 1]);

        if (channel_changed[i]) {
            any_channel_changed = true;
        }
    }

    if ((state == PIPELINE_STATE_READY) && !any_channel_changed) {
        return;
    }

    memcpy(last_block[psoc_index], &raw_35[1], sizeof(last_block[psoc_index]));
    block_cache_valid[psoc_index] = true;

    // ---- 阶段3: 校准 或 判定 ----
    switch (state) {
        case PIPELINE_STATE_CALIB_SKIP:
        case PIPELINE_STATE_CALIB_COLLECT:
            /* Each PSoC gets its own stabilization window. With a shared
             * counter, two alternating devices only skipped about five frames
             * each and a late device could skip none at all.
             */
            if (calib_skip_count[psoc_index] < CALIB_SKIP_FRAMES)
            {
                calib_skip_count[psoc_index]++;
                break;
            }

            state = PIPELINE_STATE_CALIB_COLLECT;
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
            // 只等待当前已连接 PSoC 所属的通道完成软件基线。
            enter_ready_if_possible();
            break;

        case PIPELINE_STATE_READY: {
            for (int i = 0; i < TENODATA_CHANNELS_PER_DEVICE; i++) {
                int phys_ch = base_ch + i;
                if (!calib_ready[phys_ch]) continue; // 该通道尚未校准完毕
                if (!channel_changed[i]) continue;   // 只更新实际变化的通道

                (void)detector_process_frame(&detectors[phys_ch],
                                             (uint8_t)phys_ch,
                                             channel_vals[i],
                                             setup_raw[phys_ch]);
            }

            /* update_touch_output() merges pressed, shared-region,
             * disconnected and explicitly forced mappings with OR.
             */
            update_touch_output();
            break;
        }
    }
}
