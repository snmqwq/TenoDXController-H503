#include "touch_pipeline.h"
#include "button_detector.h"
#include "button_detector_config.h"
#include "mai2touch.h"
#include "tenodata_config.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// ================= 管线常量 (对齐 run3 signal_pipeline.py) =================
#define PIPELINE_IIR_FACTOR     2.0f  // 1.0 关闭平滑, >1.0 开启 (run3: 2.0)
#define PIPELINE_WARMUP_FRAMES  100U  // 冷启动预热 (让 IIR 充分收敛)
#define PIPELINE_CALIB_FRAMES   100U  // setup_raw 基线收集帧数
#define PIPELINE_HISTORY_SIZE   16U
#define PIPELINE_DEVICE_COUNT   2U
#define PIPELINE_DEVICE_MASK    ((1U << PIPELINE_DEVICE_COUNT) - 1U)

// ================= 管线状态 =================
typedef enum {
    PIPELINE_STATE_WARMUP,
    PIPELINE_STATE_CALIB,
    PIPELINE_STATE_READY,
} PipelineState;

// ================= 每通道管线状态 (对齐 SignalPipeline) =================
typedef struct {
    float iir;                  // IIR 滤波历史 (0.0 表示未初始化)
    int history[PIPELINE_HISTORY_SIZE];  // [0] 为最近一次有效帧值
    float setup_sum;            // 基线校准样本累计
    uint16_t setup_count;       // 校准样本数 (仅计数实际变化的帧)
    float setup_raw;            // 软件基线 (校准均值)
    bool calib_ready;
    int latest_val;             // 最近一次 IIR 输出快照 (raw==0 强制按下用)
} ChannelPipeline;

static ChannelPipeline channels[TENODATA_TOTAL_CHANNELS];
static ButtonDetector detectors[TENODATA_TOTAL_CHANNELS];

/* 对齐 run3 GameCommunicator.touch_state: 每游戏区域一位,
 * 仅由实际变化的通道覆写 (同一区域多通道时最后写入者胜)。 */
static bool region_state[TENODATA_MAI2TOUCH_REGION_COUNT];

// ================= 逐设备帧去重缓存 (对齐 SerialReader 的逐设备 payload 对比) =================
static uint8_t last_block[PIPELINE_DEVICE_COUNT][34];
static bool block_cache_valid[PIPELINE_DEVICE_COUNT];

static PipelineState state;
static uint16_t warmup_count;   // 全局已处理帧数 (两片设备共同推进, 对齐 Python 按包计数)
static uint16_t calib_count;
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
        for (uint8_t region = 0U;
             region < TENODATA_MAI2TOUCH_REGION_COUNT;
             region++)
        {
            if (region_state[region])
            {
                mapped_bits |= 1ULL << region;
            }
        }
    }

    if (touch_bits != NULL)
    {
        *touch_bits = mapped_bits & TENODATA_MAI2TOUCH_VALID_MASK;
    }
}

static void reset_channel(uint8_t channel)
{
    /* 整结构清零同时实现历史队列预填充 16 个 0,
     * 与 Python deque([0] * 16) 对齐: 首帧 deriv = val - 0。 */
    memset(&channels[channel], 0, sizeof(channels[channel]));
    detector_reset(&detectors[channel]);
}

static void reset_device_calibration(uint8_t device_index)
{
    uint8_t first_channel =
        (uint8_t)(device_index * TENODATA_CHANNELS_PER_DEVICE);
    uint8_t end_channel =
        (uint8_t)(first_channel + TENODATA_CHANNELS_PER_DEVICE);

    for (uint8_t channel = first_channel; channel < end_channel; channel++)
    {
        uint8_t region = tenodata_config_get_mai2touch_region(channel);

        if (region < TENODATA_MAI2TOUCH_REGION_COUNT)
        {
            region_state[region] = false;
        }

        reset_channel(channel);
    }

    memset(last_block[device_index], 0, sizeof(last_block[device_index]));
    block_cache_valid[device_index] = false;
}

static void restart_calibration(void)
{
    state = PIPELINE_STATE_WARMUP;
    warmup_count = 0U;
    calib_count = 0U;
}

// ================= 公开 API =================

void touch_pipeline_init(void)
{
    for (uint8_t channel = 0U; channel < TENODATA_TOTAL_CHANNELS; channel++)
    {
        reset_channel(channel);
    }

    memset(region_state, 0, sizeof(region_state));
    memset(last_block, 0, sizeof(last_block));
    memset(block_cache_valid, 0, sizeof(block_cache_valid));
    restart_calibration();
    update_touch_output();
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
        /* 新设备接入: 整条管线回到预热并重新收集基线
         * (对齐 Python 重新打开串口时的 reset)。 */
        restart_calibration();
    }

    update_touch_output();
}

