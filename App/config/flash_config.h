#ifndef __FLASH_CONFIG_H__
#define __FLASH_CONFIG_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#define FLASH_CONFIG_AREA_SIZE       0x2000U
#define FLASH_CONFIG_SLOT_SIZE       0x0800U
#define FLASH_CONFIG_SLOT_PAYLOAD    (FLASH_CONFIG_SLOT_SIZE - 16U)

typedef enum
{
    FLASH_CONFIG_SLOT_TOUCH = 0,
    FLASH_CONFIG_SLOT_LIGHT,
    FLASH_CONFIG_SLOT_READER,
    FLASH_CONFIG_SLOT_KEYBOARD,
    FLASH_CONFIG_SLOT_COUNT
} flash_config_slot_t;

bool flash_config_read(flash_config_slot_t slot,
                       void *data,
                       uint16_t data_size,
                       uint16_t *out_length);
bool flash_config_write(flash_config_slot_t slot,
                        void const *data,
                        uint16_t data_length);
bool flash_config_clear(flash_config_slot_t slot);
uint32_t flash_config_slot_address(flash_config_slot_t slot);

#ifdef __cplusplus
}
#endif

#endif /* __FLASH_CONFIG_H__ */
