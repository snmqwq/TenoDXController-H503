#include "ws2812b.h"

#include <string.h>

#define WS2812B_HANDLE_MAGIC  0x57533242UL

/* Generated from round(255 * pow(input / 255, 2.24)). */
static uint8_t const ws2812b_default_gamma_table[256] =
{
      0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   2,   2,   2,   2,   2,   2,
      2,   3,   3,   3,   3,   3,   4,   4,   4,   4,   4,   5,   5,   5,   6,   6,
      6,   6,   7,   7,   7,   8,   8,   8,   9,   9,   9,  10,  10,  10,  11,  11,
     12,  12,  12,  13,  13,  14,  14,  15,  15,  15,  16,  16,  17,  17,  18,  18,
     19,  20,  20,  21,  21,  22,  22,  23,  24,  24,  25,  25,  26,  27,  27,  28,
     29,  29,  30,  31,  31,  32,  33,  33,  34,  35,  36,  36,  37,  38,  39,  40,
     40,  41,  42,  43,  44,  45,  45,  46,  47,  48,  49,  50,  51,  52,  53,  54,
     54,  55,  56,  57,  58,  59,  60,  61,  62,  63,  64,  66,  67,  68,  69,  70,
     71,  72,  73,  74,  75,  77,  78,  79,  80,  81,  82,  84,  85,  86,  87,  89,
     90,  91,  92,  94,  95,  96,  97,  99, 100, 101, 103, 104, 106, 107, 108, 110,
    111, 113, 114, 115, 117, 118, 120, 121, 123, 124, 126, 127, 129, 130, 132, 133,
    135, 137, 138, 140, 141, 143, 145, 146, 148, 150, 151, 153, 155, 156, 158, 160,
    162, 163, 165, 167, 169, 170, 172, 174, 176, 178, 179, 181, 183, 185, 187, 189,
    191, 193, 195, 197, 198, 200, 202, 204, 206, 208, 210, 212, 214, 216, 218, 221,
    223, 225, 227, 229, 231, 233, 235, 237, 240, 242, 244, 246, 248, 251, 253, 255
};

static uintptr_t ws2812b_enter_critical(WS2812B_HandleTypeDef *led)
{
    uint32_t primask;

    (void)led;
    primask = __get_PRIMASK();
    __disable_irq();
    __DMB();
    return (uintptr_t)primask;
}

static void ws2812b_exit_critical(WS2812B_HandleTypeDef *led, uintptr_t state)
{
    (void)led;
    __DMB();
    if (state == 0U)
    {
        __enable_irq();
    }
}

static bool ws2812b_get_channel_parameters(
    uint32_t channel,
    uint32_t *dma_id,
    uint32_t *dma_request,
    HAL_TIM_ActiveChannel *active_channel)
{
    if ((dma_id == NULL) ||
        (dma_request == NULL) ||
        (active_channel == NULL))
    {
        return false;
    }

    switch (channel)
    {
        case TIM_CHANNEL_1:
            *dma_id = TIM_DMA_ID_CC1;
            *dma_request = TIM_DMA_CC1;
            *active_channel = HAL_TIM_ACTIVE_CHANNEL_1;
            return true;

        case TIM_CHANNEL_2:
            *dma_id = TIM_DMA_ID_CC2;
            *dma_request = TIM_DMA_CC2;
            *active_channel = HAL_TIM_ACTIVE_CHANNEL_2;
            return true;

        case TIM_CHANNEL_3:
            *dma_id = TIM_DMA_ID_CC3;
            *dma_request = TIM_DMA_CC3;
            *active_channel = HAL_TIM_ACTIVE_CHANNEL_3;
            return true;

        case TIM_CHANNEL_4:
            *dma_id = TIM_DMA_ID_CC4;
            *dma_request = TIM_DMA_CC4;
            *active_channel = HAL_TIM_ACTIVE_CHANNEL_4;
            return true;

        default:
            return false;
    }
}