bool touch_pipeline_is_ready(void)
{
    return state == PIPELINE_STATE_READY;
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

    int base_ch = psoc_index * TENODATA_CHANNELS_PER_DEVICE;

    // ---- 阶段1: 提取数值并逐通道标记变化 ----
    uint16_t raw_vals[TENODATA_CHANNELS_PER_DEVICE];
    bool channel_changed[TENODATA_CHANNELS_PER_DEVICE];
    bool any_changed = !block_cache_valid[psoc_index];

    for (int i = 0; i < (int)TENODATA_CHANNELS_PER_DEVICE; i++) {
        int byte_offset = 1 + i * 2;
        raw_vals[i] = raw_35[byte_offset] |
            ((uint16_t)raw_35[byte_offset + 1] << 8);
        channel_changed[i] =
            !block_cache_valid[psoc_index] ||
            (last_block[psoc_index][i * 2] != raw_35[byte_offset]) ||
            (last_block[psoc_index][i * 2 + 1] != raw_35[byte_offset + 1]);

        if (channel_changed[i]) {
            any_changed = true;
        }
    }

    /* 完全重复的帧整体丢弃 (对齐 Python: 该侧 payload 无变化则跳过,
     * 不推进预热/校准计数)。 */
    if (!any_changed) {
        return;
    }

    memcpy(last_block[psoc_index], &raw_35[1], sizeof(last_block[psoc_index]));
    block_cache_valid[psoc_index] = true;

    // ---- 阶段2: IIR 滤波 (仅更新实际变化的通道, 前置于预热) ----
    for (int i = 0; i < (int)TENODATA_CHANNELS_PER_DEVICE; i++) {
        int ch = base_ch + i;
        if (!channel_changed[i]) {
            continue;
        }

        float raw = (float)raw_vals[i];
        if (channels[ch].iir == 0.0f) {
            channels[ch].iir = raw;
        } else {
            channels[ch].iir =
                (channels[ch].iir * (PIPELINE_IIR_FACTOR - 1.0f) + raw) /
                PIPELINE_IIR_FACTOR;
        }
        channels[ch].latest_val = (int)channels[ch].iir;
    }

    // ---- 阶段3: 冷启动预热 ----
    if (state == PIPELINE_STATE_WARMUP) {
        warmup_count++;
        if (warmup_count <= PIPELINE_WARMUP_FRAMES) {
            return;
        }
        state = PIPELINE_STATE_CALIB;
        calib_count = 0U;
        /* 本帧继续落入校准收集 (对齐 Python: 预热结束的同一帧开始收集) */
    }

    // ---- 阶段4: setup_raw 基线校准 ----
    if (state == PIPELINE_STATE_CALIB) {
        /* 仅收集实际发生数值更新的通道, 避免大量冗余定格拉偏均值
         * (run3 的重大修复); 已校准通道 (热插拔恢复场景) 不再收集。 */
        for (int i = 0; i < (int)TENODATA_CHANNELS_PER_DEVICE; i++) {
            int ch = base_ch + i;
            if (!channel_changed[i]) {
                continue;
            }
            if (channels[ch].calib_ready) {
                continue;
            }

            channels[ch].setup_sum += (float)channels[ch].latest_val;
            channels[ch].setup_count++;
        }

        calib_count++;
        if (calib_count >= PIPELINE_CALIB_FRAMES) {
            for (int ch = 0; ch < (int)TENODATA_TOTAL_CHANNELS; ch++) {
                if (channels[ch].setup_count > 0U) {
                    channels[ch].setup_raw =
                        channels[ch].setup_sum /
                        (float)channels[ch].setup_count;
                } else {
                    /* 校准期内从未变化的通道回退到当前值 */
                    channels[ch].setup_raw =
                        (float)channels[ch].latest_val;
                }
                channels[ch].calib_ready = true;
            }
            state = PIPELINE_STATE_READY;
        }
        return;
    }

    // ---- 阶段5: 正常检测 ----
    for (int i = 0; i < (int)TENODATA_CHANNELS_PER_DEVICE; i++) {
        int ch = base_ch + i;
        if (!channel_changed[i]) {
            continue;  // 未更新的通道保留上一帧判定结果
        }
        if (!channels[ch].calib_ready) {
            continue;
        }

        float val = (float)channels[ch].latest_val;

        /* 一阶/二阶导数。历史队列预填充 16 个 0 (与 Python deque 一致),
         * 因此首帧 deriv = val - 0, last_deriv = 0 - 0。 */
        float last_deriv = (float)channels[ch].history[0] -
                           (float)channels[ch].history[1];
        float deriv = val - (float)channels[ch].history[0];
        float deriv2 = deriv - last_deriv;

        float diff = val - channels[ch].setup_raw;

        bool pressed = detector_process_frame(&detectors[ch],
                                              (uint8_t)ch,
                                              val,
                                              diff,
                                              channels[ch].setup_raw,
                                              deriv,
                                              deriv2);

        /* 对齐 run3 GameCommunicator.update_channel_state:
         * raw 为 0 视为传感器失效强制按下; 区域由最后更新的通道覆写。 */
        uint8_t region = tenodata_config_get_mai2touch_region((uint8_t)ch);

        if (region < TENODATA_MAI2TOUCH_REGION_COUNT)
        {
            region_state[region] = (channels[ch].latest_val == 0) ||
                                   pressed;
        }

        /* 历史更新: 严格禁止将未更新的重复数据塞入历史队列,
         * 防止导数归零 (run3 关键修复)。 */
        for (int h = (int)PIPELINE_HISTORY_SIZE - 1; h > 0; h--) {
            channels[ch].history[h] = channels[ch].history[h - 1];
        }
        channels[ch].history[0] = (int)val;
    }

    /* update_touch_output() 合并按下区域与断线强制区域。 */
    update_touch_output();
}
