#include "touch_config.h"

#if defined(__GNUC__)
#pragma GCC optimize ("Os")
#endif

#include <stdint.h>
#include <string.h>

#include "flash_config.h"
#include "magic_config.h"
#include "touch/psoc_comm.h"
#include "touch/touch_app.h"

#define TOUCH_PARAM_CHANNEL_MAPPING       0x01U
#define TOUCH_PARAM_CDC_MODE              0x02U
#define TOUCH_PARAM_CHANNEL_BATCH         0x03U
#define TOUCH_PARAM_PSOC_STATUS           0x04U
#define TOUCH_PARAM_FORCE_RESEND          0x05U
#define TOUCH_PARAM_CP                    0x06U

/* 0x05 写入载荷: 1 字节, 非 0 表示强制上位机重新下发扫描参数 */
#define TOUCH_FORCE_RESEND_TRIGGER        0x01U

/* 0x06 读取载荷: 每片 = Cp(17ch × 2B 大端) + 补偿 IDAC(17ch × 1B) */
#define TOUCH_CP_DEVICE_LENGTH            (PSOC_COMM_CP_LENGTH)
#define TOUCH_CP_DEVICE_STRIDE \
    (PSOC_COMM_CP_LENGTH + PSOC_COMM_COMP_IDAC_LENGTH)
#define TOUCH_CP_PAYLOAD_LENGTH \
    (TENODATA_STATUS_DEVICE_COUNT * TOUCH_CP_DEVICE_STRIDE)

#define TOUCH_CDC_MODE_RAW                0U
#define TOUCH_CDC_MODE_MAI2TOUCH          1U
#define TOUCH_PAYLOAD_VERSION             2U
#define TOUCH_STATUS_PAYLOAD_VERSION      1U
#define TOUCH_STATUS_HEADER_LENGTH        4U
#define TOUCH_STATUS_DEVICE_LENGTH        6U
#define TOUCH_STATUS_PAYLOAD_LENGTH \
    (TOUCH_STATUS_HEADER_LENGTH + \
     TENODATA_STATUS_DEVICE_COUNT * TOUCH_STATUS_DEVICE_LENGTH)

#define TOUCH_MAPPING_PAYLOAD_LENGTH \
    (TENODATA_TOTAL_CHANNELS * sizeof(TenodataChannelMapping))
#define TOUCH_BATCH_RECORD_LENGTH \
    (1U + sizeof(TenodataChannelMapping))

typedef struct
{
    uint8_t version;
    uint8_t cdc_mode;
    uint8_t mapping[TOUCH_MAPPING_PAYLOAD_LENGTH];
} touch_config_payload_t;

_Static_assert(sizeof(TenodataChannelMapping) == 2U,
               "Touch mapping wire entry must be two bytes");
_Static_assert(TOUCH_MAPPING_PAYLOAD_LENGTH == 68U,
               "Touch mapping payload must be 68 bytes");
_Static_assert(TOUCH_BATCH_RECORD_LENGTH == 3U,
               "Touch batch record must be three bytes");
_Static_assert(sizeof(touch_config_payload_t) == 70U,
               "Touch all/Flash payload must be 70 bytes");
_Static_assert(TOUCH_STATUS_PAYLOAD_LENGTH == 16U,
               "Touch status payload must be 16 bytes");
_Static_assert(TOUCH_CP_PAYLOAD_LENGTH == 102U,
               "Touch Cp payload must be 102 bytes (2 devices x (34B Cp + 17B comp))");

/* Magic callbacks run synchronously from the main loop, so shared work
 * buffers avoid placing several mapping objects on the MCU stack.
 */
static TenodataChannelMapping
    mapping_buffer[TENODATA_TOTAL_CHANNELS];
static touch_config_payload_t payload_buffer;
static touch_config_payload_t stored_payload_buffer;

static bool cdc_mode_is_valid(uint8_t mode)
{
    return (mode == TOUCH_CDC_MODE_RAW) ||
           (mode == TOUCH_CDC_MODE_MAI2TOUCH);
}

