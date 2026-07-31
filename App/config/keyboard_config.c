#include "keyboard_config.h"

#include <stdint.h>
#include <string.h>

#include "flash_config.h"
#include "kb/keyboard_app.h"
#include "magic_config.h"

#define KEYBOARD_FLASH_VERSION              3U
#define KEYBOARD_FLASH_LEGACY_VERSION       2U
#define KEYBOARD_PAYLOAD_VERSION            2U
#define KEYBOARD_PAYLOAD_LEGACY_VERSION     1U
#define KEYBOARD_PARAM_CONFIG_KEYS          0x80U
#define KEYBOARD_PARAM_MAIN_LAYOUT          0x81U
#define KEYBOARD_LEGACY_CONFIG_KEY_COUNT    3U

typedef struct
{
    uint8_t version;
    uint8_t first_key;
    uint8_t key_count;
    uint8_t main_layout;
    uint8_t key_map[KEYBOARD_APP_CONFIG_KEY_COUNT];
} keyboard_flash_config_t;

typedef struct
{
    uint8_t version;
    uint8_t first_key;
    uint8_t key_count;
    uint8_t main_layout;
    uint8_t key_map[KEYBOARD_APP_CONFIG_KEY_COUNT];
} keyboard_config_payload_t;

typedef struct
{
    uint8_t version;
    uint8_t first_key;
    uint8_t key_count;
    uint8_t key_map[KEYBOARD_LEGACY_CONFIG_KEY_COUNT];
} keyboard_legacy_config_t;

_Static_assert(KEYBOARD_APP_KEY_COUNT == 12U,
               "Magic keyboard protocol requires 12 keys");
_Static_assert(KEYBOARD_APP_CONFIG_KEY_FIRST == 8U,
               "Magic keyboard protocol requires key 8 as the first configurable key");
_Static_assert(KEYBOARD_APP_CONFIG_KEY_COUNT == 4U,
               "Magic keyboard protocol requires four configurable keys");
_Static_assert(sizeof(keyboard_flash_config_t) == 8U,
               "Keyboard Flash layout must remain eight bytes");
_Static_assert(sizeof(keyboard_config_payload_t) == 8U,
               "Keyboard Magic payload must remain eight bytes");
_Static_assert(sizeof(keyboard_legacy_config_t) == 6U,
               "Legacy keyboard layout must remain six bytes");

static bool keyboard_config_apply(uint8_t main_layout,
                                  uint8_t const *key_map,
                                  uint8_t key_count)
{
    if ((main_layout >= (uint8_t)KEYBOARD_APP_MAIN_LAYOUT_COUNT) ||
        (key_map == NULL) ||
        (key_count != KEYBOARD_APP_CONFIG_KEY_COUNT))
    {
        return false;
    }

    return keyboard_app_set_config_keycodes(key_map, key_count) &&
           keyboard_app_set_main_layout(
               (keyboard_app_main_layout_t)main_layout);
}

static bool keyboard_config_set_legacy_keycodes(uint8_t const *key_map)
{
    if (key_map == NULL)
    {
        return false;
    }

    /*
     * Legacy key10 was PB10. PB10 is key11 in the four-button layout,
     * while the newly inserted key10 is PB2 and keeps its default.
     */
    return keyboard_app_set_keycode(KEYBOARD_APP_CONFIG_KEY_FIRST,
                                    key_map[0]) &&
           keyboard_app_set_keycode(KEYBOARD_APP_CONFIG_KEY_FIRST + 1U,
                                    key_map[1]) &&
           keyboard_app_set_keycode(KEYBOARD_APP_KEY_COUNT - 1U,
                                    key_map[2]);
}

static bool keyboard_config_apply_legacy(
    keyboard_legacy_config_t const *config)
{
    if ((config == NULL) ||
        (config->first_key != KEYBOARD_APP_CONFIG_KEY_FIRST) ||
        (config->key_count != KEYBOARD_LEGACY_CONFIG_KEY_COUNT))
    {
        return false;
    }

    keyboard_app_reset_keycodes();
    return keyboard_config_set_legacy_keycodes(config->key_map);
}

