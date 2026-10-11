#include "touch_pipeline.h"
#include "touch_core_port.h"
#include "mai2touch.h"
#include "tenodata_config.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Provided by the STM32 HAL in production; declared here so the host-side
 * parity test can link a deterministic clock without pulling in the full HAL. */
uint32_t HAL_GetTick(void);

// ================= 管线常量 (对齐 run3 signal_pipeline.py) =================
#define PIPELINE_IIR_FACTOR     2.0   // 1.0 disables smoothing, >1.0 enables it
#define PIPELINE_WARMUP_FRAMES  100U  // 冷启动预热 (让 IIR 充分收敛)
#define PIPELINE_CALIB_FRAMES   100U  // setup_raw 基线收集帧数
#define PIPELINE_HISTORY_SIZE   16U
#define PIPELINE_DEVICE_COUNT   2U
#define PIPELINE_DEVICE_MASK    ((1U << PIPELINE_DEVICE_COUNT) - 1U)
#define PIPELINE_MAX_BATCH_FRAMES 64U
/*
 * TouchHost-kaede applies report_rate_hz independently to each board after the
 * baseline is ready. The firmware uses the same nominal rate for the
 * device-side path so the two implementations consume the same updates.
 */
#define PIPELINE_REPORT_RATE_HZ 180.0
#define PIPELINE_RATE_CAP_SECONDS 0.1

// ================= 管线状态 =================
typedef enum {
    PIPELINE_STATE_WARMUP,
    PIPELINE_STATE_CALIB,
    PIPELINE_STATE_READY,
} PipelineState;

// ================= 每通道管线状态 (对齐 SignalPipeline) =================
typedef struct {
    double iir;                 // IIR history, matching Python's float (double)
    int history[PIPELINE_HISTORY_SIZE];  // [0] is the last valid frame value
    double setup_sum;           // baseline sample sum
    uint16_t setup_count;       // baseline sample count
    double setup_raw;           // frozen baseline average
    bool calib_ready;
    int latest_val;             // 最近一次 IIR 输出快照 (raw==0 强制按下用)
} ChannelPipeline;

static ChannelPipeline channels[TENODATA_TOTAL_CHANNELS];

/* 对齐 run3 GameCommunicator.touch_state: 每游戏区域一位,
 * 仅由实际变化的通道覆写 (同一区域多通道时最后写入者胜)。 */
static bool region_state[TENODATA_MAI2TOUCH_REGION_COUNT];

// ================= 逐设备帧去重缓存 (对齐 SerialReader 的逐设备 payload 对比) =================
static uint8_t last_block[PIPELINE_DEVICE_COUNT][34];
static bool block_cache_valid[PIPELINE_DEVICE_COUNT];

typedef struct {
    uint8_t raw[PIPELINE_DEVICE_COUNT][34];
    bool changed[PIPELINE_DEVICE_COUNT];
    bool accepted[PIPELINE_DEVICE_COUNT];
} PendingPacket;

static PipelineState state;
static uint16_t warmup_count;   // 全局已处理帧数 (两片设备共同推进, 对齐 Python 按包计数)
static uint16_t calib_count;
static uint8_t connected_device_mask;
static uint8_t forced_device_mask;

/*
 * Per-board consumption token buckets, matching TouchHost-kaede's
 * dev1_budget / dev2_budget. They are only consulted after calibration.
 */
static double rate_budget[PIPELINE_DEVICE_COUNT];
static uint32_t rate_last_tick;
static bool rate_has_last_tick;

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
    touch_core_port_reset_channel(channel);
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
    rate_budget[0] = 0.0;
    rate_budget[1] = 0.0;
    rate_has_last_tick = false;
}

// ================= 公开 API =================

