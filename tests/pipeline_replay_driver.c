/*
 * Full pipeline replay driver for the STM32 touch algorithm.
 *
 * Reads the same frame file as the melonloader parity harness:
 *   frame,channel,area,raw,setup_raw
 *
 * Each physical frame becomes one packet consumed by the production
 * touch_pipeline_commit_batch(): one validity byte followed by two 34-byte
 * little-endian board payloads. The CSV carries post-IIR values, so this
 * validates warm-up, calibration, board gating and the detector, not the
 * raw-byte IIR stage.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "touch_pipeline.h"
#include "tenodata_config.h"

uint32_t HAL_GetTick(void);
void touch_pipeline_test_set_iir_factor(double factor);
void touch_pipeline_test_set_rate(double rate_hz);
void touch_pipeline_test_set_force_changed(bool enabled);
bool touch_pipeline_test_emitted(void);

static uint64_t g_touch_bits;
static uint32_t g_tick_ms;

uint64_t *mai2touch_get_touch_bits(void)
{
    return &g_touch_bits;
}

uint32_t HAL_GetTick(void)
{
    return g_tick_ms;
}

typedef struct {
    int frame;
    int channel;
    char area[8];
    double raw;
    double setup_raw;
} InputRow;

int main(int argc, char **argv)
{
    FILE *input;
    char line[512];
    InputRow *rows = NULL;
    size_t row_count = 0U;
    size_t row_capacity = 0U;
    size_t frame_count = 0U;
    uint16_t channel_cache[TENODATA_TOTAL_CHANNELS];

    if (argc != 2)
    {
        fprintf(stderr, "usage: %s <frames.csv>\n", argv[0]);
        return 2;
    }

    input = fopen(argv[1], "r");
    if (input == NULL)
    {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }

    if (fgets(line, sizeof(line), input) == NULL)
    {
        fclose(input);
        return 2;
    }

    while (fgets(line, sizeof(line), input) != NULL)
    {
        InputRow row;

        if (sscanf(line, "%d,%d,%7[^,],%lf,%lf",
                   &row.frame,
                   &row.channel,
                   row.area,
                   &row.raw,
                   &row.setup_raw) != 5)
        {
            continue;
        }

        if (row_count == row_capacity)
        {
            row_capacity = row_capacity == 0U ? 4096U : row_capacity * 2U;
            rows = (InputRow *)realloc(rows, row_capacity * sizeof(*rows));
            if (rows == NULL)
            {
                fclose(input);
                return 2;
            }
        }

        rows[row_count++] = row;
    }

    fclose(input);

    touch_pipeline_init();
    touch_pipeline_set_device_masks(0x03U, 0x00U);
    touch_pipeline_test_set_iir_factor(1.0);
    touch_pipeline_test_set_rate(0.0);
    touch_pipeline_test_set_force_changed(true);
    g_tick_ms = 0U;
    memset(channel_cache, 0, sizeof(channel_cache));

    puts("frame,channel,area,raw,setup_raw,decision,baseline,eff,deriv_eff,deriv2_eff,algo_deriv,algo_deriv2,fsm,touched,relmin,large_latch,pre_judge");

    for (size_t row_index = 0U; row_index < row_count; )
    {
        int frame = rows[row_index].frame;
        size_t next = row_index;

        g_tick_ms = (uint32_t)(frame * 5U);

        {
            uint8_t packet[TOUCH_PIPELINE_PACKET_BYTES];

            memset(packet, 0, sizeof(packet));

            while ((next < row_count) && (rows[next].frame == frame))
            {
                const InputRow *row = &rows[next];

                if ((row->channel >= 0) &&
                    (row->channel < (int)TENODATA_TOTAL_CHANNELS))
                {
                    packet[0] |= (uint8_t)(1U << (row->channel / 17));
                    channel_cache[row->channel] = (uint16_t)row->raw;
                }

                next++;
            }

            for (uint8_t board = 0U; board < 2U; board++)
            {
                uint8_t *payload =
                    &packet[TOUCH_PIPELINE_PACKET_MASK_BYTES + board * 34U];

                for (int local = 0; local < 17; local++)
                {
                    uint16_t raw = channel_cache[board * 17U + local];
                    payload[local * 2] = (uint8_t)(raw & 0xFFU);
                    payload[local * 2 + 1] = (uint8_t)(raw >> 8);
                }
            }

            touch_pipeline_commit_packet(packet);
        }

        frame_count++;

        for (size_t output_index = row_index; output_index < next; output_index++)
        {
            const InputRow *row = &rows[output_index];
            TouchPipelineSnapshot snapshot;

            if ((row->channel < 0) ||
                (row->channel >= (int)TENODATA_TOTAL_CHANNELS))
            {
                continue;
            }

            if (touch_pipeline_test_emitted() &&
                touch_pipeline_snapshot((uint8_t)row->channel, &snapshot))
            {
                printf("%d,%d,%s,%.6f,%.6f,%d,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%d,%.6f,",
                       row->frame,
                       row->channel,
                       row->area,
                       (double)snapshot.latest_value,
                       row->setup_raw,
                       snapshot.pressed ? 1 : 0,
                       snapshot.baseline,
                       snapshot.effective_diff,
                       snapshot.deriv_eff,
                       snapshot.deriv2_eff,
                       snapshot.algo_deriv,
                       snapshot.algo_deriv2,
                       snapshot.fsm_state,
                       snapshot.touched_effective_diff);

                if (snapshot.released_min_valid)
                {
                    printf("%.6f,", snapshot.released_min_diff);
                }
                else
                {
                    printf(",");
                }

                printf("%d,%.6f\n",
                       snapshot.large_diff_latched,
                       snapshot.pre_judge_deriv);
            }
        }

        row_index = next;
    }

    free(rows);
    return 0;
}
