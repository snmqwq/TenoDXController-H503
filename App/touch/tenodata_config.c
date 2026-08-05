#include "tenodata_config.h"

#include <stddef.h>
#include <string.h>

#define MAI2TOUCH_BASE_A  0U
#define MAI2TOUCH_BASE_B  8U
#define MAI2TOUCH_BASE_C  16U
#define MAI2TOUCH_BASE_D  18U
#define MAI2TOUCH_BASE_E  26U

#define MAI2TOUCH_ZONE_INDEX(zone, number) \
    (MAI2TOUCH_BASE_##zone + (number) - 1U)

#define MAI2TOUCH_BLOCK_A  'A'
#define MAI2TOUCH_BLOCK_B  'B'
#define MAI2TOUCH_BLOCK_C  'C'
#define MAI2TOUCH_BLOCK_D  'D'
#define MAI2TOUCH_BLOCK_E  'E'

#define DEFAULT_CHANNEL_MAPPING(zone, number)                  \
    {                                                          \
        (uint8_t)MAI2TOUCH_ZONE_INDEX(zone, number),            \
        (uint8_t)MAI2TOUCH_BLOCK_##zone                         \
    }

/*
 * The single source of truth for the default physical-channel mapping:
 * E4,D4,B3,A3,C1,E3,D3,B2,A2,E2,D2,B1,A1,E1,D1,B8,A8,
 * E8,D8,B7,A7,C2,E7,D7,B6,A6,E6,D6,B5,A5,E5,D5,B4,A4.
 */
static TenodataChannelMapping const default_mapping[TENODATA_TOTAL_CHANNELS] =
{
    DEFAULT_CHANNEL_MAPPING(E, 4U),
    DEFAULT_CHANNEL_MAPPING(D, 4U),
    DEFAULT_CHANNEL_MAPPING(B, 3U),
    DEFAULT_CHANNEL_MAPPING(A, 3U),
    DEFAULT_CHANNEL_MAPPING(C, 1U),
    DEFAULT_CHANNEL_MAPPING(E, 3U),
    DEFAULT_CHANNEL_MAPPING(D, 3U),
    DEFAULT_CHANNEL_MAPPING(B, 2U),
    DEFAULT_CHANNEL_MAPPING(A, 2U),
    DEFAULT_CHANNEL_MAPPING(E, 2U),
    DEFAULT_CHANNEL_MAPPING(D, 2U),
    DEFAULT_CHANNEL_MAPPING(B, 1U),
    DEFAULT_CHANNEL_MAPPING(A, 1U),
    DEFAULT_CHANNEL_MAPPING(E, 1U),
    DEFAULT_CHANNEL_MAPPING(D, 1U),
    DEFAULT_CHANNEL_MAPPING(B, 8U),
    DEFAULT_CHANNEL_MAPPING(A, 8U),
    DEFAULT_CHANNEL_MAPPING(E, 8U),
    DEFAULT_CHANNEL_MAPPING(D, 8U),
    DEFAULT_CHANNEL_MAPPING(B, 7U),
    DEFAULT_CHANNEL_MAPPING(A, 7U),
    DEFAULT_CHANNEL_MAPPING(C, 2U),
    DEFAULT_CHANNEL_MAPPING(E, 7U),
    DEFAULT_CHANNEL_MAPPING(D, 7U),
    DEFAULT_CHANNEL_MAPPING(B, 6U),
    DEFAULT_CHANNEL_MAPPING(A, 6U),
    DEFAULT_CHANNEL_MAPPING(E, 6U),
    DEFAULT_CHANNEL_MAPPING(D, 6U),
    DEFAULT_CHANNEL_MAPPING(B, 5U),
    DEFAULT_CHANNEL_MAPPING(A, 5U),
    DEFAULT_CHANNEL_MAPPING(E, 5U),
    DEFAULT_CHANNEL_MAPPING(D, 5U),
    DEFAULT_CHANNEL_MAPPING(B, 4U),
    DEFAULT_CHANNEL_MAPPING(A, 4U)
};

static TenodataChannelMapping configured_mapping[TENODATA_TOTAL_CHANNELS];
static TenodataChannelMapping const *active_mapping = default_mapping;

/* Scan parameters are fixed; mapping.block only selects one of these rows. */
static TenodataChannelConfig const block_profiles[5] =
{
    { 12U, 15U, 2U, 2U }, /* A */
    { 10U, 25U, 4U, 4U }, /* B */
    { 12U, 30U, 4U, 4U }, /* C */
    {  8U, 10U, 2U, 2U }, /* D */
    {  8U,  8U, 2U, 2U }  /* E */
};

_Static_assert(sizeof(TenodataChannelMapping) == 2U,
               "Touch channel mapping wire entry must be two bytes");

static bool block_is_valid(uint8_t block)
{
    return (block >= (uint8_t)'A') && (block <= (uint8_t)'E');
}

static uint8_t block_for_region(uint8_t region)
{
    if (region < MAI2TOUCH_BASE_B)
    {
        return (uint8_t)'A';
    }
    if (region < MAI2TOUCH_BASE_C)
    {
        return (uint8_t)'B';
    }
    if (region < MAI2TOUCH_BASE_D)
    {
        return (uint8_t)'C';
    }
    if (region < MAI2TOUCH_BASE_E)
    {
        return (uint8_t)'D';
    }
    if (region < TENODATA_MAI2TOUCH_REGION_COUNT)
    {
        return (uint8_t)'E';
    }
    return 0U;
}

bool tenodata_config_validate_mapping(TenodataChannelMapping const *mapping,
                                      uint8_t count)
{
    if ((mapping == NULL) || (count != TENODATA_TOTAL_CHANNELS))
    {
        return false;
    }

    for (uint8_t channel = 0U; channel < TENODATA_TOTAL_CHANNELS; channel++)
    {
        uint8_t region = mapping[channel].mai2touch_region;
        uint8_t block = mapping[channel].block;

        if (!block_is_valid(block) ||
            (region >= TENODATA_MAI2TOUCH_REGION_COUNT) ||
            (block != block_for_region(region)))
        {
            return false;
        }
    }

    return true;
}

bool tenodata_config_set_mapping(TenodataChannelMapping const *mapping,
                                 uint8_t count)
{
    if (!tenodata_config_validate_mapping(mapping, count))
    {
        return false;
    }

    memcpy(configured_mapping, mapping, sizeof(configured_mapping));
    active_mapping = configured_mapping;
    return true;
}

bool tenodata_config_get_mapping(TenodataChannelMapping *mapping,
                                 uint8_t count)
{
    if ((mapping == NULL) || (count != TENODATA_TOTAL_CHANNELS))
    {
        return false;
    }

    memcpy(mapping, active_mapping, sizeof(default_mapping));
    return true;
}

bool tenodata_config_get_default_mapping(TenodataChannelMapping *mapping,
                                         uint8_t count)
{
    if ((mapping == NULL) || (count != TENODATA_TOTAL_CHANNELS))
    {
        return false;
    }

    memcpy(mapping, default_mapping, sizeof(default_mapping));
    return true;
}

void tenodata_config_reset_mapping(void)
{
    active_mapping = default_mapping;
}

bool tenodata_config_get_channel_mapping(uint8_t channel,
                                         TenodataChannelMapping *mapping)
{
    if ((channel >= TENODATA_TOTAL_CHANNELS) || (mapping == NULL))
    {
        return false;
    }

    *mapping = active_mapping[channel];
    return true;
}

uint8_t tenodata_config_get_mai2touch_region(uint8_t channel)
{
    if (channel >= TENODATA_TOTAL_CHANNELS)
    {
        return TENODATA_MAI2TOUCH_INVALID_REGION;
    }

    return active_mapping[channel].mai2touch_region;
}

char tenodata_config_get_block(uint8_t channel)
{
    uint8_t block;

    if (channel >= TENODATA_TOTAL_CHANNELS)
    {
        return (char)0xff;
    }

    block = block_for_region(active_mapping[channel].mai2touch_region);
    return block_is_valid(block) ? (char)block : (char)0xff;
}

TenodataChannelConfig tenodata_config_get_channel(uint8_t channel)
{
    TenodataChannelConfig zero = { 0U, 0U, 0U, 0U };
    uint8_t block;

    if (channel >= TENODATA_TOTAL_CHANNELS)
    {
        return zero;
    }

    block = (uint8_t)tenodata_config_get_block(channel);
    if (!block_is_valid(block))
    {
        return zero;
    }

    return block_profiles[block - (uint8_t)'A'];
}

void tenodata_config_get_payload(uint8_t *payload)
{
    if (payload == NULL)
    {
        return;
    }

    for (uint8_t channel = 0U; channel < TENODATA_TOTAL_CHANNELS; channel++)
    {
        TenodataChannelConfig config = tenodata_config_get_channel(channel);

        payload[0U + channel] = config.res;
        payload[34U + channel] = config.mod;
        payload[68U + channel] = config.sns;
        payload[102U + channel] = config.div;
    }
}
