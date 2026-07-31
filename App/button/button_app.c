#include "button_app.h"

#include <stddef.h>

#include "main.h"
#include "third_party/multi_button.h"

#define BUTTON_APP_TICK_INTERVAL_MS  5U

static Button buttons[BUTTON_APP_COUNT];
static button_app_callback_t long_press_callbacks[BUTTON_APP_COUNT];
static void *long_press_contexts[BUTTON_APP_COUNT];
static uint32_t last_button_tick;
static bool initialized;

static uint8_t button_app_read_gpio(uint8_t button_id)
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

static void button_app_long_press_handler(Button *handle, void *user_data)
{
    uint8_t button_id;

    (void)user_data;

    if (handle == NULL)
    {
        return;
    }

    button_id = handle->button_id;

    if ((button_id < BUTTON_APP_COUNT) &&
        (long_press_callbacks[button_id] != NULL))
    {
        long_press_callbacks[button_id](button_id,
                                        long_press_contexts[button_id]);
    }
}

void button_app_init(void)
{
    for (uint8_t i = 0; i < BUTTON_APP_COUNT; i++)
    {
        uint8_t active_level =
            (i < BUTTON_APP_MAIN_COUNT) ? GPIO_PIN_SET : GPIO_PIN_RESET;

        button_init(&buttons[i], button_app_read_gpio, active_level, i);

        if (long_press_callbacks[i] != NULL)
        {
            button_attach(&buttons[i],
                          BTN_LONG_PRESS_START,
                          button_app_long_press_handler,
                          NULL);
        }

        if ((i >= BUTTON_APP_MAIN_COUNT) ||
            (button_app_read_gpio(i) == GPIO_PIN_RESET))
        {
            (void)button_start(&buttons[i]);
        }
    }

    initialized = true;
}

void button_app_task(void)
{
    uint32_t now = HAL_GetTick();

    if ((uint32_t)(now - last_button_tick) >= BUTTON_APP_TICK_INTERVAL_MS)
    {
        last_button_tick = now;
        button_ticks();
    }
}

bool button_app_is_pressed(uint8_t button_id)
{
    if (!initialized || (button_id >= BUTTON_APP_COUNT))
    {
        return false;
    }

    return button_is_pressed(&buttons[button_id]) > 0;
}

uint16_t button_app_read_main_mask8(void)
{
    uint16_t mask = 0U;

    for (uint8_t i = 0; i < BUTTON_APP_MAIN_COUNT; i++)
    {
        if (button_app_is_pressed(i))
        {
            mask |= (uint16_t)(1U << i);
        }
    }

    return mask;
}

bool button_app_set_long_press_callback(uint8_t button_id,
                                        button_app_callback_t callback,
                                        void *context)
{
    if (button_id >= BUTTON_APP_COUNT)
    {
        return false;
    }

    long_press_callbacks[button_id] = callback;
    long_press_contexts[button_id] = context;

    if (initialized)
    {
        if (callback != NULL)
        {
            button_attach(&buttons[button_id],
                          BTN_LONG_PRESS_START,
                          button_app_long_press_handler,
                          NULL);
        }
        else
        {
            button_detach(&buttons[button_id], BTN_LONG_PRESS_START);
        }
    }

    return true;
}
