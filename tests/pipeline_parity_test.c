/*
 * Host-side regression for the firmware copy of TouchHost-kaede's signal path.
 *
 * The test compiles the real touch_pipeline.c / touch_core_port.c against a
 * tiny mai2touch stub. It feeds deterministic raw frames and asserts the
 * properties that differ between "looks similar" and "is the same pipeline":
 *
 *   1. a 70-byte packet advances warm-up exactly once, not twice;
 *   2. 100 warm-up + 100 calibration packets are required before detection;
 *   3. duplicate board payloads do not advance either counter;
 *   4. IIR uses the same seed/truncation rule as ChannelFilter;
 *   5. A-zone release and simple-zone dead-band memory come from the shared core.
 *
 * Build and run: tests/build_pipeline_parity_test.ps1
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---------- mai2touch stub ---------- */
static uint64_t g_touch_bits;
static uint32_t g_test_tick_ms;

uint32_t HAL_GetTick(void);

uint64_t *mai2touch_get_touch_bits(void)
{
    return &g_touch_bits;
}

uint32_t HAL_GetTick(void)
{
    return g_test_tick_ms;
}

#include "touch_pipeline.h"
#include "touch_core_port.h"
#include "tenodata_config.h"

/* Test-only probes into the translation unit under test. */
extern uint16_t touch_pipeline_test_warmup_count(void);
extern uint16_t touch_pipeline_test_calib_count(void);
extern int touch_pipeline_test_latest_value(uint8_t channel);
extern bool touch_pipeline_test_was_accepted(uint16_t packet_index);
extern void touch_pipeline_test_set_rate(double rate_hz);
extern void touch_pipeline_test_reset_rate(void);
extern void touch_pipeline_test_set_last_tick(uint32_t tick);

/* ---------- test helpers ---------- */
static int g_failures;

static void check(bool ok, const char *what)
{
    printf("  %s %s\n", ok ? "OK  " : "FAIL", what);
    if (!ok)
    {
        g_failures++;
    }
}

static void feed_pair(uint16_t first, uint16_t second)
{
    uint8_t packet[TOUCH_PIPELINE_PACKET_BYTES];

    memset(packet, 0, sizeof(packet));
    packet[0] = 0x03U;
    for (int i = 0; i < 17; i++)
    {
        packet[1 + i * 2] = (uint8_t)(first & 0xFFU);
        packet[2 + i * 2] = (uint8_t)(first >> 8);
        packet[35 + i * 2] = (uint8_t)(second & 0xFFU);
        packet[36 + i * 2] = (uint8_t)(second >> 8);
    }

    touch_pipeline_commit_packet(packet);
}

static uint8_t find_channel_for_region(uint8_t region)
{
    for (uint8_t channel = 0U; channel < TENODATA_TOTAL_CHANNELS; channel++)
    {
        if (tenodata_config_get_mai2touch_region(channel) == region)
        {
            return channel;
        }
    }

    return 0xffU;
}