static bool keyboard_config_load_from_flash(void)
{
    uint8_t raw_config[sizeof(keyboard_flash_config_t)];
    uint16_t config_length = 0U;

    if (!flash_config_read(FLASH_CONFIG_SLOT_KEYBOARD,
                           raw_config,
                           sizeof(raw_config),
                           &config_length))
    {
        return false;
    }

    if (config_length == sizeof(keyboard_flash_config_t))
    {
        keyboard_flash_config_t config;

        memcpy(&config, raw_config, sizeof(config));

        if ((config.version != KEYBOARD_FLASH_VERSION) ||
            (config.first_key != KEYBOARD_APP_CONFIG_KEY_FIRST) ||
            (config.key_count != KEYBOARD_APP_CONFIG_KEY_COUNT))
        {
            return false;
        }

        return keyboard_config_apply(config.main_layout,
                                     config.key_map,
                                     sizeof(config.key_map));
    }

    if (config_length == sizeof(keyboard_legacy_config_t))
    {
        keyboard_legacy_config_t config;

        memcpy(&config, raw_config, sizeof(config));

        if (config.version != KEYBOARD_FLASH_LEGACY_VERSION)
        {
            return false;
        }

        return keyboard_config_apply_legacy(&config);
    }

    return false;
}

static bool keyboard_config_save_to_flash(void)
{
    keyboard_flash_config_t config;
    keyboard_app_main_layout_t main_layout;

    memset(&config, 0xff, sizeof(config));
    config.version = KEYBOARD_FLASH_VERSION;
    config.first_key = KEYBOARD_APP_CONFIG_KEY_FIRST;
    config.key_count = KEYBOARD_APP_CONFIG_KEY_COUNT;

    if (!keyboard_app_get_main_layout(&main_layout) ||
        !keyboard_app_get_config_keycodes(config.key_map,
                                          sizeof(config.key_map)))
    {
        return false;
    }

    config.main_layout = (uint8_t)main_layout;

    return flash_config_write(FLASH_CONFIG_SLOT_KEYBOARD,
                              &config,
                              sizeof(config));
}

static bool keyboard_magic_read(uint8_t param,
                                uint8_t *data,
                                uint8_t max_length,
                                uint8_t *out_length)
{
    if ((data == NULL) || (out_length == NULL))
    {
        return false;
    }

    if (param < KEYBOARD_APP_KEY_COUNT)
    {
        if ((max_length < 1U) ||
            !keyboard_app_get_keycode(param, &data[0]))
        {
            return false;
        }

        *out_length = 1U;
        return true;
    }

    if (param == KEYBOARD_PARAM_CONFIG_KEYS)
    {
        if ((max_length < KEYBOARD_APP_CONFIG_KEY_COUNT) ||
            !keyboard_app_get_config_keycodes(data,
                                              KEYBOARD_APP_CONFIG_KEY_COUNT))
        {
            return false;
        }

        *out_length = KEYBOARD_APP_CONFIG_KEY_COUNT;
        return true;
    }

    if (param == KEYBOARD_PARAM_MAIN_LAYOUT)
    {
        keyboard_app_main_layout_t main_layout;

        if ((max_length < 1U) ||
            !keyboard_app_get_main_layout(&main_layout))
        {
            return false;
        }

        data[0] = (uint8_t)main_layout;
        *out_length = 1U;
        return true;
    }

    return false;
}

static bool keyboard_magic_write(uint8_t param,
                                 uint8_t const *data,
                                 uint8_t length)
{
    if (data == NULL)
    {
        return false;
    }

    if (param < KEYBOARD_APP_KEY_COUNT)
    {
        return (length == 1U) &&
               keyboard_app_set_keycode(param, data[0]);
    }

    if (param == KEYBOARD_PARAM_CONFIG_KEYS)
    {
        if (length == KEYBOARD_LEGACY_CONFIG_KEY_COUNT)
        {
            return keyboard_config_set_legacy_keycodes(data);
        }

        return keyboard_app_set_config_keycodes(data, length);
    }

    if (param == KEYBOARD_PARAM_MAIN_LAYOUT)
    {
        return (length == 1U) &&
               keyboard_app_set_main_layout(
                   (keyboard_app_main_layout_t)data[0]);
    }

    return false;
}