static uint8_t current_cdc_mode(void)
{
    return touch_app_is_mai2touch_active() ?
           TOUCH_CDC_MODE_MAI2TOUCH :
           TOUCH_CDC_MODE_RAW;
}

static bool touch_config_apply(TenodataChannelMapping const *mapping,
                               uint8_t cdc_mode,
                               bool force_resend)
{
    if (!cdc_mode_is_valid(cdc_mode))
    {
        return false;
    }

    return touch_app_apply_config(
        mapping,
        TENODATA_TOTAL_CHANNELS,
        cdc_mode == TOUCH_CDC_MODE_MAI2TOUCH,
        force_resend);
}

static bool touch_config_build_payload(touch_config_payload_t *payload)
{
    if ((payload == NULL) ||
        !touch_app_get_mapping(mapping_buffer, TENODATA_TOTAL_CHANNELS))
    {
        return false;
    }

    payload->version = TOUCH_PAYLOAD_VERSION;
    payload->cdc_mode = current_cdc_mode();
    memcpy(payload->mapping, mapping_buffer, sizeof(mapping_buffer));
    return true;
}

static bool touch_config_apply_payload(touch_config_payload_t const *payload)
{
    if ((payload == NULL) ||
        (payload->version != TOUCH_PAYLOAD_VERSION) ||
        !cdc_mode_is_valid(payload->cdc_mode))
    {
        return false;
    }

    memcpy(mapping_buffer, payload->mapping, sizeof(mapping_buffer));
    return touch_config_apply(mapping_buffer, payload->cdc_mode, false);
}

static bool touch_config_load_from_flash(void)
{
    uint16_t length = 0U;

    if (!flash_config_read(FLASH_CONFIG_SLOT_TOUCH,
                           &payload_buffer,
                           sizeof(payload_buffer),
                           &length) ||
        (length != sizeof(payload_buffer)))
    {
        return false;
    }

    return touch_config_apply_payload(&payload_buffer);
}

static bool touch_config_save_to_flash(void)
{
    uint16_t stored_length = 0U;

    if (!touch_config_build_payload(&payload_buffer))
    {
        return false;
    }

    if (flash_config_read(FLASH_CONFIG_SLOT_TOUCH,
                          &stored_payload_buffer,
                          sizeof(stored_payload_buffer),
                          &stored_length) &&
        (stored_length == sizeof(stored_payload_buffer)) &&
        (memcmp(&stored_payload_buffer,
                &payload_buffer,
                sizeof(payload_buffer)) == 0))
    {
        return true;
    }

    return flash_config_write(FLASH_CONFIG_SLOT_TOUCH,
                              &payload_buffer,
                              sizeof(payload_buffer));
}

