#include "flash_config.h"

#include "main.h"
#include <stddef.h>
#include <string.h>

#if defined(__GNUC__)
#pragma GCC optimize ("Os")
#endif

#define FLASH_CONFIG_RECORD_MAGIC          0x31474643UL
#define FLASH_CONFIG_RECORD_VERSION        2U
#define FLASH_CONFIG_RECORD_SIZE           128U
#define FLASH_CONFIG_RECORD_HEADER_SIZE    16U
#define FLASH_CONFIG_RECORD_COUNT \
    (FLASH_CONFIG_AREA_SIZE / FLASH_CONFIG_RECORD_SIZE)
#define FLASH_CONFIG_QUADWORD_SIZE         16U
#define FLASH_CONFIG_AREA_BASE \
    (FLASH_BASE + FLASH_SIZE_DEFAULT - FLASH_CONFIG_AREA_SIZE)

#define FLASH_CONFIG_TOUCH_CAPACITY        70U
#define FLASH_CONFIG_LIGHT_CAPACITY        20U
#define FLASH_CONFIG_KEYBOARD_CAPACITY      8U

#define FLASH_CONFIG_LEGACY_MAGIC          0x30474643UL
#define FLASH_CONFIG_LEGACY_VERSION        1U
#define FLASH_CONFIG_LEGACY_SLOT_SIZE      0x0800U

typedef struct
{
    uint32_t magic;
    uint16_t version;
    uint16_t record_size;
    uint32_t sequence;
    uint32_t crc32;
} flash_config_record_header_t;

typedef struct
{
    flash_config_record_header_t header;
    uint16_t lengths[FLASH_CONFIG_SLOT_COUNT];
    uint8_t touch[FLASH_CONFIG_TOUCH_CAPACITY];
    uint8_t light[FLASH_CONFIG_LIGHT_CAPACITY];
    uint8_t keyboard[FLASH_CONFIG_KEYBOARD_CAPACITY];
    uint8_t reserved[6U];
} flash_config_record_t;

typedef struct
{
    uint32_t magic;
    uint16_t version;
    uint16_t length;
    uint32_t slot;
    uint32_t crc32;
} flash_config_legacy_header_t;

_Static_assert(sizeof(flash_config_record_header_t) ==
                   FLASH_CONFIG_RECORD_HEADER_SIZE,
               "Flash config record header must be 16 bytes");
_Static_assert(sizeof(flash_config_record_t) == FLASH_CONFIG_RECORD_SIZE,
               "Flash config rolling record must be 128 bytes");
_Static_assert((FLASH_CONFIG_AREA_SIZE % FLASH_CONFIG_RECORD_SIZE) == 0U,
               "Flash config area must contain whole records");

static flash_config_record_t cached_record;
static flash_config_record_t backup_record;
static flash_config_record_t write_record __attribute__((aligned(16)));
static uint32_t latest_sequence;
static uint16_t latest_record_index;
static bool cache_loaded;
static bool has_rolling_record;
static bool transaction_active;
static bool transaction_dirty;

static uint32_t record_address(uint16_t record_index)
{
    return FLASH_CONFIG_AREA_BASE +
           ((uint32_t)record_index * FLASH_CONFIG_RECORD_SIZE);
}

static uint16_t slot_capacity(flash_config_slot_t slot)
{
    switch (slot)
    {
        case FLASH_CONFIG_SLOT_TOUCH:
            return FLASH_CONFIG_TOUCH_CAPACITY;

        case FLASH_CONFIG_SLOT_LIGHT:
            return FLASH_CONFIG_LIGHT_CAPACITY;

        case FLASH_CONFIG_SLOT_KEYBOARD:
            return FLASH_CONFIG_KEYBOARD_CAPACITY;

        default:
            return 0U;
    }
}

static uint8_t *slot_data(flash_config_record_t *record,
                          flash_config_slot_t slot)
{
    switch (slot)
    {
        case FLASH_CONFIG_SLOT_TOUCH:
            return record->touch;

        case FLASH_CONFIG_SLOT_LIGHT:
            return record->light;

        case FLASH_CONFIG_SLOT_KEYBOARD:
            return record->keyboard;

        default:
            return NULL;
    }
}