void touch_pipeline_init(void)
{
    /* 核心分组依赖 active 映射，必须在任何 reset_channel 之前初始化。 */
    touch_core_port_reset();

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

#ifdef TOUCH_PIPELINE_TEST_PROBES
static bool pipeline_test_last_accepted[PIPELINE_MAX_BATCH_FRAMES];
static uint16_t pipeline_test_last_count;
static double pipeline_test_rate_hz = PIPELINE_REPORT_RATE_HZ;
static double pipeline_test_iir_factor = PIPELINE_IIR_FACTOR;
static bool pipeline_test_force_changed;
static bool pipeline_test_emitted;

uint16_t touch_pipeline_test_warmup_count(void)
{
    return warmup_count;
}

uint16_t touch_pipeline_test_calib_count(void)
{
    return calib_count;
}

int touch_pipeline_test_latest_value(uint8_t channel)
{
    if (channel >= TENODATA_TOTAL_CHANNELS)
    {
        return 0;
    }

    return channels[channel].latest_val;
}

bool touch_pipeline_test_was_accepted(uint16_t packet_index)
{
    return (packet_index < pipeline_test_last_count) &&
           pipeline_test_last_accepted[packet_index];
}

void touch_pipeline_test_set_rate(double rate_hz)
{
    pipeline_test_rate_hz = rate_hz;
}

void touch_pipeline_test_set_iir_factor(double factor)
{
    pipeline_test_iir_factor = factor;
}

void touch_pipeline_test_set_force_changed(bool enabled)
{
    pipeline_test_force_changed = enabled;
}

void touch_pipeline_test_reset_rate(void)
{
    rate_budget[0] = 0.0;
    rate_budget[1] = 0.0;
    rate_has_last_tick = false;
}

void touch_pipeline_test_set_last_tick(uint32_t tick)
{
    rate_last_tick = tick;
    rate_has_last_tick = true;
}

bool touch_pipeline_snapshot(uint8_t channel, TouchPipelineSnapshot *snapshot)
{
    ChannelPipeline *pipeline;
    const TenoTouchDecision *decision;

    if ((channel >= TENODATA_TOTAL_CHANNELS) || (snapshot == NULL))
    {
        return false;
    }

    pipeline = &channels[channel];
    decision = touch_core_port_decision(channel);

    snapshot->latest_value = pipeline->latest_val;
    snapshot->setup_raw = pipeline->setup_raw;

    if (decision == NULL)
    {
        return false;
    }

    snapshot->pressed = decision->pressed != 0U;
    snapshot->baseline_valid = channels[channel].calib_ready;
    snapshot->baseline = decision->baseline;
    snapshot->effective_diff = decision->effective_diff;
    snapshot->deriv_eff = decision->deriv;
    snapshot->deriv2_eff = decision->deriv2;
    snapshot->algo_deriv = decision->deriv;
    snapshot->algo_deriv2 = decision->deriv2;
    snapshot->fsm_state = (int)decision->fsm_state;
    snapshot->touched_effective_diff = decision->peak_effective_diff;

    /*
     * 核心不产生 released_min / large_latch：那两个量属于被移除了的旧语义。
     * 回放驱动仍会读它们，这里给出明确的"不适用"值。
     */
    snapshot->released_min_diff = 0.0;
    snapshot->released_min_valid = false;
    snapshot->large_diff_latched = -1;
    snapshot->pre_judge_deriv = decision->pre_judge_deriv;

    return true;
}

bool touch_pipeline_test_emitted(void)
{
    return pipeline_test_emitted;
}
#endif

void touch_pipeline_commit_batch(const uint8_t *packets,
                                 uint16_t packet_count)
{
    /*
     * 单帧 64 x 76B 的临时分类缓冲, 只在主循环 (非中断、不可重入) 使用。
     * 放栈上会一次吃掉约 4.6 KB, 而本板 MSP 只有约 10 KB 可用 (中断共用),
     * 所以固定放 .bss。
     */
    static PendingPacket pending[PIPELINE_MAX_BATCH_FRAMES];
    uint16_t accepted_count = 0U;

    if ((packets == NULL) || (packet_count == 0U))
    {
        return;
    }

    if (packet_count > PIPELINE_MAX_BATCH_FRAMES)
    {
        packet_count = PIPELINE_MAX_BATCH_FRAMES;
    }

#ifdef TOUCH_PIPELINE_TEST_PROBES
    pipeline_test_last_count = packet_count;
    pipeline_test_emitted = false;
#endif

    /* Classify every packet first: a repeated board does not advance any state. */
    for (uint16_t packet_index = 0U;
         packet_index < packet_count;
         packet_index++)
    {
        PendingPacket *packet = &pending[packet_index];
        const uint8_t *frame =
            packets + ((size_t)packet_index *
                       TOUCH_PIPELINE_PACKET_BYTES);
        uint8_t valid_mask = frame[0];

        for (uint8_t psoc_index = 0U;
             psoc_index < PIPELINE_DEVICE_COUNT;
             psoc_index++)
        {
            if ((valid_mask & device_bit(psoc_index)) == 0U)
            {
                packet->changed[psoc_index] = false;
                packet->accepted[psoc_index] = false;
                continue;
            }

            const uint8_t *raw =
                frame + TOUCH_PIPELINE_PACKET_MASK_BYTES +
                ((size_t)psoc_index * 34U);

            memcpy(packet->raw[psoc_index], raw, sizeof(packet->raw[psoc_index]));
            packet->changed[psoc_index] =
                !block_cache_valid[psoc_index] ||
                (memcmp(last_block[psoc_index],
                        raw,
                        sizeof(packet->raw[psoc_index])) != 0);
#ifdef TOUCH_PIPELINE_TEST_PROBES
            if (pipeline_test_force_changed)
            {
                packet->changed[psoc_index] = true;
            }
#endif
            packet->accepted[psoc_index] = packet->changed[psoc_index];
        }
    }

    /*
     * Apply TouchHost-kaede's post-calibration rate limit before any IIR or
     * detector state changes. Alpha spends each board's budget while walking
     * the drained batch from newest to oldest, so a short burst keeps the
     * latest update.
     */
#ifdef TOUCH_PIPELINE_TEST_PROBES
    double report_rate_hz = pipeline_test_rate_hz;
    const double iir_factor = pipeline_test_iir_factor;
#else
    const double report_rate_hz = PIPELINE_REPORT_RATE_HZ;
    const double iir_factor = PIPELINE_IIR_FACTOR;
#endif

    if (state == PIPELINE_STATE_READY && report_rate_hz > 0.0)
    {
        uint32_t now = HAL_GetTick();

        if (rate_has_last_tick)
        {
            double elapsed =
                (double)(uint32_t)(now - rate_last_tick) / 1000.0;
            double cap = report_rate_hz * PIPELINE_RATE_CAP_SECONDS;

            for (uint8_t psoc_index = 0U;
                 psoc_index < PIPELINE_DEVICE_COUNT;
                 psoc_index++)
            {
                rate_budget[psoc_index] += report_rate_hz * elapsed;
                if (rate_budget[psoc_index] > cap)
                {
                    rate_budget[psoc_index] = cap;
                }
            }
        }

        rate_last_tick = now;
        rate_has_last_tick = true;

        for (uint16_t packet_index = packet_count;
             packet_index > 0U;
             packet_index--)
        {
            PendingPacket *packet = &pending[packet_index - 1U];

            for (uint8_t psoc_index = 0U;
                 psoc_index < PIPELINE_DEVICE_COUNT;
                 psoc_index++)
            {
                if (!packet->accepted[psoc_index])
                {
                    continue;
                }

                if (rate_budget[psoc_index] > 0.0)
                {
                    rate_budget[psoc_index] -= 1.0;
                }
                else
                {
                    packet->accepted[psoc_index] = false;
                }
            }
        }
    }

    for (uint16_t packet_index = 0U;
         packet_index < packet_count;
         packet_index++)
    {
        PendingPacket *packet = &pending[packet_index];
        bool any_accepted = false;
        bool froze_baseline = false;

        for (uint8_t psoc_index = 0U;
             psoc_index < PIPELINE_DEVICE_COUNT;
             psoc_index++)
        {
            any_accepted = any_accepted || packet->accepted[psoc_index];
        }

#ifdef TOUCH_PIPELINE_TEST_PROBES
        pipeline_test_last_accepted[packet_index] = packet->accepted[0];
#endif

        if (!any_accepted)
        {
            continue;
        }

        accepted_count++;

        /* ---- IIR: update the accepted boards for this packet before its state is used. ---- */
        for (uint8_t psoc_index = 0U;
             psoc_index < PIPELINE_DEVICE_COUNT;
             psoc_index++)
        {
            if (!packet->accepted[psoc_index])
            {
                continue;
            }

            int base_ch = psoc_index * TENODATA_CHANNELS_PER_DEVICE;

            memcpy(last_block[psoc_index],
                   packet->raw[psoc_index],
                   sizeof(last_block[psoc_index]));
            block_cache_valid[psoc_index] = true;

            for (int i = 0; i < (int)TENODATA_CHANNELS_PER_DEVICE; i++) {
                int ch = base_ch + i;

                double raw = (double)(
                    packet->raw[psoc_index][i * 2] |
                    ((uint16_t)packet->raw[psoc_index][i * 2 + 1] << 8));
                if (channels[ch].iir == 0.0) {
                    channels[ch].iir = raw;
                } else {
                    channels[ch].iir =
                        (channels[ch].iir * (iir_factor - 1.0) + raw) /
                        iir_factor;
                }
                channels[ch].latest_val = (int)channels[ch].iir;
            }
        }

        /*
         * Advance the warm-up first. Python decrements warmup_frames in the
         * same process_packet call and continues into calibration in that
         * same call when it reaches zero.
         */
        if (state == PIPELINE_STATE_WARMUP) {
            if (warmup_count < PIPELINE_WARMUP_FRAMES) {
                warmup_count++;
                continue;
            }

            warmup_count++;
            state = PIPELINE_STATE_CALIB;
            calib_count = 0U;
        }

        /* ---- Calibration collection, one packet per count. ---- */
        if (state == PIPELINE_STATE_CALIB) {
            for (uint8_t psoc_index = 0U;
                 psoc_index < PIPELINE_DEVICE_COUNT;
                 psoc_index++)
            {
                int base_ch = psoc_index * TENODATA_CHANNELS_PER_DEVICE;

                if (!packet->accepted[psoc_index])
                {
                    continue;
                }

                for (int i = 0; i < (int)TENODATA_CHANNELS_PER_DEVICE; i++) {
                    int ch = base_ch + i;
                    if (channels[ch].calib_ready) {
                        continue;
                    }

                    channels[ch].setup_sum += (double)channels[ch].latest_val;
                    channels[ch].setup_count++;
                }
            }

            calib_count++;
            if (calib_count >= PIPELINE_CALIB_FRAMES) {
                for (int ch = 0; ch < (int)TENODATA_TOTAL_CHANNELS; ch++) {
                    if (channels[ch].setup_count > 0U) {
                        channels[ch].setup_raw =
                            channels[ch].setup_sum /
                            (double)channels[ch].setup_count;
                    } else {
                        channels[ch].setup_raw = (double)channels[ch].latest_val;
                    }
                    channels[ch].calib_ready = true;
                }
                state = PIPELINE_STATE_READY;
                /*
                 * TouchHost-kaede's process_packet returns None on the frame that
                 * freezes the baseline. The first judged frame is the next
                 * accepted packet, not this one.
                 */
                froze_baseline = true;
            }
        }

        if ((state == PIPELINE_STATE_READY) && !froze_baseline)
        {
#ifdef TOUCH_PIPELINE_TEST_PROBES
            pipeline_test_emitted = true;
#endif
            /* ---- Detector pass: only accepted boards, in packet order. ---- */
            for (uint8_t psoc_index = 0U;
                 psoc_index < PIPELINE_DEVICE_COUNT;
                 psoc_index++)
            {
                int base_ch = psoc_index * TENODATA_CHANNELS_PER_DEVICE;

                if (!packet->accepted[psoc_index])
                {
                    continue;
                }

                for (int i = 0; i < (int)TENODATA_CHANNELS_PER_DEVICE; i++) {
                    int ch = base_ch + i;
                    if (!channels[ch].calib_ready) {
                        continue;
                    }

                    double val = (double)channels[ch].latest_val;
                    double diff = val - channels[ch].setup_raw;

                    bool pressed = touch_core_port_process((uint8_t)ch,
                                                           val,
                                                           diff,
                                                           channels[ch].setup_raw);

                    /* Align GameCommunicator.update_channel_state: raw == 0 forces
                     * pressed, and the last updated channel wins for a region. */
                    uint8_t region =
                        tenodata_config_get_mai2touch_region((uint8_t)ch);

                    if (region < TENODATA_MAI2TOUCH_REGION_COUNT)
                    {
                        region_state[region] =
                            (channels[ch].latest_val == 0) || pressed;
                    }
                }
            }
        }
    }

    update_touch_output();
}

void touch_pipeline_commit_packet(const uint8_t *packet)
{
    if (packet != NULL)
    {
        touch_pipeline_commit_batch(packet, 1U);
    }
}

uint64_t touch_pipeline_get_touch_bits(void)
{
    uint64_t bits = 0U;

    for (uint8_t region = 0U; region < TENODATA_MAI2TOUCH_REGION_COUNT; region++)
    {
        if (region_state[region])
        {
            bits |= 1ULL << region;
        }
    }

    return bits & TENODATA_MAI2TOUCH_VALID_MASK;
}
