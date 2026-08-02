#include "app.h"

#include <stddef.h>

#include "aime/aime_reader_app.h"
#include "button/button_app.h"
#include "config/config_app.h"
#include "kb/keyboard_app.h"
#include "led/mai2led_app.h"
#include "status/status_led_app.h"
#include "cdc_manager.h"
#include "mai2touch.h"
#include "tenodata.h"
#include "tim.h"
#include "tusb.h"
#include "ws28xx.h"

#define APP_IDLE_LIGHT_RESTORE_BUTTON  8U

static WS28XX_HandleTypeDef led_handle;

static void app_restore_idle_lights(uint8_t button_id, void *context)
{
    (void)button_id;
    (void)context;
    mai2led_app_restore_idle_lights();
}

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
    bool init_ok;

    status_led_app_init();

    init_ok = WS28XX_Init(&led_handle,
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

    init_ok = tusb_init() && init_ok;
    cdc_manager_init();
    aime_reader_app_init();
    tenodata_init();
    mai2touch_init();
    (void)button_app_set_long_press_callback(
        APP_IDLE_LIGHT_RESTORE_BUTTON,
        app_restore_idle_lights,
        NULL);
    button_app_init();
    keyboard_app_init();
    mai2led_app_init(&(mai2led_app_config_t)
    {
        .led = &led_handle,
        .led_per_bit = MAI2LED_APP_DEFAULT_LED_PER_BIT,
        .button_read = button_app_read_main_mask8
    });
    init_ok = config_app_init() && init_ok;

    if (init_ok)
    {
        status_led_app_set_running();
    }
    else
    {
        status_led_app_set_error();
    }
}

void app_task(void)
{
    status_led_app_task();
    tud_task();
    aime_reader_app_task();
    tenodata_task();
    mai2touch_task();
    button_app_task();
    keyboard_app_task();
    mai2led_app_task();
    config_app_task();
}

void HAL_TIM_PWM_PulseFinishedCallback(TIM_HandleTypeDef *htim)
{
    if ((htim != NULL) && (htim->Instance == TIM3))
    {
        HAL_TIM_PWM_Stop_DMA(htim, TIM_CHANNEL_3);
    }
}
