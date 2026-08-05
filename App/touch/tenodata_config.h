#ifndef __TENODATA_CONFIG_H__
#define __TENODATA_CONFIG_H__

#include <stdbool.h>
#include <stdint.h>

#define TENODATA_TOTAL_CHANNELS          34U
#define TENODATA_CHANNELS_PER_DEVICE     17U
#define TENODATA_MAI2TOUCH_REGION_COUNT  34U
#define TENODATA_MAI2TOUCH_INVALID_REGION 0xffU
#define TENODATA_MAI2TOUCH_VALID_MASK    ((1ULL << TENODATA_MAI2TOUCH_REGION_COUNT) - 1ULL)

/*
 * Every physical channel maps to exactly one Mai2Touch region. Different
 * physical channels may share the same region. block is stored on the wire
 * for display and must match the region's A-E group.
 */
typedef struct
{
    uint8_t mai2touch_region;
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
uint8_t tenodata_config_get_mai2touch_region(uint8_t channel);
char tenodata_config_get_block(uint8_t channel);

TenodataChannelConfig tenodata_config_get_channel(uint8_t channel);

/* Format: 34 x Res + 34 x Mod + 34 x Sns + 34 x Div. */
void tenodata_config_get_payload(uint8_t *payload);

#endif /* __TENODATA_CONFIG_H__ */