static void ws2812b_set_status(WS2812B_HandleTypeDef *led, WS2812B_Status status)
{
    if (led != NULL)
    {
        led->LastStatus = status;
    }
}

static bool ws2812b_is_initialized(WS2812B_HandleTypeDef const *led)
{
    return (led != NULL) &&
           (led->Magic == WS2812B_HANDLE_MAGIC) &&
           (led->State != WS2812B_STATE_UNINITIALIZED) &&
           (led->Timer != NULL) &&
           (led->Dma != NULL) &&
           (led->Dma->Instance != NULL) &&
           (led->Timer->hdma[led->TimerDmaId] == led->Dma) &&
           (led->MaxPixel > 0U) &&
           (led->MaxPixel <= WS2812B_PIXEL_MAX);
}

static bool ws2812b_configure_pwm(WS2812B_HandleTypeDef *led)
{
    if ((led == NULL) ||
        (led->Timer == NULL) ||
        (led->PeriodTicks == 0U) ||
        (led->PeriodTicks > 65536U) ||
        ((uint32_t)led->Pulse0 >= led->PeriodTicks) ||
        ((uint32_t)led->Pulse1 >= led->PeriodTicks))
    {
        return false;
    }

    __HAL_TIM_SET_PRESCALER(led->Timer, 0U);
    __HAL_TIM_SET_AUTORELOAD(led->Timer, led->PeriodTicks - 1U);
    __HAL_TIM_SET_COMPARE(led->Timer, led->TimerChannel, 0U);
    __HAL_TIM_SET_COUNTER(led->Timer, 0U);

    /* Prescaler writes are buffered by the timer. Commit all timing registers
       before the first frame so a different timer setup cannot affect it. */
    if (HAL_TIM_GenerateEvent(led->Timer, TIM_EVENTSOURCE_UPDATE) != HAL_OK)
    {
        return false;
    }
    __HAL_TIM_CLEAR_FLAG(led->Timer, TIM_FLAG_UPDATE);
    __HAL_TIM_SET_COUNTER(led->Timer, 0U);
    return true;
}

static bool ws2812b_start_dma(WS2812B_HandleTypeDef *led,
                              WS2812B_DmaSample const *samples,
                              size_t sample_count)
{
    if ((led == NULL) ||
        (led->Timer == NULL) ||
        (led->Dma == NULL) ||
        (samples == NULL) ||
        (sample_count == 0U) ||
        (sample_count > UINT16_MAX) ||
        (led->Timer->hdma[led->TimerDmaId] != led->Dma))
    {
        return false;
    }

    return HAL_TIM_PWM_Start_DMA(
               led->Timer,
               led->TimerChannel,
               (uint32_t const *)(void const *)samples,
               (uint16_t)sample_count) == HAL_OK;
}

static bool ws2812b_stop_dma(WS2812B_HandleTypeDef *led)
{
    if ((led == NULL) ||
        (led->Timer == NULL) ||
        (led->Dma == NULL))
    {
        return false;
    }

    /* STM32H5 HAL_TIM_PWM_Stop_DMA requests an asynchronous abort even after
       transfer completion. Abort an active channel synchronously and only
       release the frame buffer after HAL reports DMA ready. */
    if ((HAL_DMA_GetState(led->Dma) == HAL_DMA_STATE_BUSY) &&
        (HAL_DMA_Abort(led->Dma) != HAL_OK) &&
        (HAL_DMA_GetState(led->Dma) != HAL_DMA_STATE_READY))
    {
        return false;
    }

    if (HAL_DMA_GetState(led->Dma) != HAL_DMA_STATE_READY)
    {
        return false;
    }

    __HAL_TIM_DISABLE_DMA(led->Timer, led->TimerDmaRequest);
    if (HAL_TIM_PWM_Stop(led->Timer, led->TimerChannel) != HAL_OK)
    {
        return false;
    }

    __HAL_TIM_SET_COMPARE(led->Timer, led->TimerChannel, 0U);
    __HAL_TIM_SET_COUNTER(led->Timer, 0U);
    if (HAL_TIM_GenerateEvent(led->Timer, TIM_EVENTSOURCE_UPDATE) != HAL_OK)
    {
        return false;
    }
    __HAL_TIM_SET_COUNTER(led->Timer, 0U);
    return true;
}

