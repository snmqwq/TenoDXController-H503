/*
 * Host-side test for the PA0..PA7 button mapping. PA pins use the H503
 * pull-up: an open-drain button pulls low when released and releases high when
 * pressed. A boot-high main input must show a stable release before arming.
 * PB0/PB1/PB2/PB10 remain active-low.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "button_app.h"
#include "main.h"

GPIO_TypeDef native_gpio_a = {.port_id = 0U};
GPIO_TypeDef native_gpio_b = {.port_id = 1U};
uint32_t native_test_tick;

static GPIO_PinState pa_levels[8];
static GPIO_PinState pb_levels[4];

uint32_t HAL_GetTick(void)
{
    return native_test_tick;
}

static GPIO_PinState pb_level(uint16_t pin)
{
    switch (pin)
    {
        case BTN8_Pin:
            return pb_levels[0];

        case BTN9_Pin:
            return pb_levels[1];

        case BTN10_Pin:
            return pb_levels[2];

        case BTN11_Pin:
            return pb_levels[3];

        default:
            return GPIO_PIN_SET;
    }
}

GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef *port, uint16_t pin)
{
    if (port == GPIOA)
    {
        for (uint8_t i = 0U; i < 8U; i++)
        {
            if (pin == (uint16_t)(1U << i))
            {
                return pa_levels[i];
            }
        }
    }
    else if (port == GPIOB)
    {
        return pb_level(pin);
    }

    return GPIO_PIN_SET;
}

static void run_button_ticks(unsigned count)
{
    for (unsigned i = 0U; i < count; i++)
    {
        native_test_tick += 5U;
        button_app_task();
    }
}

static int failures;
static void record_long_press(uint8_t id, void *context)
{
    if (id == 1U)
        ++*(unsigned *)context;
}

static void check(bool condition, const char *name)
{
    printf("  %s %s\n", condition ? "OK  " : "FAIL", name);
    if (!condition)
    {
        failures++;
    }
}

int main(void)
{
    printf("=== native button test ===\n");

    for (uint8_t i = 0U; i < 8U; i++)
    {
        pa_levels[i] = GPIO_PIN_RESET;
    }
    for (uint8_t i = 0U; i < 4U; i++)
    {
        pb_levels[i] = GPIO_PIN_SET;
    }

    /* PA1 is held at boot; PA2 is an empty pulled-up connector. */
    pa_levels[1] = GPIO_PIN_SET;
    pa_levels[2] = GPIO_PIN_SET;
    pb_levels[1] = GPIO_PIN_RESET;
    button_app_init();
    run_button_ticks(4U);
    check(!button_app_is_enabled(1U) && !button_app_is_enabled(2U),
          "held and unconnected main keys are initially masked");
    unsigned long_presses = 0U;
    check(button_app_set_long_press_callback(1U, record_long_press, &long_presses),
          "a boot-held key accepts its long-press callback before arming");
    check(button_app_is_pressed_immediate(9U), "a config key held at boot is enabled");

    pa_levels[0] = GPIO_PIN_SET;
    check(button_app_is_pressed_immediate(0U), "installed main key presses immediately");
    run_button_ticks(3U);
    check(button_app_is_pressed(0U), "PA0 high triggers after debounce");
    check(button_app_read_main_mask8() == 0x0001U,
          "PA0 is included in the main button mask");

    pa_levels[0] = GPIO_PIN_RESET;
    check(!button_app_is_pressed_immediate(0U), "installed main key releases immediately");
    run_button_ticks(4U);
    check(!button_app_is_pressed(0U), "PA0 release clears the button");

    pa_levels[1] = GPIO_PIN_RESET;
    run_button_ticks(2U);
    pa_levels[1] = GPIO_PIN_SET;
    run_button_ticks(1U);
    pa_levels[1] = GPIO_PIN_RESET;
    run_button_ticks(2U);
    check(!button_app_is_enabled(1U), "an interrupted release does not arm PA1");
    run_button_ticks(1U);
    check(button_app_is_enabled(1U) && !button_app_is_pressed_immediate(1U),
          "a stable release arms PA1 without reporting a press");
    pa_levels[1] = GPIO_PIN_SET;
    check(button_app_read_main_mask8() == 0x0002U, "next PA1 press reaches the mask immediately");
    run_button_ticks(4U);
    check(button_app_is_pressed(1U), "recovered PA1 also participates in MultiButton");
    check(!button_app_is_enabled(2U) && !button_app_is_pressed_immediate(2U),
          "the empty PA2 connector remains masked");
    run_button_ticks(1002U);
    check(long_presses == 1U, "the recovered key dispatches its registered long press");

    pb_levels[0] = GPIO_PIN_RESET;
    check(!button_app_is_pressed_immediate(8U), "config input still requires debounce");
    run_button_ticks(3U);
    check(button_app_is_pressed(8U), "PB0 remains active-low");

    if (failures == 0)
    {
        printf("all native button checks passed\n");
        return 0;
    }

    printf("%d native button check(s) failed\n", failures);
    return 1;
}