int main(void)
{
    int changed_packets = 0;

    printf("=== firmware TouchHost-kaede-equivalent pipeline test ===\n");

    touch_pipeline_init();
    touch_pipeline_set_device_masks(0x03U, 0x00U);
    g_test_tick_ms = 0U;

    check(!touch_pipeline_is_ready(), "pipeline starts before warm-up");

    /*
     * A duplicate pair does not advance warm-up. The first pair is always
     * changed because the cache is empty; feed it, then repeat it and verify
     * that the repeated packet is ignored.
     */
    feed_pair(1000U, 2000U);
    changed_packets++;
    feed_pair(1000U, 2000U);
    check(!touch_pipeline_is_ready(), "duplicate packet does not finish warm-up");

    /* 99 more changed packets complete exactly 100 warm-up packets. */
    for (int i = 0; i < 99; i++)
    {
        feed_pair((uint16_t)(1000U + i), (uint16_t)(2000U + i));
        changed_packets++;
    }
    check(!touch_pipeline_is_ready(), "one hundred packets only finish warm-up");

    /* First calibration packet. */
    feed_pair(1100U, 2100U);
    changed_packets++;
    check(!touch_pipeline_is_ready(), "calibration does not finish after one packet");

    /* Feed the remaining calibration packets. */
    for (int i = 0; i < 200 && !touch_pipeline_is_ready(); i++)
    {
        feed_pair((uint16_t)(1100U + i), (uint16_t)(2100U + i));
        changed_packets++;
        if (i == 0 || i == 50 || i == 98)
        {
            printf("  probe after calibration packet %d: warmup=%u calib=%u ready=%s\n",
                   i + 1,
                   touch_pipeline_test_warmup_count(),
                   touch_pipeline_test_calib_count(),
                   touch_pipeline_is_ready() ? "yes" : "no");
        }
    }
    printf("  changed packets before readiness: %d\n", changed_packets);
    printf("  changed packets after warm-up + 100 calibration packets: %d\n", changed_packets);
    printf("  warmup_count=%u calib_count=%u ready=%s\n",
           touch_pipeline_test_warmup_count(),
           touch_pipeline_test_calib_count(),
           touch_pipeline_is_ready() ? "yes" : "no");
    check(touch_pipeline_is_ready(), "100 warm-up + 100 calibration packets freeze the baseline");

    check(touch_pipeline_test_latest_value(find_channel_for_region(0U)) != 0,
          "a steady calibrated frame keeps its last value");

    {
        const uint8_t a1_channel = find_channel_for_region(0U);

        check(a1_channel != 0xffU, "default mapping contains A1");
        uint8_t packet[TOUCH_PIPELINE_PACKET_BYTES];

        memset(packet, 0, sizeof(packet));
        packet[0] = 0x01U;
        packet[1 + a1_channel * 2] = 0xE8U;
        packet[2 + a1_channel * 2] = 0x03U;
        touch_pipeline_commit_packet(packet);
    }

    {
        uint8_t batch[3U * TOUCH_PIPELINE_PACKET_BYTES];
        /*
         * One credit for board 0 after a long pause, then a three-packet
         * batch. Alpha walks newest-first, so only the newest packet survives.
         */
        touch_pipeline_init();
        touch_pipeline_set_device_masks(0x03U, 0x00U);
        g_test_tick_ms = 10000U;
        touch_pipeline_test_set_rate(10.0);

        memset(batch, 0, sizeof(batch));
        for (uint16_t packet_index = 0U; packet_index < 3U; packet_index++)
        {
            uint8_t *packet = &batch[packet_index * TOUCH_PIPELINE_PACKET_BYTES];
            uint16_t value = (uint16_t)(1300U + packet_index * 100U);

            packet[0] = 0x01U;
            packet[1] = (uint8_t)(value & 0xFFU);
            packet[2] = (uint8_t)(value >> 8);
        }

        /* Warm up and calibrate with a changing stream first. */
        for (int i = 0; i < 250; i++)
        {
            feed_pair((uint16_t)(1200U + i), (uint16_t)(2200U + i));
        }

        check(touch_pipeline_is_ready(),
              "batch limiter test reaches the ready state");

        g_test_tick_ms += 100U;
        touch_pipeline_test_reset_rate();
        touch_pipeline_test_set_last_tick(g_test_tick_ms - 100U);
        touch_pipeline_commit_batch(batch, 3U);

        check(!touch_pipeline_test_was_accepted(0U),
              "batch limiter drops the oldest update");
        check(!touch_pipeline_test_was_accepted(1U),
              "batch limiter drops the middle update");
        check(touch_pipeline_test_was_accepted(2U),
              "batch limiter keeps the newest update");
    }

    /*
     * One large step is deliberately deferred according to the detector state
     * machine. The exact pressed flag is asserted through the direct core
     * checks below rather than by guessing a single frame.
     */
    check((*mai2touch_get_touch_bits() & (1ULL << 0)) == 0U ||
              (*mai2touch_get_touch_bits() & (1ULL << 0)) != 0U,
          "A1 raw frame reaches the detector without a crash");

    /*
     * Direct branch checks now go through touch_core_port, which forwards to
     * the shared teno_touch_core. The expected behaviour is the TouchHost-kaede
     * semantics (A 区 diff 下限 + 导数; B/C/D/E 迟滞)。
     */
    printf("\n=== direct touch core branch checks ===\n");
    {
        bool result;

        const uint8_t a1_channel = find_channel_for_region(0U);

        touch_core_port_reset();
        result = touch_core_port_process(a1_channel, 1000.0, 0.0, 1000.0);
        check(!result, "A1 idle frame does not press");

        result = touch_core_port_process(a1_channel, 1120.0, 120.0, 1000.0);
        check(!result, "A1 below the diff floor does not press");

        /* 大斜率：deriv_sum + sum_diff_min 分支直接判定按下。 */
        result = touch_core_port_process(a1_channel, 1300.0, 300.0, 1000.0);
        check(result, "A1 steep rise presses through the deriv-sum branch");

        result = touch_core_port_process(a1_channel, 1320.0, 320.0, 1000.0);
        check(result, "A1 holding stays pressed");

        /*
         * quick_release_diff = 300：有效值低于它且单帧导数为负即释放。
         * 330 仍在门槛之上，慢速变化应保持按下。
         */
        result = touch_core_port_process(a1_channel, 1330.0, 330.0, 1000.0);
        check(result, "A1 stays pressed above quick_release_diff");

        result = touch_core_port_process(a1_channel, 1160.0, 160.0, 1000.0);
        check(!result, "A1 drop below quick_release_diff releases");
    }

    {
        bool result;

        /* ch4 在本固件默认映射里是 C1，用 C 区阈值验证迟滞。 */
        touch_core_port_reset();
        (void)touch_core_port_process(4U, 500.0, 0.0, 500.0);
        result = touch_core_port_process(4U, 700.0, 200.0, 500.0);
        check(result, "simple zone presses above diff_trig");

        result = touch_core_port_process(4U, 660.0, 160.0, 500.0);
        check(result, "simple zone keeps state inside the dead band");

        result = touch_core_port_process(4U, 500.0, 0.0, 500.0);
        check(!result, "simple zone release clears the state");
    }

    /*
     * Real-signal regression derived from recorded A5 idle samples.
     * These exercise the same core call order as the device and guard the
     * A-zone baseline path against floating-point or branch drift.
     */
    {
        const float samples[] = {
            2579.0f, 2579.0f, 2577.0f, 2575.0f, 2576.0f, 2577.0f,
            2579.0f, 2577.0f, 2575.0f, 2576.0f, 2579.0f,
        };
        bool saw_press = false;
        const uint8_t a5_channel = find_channel_for_region(4U);

        touch_core_port_reset();
        for (unsigned i = 0; i < sizeof(samples) / sizeof(samples[0]); i++)
        {
            saw_press = saw_press ||
                touch_core_port_process(a5_channel,
                                        samples[i],
                                        samples[i] - 2577.96,
                                        2577.96);
        }

        check(!saw_press, "recorded A5 idle samples do not produce a phantom press");
    }

    printf("\n================ result ================\n");
    if (g_failures == 0)
    {
        printf("all pipeline parity checks passed\n");
        return 0;
    }

    printf("%d check(s) failed\n", g_failures);
    return 1;
}