static bool slot_is_valid(flash_config_slot_t slot)
{
    return slot_capacity(slot) != 0U;
}

static uint32_t crc32_update(uint32_t crc, uint8_t data)
{
    crc ^= data;

    for (uint8_t i = 0U; i < 8U; i++)
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

    for (uint16_t i = 0U; i < length; i++)
    {
        crc = crc32_update(crc, data[i]);
    }

    return ~crc;
}

static uint32_t record_crc32(flash_config_record_t const *record)
{
    return crc32_calc(((uint8_t const *)record) +
                          FLASH_CONFIG_RECORD_HEADER_SIZE,
                      FLASH_CONFIG_RECORD_SIZE -
                          FLASH_CONFIG_RECORD_HEADER_SIZE);
}

static bool record_lengths_are_valid(flash_config_record_t const *record)
{
    return (record->lengths[FLASH_CONFIG_SLOT_TOUCH] <=
                FLASH_CONFIG_TOUCH_CAPACITY) &&
           (record->lengths[FLASH_CONFIG_SLOT_LIGHT] <=
                FLASH_CONFIG_LIGHT_CAPACITY) &&
           (record->lengths[FLASH_CONFIG_SLOT_RESERVED] == 0U) &&
           (record->lengths[FLASH_CONFIG_SLOT_KEYBOARD] <=
                FLASH_CONFIG_KEYBOARD_CAPACITY);
}

static bool record_is_valid(flash_config_record_t const *record)
{
    return (record->header.magic == FLASH_CONFIG_RECORD_MAGIC) &&
           (record->header.version == FLASH_CONFIG_RECORD_VERSION) &&
           (record->header.record_size == FLASH_CONFIG_RECORD_SIZE) &&
           (record->header.sequence != 0U) &&
           record_lengths_are_valid(record) &&
           (record_crc32(record) == record->header.crc32);
}

static bool sequence_is_newer(uint32_t candidate, uint32_t current)
{
    return (int32_t)(candidate - current) > 0;
}

static bool memory_is_erased(uint8_t const *data, uint16_t length)
{
    for (uint16_t i = 0U; i < length; i++)
    {
        if (data[i] != 0xffU)
        {
            return false;
        }
    }

    return true;
}

static bool record_slot_is_erased(uint16_t record_index)
{
    return memory_is_erased((uint8_t const *)record_address(record_index),
                            FLASH_CONFIG_RECORD_SIZE);
}

static void reset_cached_record(void)
{
    memset(&cached_record, 0xff, sizeof(cached_record));
    memset(cached_record.lengths, 0, sizeof(cached_record.lengths));
    cached_record.header.magic = FLASH_CONFIG_RECORD_MAGIC;
    cached_record.header.version = FLASH_CONFIG_RECORD_VERSION;
    cached_record.header.record_size = FLASH_CONFIG_RECORD_SIZE;
    cached_record.header.sequence = 0U;
    cached_record.header.crc32 = 0U;
}

static bool load_legacy_slot(flash_config_slot_t slot)
{
    uint16_t capacity = slot_capacity(slot);
    uint32_t address = FLASH_CONFIG_AREA_BASE +
                       ((uint32_t)slot * FLASH_CONFIG_LEGACY_SLOT_SIZE);
    flash_config_legacy_header_t const *header =
        (flash_config_legacy_header_t const *)address;
    uint8_t const *payload =
        (uint8_t const *)(address + sizeof(flash_config_legacy_header_t));
    uint8_t *destination = slot_data(&cached_record, slot);

    if ((capacity == 0U) || (destination == NULL) ||
        (header->magic != FLASH_CONFIG_LEGACY_MAGIC) ||
        (header->version != FLASH_CONFIG_LEGACY_VERSION) ||
        (header->slot != (uint32_t)slot) ||
        (header->length == 0U) || (header->length > capacity) ||
        (crc32_calc(payload, header->length) != header->crc32))
    {
        return false;
    }

    memset(destination, 0xff, capacity);
    memcpy(destination, payload, header->length);
    cached_record.lengths[slot] = header->length;
    return true;
}

