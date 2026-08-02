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

typedef struct
{
    TIM_HandleTypeDef *timer;
    uint32_t channel;
} app_led_transport_t;

static WS28XX_HandleTypeDef led_handle;
static app_led_transport_t led_transport =
{
    .timer = &htim3,
    .channel = TIM_CHANNEL_3
};

static DMA_HandleTypeDef *app_led_transport_dma(app_led_transport_t const *transport)
{
    if ((transport == NULL) || (transport->timer == NULL))
    {
        return NULL;
    }

    switch (transport->channel)
    {
        case TIM_CHANNEL_1:
            return transport->timer->hdma[TIM_DMA_ID_CC1];

        case TIM_CHANNEL_2:
            return transport->timer->hdma[TIM_DMA_ID_CC2];

        case TIM_CHANNEL_3:
            return transport->timer->hdma[TIM_DMA_ID_CC3];

        case TIM_CHANNEL_4:
            return transport->timer->hdma[TIM_DMA_ID_CC4];

        default:
            return NULL;
    }
}

static bool app_led_configure_pwm(void *context,
                                  uint32_t period_ticks,
                                  WS28XX_DmaSample pulse0_ticks,
                                  WS28XX_DmaSample pulse1_ticks)
{
    app_led_transport_t *transport = context;

    if ((transport == NULL) ||
        (transport->timer == NULL) ||
        (period_ticks == 0U) ||
        (period_ticks > 65536U) ||
        ((uint32_t)pulse0_ticks >= period_ticks) ||
        ((uint32_t)pulse1_ticks >= period_ticks))
    {
        return false;
    }

    __HAL_TIM_SET_PRESCALER(transport->timer, 0U);
    __HAL_TIM_SET_AUTORELOAD(transport->timer, period_ticks - 1U);
    __HAL_TIM_SET_COMPARE(transport->timer, transport->channel, 0U);
    __HAL_TIM_SET_COUNTER(transport->timer, 0U);
    return true;
}

static bool app_led_start_dma(void *context,
                              WS28XX_DmaSample const *samples,
                              size_t sample_count)
{
    app_led_transport_t *transport = context;

    if ((transport == NULL) ||
        (transport->timer == NULL) ||
        (samples == NULL) ||
        (sample_count == 0U) ||
        (sample_count > UINT16_MAX))
    {
        return false;
    }

    return HAL_TIM_PWM_Start_DMA(
               transport->timer,
               transport->channel,
               (uint32_t const *)(void const *)samples,
               (uint16_t)sample_count) == HAL_OK;
}

static bool app_led_stop_dma(void *context)
{
    app_led_transport_t *transport = context;
    DMA_HandleTypeDef *dma = app_led_transport_dma(transport);
    uint32_t dma_request;

    if ((transport == NULL) || (transport->timer == NULL) || (dma == NULL))
    {
        return false;
    }

    switch (transport->channel)
    {
        case TIM_CHANNEL_1:
            dma_request = TIM_DMA_CC1;
            break;

        case TIM_CHANNEL_2:
            dma_request = TIM_DMA_CC2;
            break;

        case TIM_CHANNEL_3:
            dma_request = TIM_DMA_CC3;
            break;

        case TIM_CHANNEL_4:
            dma_request = TIM_DMA_CC4;
            break;

        default:
            return false;
    }

    /* STM32H5 HAL_TIM_PWM_Stop_DMA always requests an asynchronous DMA abort,
       even from the transfer-complete callback. Stop an active DMA channel
       synchronously, then disable the request and PWM without asking HAL to
       abort an already-completed transfer. */
    if ((HAL_DMA_GetState(dma) == HAL_DMA_STATE_BUSY) &&
        (HAL_DMA_Abort(dma) != HAL_OK) &&
        (HAL_DMA_GetState(dma) != HAL_DMA_STATE_READY))
    {
        return false;
    }

    if (HAL_DMA_GetState(dma) != HAL_DMA_STATE_READY)
    {
        return false;
    }

    __HAL_TIM_DISABLE_DMA(transport->timer, dma_request);
    if (HAL_TIM_PWM_Stop(transport->timer, transport->channel) != HAL_OK)
    {
        return false;
    }

    /* Restore a known low baseline after complete, error, or mid-frame abort.
       The update event commits CCR=0 even when compare preload is enabled. */
    __HAL_TIM_SET_COMPARE(transport->timer, transport->channel, 0U);
    __HAL_TIM_SET_COUNTER(transport->timer, 0U);
    if (HAL_TIM_GenerateEvent(transport->timer,
                              TIM_EVENTSOURCE_UPDATE) != HAL_OK)
    {
        return false;
    }
    __HAL_TIM_SET_COUNTER(transport->timer, 0U);
    return true;
}

static uintptr_t app_led_enter_critical(void *context)
{
    uint32_t primask;

    (void)context;
    primask = __get_PRIMASK();
    __disable_irq();
    __DMB();
    return primask;
}

static void app_led_exit_critical(void *context, uintptr_t state)
{
    (void)context;
    __DMB();
    if (state == 0U)
    {
        __enable_irq();
    }
}

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

    init_ok = WS28XX_Init(&led_handle, &(WS28XX_Config)
    {
        .TransportContext = &led_transport,
        .ConfigurePwm = app_led_configure_pwm,
        .StartDma = app_led_start_dma,
        .StopDma = app_led_stop_dma,
        .EnterCritical = app_led_enter_critical,
        .ExitCritical = app_led_exit_critical,
        .TimerFrequencyHz = 250000000U,
        .BitPeriodNs = WS28XX_DEFAULT_BIT_PERIOD_NS,
        .T0HNs = WS28XX_DEFAULT_T0H_NS,
        .T1HNs = WS28XX_DEFAULT_T1H_NS,
        .PixelCount = MAI2LED_APP_MAX_LED_TOTAL,
        .ColorOrder = WS28XX_COLOR_ORDER_GRB,
        .GammaEnabled = true,
        .GammaTable = NULL
    });

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
    if ((htim != NULL) &&
        (htim->Instance == TIM3) &&
        (htim->Channel == HAL_TIM_ACTIVE_CHANNEL_3))
    {
        if (WS28XX_GetState(&led_handle) == WS28XX_STATE_TX_BUSY)
        {
            DMA_HandleTypeDef *dma = app_led_transport_dma(&led_transport);

            if ((dma != NULL) &&
                (HAL_DMA_GetError(dma) != HAL_DMA_ERROR_NONE))
            {
                (void)WS28XX_NotifyTransferError(&led_handle);
                mai2led_app_notify_tx_error();
            }
            else if (WS28XX_NotifyTransferComplete(&led_handle))
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
    if ((htim != NULL) &&
        (htim->Instance == TIM3) &&
        (htim->Channel == HAL_TIM_ACTIVE_CHANNEL_3))
    {
        if (WS28XX_GetState(&led_handle) == WS28XX_STATE_TX_BUSY)
        {
            (void)WS28XX_NotifyTransferError(&led_handle);
            mai2led_app_notify_tx_error();
        }
    }
}
