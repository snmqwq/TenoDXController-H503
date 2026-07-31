#include "keyboard_config.h"

#include <stdint.h>
#include <string.h>

#include "flash_config.h"
#include "kb/keyboard_app.h"
#include "magic_config.h"

#define KEYBOARD_FLASH_VERSION          2U
#define KEYBOARD_PAYLOAD_VERSION        1U
#define KEYBOARD_PARAM_CONFIG_KEYS      0x80U

typedef struct
{
    uint8_t version;
    uint8_t first_key;
    uint8_t key_count;
    uint8_t key_map[KEYBOARD_APP_CONFIG_KEY_COUNT];
} keyboard_flash_config_t;

typedef struct
{
    uint8_t version;
    uint8_t first_key;
    uint8_t key_count;
    uint8_t key_map[KEYBOARD_APP_CONFIG_KEY_COUNT];
} keyboard_config_payload_t;

_Static_assert(KEYBOARD_APP_KEY_COUNT == 11U,
               "Magic keyboard protocol requires 11 keys");
_Static_assert(KEYBOARD_APP_CONFIG_KEY_FIRST == 8U,
               "Magic keyboard protocol requires key 8 as the first configurable key");
_Static_assert(KEYBOARD_APP_CONFIG_KEY_COUNT == 3U,
               "Magic keyboard protocol requires three configurable keys");
_Static_assert(sizeof(keyboard_flash_config_t) == 6U,
               "Keyboard Flash layout must remain compatible");
_Static_assert(sizeof(keyboard_config_payload_t) == 6U,
               "Keyboard Magic payload must remain compatible");

static bool keyboard_config_load_from_flash(void)
{
    keyboard_flash_config_t config;

    if (!flash_config_read(FLASH_CONFIG_SLOT_KEYBOARD,
                           &config,
                           sizeof(config),
                           NULL) ||
        (config.version != KEYBOARD_FLASH_VERSION) ||
        (config.first_key != KEYBOARD_APP_CONFIG_KEY_FIRST) ||
        (config.key_count != KEYBOARD_APP_CONFIG_KEY_COUNT))
    {
        return false;
    }

    return keyboard_app_set_config_keycodes(config.key_map,
                                            sizeof(config.key_map));
}

static bool keyboard_config_save_to_flash(void)
{
    keyboard_flash_config_t config;

    memset(&config, 0xff, sizeof(config));
    config.version = KEYBOARD_FLASH_VERSION;
    config.first_key = KEYBOARD_APP_CONFIG_KEY_FIRST;
    config.key_count = KEYBOARD_APP_CONFIG_KEY_COUNT;

    if (!keyboard_app_get_config_keycodes(config.key_map,
                                          sizeof(config.key_map)))
    {
        return false;
    }

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
        return keyboard_app_set_config_keycodes(data, length);
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

    if ((data == NULL) || (out_length == NULL) || (max_length < 4U))
    {
        return false;
    }

    data[0] = KEYBOARD_APP_KEY_COUNT;
    data[1] = KEYBOARD_APP_CONFIG_KEY_FIRST;
    data[2] = KEYBOARD_APP_CONFIG_KEY_COUNT;
    data[3] = KEYBOARD_PARAM_CONFIG_KEYS;
    *out_length = 4U;
    return true;
}

static bool keyboard_magic_read_all(uint8_t *data,
                                    uint8_t max_length,
                                    uint8_t *out_length)
{
    keyboard_config_payload_t payload;

    if ((data == NULL) ||
        (out_length == NULL) ||
        (max_length < sizeof(payload)))
    {
        return false;
    }

    payload.version = KEYBOARD_PAYLOAD_VERSION;
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
    keyboard_config_payload_t payload;

    if ((data == NULL) || (length != sizeof(payload)))
    {
        return false;
    }

    memcpy(&payload, data, sizeof(payload));

    if ((payload.version != KEYBOARD_PAYLOAD_VERSION) ||
        (payload.first_key != KEYBOARD_APP_CONFIG_KEY_FIRST) ||
        (payload.key_count != KEYBOARD_APP_CONFIG_KEY_COUNT))
    {
        return false;
    }

    return keyboard_app_set_config_keycodes(payload.key_map,
                                            sizeof(payload.key_map));
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
