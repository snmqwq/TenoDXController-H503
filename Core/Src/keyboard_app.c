#include "keyboard_app.h"

#include "flash_config.h"
#include "magic_config.h"
#include "main.h"
#include "multi_button.h"
#include "tusb.h"
#include <string.h>

#define BUTTON_TICK_INTERVAL_MS     5U
#define HID_REPORT_INTERVAL_MS      5U
#define KEYBOARD_FIXED_KEY_COUNT    8U
#define KEYBOARD_CONFIG_KEY_FIRST   8U
#define KEYBOARD_CONFIG_KEY_COUNT   (KEYBOARD_APP_KEY_COUNT - KEYBOARD_CONFIG_KEY_FIRST)
#define KEYBOARD_FLASH_VERSION      2U
#define KEYBOARD_CONFIG_PAYLOAD_VERSION  1U

#define KEYBOARD_PARAM_CONFIG_KEYS  0x80U

typedef struct __attribute__((packed))
{
    uint8_t modifier;
    uint8_t reserved;
    uint8_t keycode[KEYBOARD_APP_KEY_COUNT];
} hid_keyboard_11kro_report_t;

typedef struct
{
    uint8_t version;
    uint8_t first_key;
    uint8_t key_count;
    uint8_t key_map[KEYBOARD_CONFIG_KEY_COUNT];
} keyboard_flash_config_t;

typedef struct
{
    uint8_t version;
    uint8_t first_key;
    uint8_t key_count;
    uint8_t key_map[KEYBOARD_CONFIG_KEY_COUNT];
} keyboard_config_payload_t;

static Button buttons[KEYBOARD_APP_KEY_COUNT];
static keyboard_app_event_cb_t btn8_long_press_callback;

static const uint8_t default_hid_key_map[KEYBOARD_APP_KEY_COUNT] =
{
    HID_KEY_W,
    HID_KEY_E,
    HID_KEY_D,
    HID_KEY_C,
    HID_KEY_X,
    HID_KEY_Z,
    HID_KEY_A,
    HID_KEY_Q,
    HID_KEY_3,
    HID_KEY_KEYPAD_MULTIPLY,
    HID_KEY_9
};

static uint8_t hid_key_map[KEYBOARD_APP_KEY_COUNT];

static uint8_t keyboard_read_button_gpio(uint8_t button_id)
{
    switch (button_id)
    {
        case 0:
            return HAL_GPIO_ReadPin(BTN0_GPIO_Port, BTN0_Pin);

        case 1:
            return HAL_GPIO_ReadPin(BTN1_GPIO_Port, BTN1_Pin);

        case 2:
            return HAL_GPIO_ReadPin(BTN2_GPIO_Port, BTN2_Pin);

        case 3:
            return HAL_GPIO_ReadPin(BTN3_GPIO_Port, BTN3_Pin);

        case 4:
            return HAL_GPIO_ReadPin(BTN4_GPIO_Port, BTN4_Pin);

        case 5:
            return HAL_GPIO_ReadPin(BTN5_GPIO_Port, BTN5_Pin);

        case 6:
            return HAL_GPIO_ReadPin(BTN6_GPIO_Port, BTN6_Pin);

        case 7:
            return HAL_GPIO_ReadPin(BTN7_GPIO_Port, BTN7_Pin);

        case 8:
            return HAL_GPIO_ReadPin(BTN8_GPIO_Port, BTN8_Pin);

        case 9:
            return HAL_GPIO_ReadPin(BTN9_GPIO_Port, BTN9_Pin);

        case 10:
            return HAL_GPIO_ReadPin(BTN10_GPIO_Port, BTN10_Pin);

        default:
            return GPIO_PIN_SET;
    }
}

static void btn8_long_press_handler(Button *handle, void *user_data)
{
    (void)handle;
    (void)user_data;

    if (btn8_long_press_callback != NULL)
    {
        btn8_long_press_callback();
    }
}

static void keyboard_load_default(void)
{
    memcpy(hid_key_map, default_hid_key_map, sizeof(hid_key_map));
}

static bool keyboard_load_from_flash(void)
{
    keyboard_flash_config_t config;

    if (!flash_config_read(FLASH_CONFIG_SLOT_KEYBOARD, &config, sizeof(config), NULL) ||
        (config.version != KEYBOARD_FLASH_VERSION) ||
        (config.first_key != KEYBOARD_CONFIG_KEY_FIRST) ||
        (config.key_count != KEYBOARD_CONFIG_KEY_COUNT))
    {
        return false;
    }

    memcpy(&hid_key_map[KEYBOARD_CONFIG_KEY_FIRST], config.key_map, KEYBOARD_CONFIG_KEY_COUNT);
    return true;
}

static bool keyboard_save_to_flash(void)
{
    keyboard_flash_config_t config;

    memset(&config, 0xff, sizeof(config));
    config.version = KEYBOARD_FLASH_VERSION;
    config.first_key = KEYBOARD_CONFIG_KEY_FIRST;
    config.key_count = KEYBOARD_CONFIG_KEY_COUNT;
    memcpy(config.key_map, &hid_key_map[KEYBOARD_CONFIG_KEY_FIRST], KEYBOARD_CONFIG_KEY_COUNT);

    return flash_config_write(FLASH_CONFIG_SLOT_KEYBOARD, &config, sizeof(config));
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
        if (max_length < 1U)
        {
            return false;
        }

        data[0] = hid_key_map[param];
        *out_length = 1U;
        return true;
    }

    if (param == KEYBOARD_PARAM_CONFIG_KEYS)
    {
        if (max_length < KEYBOARD_CONFIG_KEY_COUNT)
        {
            return false;
        }

        memcpy(data, &hid_key_map[KEYBOARD_CONFIG_KEY_FIRST], KEYBOARD_CONFIG_KEY_COUNT);
        *out_length = KEYBOARD_CONFIG_KEY_COUNT;
        return true;
    }

    return false;
}

