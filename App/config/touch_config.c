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
#include "touch/touch_core_port.h"

#define TOUCH_PARAM_CHANNEL_MAPPING       0x01U
#define TOUCH_PARAM_CDC_MODE              0x02U
#define TOUCH_PARAM_CHANNEL_BATCH         0x03U
#define TOUCH_PARAM_PSOC_STATUS           0x04U
#define TOUCH_PARAM_FORCE_RESEND          0x05U
#define TOUCH_PARAM_CP                    0x06U
#define TOUCH_PARAM_CORE_INFO             0x07U

/* 0x05 写入载荷: 1 字节, 非 0 表示强制上位机重新下发扫描参数 */
#define TOUCH_FORCE_RESEND_TRIGGER        0x01U

/* 能力载荷 data[7]: bit0 = 支持 0x05 强制重新下发扫描参数,
 *                  bit1 = 支持 0x07 读取判定核心身份。
 * 只加位、不改长度, 老上位机仍能解析。 */
#define TOUCH_CAPABILITY_FORCE_RESEND     0x01U
#define TOUCH_CAPABILITY_CORE_INFO        0x02U

/* 0x06 读取载荷: 每片 = Cp(17ch × 2B 大端) + 补偿 IDAC(17ch × 1B) + 快照年龄(2B 大端, ms) */
#define TOUCH_CP_DEVICE_AGE_LENGTH        2U
#define TOUCH_CP_DEVICE_LENGTH            (PSOC_COMM_CP_LENGTH)
#define TOUCH_CP_DEVICE_STRIDE \
    (PSOC_COMM_CP_LENGTH + PSOC_COMM_COMP_IDAC_LENGTH + \
     TOUCH_CP_DEVICE_AGE_LENGTH)
#define TOUCH_CP_PAYLOAD_LENGTH \
    (TENODATA_STATUS_DEVICE_COUNT * TOUCH_CP_DEVICE_STRIDE)

/* 0x07 读取载荷: 判定核心身份, 3 × u32 小端 = ABI / 算法版本 / 参数指纹。
 * 三端各报一组, 用来核对"是不是同一版判定、同一组阈值"。 */
#define TOUCH_CORE_INFO_U32_COUNT         3U
#define TOUCH_CORE_INFO_PAYLOAD_LENGTH \
    (TOUCH_CORE_INFO_U32_COUNT * 4U)

/* cdc_mode 取值: 0/1 = 强制固定到某一侧, 2 = 自动识别 (默认)。
 * 读回时会报告 2, 当前实际在跑哪一侧由状态参数 0x04 显示。 */
#define TOUCH_CDC_MODE_RAW                0U
#define TOUCH_CDC_MODE_MAI2TOUCH          1U
#define TOUCH_CDC_MODE_AUTO               2U
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
_Static_assert(TOUCH_CP_PAYLOAD_LENGTH == 106U,
               "Touch Cp payload must be 106 bytes (2 x (34B Cp + 17B comp + 2B age))");
_Static_assert(TOUCH_CORE_INFO_PAYLOAD_LENGTH == 12U,
               "Touch core info payload must be three little-endian u32");

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
           (mode == TOUCH_CDC_MODE_MAI2TOUCH) ||
           (mode == TOUCH_CDC_MODE_AUTO);
}

static uint8_t current_cdc_mode(void)
{
    /* 处于自动识别时报告 AUTO(2), 而不是"当前碰巧在跑的那一侧":
     * 这样 (a) 读回的模式是真实状态, (b) 存进 Flash 的也是 AUTO,
     * 下次上电不会因为"上次存配置时游戏正好在跑"而被钉死在 Mai2Touch。
     * 当前实际在跑哪一侧由 `touch status` (参数 0x04) 显示。 */
    if (touch_app_is_auto_mode())
    {
        return TOUCH_CDC_MODE_AUTO;
    }

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

    if (cdc_mode == TOUCH_CDC_MODE_AUTO)
    {
        /* 恢复自动识别: 映射照常应用, 但模式留在当前这一侧不动,
         * 之后由 touch_mode 按收到的帧特征自己纠正。
         * 写入与当前一致的模式不会退出自动识别 (见 cdc_manager_set_mai2touch_active),
         * 所以这里传"当前实际模式"即可。 */
        bool ok = touch_app_apply_config(
            mapping,
            TENODATA_TOTAL_CHANNELS,
            touch_app_is_mai2touch_active(),
            force_resend);

        touch_app_set_auto_mode();
        return ok;
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

/* 判定核心身份按小端 u32 发出, 与上位机的 struct.unpack("<I") 对应。 */
static void put_u32_le(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value & 0xffU);
    out[1] = (uint8_t)((value >> 8U) & 0xffU);
    out[2] = (uint8_t)((value >> 16U) & 0xffU);
    out[3] = (uint8_t)((value >> 24U) & 0xffU);
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

        case TOUCH_PARAM_CORE_INFO:
        {
            /* 判定核心身份 (ABI / 算法版本 / 参数指纹)。核心未初始化时
             * touch_core_port_* 访问器会自行完成初始化, 所以这里可以直接读。 */
            if (max_length < TOUCH_CORE_INFO_PAYLOAD_LENGTH)
            {
                return false;
            }

            put_u32_le(&data[0], touch_core_port_abi_version());
            put_u32_le(&data[4], touch_core_port_algorithm_version());
            put_u32_le(&data[8], touch_core_port_params_hash());
            *out_length = TOUCH_CORE_INFO_PAYLOAD_LENGTH;
            return true;
        }

        case TOUCH_PARAM_CP:
        {
            /* 每片: Cp 34 字节 (17 通道 × 2 字节大端) + 补偿 IDAC 17 字节
             *       + 快照年龄 2 字节 (大端, ms)。
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
                uint8_t *age = &slot[TOUCH_CP_DEVICE_LENGTH +
                                    PSOC_COMM_COMP_IDAC_LENGTH];
                uint16_t age_ms = 0U;

                (void)touch_app_get_cp(index, slot);
                (void)touch_app_get_comp_idac(
                    index, &slot[TOUCH_CP_DEVICE_LENGTH]);

                /* 让上位机知道这份快照是多久以前抓的:
                 * 数值不变但年龄在涨 = PSoC 没有重新测量。 */
                if (touch_app_get_cal_age_ms(index, &age_ms))
                {
                    age[0] = (uint8_t)(age_ms >> 8U);
                    age[1] = (uint8_t)(age_ms & 0xFFU);
                }
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

    bool ok = touch_config_apply(mapping_buffer, mode, false);

    if (mode != TOUCH_CDC_MODE_AUTO)
    {
        /* 显式写 0/1 = 强制固定: touch_config_apply 只在"输出侧真的变了"时才动
         * 模式, 所以这里再补一次无条件强制, 确保 AUTO 被关掉。 */
        touch_app_force_cdc_mode(mode == TOUCH_CDC_MODE_MAI2TOUCH);
    }

    return ok;
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

    return touch_config_apply(mapping_buffer, TOUCH_CDC_MODE_AUTO, false);
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
    /* 能力位: bit0 = 强制重新下发扫描参数, bit1 = 读取判定核心身份 */
    data[7] = TOUCH_CAPABILITY_FORCE_RESEND | TOUCH_CAPABILITY_CORE_INFO;
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