static uint32_t ws2812b_ns_to_ticks(uint32_t frequency_hz, uint32_t duration_ns)
{
    uint64_t scaled;
    uint64_t ticks;

    if ((frequency_hz == 0U) ||
        ((uint64_t)duration_ns >
         ((UINT64_MAX - 500000000ULL) / frequency_hz)))
    {
        return 0U;
    }

    scaled = ((uint64_t)frequency_hz * duration_ns) + 500000000ULL;
    ticks = scaled / 1000000000ULL;

    if (ticks > UINT32_MAX)
    {
        return 0U;
    }

    return (uint32_t)ticks;
}

static uint8_t ws2812b_default_gamma(uint8_t value)
{
    return ws2812b_default_gamma_table[value];
}

static uint8_t ws2812b_apply_gamma(WS2812B_HandleTypeDef const *led,
                                  uint8_t value)
{
    if (!led->GammaEnabled)
    {
        return value;
    }

    if (led->GammaTable != NULL)
    {
        return led->GammaTable[value];
    }

    return ws2812b_default_gamma(value);
}

static uint8_t ws2812b_scale(uint8_t value, uint8_t brightness)
{
    return (uint8_t)(((uint16_t)value * brightness) / 255U);
}

static bool ws2812b_set_rgb(WS2812B_HandleTypeDef *led,
                           uint16_t pixel,
                           uint8_t red,
                           uint8_t green,
                           uint8_t blue)
{
    uintptr_t critical_state;

    if (!ws2812b_is_initialized(led))
    {
        ws2812b_set_status(led, WS2812B_STATUS_INVALID_ARGUMENT);
        return false;
    }

    if (pixel >= led->MaxPixel)
    {
        ws2812b_set_status(led, WS2812B_STATUS_OUT_OF_RANGE);
        return false;
    }

    critical_state = ws2812b_enter_critical(led);
    if ((led->State == WS2812B_STATE_ENCODING) ||
        (led->State == WS2812B_STATE_STOPPING))
    {
        led->LastStatus = WS2812B_STATUS_BUSY;
        ws2812b_exit_critical(led, critical_state);
        return false;
    }

    led->Pixel[pixel][0] = red;
    led->Pixel[pixel][1] = green;
    led->Pixel[pixel][2] = blue;
    led->LastStatus = WS2812B_STATUS_OK;
    ws2812b_exit_critical(led, critical_state);
    return true;
}

static void ws2812b_encode_byte(WS2812B_HandleTypeDef const *led,
                               uint8_t value,
                               WS2812B_DmaSample *buffer,
                               size_t *index)
{
    uint8_t mask = 0x80U;

    while (mask != 0U)
    {
        buffer[*index] = ((value & mask) != 0U) ? led->Pulse1 : led->Pulse0;
        (*index)++;
        mask >>= 1U;
    }
}

