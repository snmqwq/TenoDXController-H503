#include "led_config.h"

#include <stdint.h>
#include <string.h>

#include "flash_config.h"
#include "led/mai2led_app.h"
#include "magic_config.h"

#define LED_FLASH_CONFIG_MAGIC       0x324C414DUL
#define LED_FLASH_CONFIG_VERSION     3U

#define LIGHT_PARAM_LED_PER_BIT      0x01U
#define LIGHT_PARAM_RAINBOW_ENABLE   0x02U
#define LIGHT_PAYLOAD_VERSION        2U

typedef struct
{
    uint32_t magic;
    uint8_t version;
    uint8_t led_per_bit;
    uint8_t rainbow_mode_enable;
    uint8_t reserved0;
    uint8_t reserved[12];
} led_flash_config_t;

typedef struct
{
    uint8_t version;
    uint8_t led_per_bit;
    uint8_t rainbow_mode_enable;
} light_config_payload_t;

_Static_assert(sizeof(led_flash_config_t) == 20U,
               "LED Flash layout must remain compatible");
_Static_assert(sizeof(light_config_payload_t) == 3U,
               "Light Magic payload must remain compatible");

static bool led_config_load_from_flash(void)
{
    led_flash_config_t config;
    uint16_t length = 0U;

    memset(&config, 0, sizeof(config));
    if (!flash_config_read(FLASH_CONFIG_SLOT_LIGHT,
                           &config,
                           sizeof(config),
                           &length))
    {
        return false;
    }

    if ((length != sizeof(config)) ||
        (config.magic != LED_FLASH_CONFIG_MAGIC) ||
        (config.version != LED_FLASH_CONFIG_VERSION))
    {
        return false;
    }

    if (mai2led_app_is_led_per_bit_valid(config.led_per_bit))
    {
        mai2led_app_set_led_per_bit(config.led_per_bit);
    }

    mai2led_app_set_rainbow_mode(config.rainbow_mode_enable != 0U);
    return true;
}

static bool led_config_save_to_flash(void)
{
    led_flash_config_t config;

    memset(&config, 0xff, sizeof(config));
    config.magic = LED_FLASH_CONFIG_MAGIC;
    config.version = LED_FLASH_CONFIG_VERSION;
    config.led_per_bit = mai2led_app_get_led_per_bit();
    config.rainbow_mode_enable =
        mai2led_app_get_rainbow_mode() ? 1U : 0U;

    return flash_config_write(FLASH_CONFIG_SLOT_LIGHT,
                              &config,
                              sizeof(config));
}

static bool light_magic_read(uint8_t param,
                             uint8_t *data,
                             uint8_t max_length,
                             uint8_t *out_length)
{
    if ((data == NULL) || (out_length == NULL) || (max_length < 1U))
    {
        return false;
    }

    switch (param)
    {
        case LIGHT_PARAM_LED_PER_BIT:
            data[0] = mai2led_app_get_led_per_bit();
            *out_length = 1U;
            return true;

        case LIGHT_PARAM_RAINBOW_ENABLE:
            data[0] = mai2led_app_get_rainbow_mode() ? 1U : 0U;
            *out_length = 1U;
            return true;

        default:
            return false;
    }
}

static bool light_magic_write(uint8_t param,
                              uint8_t const *data,
                              uint8_t length)
{
    if ((data == NULL) || (length != 1U))
    {
        return false;
    }

    switch (param)
    {
        case LIGHT_PARAM_LED_PER_BIT:
            if (!mai2led_app_is_led_per_bit_valid(data[0]))
            {
                return false;
            }
            mai2led_app_set_led_per_bit(data[0]);
            return true;

        case LIGHT_PARAM_RAINBOW_ENABLE:
            mai2led_app_set_rainbow_mode(data[0] != 0U);
            return true;

        default:
            return false;
    }
}

static bool light_magic_save(uint8_t param)
{
    (void)param;
    return led_config_save_to_flash();
}

static bool light_magic_load_default(uint8_t param)
{
    (void)param;
    mai2led_app_reset_light_config();
    return true;
}

static bool light_magic_info(uint8_t param,
                             uint8_t *data,
                             uint8_t max_length,
                             uint8_t *out_length)
{
    (void)param;

    if ((data == NULL) || (out_length == NULL) || (max_length < 2U))
    {
        return false;
    }

    data[0] = LIGHT_PARAM_LED_PER_BIT;
    data[1] = LIGHT_PARAM_RAINBOW_ENABLE;
    *out_length = 2U;
    return true;
}

static bool light_magic_read_all(uint8_t *data,
                                 uint8_t max_length,
                                 uint8_t *out_length)
{
    light_config_payload_t payload;

    if ((data == NULL) ||
        (out_length == NULL) ||
        (max_length < sizeof(payload)))
    {
        return false;
    }

    payload.version = LIGHT_PAYLOAD_VERSION;
    payload.led_per_bit = mai2led_app_get_led_per_bit();
    payload.rainbow_mode_enable =
        mai2led_app_get_rainbow_mode() ? 1U : 0U;

    memcpy(data, &payload, sizeof(payload));
    *out_length = sizeof(payload);
    return true;
}

static bool light_magic_write_all(uint8_t const *data, uint8_t length)
{
    light_config_payload_t payload;

    if ((data == NULL) || (length != sizeof(payload)))
    {
        return false;
    }

    memcpy(&payload, data, sizeof(payload));

    if ((payload.version != LIGHT_PAYLOAD_VERSION) ||
        !mai2led_app_is_led_per_bit_valid(payload.led_per_bit))
    {
        return false;
    }

    mai2led_app_set_led_per_bit(payload.led_per_bit);
    mai2led_app_set_rainbow_mode(payload.rainbow_mode_enable != 0U);
    return true;
}

bool led_config_init(void)
{
    static const magic_config_module_t module =
    {
        .module = MAGIC_CONFIG_MODULE_LIGHT,
        .read = light_magic_read,
        .write = light_magic_write,
        .save = light_magic_save,
        .load_default = light_magic_load_default,
        .get_info = light_magic_info,
        .read_all = light_magic_read_all,
        .write_all = light_magic_write_all
    };

    (void)led_config_load_from_flash();
    return magic_config_register(&module);
}
