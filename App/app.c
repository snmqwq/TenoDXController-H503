#include "app.h"

#include <stddef.h>

#include "aime/aime_reader_app.h"
#include "button/button_app.h"
#include "config/config_app.h"
#include "kb/keyboard_app.h"
#include "led/mai2led_app.h"
#include "status/status_led_app.h"
#include "touch/touch_app.h"
#include "tim.h"
#include "tusb.h"
#include "led/ws2812b/ws2812b.h"

#define APP_IDLE_LIGHT_RESTORE_BUTTON  8U

static WS2812B_HandleTypeDef led_handle;

static void app_restore_idle_lights(uint8_t button_id, void *context)
{
    (void)button_id;
    (void)context;
    mai2led_app_restore_idle_lights();
}

void app_init(void)
{
    bool init_ok;

    status_led_app_init();

    init_ok = WS2812B_Init(&led_handle, &(WS2812B_Config)
    {
        .Timer = &htim3,
        .Dma = htim3.hdma[TIM_DMA_ID_CC3],
        .TimerChannel = TIM_CHANNEL_3,
        .TimerFrequencyHz = 250000000U,
        .BitPeriodNs = WS2812B_DEFAULT_BIT_PERIOD_NS,
        .T0HNs = WS2812B_DEFAULT_T0H_NS,
        .T1HNs = WS2812B_DEFAULT_T1H_NS,
        .PixelCount = MAI2LED_APP_MAX_LED_TOTAL,
        .ColorOrder = WS2812B_COLOR_ORDER_GRB,
        .GammaEnabled = true,
        .GammaTable = NULL
    });

    init_ok = tusb_init() && init_ok;
    aime_reader_app_init();
    touch_app_init();
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
    touch_app_task();
    button_app_task();
    keyboard_app_task();
    mai2led_app_task();
    config_app_task();
}

void HAL_TIM_PWM_PulseFinishedCallback(TIM_HandleTypeDef *htim)
{
    if (WS2812B_IsTimerCallback(&led_handle, htim))
    {
        if (WS2812B_GetState(&led_handle) == WS2812B_STATE_TX_BUSY)
        {
            if (WS2812B_HasDmaError(&led_handle))
            {
                (void)WS2812B_NotifyTransferError(&led_handle);
                mai2led_app_notify_tx_error();
            }
            else if (WS2812B_NotifyTransferComplete(&led_handle))
            {
                mai2led_app_notify_tx_complete();
            }
            else
            {
                mai2led_app_notify_tx_error();
            }
        }
    }
}

void HAL_TIM_ErrorCallback(TIM_HandleTypeDef *htim)
{
    if (WS2812B_IsTimerCallback(&led_handle, htim))
    {
        if (WS2812B_GetState(&led_handle) == WS2812B_STATE_TX_BUSY)
        {
            (void)WS2812B_NotifyTransferError(&led_handle);
            mai2led_app_notify_tx_error();
        }
    }
}