static bool keyboard_magic_write(uint8_t param, uint8_t const *data, uint8_t length)
{
    if (data == NULL)
    {
        return false;
    }

    if (param < KEYBOARD_APP_KEY_COUNT)
    {
        if ((param < KEYBOARD_CONFIG_KEY_FIRST) || (length != 1U))
        {
            return false;
        }

        hid_key_map[param] = data[0];
        return true;
    }

    if (param == KEYBOARD_PARAM_CONFIG_KEYS)
    {
        if (length != KEYBOARD_CONFIG_KEY_COUNT)
        {
            return false;
        }

        memcpy(&hid_key_map[KEYBOARD_CONFIG_KEY_FIRST], data, KEYBOARD_CONFIG_KEY_COUNT);
        return true;
    }

    return false;
}

static bool keyboard_magic_save(uint8_t param)
{
    (void)param;
    return keyboard_save_to_flash();
}

static bool keyboard_magic_load_default(uint8_t param)
{
    (void)param;
    keyboard_load_default();
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
    data[1] = KEYBOARD_CONFIG_KEY_FIRST;
    data[2] = KEYBOARD_CONFIG_KEY_COUNT;
    data[3] = KEYBOARD_PARAM_CONFIG_KEYS;
    *out_length = 4U;
    return true;
}

static bool keyboard_magic_read_all(uint8_t *data, uint8_t max_length, uint8_t *out_length)
{
    keyboard_config_payload_t payload;

    if ((data == NULL) || (out_length == NULL) || (max_length < sizeof(payload)))
    {
        return false;
    }

    payload.version = KEYBOARD_CONFIG_PAYLOAD_VERSION;
    payload.first_key = KEYBOARD_CONFIG_KEY_FIRST;
    payload.key_count = KEYBOARD_CONFIG_KEY_COUNT;
    memcpy(payload.key_map, &hid_key_map[KEYBOARD_CONFIG_KEY_FIRST], KEYBOARD_CONFIG_KEY_COUNT);

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

    if ((payload.version != KEYBOARD_CONFIG_PAYLOAD_VERSION) ||
        (payload.first_key != KEYBOARD_CONFIG_KEY_FIRST) ||
        (payload.key_count != KEYBOARD_CONFIG_KEY_COUNT))
    {
        return false;
    }

    memcpy(&hid_key_map[KEYBOARD_CONFIG_KEY_FIRST], payload.key_map, KEYBOARD_CONFIG_KEY_COUNT);

    return true;
}

static void keyboard_register_magic(void)
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

    (void)magic_config_register(&module);
}

void keyboard_app_init(keyboard_app_event_cb_t btn8_long_press_cb)
{
    btn8_long_press_callback = btn8_long_press_cb;
    keyboard_load_default();
    (void)keyboard_load_from_flash();

    for (uint8_t i = 0; i < KEYBOARD_APP_KEY_COUNT; i++)
    {
        uint8_t active_level = (i <= 7U) ? GPIO_PIN_SET : GPIO_PIN_RESET;

        button_init(&buttons[i], keyboard_read_button_gpio, active_level, i);

        if (i == 8U)
        {
            button_attach(&buttons[i], BTN_LONG_PRESS_START, btn8_long_press_handler, NULL);
        }

        if ((i > 7U) || (keyboard_read_button_gpio(i) == GPIO_PIN_RESET))
        {
            button_start(&buttons[i]);
        }
    }

    keyboard_register_magic();
}

static void hid_key_task(void)
{
    hid_keyboard_11kro_report_t report;
    uint8_t key_count = 0;

    memset(&report, 0, sizeof(report));

    for (uint8_t i = 0; i < KEYBOARD_APP_KEY_COUNT; i++)
    {
        if (button_is_pressed(&buttons[i]) && (key_count < KEYBOARD_APP_KEY_COUNT))
        {
            report.keycode[key_count++] = hid_key_map[i];
        }
    }

    if (tud_hid_n_ready(0))
    {
        tud_hid_n_report(0, 0, &report, sizeof(report));
    }
}

void keyboard_app_poll(void)
{
    static uint32_t last_button_tick = 0;
    static uint32_t last_hid_report = 0;
    uint32_t now = HAL_GetTick();

    if ((uint32_t)(now - last_button_tick) >= BUTTON_TICK_INTERVAL_MS)
    {
        last_button_tick = now;
        button_ticks();
    }

    if ((uint32_t)(now - last_hid_report) >= HID_REPORT_INTERVAL_MS)
    {
        last_hid_report = now;
        hid_key_task();
    }
}

uint16_t keyboard_app_button_read_mask8(void)
{
    uint16_t mask = 0;

    for (uint8_t i = 0; i < KEYBOARD_APP_MAIN_BUTTON_COUNT; i++)
    {
        if (button_is_pressed(&buttons[i]))
        {
            mask |= (uint16_t)(1U << i);
        }
    }

    return mask;
}

uint8_t *keyboard_app_get_key_map(void)
{
    return hid_key_map;
}

uint8_t const *keyboard_app_get_default_key_map(void)
{
    return default_hid_key_map;
}

uint8_t keyboard_app_get_key_count(void)
{
    return KEYBOARD_APP_KEY_COUNT;
}

void tud_hid_set_report_cb(uint8_t instance,
                           uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer,
                           uint16_t bufsize)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)bufsize;
}

uint16_t tud_hid_get_report_cb(uint8_t instance,
                               uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer,
                               uint16_t reqlen)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;

    return 0;
}
