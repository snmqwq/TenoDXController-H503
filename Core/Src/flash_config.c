#include "flash_config.h"

#include "main.h"
#include <string.h>

#define FLASH_CONFIG_MAGIC             0x30474643UL
#define FLASH_CONFIG_FORMAT_VERSION    1U
#define FLASH_CONFIG_QUADWORD_SIZE     16U
#define FLASH_CONFIG_AREA_BASE         (FLASH_BASE + FLASH_SIZE_DEFAULT - FLASH_CONFIG_AREA_SIZE)

typedef struct
{
    uint32_t magic;
    uint16_t version;
    uint16_t length;
    uint32_t slot;
    uint32_t crc32;
} flash_config_header_t;

static uint8_t page_buffer[FLASH_CONFIG_AREA_SIZE] __attribute__((aligned(16)));

static bool slot_is_valid(flash_config_slot_t slot)
{
    return (uint32_t)slot < (uint32_t)FLASH_CONFIG_SLOT_COUNT;
}

uint32_t flash_config_slot_address(flash_config_slot_t slot)
{
    return FLASH_CONFIG_AREA_BASE + ((uint32_t)slot * FLASH_CONFIG_SLOT_SIZE);
}

static uint32_t crc32_update(uint32_t crc, uint8_t data)
{
    crc ^= data;

    for (uint8_t i = 0; i < 8U; i++)
    {
        if ((crc & 1U) != 0U)
        {
            crc = (crc >> 1) ^ 0xedb88320UL;
        }
        else
        {
            crc >>= 1;
        }
    }

    return crc;
}

static uint32_t crc32_calc(uint8_t const *data, uint16_t length)
{
    uint32_t crc = 0xffffffffUL;

    for (uint16_t i = 0; i < length; i++)
    {
        crc = crc32_update(crc, data[i]);
    }

    return ~crc;
}

static void get_erase_target(uint32_t address, uint32_t *bank, uint32_t *sector)
{
    uint32_t bank_offset = address - FLASH_BASE;

    if (bank_offset >= FLASH_BANK_SIZE)
    {
        *bank = FLASH_BANK_2;
        bank_offset -= FLASH_BANK_SIZE;
    }
    else
    {
        *bank = FLASH_BANK_1;
    }

    *sector = bank_offset / FLASH_SECTOR_SIZE;
}

static bool erase_config_area(void)
{
    FLASH_EraseInitTypeDef erase;
    uint32_t sector_error = 0xffffffffUL;
    uint32_t bank;
    uint32_t sector;

    get_erase_target(FLASH_CONFIG_AREA_BASE, &bank, &sector);

    memset(&erase, 0, sizeof(erase));
    erase.TypeErase = FLASH_TYPEERASE_SECTORS;
    erase.Banks = bank;
    erase.Sector = sector;
    erase.NbSectors = 1U;

    return HAL_FLASHEx_Erase(&erase, &sector_error) == HAL_OK;
}

static bool program_config_area(void)
{
    uint32_t address = FLASH_CONFIG_AREA_BASE;

    for (uint32_t offset = 0; offset < FLASH_CONFIG_AREA_SIZE; offset += FLASH_CONFIG_QUADWORD_SIZE)
    {
        bool blank = true;

        for (uint32_t i = 0; i < FLASH_CONFIG_QUADWORD_SIZE; i++)
        {
            if (page_buffer[offset + i] != 0xffU)
            {
                blank = false;
                break;
            }
        }

        if (blank)
        {
            continue;
        }

        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_QUADWORD,
                              address + offset,
                              (uint32_t)&page_buffer[offset]) != HAL_OK)
        {
            return false;
        }
    }

    return true;
}

bool flash_config_read(flash_config_slot_t slot,
                       void *data,
                       uint16_t data_size,
                       uint16_t *out_length)
{
    flash_config_header_t const *header;
    uint8_t const *payload;

    if (!slot_is_valid(slot) || (data == NULL))
    {
        return false;
    }

    header = (flash_config_header_t const *)flash_config_slot_address(slot);
    payload = (uint8_t const *)(flash_config_slot_address(slot) + sizeof(flash_config_header_t));

    if ((header->magic != FLASH_CONFIG_MAGIC) ||
        (header->version != FLASH_CONFIG_FORMAT_VERSION) ||
        (header->slot != (uint32_t)slot) ||
        (header->length > FLASH_CONFIG_SLOT_PAYLOAD) ||
        (header->length > data_size))
    {
        return false;
    }

    if (crc32_calc(payload, header->length) != header->crc32)
    {
        return false;
    }

    memcpy(data, payload, header->length);

    if (out_length != NULL)
    {
        *out_length = header->length;
    }

    return true;
}

bool flash_config_write(flash_config_slot_t slot,
                        void const *data,
                        uint16_t data_length)
{
    flash_config_header_t header;
    uint32_t slot_offset;
    bool ok = false;

    if (!slot_is_valid(slot) || (data == NULL) || (data_length > FLASH_CONFIG_SLOT_PAYLOAD))
    {
        return false;
    }

    memcpy(page_buffer, (void const *)FLASH_CONFIG_AREA_BASE, sizeof(page_buffer));

    slot_offset = (uint32_t)slot * FLASH_CONFIG_SLOT_SIZE;
    memset(&page_buffer[slot_offset], 0xff, FLASH_CONFIG_SLOT_SIZE);

    header.magic = FLASH_CONFIG_MAGIC;
    header.version = FLASH_CONFIG_FORMAT_VERSION;
    header.length = data_length;
    header.slot = (uint32_t)slot;
    header.crc32 = crc32_calc((uint8_t const *)data, data_length);

    memcpy(&page_buffer[slot_offset], &header, sizeof(header));
    memcpy(&page_buffer[slot_offset + sizeof(header)], data, data_length);

    if (HAL_FLASH_Unlock() == HAL_OK)
    {
        ok = erase_config_area() && program_config_area();
        (void)HAL_FLASH_Lock();
    }

    return ok;
}

bool flash_config_clear(flash_config_slot_t slot)
{
    uint32_t slot_offset;
    bool ok = false;

    if (!slot_is_valid(slot))
    {
        return false;
    }

    memcpy(page_buffer, (void const *)FLASH_CONFIG_AREA_BASE, sizeof(page_buffer));

    slot_offset = (uint32_t)slot * FLASH_CONFIG_SLOT_SIZE;
    memset(&page_buffer[slot_offset], 0xff, FLASH_CONFIG_SLOT_SIZE);

    if (HAL_FLASH_Unlock() == HAL_OK)
    {
        ok = erase_config_area() && program_config_area();
        (void)HAL_FLASH_Lock();
    }

    return ok;
}