static bool touch_magic_read(uint8_t param,
                             uint8_t *data,
                             uint8_t max_length,
                             uint8_t *out_length)
{
    if ((data == NULL) || (out_length == NULL))
    {
        return false;
    }

    switch (param)
    {
        case TOUCH_PARAM_CHANNEL_MAPPING:
            if (max_length < TOUCH_MAPPING_PAYLOAD_LENGTH)
            {
                return false;
            }

            if (!touch_app_get_mapping(
                    (TenodataChannelMapping *)data,
                    TENODATA_TOTAL_CHANNELS))
            {
                return false;
            }

            *out_length = TOUCH_MAPPING_PAYLOAD_LENGTH;
            return true;

        case TOUCH_PARAM_CDC_MODE:
            if (max_length < 1U)
            {
                return false;
            }

            data[0] = current_cdc_mode();
            *out_length = 1U;
            return true;

        case TOUCH_PARAM_PSOC_STATUS:
        {
            TenodataStatusSnapshot snapshot;

            if ((max_length < TOUCH_STATUS_PAYLOAD_LENGTH) ||
                !touch_app_get_psoc_status(&snapshot))
            {
                return false;
            }

            data[0] = TOUCH_STATUS_PAYLOAD_VERSION;
            data[1] = snapshot.state;
            data[2] = snapshot.flags;
            data[3] = snapshot.device_count;

            for (uint8_t index = 0U;
                 index < TENODATA_STATUS_DEVICE_COUNT;
                 index++)
            {
                uint8_t offset = (uint8_t)(
                    TOUCH_STATUS_HEADER_LENGTH +
                    index * TOUCH_STATUS_DEVICE_LENGTH);
                TenodataPsocStatus const *device =
                    &snapshot.devices[index];

                data[offset] = device->address;
                data[offset + 1U] = device->status;
                data[offset + 2U] = device->flags;
                data[offset + 3U] = device->consecutive_failures;
                data[offset + 4U] =
                    (uint8_t)(device->status_age_ms & 0xffU);
                data[offset + 5U] =
                    (uint8_t)(device->status_age_ms >> 8U);
            }

            *out_length = TOUCH_STATUS_PAYLOAD_LENGTH;
            return true;
        }

        case TOUCH_PARAM_CP:
        {
            /* 每片: Cp 34 字节 (17 通道 × 2 字节大端) + 补偿 IDAC 17 字节。
             * 无效片 (未校准成功/刚重配) 整片填 0, 上位机会显示为 "-",
             * 因此参数整体只要长度够就返回成功。 */
            if (max_length < TOUCH_CP_PAYLOAD_LENGTH)
            {
                return false;
            }

            memset(data, 0, TOUCH_CP_PAYLOAD_LENGTH);

            for (uint8_t index = 0U;
                 index < TENODATA_STATUS_DEVICE_COUNT;
                 index++)
            {
                uint8_t *slot = &data[index * TOUCH_CP_DEVICE_STRIDE];

                (void)touch_app_get_cp(index, slot);
                (void)touch_app_get_comp_idac(
                    index, &slot[TOUCH_CP_DEVICE_LENGTH]);
            }

            *out_length = TOUCH_CP_PAYLOAD_LENGTH;
            return true;
        }

        default:
            return false;
    }
}

static bool touch_magic_write_mapping(uint8_t const *data, uint8_t length)
{
    if ((data == NULL) || (length != TOUCH_MAPPING_PAYLOAD_LENGTH))
    {
        return false;
    }

    memcpy(mapping_buffer, data, sizeof(mapping_buffer));
    return touch_config_apply(mapping_buffer, current_cdc_mode(), false);
}

static bool touch_magic_write_batch(uint8_t const *data, uint8_t length)
{
    uint64_t used_channels = 0U;
    uint8_t record_count;

    if ((data == NULL) ||
        (length == 0U) ||
        ((length % TOUCH_BATCH_RECORD_LENGTH) != 0U) ||
        !touch_app_get_mapping(mapping_buffer, TENODATA_TOTAL_CHANNELS))
    {
        return false;
    }

    record_count = (uint8_t)(length / TOUCH_BATCH_RECORD_LENGTH);
    if (record_count > TENODATA_TOTAL_CHANNELS)
    {
        return false;
    }

    for (uint8_t record = 0U; record < record_count; record++)
    {
        uint8_t offset = (uint8_t)(record * TOUCH_BATCH_RECORD_LENGTH);
        uint8_t channel = data[offset];
        uint64_t channel_bit;

        if (channel >= TENODATA_TOTAL_CHANNELS)
        {
            return false;
        }

        channel_bit = 1ULL << channel;
        if ((used_channels & channel_bit) != 0U)
        {
            return false;
        }

        used_channels |= channel_bit;
        memcpy(&mapping_buffer[channel],
               &data[offset + 1U],
               sizeof(TenodataChannelMapping));
    }

    return touch_config_apply(mapping_buffer, current_cdc_mode(), false);
}

static bool touch_magic_write_mode(uint8_t mode)
{
    if (!cdc_mode_is_valid(mode) ||
        !touch_app_get_mapping(mapping_buffer, TENODATA_TOTAL_CHANNELS))
    {
        return false;
    }

    return touch_config_apply(mapping_buffer, mode, false);
}

/* 0x05: 清掉本地缓存的上位机扫描参数覆盖, 并重走初始化流程,
 * 使固件重新上报 0x01 向上位机索要参数帧。 */
