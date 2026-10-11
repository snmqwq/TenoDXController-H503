#include "keyboard_app.h"

#include <string.h>

#include "button/button_app.h"
#include "stm32h5xx_hal.h"
#include "tusb.h"

typedef struct __attribute__((packed))
{
    uint8_t modifier;
    uint8_t reserved;
    uint8_t keycode[KEYBOARD_APP_KEY_COUNT];
} hid_keyboard_12kro_report_t;

_Static_assert(KEYBOARD_APP_KEY_COUNT == BUTTON_APP_COUNT,
               "Every keyboard key must have a matching button");
_Static_assert(KEYBOARD_APP_CONFIG_KEY_FIRST == KEYBOARD_APP_MAIN_KEY_COUNT,
               "Configurable keys must follow the main keys");
_Static_assert((KEYBOARD_APP_MAIN_KEY_COUNT +
                KEYBOARD_APP_CONFIG_KEY_COUNT) == KEYBOARD_APP_KEY_COUNT,
               "Main and configurable key counts must cover every key");
_Static_assert(sizeof(hid_keyboard_12kro_report_t) == 14U,
               "The HID report must remain 14 bytes");

static const uint8_t
main_hid_key_maps[KEYBOARD_APP_MAIN_LAYOUT_COUNT][KEYBOARD_APP_MAIN_KEY_COUNT] =
{
    [KEYBOARD_APP_MAIN_LAYOUT_1P] =
    {
        HID_KEY_W,
        HID_KEY_E,
        HID_KEY_D,
        HID_KEY_C,
        HID_KEY_X,
        HID_KEY_Z,
        HID_KEY_A,
        HID_KEY_Q
    },
    [KEYBOARD_APP_MAIN_LAYOUT_2P] =
    {
        HID_KEY_KEYPAD_8,
        HID_KEY_KEYPAD_9,
        HID_KEY_KEYPAD_6,
        HID_KEY_KEYPAD_3,
        HID_KEY_KEYPAD_2,
        HID_KEY_KEYPAD_1,
        HID_KEY_KEYPAD_4,
        HID_KEY_KEYPAD_7
    }
};

static const uint8_t default_config_hid_key_map[KEYBOARD_APP_CONFIG_KEY_COUNT] =
{
    HID_KEY_3,
    HID_KEY_9,
    HID_KEY_8,
    0U
};

static uint8_t config_hid_key_map[KEYBOARD_APP_CONFIG_KEY_COUNT];
static keyboard_app_main_layout_t main_layout;
static bool io_mode_enabled;

static uint8_t keyboard_app_keycode_for_index(uint8_t index)
{
    if (index < KEYBOARD_APP_MAIN_KEY_COUNT)
    {
        return main_hid_key_maps[main_layout][index];
    }

    return config_hid_key_map[index - KEYBOARD_APP_CONFIG_KEY_FIRST];
}

static bool keyboard_app_send_hid_report(void)
{
    hid_keyboard_12kro_report_t report;
    uint8_t key_count = 0U;

    memset(&report, 0, sizeof(report));

    for (uint8_t i = 0; i < KEYBOARD_APP_KEY_COUNT; i++)
    {
        if (io_mode_enabled && i != 11U)
        {
            continue;
        }

        /* Game input: main keys are read without MultiButton's debounce. */
        if (button_app_is_pressed_immediate(i) &&
            (key_count < KEYBOARD_APP_KEY_COUNT))
        {
            report.keycode[key_count++] =
                keyboard_app_keycode_for_index(i);
        }
    }

    if (!tud_hid_n_ready(0))
    {
        return false;
    }

    return tud_hid_n_report(0, 0, &report, sizeof(report));
}

void keyboard_app_init(void)
{
    keyboard_app_reset_keycodes();
}

void keyboard_app_task(void)
{
    /*
     * 端点一空就用当前按键状态补一帧, 不再等固定 2 ms 拍点: 主机每 2 ms
     * 轮询一次 IN 端点, 采样时刻越贴近轮询时刻, 按键就越快被上报。
     * 上一帧还没发走时 tud_hid_n_ready() 为假, 所以不会堆积报文。
     */
    (void)keyboard_app_send_hid_report();
}

bool keyboard_app_get_keycode(uint8_t index, uint8_t *out_keycode)
{
    if ((index >= KEYBOARD_APP_KEY_COUNT) || (out_keycode == NULL))
    {
        return false;
    }

    *out_keycode = keyboard_app_keycode_for_index(index);
    return true;
}

bool keyboard_app_set_keycode(uint8_t index, uint8_t keycode)
{
    if ((index < KEYBOARD_APP_CONFIG_KEY_FIRST) ||
        (index >= KEYBOARD_APP_KEY_COUNT))
    {
        return false;
    }

    config_hid_key_map[index - KEYBOARD_APP_CONFIG_KEY_FIRST] = keycode;
    return true;
}

bool keyboard_app_get_config_keycodes(uint8_t *data, uint8_t length)
{
    if ((data == NULL) || (length != KEYBOARD_APP_CONFIG_KEY_COUNT))
    {
        return false;
    }

    memcpy(data, config_hid_key_map, KEYBOARD_APP_CONFIG_KEY_COUNT);
    return true;
}

bool keyboard_app_set_config_keycodes(uint8_t const *data, uint8_t length)
{
    if ((data == NULL) || (length != KEYBOARD_APP_CONFIG_KEY_COUNT))
    {
        return false;
    }

    memcpy(config_hid_key_map, data, KEYBOARD_APP_CONFIG_KEY_COUNT);
    return true;
}

bool keyboard_app_get_main_layout(keyboard_app_main_layout_t *out_layout)
{
    if (out_layout == NULL)
    {
        return false;
    }

    *out_layout = main_layout;
    return true;
}

bool keyboard_app_set_main_layout(keyboard_app_main_layout_t layout)
{
    if ((layout != KEYBOARD_APP_MAIN_LAYOUT_1P) &&
        (layout != KEYBOARD_APP_MAIN_LAYOUT_2P))
    {
        return false;
    }

    main_layout = layout;
    return true;
}

void keyboard_app_reset_keycodes(void)
{
    main_layout = KEYBOARD_APP_MAIN_LAYOUT_1P;
    memcpy(config_hid_key_map,
           default_config_hid_key_map,
           sizeof(config_hid_key_map));
}

void keyboard_app_set_io_mode(bool enabled)
{
    io_mode_enabled = enabled;
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