static void ws2812b_encode_pixel(WS2812B_HandleTypeDef const *led,
                                uint16_t pixel,
                                WS2812B_DmaSample *buffer,
                                size_t *index)
{
    uint8_t red = ws2812b_apply_gamma(led, led->Pixel[pixel][0]);
    uint8_t green = ws2812b_apply_gamma(led, led->Pixel[pixel][1]);
    uint8_t blue = ws2812b_apply_gamma(led, led->Pixel[pixel][2]);

    switch (led->ColorOrder)
    {
        case WS2812B_COLOR_ORDER_RGB:
            ws2812b_encode_byte(led, red, buffer, index);
            ws2812b_encode_byte(led, green, buffer, index);
            ws2812b_encode_byte(led, blue, buffer, index);
            break;

        case WS2812B_COLOR_ORDER_RBG:
            ws2812b_encode_byte(led, red, buffer, index);
            ws2812b_encode_byte(led, blue, buffer, index);
            ws2812b_encode_byte(led, green, buffer, index);
            break;

        case WS2812B_COLOR_ORDER_GRB:
            ws2812b_encode_byte(led, green, buffer, index);
            ws2812b_encode_byte(led, red, buffer, index);
            ws2812b_encode_byte(led, blue, buffer, index);
            break;

        case WS2812B_COLOR_ORDER_GBR:
            ws2812b_encode_byte(led, green, buffer, index);
            ws2812b_encode_byte(led, blue, buffer, index);
            ws2812b_encode_byte(led, red, buffer, index);
            break;

        case WS2812B_COLOR_ORDER_BRG:
            ws2812b_encode_byte(led, blue, buffer, index);
            ws2812b_encode_byte(led, red, buffer, index);
            ws2812b_encode_byte(led, green, buffer, index);
            break;

        case WS2812B_COLOR_ORDER_BGR:
        default:
            ws2812b_encode_byte(led, blue, buffer, index);
            ws2812b_encode_byte(led, green, buffer, index);
            ws2812b_encode_byte(led, red, buffer, index);
            break;
    }
}

static bool ws2812b_begin_encoding(WS2812B_HandleTypeDef *led)
{
    uintptr_t critical_state = ws2812b_enter_critical(led);

    if (led->State != WS2812B_STATE_IDLE)
    {
        led->LastStatus = WS2812B_STATUS_BUSY;
        ws2812b_exit_critical(led, critical_state);
        return false;
    }

    led->State = WS2812B_STATE_ENCODING;
    ws2812b_exit_critical(led, critical_state);
    return true;
}

static bool ws2812b_finish_transfer(WS2812B_HandleTypeDef *led,
                                   WS2812B_Status status)
{
    uintptr_t critical_state;
    bool stopped;

    if (!ws2812b_is_initialized(led))
    {
        ws2812b_set_status(led, WS2812B_STATUS_INVALID_ARGUMENT);
        return false;
    }

    critical_state = ws2812b_enter_critical(led);
    if (led->State != WS2812B_STATE_TX_BUSY)
    {
        ws2812b_exit_critical(led, critical_state);
        return false;
    }
    led->State = WS2812B_STATE_STOPPING;
    ws2812b_exit_critical(led, critical_state);

    stopped = ws2812b_stop_dma(led);

    critical_state = ws2812b_enter_critical(led);
    if (stopped)
    {
        led->LastStatus = status;
        led->State = WS2812B_STATE_IDLE;
    }
    else
    {
        /* Keep ownership of Buffer when the transport cannot prove that DMA
           has stopped. A later Abort may retry the synchronous stop. */
        led->LastStatus = WS2812B_STATUS_TRANSPORT_ERROR;
        led->State = WS2812B_STATE_TX_BUSY;
    }
    ws2812b_exit_critical(led, critical_state);
    return stopped;
}

