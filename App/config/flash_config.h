#ifndef __FLASH_CONFIG_H__
#define __FLASH_CONFIG_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#define FLASH_CONFIG_AREA_SIZE       0x2000U

typedef enum
{
    FLASH_CONFIG_SLOT_TOUCH = 0,
    FLASH_CONFIG_SLOT_LIGHT = 1,
    FLASH_CONFIG_SLOT_RESERVED = 2,
    FLASH_CONFIG_SLOT_KEYBOARD = 3,
    FLASH_CONFIG_SLOT_COUNT = 4
} flash_config_slot_t;

bool flash_config_read(flash_config_slot_t slot,
                       void *data,
                       uint16_t data_size,
                       uint16_t *out_length);
bool flash_config_write(flash_config_slot_t slot,
                        void const *data,
                        uint16_t data_length);
bool flash_config_clear(flash_config_slot_t slot);

/* Group several slot updates into one atomic rolling record. */
bool flash_config_transaction_begin(void);
bool flash_config_transaction_commit(void);
void flash_config_transaction_abort(void);

#ifdef __cplusplus
}
#endif

#endif /* __FLASH_CONFIG_H__ */
