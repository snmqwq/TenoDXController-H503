#include "app.h"

#include <stddef.h>

#include "aime/aime_reader_app.h"
#include "config/magic_config.h"
#include "config/magic_config_light.h"
#include "kb/keyboard_app.h"
#include "led/mai2led_app.h"
#include "cdc_manager.h"
#include "mai2touch.h"
#include "tenodata.h"
#include "tim.h"
#include "tusb.h"
#include "ws28xx.h"

static WS28XX_HandleTypeDef led_handle;

static bool app_set_pixels_rgb(WS28XX_HandleTypeDef *led,
                               uint16_t start_pixel,
                               uint16_t end_pixel,
                               uint8_t red,
                               uint8_t green,
                               uint8_t blue)
{
    if ((led == NULL) ||
        (start_pixel > end_pixel) ||
        (end_pixel >= led->MaxPixel))
    {
        return false;
    }

    for (uint16_t pixel = start_pixel; pixel <= end_pixel; pixel++)
    {
        if (!WS28XX_SetPixel_RGB(led, pixel, red, green, blue))
        {
            return false;
        }
    }

    return true;
}

void app_init(void)
{
    WS28XX_Init(&led_handle,
                 &htim3,
                 250U,
                 TIM_CHANNEL_3,
                 MAI2LED_APP_MAX_LED_TOTAL);

    if (led_handle.MaxPixel > 0U)
    {
        app_set_pixels_rgb(&led_handle,
                           0U,
                           led_handle.MaxPixel - 1U,
                           0U,
                           0U,
                           0U);
        WS28XX_Update(&led_handle);
    }

    tusb_init();
    cdc_manager_init();
    aime_reader_app_init();
    tenodata_init();
    mai2touch_init();
    magic_config_init();
    keyboard_app_init(mai2led_app_restore_idle_lights);
    mai2led_app_init(&(mai2led_app_config_t)
    {
        .led = &led_handle,
        .led_per_bit = MAI2LED_APP_DEFAULT_LED_PER_BIT,
        .button_read = keyboard_app_button_read_mask8
    });
    (void)magic_config_light_register();
}

void app_task(void)
{
    tud_task();
    aime_reader_app_task();
    tenodata_task();
    mai2touch_task();
    keyboard_app_poll();
    mai2led_app_task();
    magic_config_task();
}

void HAL_TIM_PWM_PulseFinishedCallback(TIM_HandleTypeDef *htim)
{
    if ((htim != NULL) && (htim->Instance == TIM3))
    {
        HAL_TIM_PWM_Stop_DMA(htim, TIM_CHANNEL_3);
    }
}