bool WS2812B_Init(WS2812B_HandleTypeDef *led, WS2812B_Config const *config)
{
    uint32_t bit_period_ns;
    uint32_t t0h_ns;
    uint32_t t1h_ns;
    uint32_t period_ticks;
    uint32_t pulse0_ticks;
    uint32_t pulse1_ticks;
    uint32_t timer_dma_id;
    uint32_t timer_dma_request;
    HAL_TIM_ActiveChannel timer_active_channel;
    WS2812B_DmaSample sample_max = (WS2812B_DmaSample)~(WS2812B_DmaSample)0;

    if (led == NULL)
    {
        return false;
    }

    /* Handles must initially be zeroed (or use WS2812B_HANDLE_INITIALIZER).
       Once initialized, reconfiguration is rejected while DMA owns Buffer. */
    if ((led->Magic == WS2812B_HANDLE_MAGIC) && WS2812B_IsBusy(led))
    {
        led->LastStatus = WS2812B_STATUS_BUSY;
        return false;
    }

    memset(led, 0, sizeof(*led));
    led->Magic = WS2812B_HANDLE_MAGIC;
    led->State = WS2812B_STATE_UNINITIALIZED;
    led->LastStatus = WS2812B_STATUS_INVALID_ARGUMENT;

    if ((config == NULL) ||
        (config->Timer == NULL) ||
        (config->Timer->Instance == NULL) ||
        (config->Dma == NULL) ||
        (config->Dma->Instance == NULL) ||
        (HAL_DMA_GetState(config->Dma) != HAL_DMA_STATE_READY) ||
        (config->TimerFrequencyHz == 0U) ||
        (config->PixelCount == 0U) ||
        (config->PixelCount > WS2812B_PIXEL_MAX) ||
        (config->ColorOrder > WS2812B_COLOR_ORDER_BGR) ||
        !ws2812b_get_channel_parameters(config->TimerChannel,
                                         &timer_dma_id,
                                         &timer_dma_request,
                                         &timer_active_channel))
    {
        return false;
    }

    bit_period_ns = (config->BitPeriodNs == 0U) ?
                    WS2812B_DEFAULT_BIT_PERIOD_NS : config->BitPeriodNs;
    t0h_ns = (config->T0HNs == 0U) ? WS2812B_DEFAULT_T0H_NS : config->T0HNs;
    t1h_ns = (config->T1HNs == 0U) ? WS2812B_DEFAULT_T1H_NS : config->T1HNs;

    if ((t0h_ns >= bit_period_ns) ||
        (t1h_ns >= bit_period_ns) ||
        (t0h_ns >= t1h_ns))
    {
        return false;
    }

    period_ticks = ws2812b_ns_to_ticks(config->TimerFrequencyHz,
                                       bit_period_ns);
    pulse0_ticks = ws2812b_ns_to_ticks(config->TimerFrequencyHz, t0h_ns);
    pulse1_ticks = ws2812b_ns_to_ticks(config->TimerFrequencyHz, t1h_ns);

    if ((period_ticks < 2U) ||
        (pulse0_ticks == 0U) ||
        (pulse0_ticks >= pulse1_ticks) ||
        (pulse1_ticks >= period_ticks) ||
        (pulse1_ticks > (uint32_t)sample_max))
    {
        return false;
    }

    if (((config->Timer->hdma[timer_dma_id] != NULL) &&
         (config->Timer->hdma[timer_dma_id] != config->Dma)) ||
        ((config->Dma->Parent != NULL) &&
         (config->Dma->Parent != config->Timer)))
    {
        return false;
    }

    /* Establish the same linkage as __HAL_LINKDMA when board code leaves the
       timer slot empty; an existing, conflicting linkage is never replaced. */
    config->Timer->hdma[timer_dma_id] = config->Dma;
    config->Dma->Parent = config->Timer;

    led->Timer = config->Timer;
    led->Dma = config->Dma;
    led->TimerChannel = config->TimerChannel;
    led->TimerDmaId = timer_dma_id;
    led->TimerDmaRequest = timer_dma_request;
    led->TimerActiveChannel = timer_active_channel;
    led->PeriodTicks = period_ticks;
    led->Pulse0 = (WS2812B_DmaSample)pulse0_ticks;
    led->Pulse1 = (WS2812B_DmaSample)pulse1_ticks;
    led->MaxPixel = config->PixelCount;
    led->ColorOrder = config->ColorOrder;
    led->GammaEnabled = config->GammaEnabled;
    led->GammaTable = config->GammaTable;

    if (!ws2812b_configure_pwm(led))
    {
        led->LastStatus = WS2812B_STATUS_TRANSPORT_ERROR;
        return false;
    }

    led->State = WS2812B_STATE_IDLE;
    led->LastStatus = WS2812B_STATUS_OK;
    return true;
}

bool WS2812B_SetPixel_RGB(WS2812B_HandleTypeDef *led,
                         uint16_t pixel,
                         uint8_t red,
                         uint8_t green,
                         uint8_t blue)
{
    return ws2812b_set_rgb(led, pixel, red, green, blue);
}

