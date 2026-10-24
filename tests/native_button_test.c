/*
 * Host-side test for the PA0..PA7 button mapping. PA pins use the H503
 * pull-up: an open-drain button pulls low when released and releases high when
 * pressed. Any PA pin high during button_app_init() is masked for this boot.
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

static void run_button_ticks(uint8_t count)
{
    for (uint8_t i = 0U; i < count; i++)
    {
        native_test_tick += 5U;
        button_app_task();
    }
}

static int failures;

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

    /* PA1 is held high at boot, so it must be unavailable for this boot. */
    pa_levels[1] = GPIO_PIN_SET;
    button_app_init();
    run_button_ticks(4U);

    pa_levels[0] = GPIO_PIN_SET;
    run_button_ticks(3U);
    check(button_app_is_pressed(0U), "PA0 high triggers after debounce");
    check(button_app_read_main_mask8() == 0x0001U,
          "PA0 is included in the main button mask");

    pa_levels[0] = GPIO_PIN_RESET;
    run_button_ticks(4U);
    check(!button_app_is_pressed(0U), "PA0 release clears the button");

    pa_levels[1] = GPIO_PIN_RESET;
    run_button_ticks(4U);
    pa_levels[1] = GPIO_PIN_SET;
    run_button_ticks(4U);
    check(!button_app_is_pressed(1U), "boot-high PA1 remains masked");
    check((button_app_read_main_mask8() & 0x0002U) == 0U,
          "boot-high PA1 is absent from the main button mask");

    pb_levels[0] = GPIO_PIN_RESET;
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