static void load_cache(void)
{
    bool found = false;

    reset_cached_record();
    latest_sequence = 0U;
    latest_record_index = 0U;

    for (uint16_t index = 0U;
         index < FLASH_CONFIG_RECORD_COUNT;
         index++)
    {
        flash_config_record_t const *candidate =
            (flash_config_record_t const *)record_address(index);

        if (record_is_valid(candidate) &&
            (!found ||
             sequence_is_newer(candidate->header.sequence,
                               latest_sequence)))
        {
            memcpy(&cached_record, candidate, sizeof(cached_record));
            latest_sequence = candidate->header.sequence;
            latest_record_index = index;
            found = true;
        }
    }

    has_rolling_record = found;
    if (!found)
    {
        (void)load_legacy_slot(FLASH_CONFIG_SLOT_TOUCH);
        (void)load_legacy_slot(FLASH_CONFIG_SLOT_LIGHT);
        (void)load_legacy_slot(FLASH_CONFIG_SLOT_KEYBOARD);
    }

    cache_loaded = true;
}

static void ensure_cache_loaded(void)
{
    if (!cache_loaded)
    {
        load_cache();
    }
}

static void get_erase_target(uint32_t address,
                             uint32_t *bank,
                             uint32_t *sector)
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

static int32_t find_erased_record(void)
{
    uint16_t start = has_rolling_record ?
                     (uint16_t)((latest_record_index + 1U) %
                                FLASH_CONFIG_RECORD_COUNT) :
                     0U;

    for (uint16_t offset = 0U;
         offset < FLASH_CONFIG_RECORD_COUNT;
         offset++)
    {
        uint16_t index =
            (uint16_t)((start + offset) % FLASH_CONFIG_RECORD_COUNT);

        if (record_slot_is_erased(index))
        {
            return (int32_t)index;
        }
    }

    return -1;
}

static bool program_record(uint16_t record_index, bool erase_first)
{
    uint32_t address = record_address(record_index);
    bool ok = false;

    if (HAL_FLASH_Unlock() != HAL_OK)
    {
        return false;
    }

    ok = !erase_first || erase_config_area();

    /* Program the payload first. The first quadword contains the magic and is
     * written last, so an interrupted write can never become a valid record.
     */
    for (uint16_t offset = FLASH_CONFIG_RECORD_HEADER_SIZE;
         ok && (offset < FLASH_CONFIG_RECORD_SIZE);
         offset += FLASH_CONFIG_QUADWORD_SIZE)
    {
        uint8_t const *quadword = ((uint8_t const *)&write_record) + offset;

        if (!memory_is_erased(quadword, FLASH_CONFIG_QUADWORD_SIZE))
        {
            ok = HAL_FLASH_Program(
                     FLASH_TYPEPROGRAM_QUADWORD,
                     address + offset,
                     (uint32_t)(uintptr_t)quadword) == HAL_OK;
        }
    }

    if (ok)
    {
        ok = HAL_FLASH_Program(
                 FLASH_TYPEPROGRAM_QUADWORD,
                 address,
                 (uint32_t)(uintptr_t)&write_record) == HAL_OK;
    }

    (void)HAL_FLASH_Lock();

    return ok && record_is_valid(
                     (flash_config_record_t const *)address);
}

