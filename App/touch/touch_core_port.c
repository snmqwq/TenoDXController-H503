#include "touch_core_port.h"

#include "teno_touch_core.h"
#include "tenodata_config.h"

#include <string.h>

static TenoTouchCore core;
static TenoTouchDecision decisions[TENO_TOUCH_CHANNELS];
static bool initialized;

/*
 * 把固件的 block 字符 (A/B/C/D/E) 映射成核心分组。
 * 固件的硬件映射是可配置的，所以分组必须从 active mapping 派生，
 * 不能用核心里的 kaede 默认表。
 */
static uint8_t group_from_block(char block)
{
    switch (block)
    {
    case 'A':
        return TENO_TOUCH_GROUP_A;
    case 'B':
        return TENO_TOUCH_GROUP_B;
    case 'C':
        return TENO_TOUCH_GROUP_C;
    case 'D':
        return TENO_TOUCH_GROUP_D;
    case 'E':
        return TENO_TOUCH_GROUP_E;
    default:
        return TENO_TOUCH_GROUP_NONE;
    }
}

static void apply_channel_map(TenoTouchParams *params)
{
    uint8_t channel;

    for (channel = 0U; channel < TENO_TOUCH_CHANNELS; ++channel)
    {
        params->channel_group[channel] =
            group_from_block(tenodata_config_get_block(channel));
    }
}

void touch_core_port_reset(void)
{
    TenoTouchParams params;

    teno_touch_params_defaults(&params);
    apply_channel_map(&params);

    teno_touch_core_init(&core, &params);
    memset(decisions, 0, sizeof(decisions));
    initialized = true;
}

void touch_core_port_reset_channel(uint8_t channel)
{
    if (!initialized)
    {
        touch_core_port_reset();
    }

    if (channel >= TENO_TOUCH_CHANNELS)
    {
        return;
    }

    teno_touch_core_reset_channel(&core, channel);
    memset(&decisions[channel], 0, sizeof(decisions[channel]));
    decisions[channel].baseline = 0.0;
}

bool touch_core_port_process(uint8_t channel,
                             double raw,
                             double diff,
                             double setup_raw)
{
    if (!initialized)
    {
        touch_core_port_reset();
    }

    if (channel >= TENO_TOUCH_CHANNELS)
    {
        return false;
    }

    if (teno_touch_core_process(&core, channel, raw, diff, setup_raw,
                                &decisions[channel]) < 0)
    {
        return false;
    }

    return decisions[channel].pressed != 0U;
}

uint32_t touch_core_port_abi_version(void)
{
    return TENO_TOUCH_ABI_VERSION;
}

uint32_t touch_core_port_algorithm_version(void)
{
    return TENO_TOUCH_ALGORITHM_VERSION;
}

uint32_t touch_core_port_params_hash(void)
{
    return teno_touch_params_hash(&core.params);
}

uint32_t touch_core_port_features(void)
{
    return teno_touch_core_features();
}

int32_t touch_core_port_set_zone_tuning(uint8_t group,
                                        double diff_trig,
                                        double deriv_trig,
                                        double deriv_rel,
                                        double diff_rel)
{
    if (!initialized)
    {
        touch_core_port_reset();
    }

    return teno_touch_core_set_zone_tuning(&core, group, diff_trig, deriv_trig,
                                           deriv_rel, diff_rel);
}

void touch_core_port_reset_zone(uint8_t group)
{
    uint8_t channel;

    if (!initialized)
    {
        touch_core_port_reset();
    }

    teno_touch_core_reset_zone(&core, group);

    /* 核心只清通道状态；本层缓存的输出也要跟着清，避免读到上一帧。 */
    for (channel = 0U; channel < TENO_TOUCH_CHANNELS; ++channel)
    {
        if (core.params.channel_group[channel] == group)
        {
            memset(&decisions[channel], 0, sizeof(decisions[channel]));
        }
    }
}

const TenoTouchDecision *touch_core_port_decision(uint8_t channel)
{
    if (channel >= TENO_TOUCH_CHANNELS)
    {
        return NULL;
    }

    return &decisions[channel];
}