static bool touch_magic_write_force_resend(uint8_t const *data, uint8_t length)
{
    if ((data == NULL) || (length != 1U) ||
        (data[0] != TOUCH_FORCE_RESEND_TRIGGER))
    {
        return false;
    }

    touch_app_force_host_config_resend();
    return true;
}

static bool touch_magic_write(uint8_t param,
                              uint8_t const *data,
                              uint8_t length)
{
    switch (param)
    {
        case TOUCH_PARAM_CHANNEL_MAPPING:
            return touch_magic_write_mapping(data, length);

        case TOUCH_PARAM_CDC_MODE:
            if ((data == NULL) ||
                (length != 1U) ||
                !cdc_mode_is_valid(data[0]))
            {
                return false;
            }

            return touch_magic_write_mode(data[0]);

        case TOUCH_PARAM_CHANNEL_BATCH:
            return touch_magic_write_batch(data, length);

        case TOUCH_PARAM_FORCE_RESEND:
            return touch_magic_write_force_resend(data, length);

        default:
            return false;
    }
}

static bool touch_magic_save(uint8_t param)
{
    (void)param;
    return touch_config_save_to_flash();
}

static bool touch_magic_load_default(uint8_t param)
{
    (void)param;
    if (!touch_app_get_default_mapping(mapping_buffer,
                                       TENODATA_TOTAL_CHANNELS))
    {
        return false;
    }

    return touch_config_apply(mapping_buffer, TOUCH_CDC_MODE_MAI2TOUCH, false);
}

static bool touch_magic_info(uint8_t param,
                             uint8_t *data,
                             uint8_t max_length,
                             uint8_t *out_length)
{
    if ((data == NULL) || (out_length == NULL))
    {
        return false;
    }

    if (param == TOUCH_PARAM_PSOC_STATUS)
    {
        if (max_length < 2U)
        {
            return false;
        }

        data[0] = TOUCH_STATUS_PAYLOAD_VERSION;
        data[1] = TOUCH_STATUS_PAYLOAD_LENGTH;
        *out_length = 2U;
        return true;
    }

    if ((param != 0U) || (max_length < 10U))
    {
        return false;
    }

    data[0] = TOUCH_PARAM_CHANNEL_MAPPING;
    data[1] = TOUCH_PARAM_CDC_MODE;
    data[2] = TOUCH_PARAM_CHANNEL_BATCH;
    data[3] = TENODATA_TOTAL_CHANNELS;
    data[4] = sizeof(TenodataChannelMapping);
    data[5] = TOUCH_BATCH_RECORD_LENGTH;
    data[6] = TOUCH_PAYLOAD_VERSION;
    /* 能力位: bit0 = 支持 0x05 强制重新下发扫描参数 */
    data[7] = 0x01U;
    /* 只读参数 */
    data[8] = TOUCH_PARAM_PSOC_STATUS;
    data[9] = TOUCH_PARAM_CP;
    *out_length = 10U;
    return true;
}

static bool touch_magic_read_all(uint8_t *data,
                                 uint8_t max_length,
                                 uint8_t *out_length)
{
    if ((data == NULL) ||
        (out_length == NULL) ||
        (max_length < sizeof(payload_buffer)) ||
        !touch_config_build_payload(&payload_buffer))
    {
        return false;
    }

    memcpy(data, &payload_buffer, sizeof(payload_buffer));
    *out_length = sizeof(payload_buffer);
    return true;
}

static bool touch_magic_write_all(uint8_t const *data, uint8_t length)
{
    if ((data == NULL) || (length != sizeof(payload_buffer)))
    {
        return false;
    }

    memcpy(&payload_buffer, data, sizeof(payload_buffer));
    return touch_config_apply_payload(&payload_buffer);
}

bool touch_config_init(void)
{
    static magic_config_module_t const module =
    {
        .module = MAGIC_CONFIG_MODULE_TOUCH,
        .read = touch_magic_read,
        .write = touch_magic_write,
        .save = touch_magic_save,
        .load_default = touch_magic_load_default,
        .get_info = touch_magic_info,
        .read_all = touch_magic_read_all,
        .write_all = touch_magic_write_all
    };

    (void)touch_config_load_from_flash();
    return magic_config_register(&module);
}