static bool keyboard_magic_save(uint8_t param)
{
    (void)param;
    return keyboard_config_save_to_flash();
}

static bool keyboard_magic_load_default(uint8_t param)
{
    (void)param;
    keyboard_app_reset_keycodes();
    return true;
}

static bool keyboard_magic_info(uint8_t param,
                                uint8_t *data,
                                uint8_t max_length,
                                uint8_t *out_length)
{
    (void)param;

    if ((data == NULL) || (out_length == NULL) || (max_length < 6U))
    {
        return false;
    }

    data[0] = KEYBOARD_APP_KEY_COUNT;
    data[1] = KEYBOARD_APP_CONFIG_KEY_FIRST;
    data[2] = KEYBOARD_APP_CONFIG_KEY_COUNT;
    data[3] = KEYBOARD_PARAM_CONFIG_KEYS;
    data[4] = KEYBOARD_PARAM_MAIN_LAYOUT;
    data[5] = KEYBOARD_APP_MAIN_LAYOUT_COUNT;
    *out_length = 6U;
    return true;
}

static bool keyboard_magic_read_all(uint8_t *data,
                                    uint8_t max_length,
                                    uint8_t *out_length)
{
    keyboard_config_payload_t payload;
    keyboard_app_main_layout_t main_layout;

    if ((data == NULL) ||
        (out_length == NULL) ||
        (max_length < sizeof(payload)))
    {
        return false;
    }

    payload.version = KEYBOARD_PAYLOAD_VERSION;
    if (!keyboard_app_get_main_layout(&main_layout))
    {
        return false;
    }

    payload.main_layout = (uint8_t)main_layout;
    payload.first_key = KEYBOARD_APP_CONFIG_KEY_FIRST;
    payload.key_count = KEYBOARD_APP_CONFIG_KEY_COUNT;

    if (!keyboard_app_get_config_keycodes(payload.key_map,
                                          sizeof(payload.key_map)))
    {
        return false;
    }

    memcpy(data, &payload, sizeof(payload));
    *out_length = sizeof(payload);
    return true;
}

static bool keyboard_magic_write_all(uint8_t const *data, uint8_t length)
{
    if (data == NULL)
    {
        return false;
    }

    if (length == sizeof(keyboard_config_payload_t))
    {
        keyboard_config_payload_t payload;

        memcpy(&payload, data, sizeof(payload));

        if ((payload.version != KEYBOARD_PAYLOAD_VERSION) ||
            (payload.first_key != KEYBOARD_APP_CONFIG_KEY_FIRST) ||
            (payload.key_count != KEYBOARD_APP_CONFIG_KEY_COUNT))
        {
            return false;
        }

        return keyboard_config_apply(payload.main_layout,
                                     payload.key_map,
                                     sizeof(payload.key_map));
    }

    if (length == sizeof(keyboard_legacy_config_t))
    {
        keyboard_legacy_config_t payload;

        memcpy(&payload, data, sizeof(payload));

        if (payload.version != KEYBOARD_PAYLOAD_LEGACY_VERSION)
        {
            return false;
        }

        return keyboard_config_apply_legacy(&payload);
    }

    return false;
}

bool keyboard_config_init(void)
{
    static const magic_config_module_t module =
    {
        .module = MAGIC_CONFIG_MODULE_KEYBOARD,
        .read = keyboard_magic_read,
        .write = keyboard_magic_write,
        .save = keyboard_magic_save,
        .load_default = keyboard_magic_load_default,
        .get_info = keyboard_magic_info,
        .read_all = keyboard_magic_read_all,
        .write_all = keyboard_magic_write_all
    };

    (void)keyboard_config_load_from_flash();
    return magic_config_register(&module);
}