bool WS2812B_SetPixel_RGB_565(WS2812B_HandleTypeDef *led,
                             uint16_t pixel,
                             uint16_t color)
{
    uint8_t red5 = (uint8_t)((color >> 11U) & 0x1FU);
    uint8_t green6 = (uint8_t)((color >> 5U) & 0x3FU);
    uint8_t blue5 = (uint8_t)(color & 0x1FU);
    uint8_t red = (uint8_t)((red5 << 3U) | (red5 >> 2U));
    uint8_t green = (uint8_t)((green6 << 2U) | (green6 >> 4U));
    uint8_t blue = (uint8_t)((blue5 << 3U) | (blue5 >> 2U));

    return ws2812b_set_rgb(led, pixel, red, green, blue);
}

bool WS2812B_SetPixel_RGB_888(WS2812B_HandleTypeDef *led,
                             uint16_t pixel,
                             uint32_t color)
{
    return ws2812b_set_rgb(led,
                          pixel,
                          (uint8_t)(color >> 16U),
                          (uint8_t)(color >> 8U),
                          (uint8_t)color);
}

bool WS2812B_SetPixel_RGBW_565(WS2812B_HandleTypeDef *led,
                              uint16_t pixel,
                              uint16_t color,
                              uint8_t brightness)
{
    uint8_t red5 = (uint8_t)((color >> 11U) & 0x1FU);
    uint8_t green6 = (uint8_t)((color >> 5U) & 0x3FU);
    uint8_t blue5 = (uint8_t)(color & 0x1FU);
    uint8_t red = (uint8_t)((red5 << 3U) | (red5 >> 2U));
    uint8_t green = (uint8_t)((green6 << 2U) | (green6 >> 4U));
    uint8_t blue = (uint8_t)((blue5 << 3U) | (blue5 >> 2U));

    return ws2812b_set_rgb(led,
                          pixel,
                          ws2812b_scale(red, brightness),
                          ws2812b_scale(green, brightness),
                          ws2812b_scale(blue, brightness));
}

bool WS2812B_SetPixel_RGBW_888(WS2812B_HandleTypeDef *led,
                              uint16_t pixel,
                              uint32_t color,
                              uint8_t brightness)
{
    uint8_t red = (uint8_t)(color >> 16U);
    uint8_t green = (uint8_t)(color >> 8U);
    uint8_t blue = (uint8_t)color;

    return ws2812b_set_rgb(led,
                          pixel,
                          ws2812b_scale(red, brightness),
                          ws2812b_scale(green, brightness),
                          ws2812b_scale(blue, brightness));
}

bool WS2812B_Update(WS2812B_HandleTypeDef *led)
{
    uintptr_t critical_state;
    size_t transfer_length;
    size_t index = 0U;
    bool started;

    if (!ws2812b_is_initialized(led))
    {
        ws2812b_set_status(led, WS2812B_STATUS_INVALID_ARGUMENT);
        return false;
    }

    if (!ws2812b_begin_encoding(led))
    {
        return false;
    }

    transfer_length = WS2812B_GetTransferLength(led);
    memset(led->Buffer, 0, transfer_length * sizeof(led->Buffer[0]));
    index = WS2812B_LEADING_LOW_SLOTS;

    for (uint16_t pixel = 0U; pixel < led->MaxPixel; pixel++)
    {
        ws2812b_encode_pixel(led, pixel, led->Buffer, &index);
    }

    /* Keep the transition and HAL start atomic with respect to Abort and the
       DMA callbacks. Once TX_BUSY is externally visible, DMA owns Buffer. */
    critical_state = ws2812b_enter_critical(led);
    led->State = WS2812B_STATE_TX_BUSY;
    started = ws2812b_start_dma(led, led->Buffer, transfer_length);
    if (started)
    {
        led->LastStatus = WS2812B_STATUS_OK;
    }
    else
    {
        led->State = WS2812B_STATE_STOPPING;
    }
    ws2812b_exit_critical(led, critical_state);

    if (!started)
    {
        bool stopped = ws2812b_stop_dma(led);

        critical_state = ws2812b_enter_critical(led);
        led->State = stopped ? WS2812B_STATE_IDLE : WS2812B_STATE_TX_BUSY;
        led->LastStatus = WS2812B_STATUS_TRANSPORT_ERROR;
        ws2812b_exit_critical(led, critical_state);
        return false;
    }

    return true;
}

