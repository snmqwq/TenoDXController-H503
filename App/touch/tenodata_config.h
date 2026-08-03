#ifndef __TENODATA_CONFIG_H__
#define __TENODATA_CONFIG_H__

#include <stdbool.h>
#include <stdint.h>

#define TENODATA_TOTAL_CHANNELS          34U
#define TENODATA_CHANNELS_PER_DEVICE     17U
#define TENODATA_MAI2TOUCH_REGION_COUNT  34U
#define TENODATA_MAI2TOUCH_MASK_BYTES    5U
#define TENODATA_MAI2TOUCH_VALID_MASK    ((1ULL << TENODATA_MAI2TOUCH_REGION_COUNT) - 1ULL)

/*
 * One physical channel may drive any subset of the 34 Mai2Touch regions.
 * mai2touch_mask is a five-byte little-endian bit mask. block is shared by
 * the local detector and the fixed PSoC scan-profile selector.
 */
typedef struct
{
    uint8_t mai2touch_mask[TENODATA_MAI2TOUCH_MASK_BYTES];
    uint8_t block;
} TenodataChannelMapping;

/* Fixed PSoC scan parameters selected by mapping.block. */
typedef struct
{
    uint8_t res;
    uint8_t mod;
    uint8_t sns;
    uint8_t div;
} TenodataChannelConfig;

uint64_t tenodata_config_mapping_get_mask(
    TenodataChannelMapping const *mapping);
bool tenodata_config_mapping_set_mask(TenodataChannelMapping *mapping,
                                      uint64_t mask);

bool tenodata_config_validate_mapping(TenodataChannelMapping const *mapping,
                                      uint8_t count);
bool tenodata_config_set_mapping(TenodataChannelMapping const *mapping,
                                 uint8_t count);
bool tenodata_config_get_mapping(TenodataChannelMapping *mapping,
                                 uint8_t count);
bool tenodata_config_get_default_mapping(TenodataChannelMapping *mapping,
                                         uint8_t count);
void tenodata_config_reset_mapping(void);

bool tenodata_config_get_channel_mapping(uint8_t channel,
                                         TenodataChannelMapping *mapping);
uint64_t tenodata_config_get_mai2touch_mask(uint8_t channel);
char tenodata_config_get_block(uint8_t channel);

TenodataChannelConfig tenodata_config_get_channel(uint8_t channel);

/* Format: 34 x Res + 34 x Mod + 34 x Sns + 34 x Div. */
void tenodata_config_get_payload(uint8_t *payload);

#endif /* __TENODATA_CONFIG_H__ */