static bool append_cached_record(void)
{
    int32_t erased_index = find_erased_record();
    bool erase_first = erased_index < 0;
    uint16_t record_index = erase_first ? 0U : (uint16_t)erased_index;
    uint32_t next_sequence = latest_sequence + 1U;

    if (next_sequence == 0U)
    {
        next_sequence = 1U;
    }

    memcpy(&write_record, &cached_record, sizeof(write_record));
    write_record.header.magic = FLASH_CONFIG_RECORD_MAGIC;
    write_record.header.version = FLASH_CONFIG_RECORD_VERSION;
    write_record.header.record_size = FLASH_CONFIG_RECORD_SIZE;
    write_record.header.sequence = next_sequence;
    write_record.header.crc32 = record_crc32(&write_record);

    if (!program_record(record_index, erase_first))
    {
        return false;
    }

    memcpy(&cached_record, &write_record, sizeof(cached_record));
    latest_sequence = next_sequence;
    latest_record_index = record_index;
    has_rolling_record = true;
    return true;
}

bool flash_config_read(flash_config_slot_t slot,
                       void *data,
                       uint16_t data_size,
                       uint16_t *out_length)
{
    uint16_t length;
    uint8_t *source;

    if (out_length != NULL)
    {
        *out_length = 0U;
    }

    if (!slot_is_valid(slot) || (data == NULL))
    {
        return false;
    }

    ensure_cache_loaded();
    length = cached_record.lengths[slot];
    source = slot_data(&cached_record, slot);

    if ((source == NULL) || (length == 0U) ||
        (length > slot_capacity(slot)) || (length > data_size))
    {
        return false;
    }

    memcpy(data, source, length);
    if (out_length != NULL)
    {
        *out_length = length;
    }

    return true;
}

bool flash_config_write(flash_config_slot_t slot,
                        void const *data,
                        uint16_t data_length)
{
    uint16_t capacity;
    uint8_t *destination;

    if (!slot_is_valid(slot) || (data == NULL))
    {
        return false;
    }

    capacity = slot_capacity(slot);
    if ((data_length == 0U) || (data_length > capacity))
    {
        return false;
    }

    ensure_cache_loaded();
    destination = slot_data(&cached_record, slot);

    if ((cached_record.lengths[slot] == data_length) &&
        (memcmp(destination, data, data_length) == 0))
    {
        return true;
    }

    if (!transaction_active)
    {
        memcpy(&backup_record, &cached_record, sizeof(backup_record));
    }

    memset(destination, 0xff, capacity);
    memcpy(destination, data, data_length);
    cached_record.lengths[slot] = data_length;

    if (transaction_active)
    {
        transaction_dirty = true;
        return true;
    }

    if (append_cached_record())
    {
        return true;
    }

    memcpy(&cached_record, &backup_record, sizeof(cached_record));
    return false;
}

bool flash_config_clear(flash_config_slot_t slot)
{
    uint16_t capacity;
    uint8_t *destination;

    if (!slot_is_valid(slot))
    {
        return false;
    }

    ensure_cache_loaded();
    if (cached_record.lengths[slot] == 0U)
    {
        return true;
    }

    if (!transaction_active)
    {
        memcpy(&backup_record, &cached_record, sizeof(backup_record));
    }

    capacity = slot_capacity(slot);
    destination = slot_data(&cached_record, slot);
    memset(destination, 0xff, capacity);
    cached_record.lengths[slot] = 0U;

    if (transaction_active)
    {
        transaction_dirty = true;
        return true;
    }

    if (append_cached_record())
    {
        return true;
    }

    memcpy(&cached_record, &backup_record, sizeof(cached_record));
    return false;
}

bool flash_config_transaction_begin(void)
{
    if (transaction_active)
    {
        return false;
    }

    ensure_cache_loaded();
    memcpy(&backup_record, &cached_record, sizeof(backup_record));
    transaction_active = true;
    transaction_dirty = false;
    return true;
}

bool flash_config_transaction_commit(void)
{
    bool ok;

    if (!transaction_active)
    {
        return false;
    }

    ok = !transaction_dirty || append_cached_record();
    if (!ok)
    {
        memcpy(&cached_record, &backup_record, sizeof(cached_record));
    }

    transaction_active = false;
    transaction_dirty = false;
    return ok;
}

void flash_config_transaction_abort(void)
{
    if (transaction_active)
    {
        memcpy(&cached_record, &backup_record, sizeof(cached_record));
    }

    transaction_active = false;
    transaction_dirty = false;
}