bool WS2812B_IsBusy(WS2812B_HandleTypeDef const *led)
{
    return (led != NULL) &&
           ((led->State == WS2812B_STATE_ENCODING) ||
            (led->State == WS2812B_STATE_TX_BUSY) ||
            (led->State == WS2812B_STATE_STOPPING));
}

WS2812B_State WS2812B_GetState(WS2812B_HandleTypeDef const *led)
{
    return (led == NULL) ? WS2812B_STATE_UNINITIALIZED : led->State;
}

WS2812B_Status WS2812B_GetLastStatus(WS2812B_HandleTypeDef const *led)
{
    return (led == NULL) ? WS2812B_STATUS_INVALID_ARGUMENT : led->LastStatus;
}

uint16_t WS2812B_GetPixelCount(WS2812B_HandleTypeDef const *led)
{
    return ws2812b_is_initialized(led) ? led->MaxPixel : 0U;
}

size_t WS2812B_GetTransferLength(WS2812B_HandleTypeDef const *led)
{
    if (!ws2812b_is_initialized(led))
    {
        return 0U;
    }

    return WS2812B_LEADING_LOW_SLOTS +
           ((size_t)led->MaxPixel * WS2812B_BITS_PER_PIXEL) +
           WS2812B_RESET_LOW_SLOTS;
}

bool WS2812B_IsTimerCallback(WS2812B_HandleTypeDef const *led,
                             TIM_HandleTypeDef const *timer)
{
    return ws2812b_is_initialized(led) &&
           (timer == led->Timer) &&
           (timer->Channel == led->TimerActiveChannel);
}

bool WS2812B_HasDmaError(WS2812B_HandleTypeDef const *led)
{
    return !ws2812b_is_initialized(led) ||
           (HAL_DMA_GetError(led->Dma) != HAL_DMA_ERROR_NONE);
}

bool WS2812B_NotifyTransferComplete(WS2812B_HandleTypeDef *led)
{
    return ws2812b_finish_transfer(led, WS2812B_STATUS_OK);
}

bool WS2812B_NotifyTransferError(WS2812B_HandleTypeDef *led)
{
    return ws2812b_finish_transfer(led, WS2812B_STATUS_TRANSPORT_ERROR);
}

bool WS2812B_Abort(WS2812B_HandleTypeDef *led)
{
    uintptr_t critical_state;
    WS2812B_State previous_state;
    bool stopped;

    if (!ws2812b_is_initialized(led))
    {
        ws2812b_set_status(led, WS2812B_STATUS_INVALID_ARGUMENT);
        return false;
    }

    critical_state = ws2812b_enter_critical(led);
    previous_state = led->State;
    if (previous_state != WS2812B_STATE_TX_BUSY)
    {
        if (previous_state == WS2812B_STATE_ENCODING)
        {
            led->LastStatus = WS2812B_STATUS_BUSY;
        }
        ws2812b_exit_critical(led, critical_state);
        return previous_state == WS2812B_STATE_IDLE;
    }
    led->State = WS2812B_STATE_STOPPING;
    ws2812b_exit_critical(led, critical_state);

    stopped = ws2812b_stop_dma(led);

    critical_state = ws2812b_enter_critical(led);
    if (stopped)
    {
        led->State = WS2812B_STATE_IDLE;
        led->LastStatus = WS2812B_STATUS_ABORTED;
    }
    else
    {
        led->State = WS2812B_STATE_TX_BUSY;
        led->LastStatus = WS2812B_STATUS_TRANSPORT_ERROR;
    }
    ws2812b_exit_critical(led, critical_state);

    return stopped;
}
