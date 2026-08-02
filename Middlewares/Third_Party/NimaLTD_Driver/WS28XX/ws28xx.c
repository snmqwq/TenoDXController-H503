#include "ws28xx.h"

#include <string.h>

#define WS28XX_HANDLE_MAGIC  0x57533238UL

/* Generated from round(255 * pow(input / 255, 2.24)). */
static uint8_t const ws28xx_default_gamma_table[256] =
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

static uintptr_t ws28xx_enter_critical(WS28XX_HandleTypeDef *led)
{
    if ((led != NULL) && (led->EnterCritical != NULL))
    {
        return led->EnterCritical(led->TransportContext);
    }

    return 0U;
}

static void ws28xx_exit_critical(WS28XX_HandleTypeDef *led, uintptr_t state)
{
    if ((led != NULL) && (led->ExitCritical != NULL))
    {
        led->ExitCritical(led->TransportContext, state);
    }
}

static void ws28xx_set_status(WS28XX_HandleTypeDef *led, WS28XX_Status status)
{
    if (led != NULL)
    {
        led->LastStatus = status;
    }
}

static bool ws28xx_is_initialized(WS28XX_HandleTypeDef const *led)
{
    return (led != NULL) &&
           (led->Magic == WS28XX_HANDLE_MAGIC) &&
           (led->State != WS28XX_STATE_UNINITIALIZED) &&
           (led->ConfigurePwm != NULL) &&
           (led->StartDma != NULL) &&
           (led->StopDma != NULL) &&
           (led->MaxPixel > 0U) &&
           (led->MaxPixel <= WS28XX_PIXEL_MAX);
}

static uint32_t ws28xx_ns_to_ticks(uint32_t frequency_hz, uint32_t duration_ns)
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

static uint8_t ws28xx_default_gamma(uint8_t value)
{
    return ws28xx_default_gamma_table[value];
}

static uint8_t ws28xx_apply_gamma(WS28XX_HandleTypeDef const *led,
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

    return ws28xx_default_gamma(value);
}

static uint8_t ws28xx_scale(uint8_t value, uint8_t brightness)
{
    return (uint8_t)(((uint16_t)value * brightness) / 255U);
}

static bool ws28xx_set_rgb(WS28XX_HandleTypeDef *led,
                           uint16_t pixel,
                           uint8_t red,
                           uint8_t green,
                           uint8_t blue)
{
    if (!ws28xx_is_initialized(led))
    {
        ws28xx_set_status(led, WS28XX_STATUS_INVALID_ARGUMENT);
        return false;
    }

    if (pixel >= led->MaxPixel)
    {
        ws28xx_set_status(led, WS28XX_STATUS_OUT_OF_RANGE);
        return false;
    }

    led->Pixel[pixel][0] = red;
    led->Pixel[pixel][1] = green;
    led->Pixel[pixel][2] = blue;
    ws28xx_set_status(led, WS28XX_STATUS_OK);
    return true;
}

static void ws28xx_encode_byte(WS28XX_HandleTypeDef const *led,
                               uint8_t value,
                               WS28XX_DmaSample *buffer,
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

static void ws28xx_encode_pixel(WS28XX_HandleTypeDef const *led,
                                uint16_t pixel,
                                WS28XX_DmaSample *buffer,
                                size_t *index)
{
    uint8_t red = ws28xx_apply_gamma(led, led->Pixel[pixel][0]);
    uint8_t green = ws28xx_apply_gamma(led, led->Pixel[pixel][1]);
    uint8_t blue = ws28xx_apply_gamma(led, led->Pixel[pixel][2]);

    switch (led->ColorOrder)
    {
        case WS28XX_COLOR_ORDER_RGB:
            ws28xx_encode_byte(led, red, buffer, index);
            ws28xx_encode_byte(led, green, buffer, index);
            ws28xx_encode_byte(led, blue, buffer, index);
            break;

        case WS28XX_COLOR_ORDER_RBG:
            ws28xx_encode_byte(led, red, buffer, index);
            ws28xx_encode_byte(led, blue, buffer, index);
            ws28xx_encode_byte(led, green, buffer, index);
            break;

        case WS28XX_COLOR_ORDER_GRB:
            ws28xx_encode_byte(led, green, buffer, index);
            ws28xx_encode_byte(led, red, buffer, index);
            ws28xx_encode_byte(led, blue, buffer, index);
            break;

        case WS28XX_COLOR_ORDER_GBR:
            ws28xx_encode_byte(led, green, buffer, index);
            ws28xx_encode_byte(led, blue, buffer, index);
            ws28xx_encode_byte(led, red, buffer, index);
            break;

        case WS28XX_COLOR_ORDER_BRG:
            ws28xx_encode_byte(led, blue, buffer, index);
            ws28xx_encode_byte(led, red, buffer, index);
            ws28xx_encode_byte(led, green, buffer, index);
            break;

        case WS28XX_COLOR_ORDER_BGR:
        default:
            ws28xx_encode_byte(led, blue, buffer, index);
            ws28xx_encode_byte(led, green, buffer, index);
            ws28xx_encode_byte(led, red, buffer, index);
            break;
    }
}

static bool ws28xx_begin_encoding(WS28XX_HandleTypeDef *led)
{
    uintptr_t critical_state = ws28xx_enter_critical(led);

    if (led->State != WS28XX_STATE_IDLE)
    {
        led->LastStatus = WS28XX_STATUS_BUSY;
        ws28xx_exit_critical(led, critical_state);
        return false;
    }

    led->State = WS28XX_STATE_ENCODING;
    ws28xx_exit_critical(led, critical_state);
    return true;
}

static bool ws28xx_finish_transfer(WS28XX_HandleTypeDef *led,
                                   WS28XX_Status status)
{
    uintptr_t critical_state;
    bool stopped;

    if (!ws28xx_is_initialized(led))
    {
        ws28xx_set_status(led, WS28XX_STATUS_INVALID_ARGUMENT);
        return false;
    }

    critical_state = ws28xx_enter_critical(led);
    if (led->State != WS28XX_STATE_TX_BUSY)
    {
        ws28xx_exit_critical(led, critical_state);
        return false;
    }
    led->State = WS28XX_STATE_STOPPING;
    ws28xx_exit_critical(led, critical_state);

    stopped = led->StopDma(led->TransportContext);

    critical_state = ws28xx_enter_critical(led);
    if (stopped)
    {
        led->LastStatus = status;
        led->State = WS28XX_STATE_IDLE;
    }
    else
    {
        /* Keep ownership of Buffer when the transport cannot prove that DMA
           has stopped. A later Abort may retry the synchronous stop. */
        led->LastStatus = WS28XX_STATUS_TRANSPORT_ERROR;
        led->State = WS28XX_STATE_TX_BUSY;
    }
    ws28xx_exit_critical(led, critical_state);
    return stopped;
}

bool WS28XX_Init(WS28XX_HandleTypeDef *led, WS28XX_Config const *config)
{
    uint32_t bit_period_ns;
    uint32_t t0h_ns;
    uint32_t t1h_ns;
    uint32_t period_ticks;
    uint32_t pulse0_ticks;
    uint32_t pulse1_ticks;
    WS28XX_DmaSample sample_max = (WS28XX_DmaSample)~(WS28XX_DmaSample)0;

    if (led == NULL)
    {
        return false;
    }

    /* Handles must initially be zeroed (or use WS28XX_HANDLE_INITIALIZER).
       Once initialized, reconfiguration is rejected while DMA owns Buffer. */
    if ((led->Magic == WS28XX_HANDLE_MAGIC) && WS28XX_IsBusy(led))
    {
        led->LastStatus = WS28XX_STATUS_BUSY;
        return false;
    }

    memset(led, 0, sizeof(*led));
    led->Magic = WS28XX_HANDLE_MAGIC;
    led->State = WS28XX_STATE_UNINITIALIZED;
    led->LastStatus = WS28XX_STATUS_INVALID_ARGUMENT;

    if ((config == NULL) ||
        (config->ConfigurePwm == NULL) ||
        (config->StartDma == NULL) ||
        (config->StopDma == NULL) ||
        ((config->EnterCritical == NULL) != (config->ExitCritical == NULL)) ||
        (config->TimerFrequencyHz == 0U) ||
        (config->PixelCount == 0U) ||
        (config->PixelCount > WS28XX_PIXEL_MAX) ||
        (config->ColorOrder > WS28XX_COLOR_ORDER_BGR))
    {
        return false;
    }

    bit_period_ns = (config->BitPeriodNs == 0U) ?
                    WS28XX_DEFAULT_BIT_PERIOD_NS : config->BitPeriodNs;
    t0h_ns = (config->T0HNs == 0U) ? WS28XX_DEFAULT_T0H_NS : config->T0HNs;
    t1h_ns = (config->T1HNs == 0U) ? WS28XX_DEFAULT_T1H_NS : config->T1HNs;

    if ((t0h_ns >= bit_period_ns) ||
        (t1h_ns >= bit_period_ns) ||
        (t0h_ns >= t1h_ns))
    {
        return false;
    }

    period_ticks = ws28xx_ns_to_ticks(config->TimerFrequencyHz,
                                       bit_period_ns);
    pulse0_ticks = ws28xx_ns_to_ticks(config->TimerFrequencyHz, t0h_ns);
    pulse1_ticks = ws28xx_ns_to_ticks(config->TimerFrequencyHz, t1h_ns);

    if ((period_ticks < 2U) ||
        (pulse0_ticks == 0U) ||
        (pulse0_ticks >= pulse1_ticks) ||
        (pulse1_ticks >= period_ticks) ||
        (pulse1_ticks > (uint32_t)sample_max))
    {
        return false;
    }

    led->TransportContext = config->TransportContext;
    led->ConfigurePwm = config->ConfigurePwm;
    led->StartDma = config->StartDma;
    led->StopDma = config->StopDma;
    led->EnterCritical = config->EnterCritical;
    led->ExitCritical = config->ExitCritical;
    led->PeriodTicks = period_ticks;
    led->Pulse0 = (WS28XX_DmaSample)pulse0_ticks;
    led->Pulse1 = (WS28XX_DmaSample)pulse1_ticks;
    led->MaxPixel = config->PixelCount;
    led->ColorOrder = config->ColorOrder;
    led->GammaEnabled = config->GammaEnabled;
    led->GammaTable = config->GammaTable;

    if (!led->ConfigurePwm(led->TransportContext,
                           led->PeriodTicks,
                           led->Pulse0,
                           led->Pulse1))
    {
        led->LastStatus = WS28XX_STATUS_TRANSPORT_ERROR;
        return false;
    }

    led->State = WS28XX_STATE_IDLE;
    led->LastStatus = WS28XX_STATUS_OK;
    return true;
}

bool WS28XX_SetPixel_RGB(WS28XX_HandleTypeDef *led,
                         uint16_t pixel,
                         uint8_t red,
                         uint8_t green,
                         uint8_t blue)
{
    return ws28xx_set_rgb(led, pixel, red, green, blue);
}

bool WS28XX_SetPixel_RGB_565(WS28XX_HandleTypeDef *led,
                             uint16_t pixel,
                             uint16_t color)
{
    uint8_t red5 = (uint8_t)((color >> 11U) & 0x1FU);
    uint8_t green6 = (uint8_t)((color >> 5U) & 0x3FU);
    uint8_t blue5 = (uint8_t)(color & 0x1FU);
    uint8_t red = (uint8_t)((red5 << 3U) | (red5 >> 2U));
    uint8_t green = (uint8_t)((green6 << 2U) | (green6 >> 4U));
    uint8_t blue = (uint8_t)((blue5 << 3U) | (blue5 >> 2U));

    return ws28xx_set_rgb(led, pixel, red, green, blue);
}

bool WS28XX_SetPixel_RGB_888(WS28XX_HandleTypeDef *led,
                             uint16_t pixel,
                             uint32_t color)
{
    return ws28xx_set_rgb(led,
                          pixel,
                          (uint8_t)(color >> 16U),
                          (uint8_t)(color >> 8U),
                          (uint8_t)color);
}

bool WS28XX_SetPixel_RGBW_565(WS28XX_HandleTypeDef *led,
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

    return ws28xx_set_rgb(led,
                          pixel,
                          ws28xx_scale(red, brightness),
                          ws28xx_scale(green, brightness),
                          ws28xx_scale(blue, brightness));
}

bool WS28XX_SetPixel_RGBW_888(WS28XX_HandleTypeDef *led,
                              uint16_t pixel,
                              uint32_t color,
                              uint8_t brightness)
{
    uint8_t red = (uint8_t)(color >> 16U);
    uint8_t green = (uint8_t)(color >> 8U);
    uint8_t blue = (uint8_t)color;

    return ws28xx_set_rgb(led,
                          pixel,
                          ws28xx_scale(red, brightness),
                          ws28xx_scale(green, brightness),
                          ws28xx_scale(blue, brightness));
}

bool WS28XX_Update(WS28XX_HandleTypeDef *led)
{
    uintptr_t critical_state;
    size_t transfer_length;
    size_t index = 0U;

    if (!ws28xx_is_initialized(led))
    {
        ws28xx_set_status(led, WS28XX_STATUS_INVALID_ARGUMENT);
        return false;
    }

    if (!ws28xx_begin_encoding(led))
    {
        return false;
    }

    transfer_length = WS28XX_GetTransferLength(led);
    memset(led->Buffer, 0, transfer_length * sizeof(led->Buffer[0]));
    index = WS28XX_LEADING_LOW_SLOTS;

    for (uint16_t pixel = 0U; pixel < led->MaxPixel; pixel++)
    {
        ws28xx_encode_pixel(led, pixel, led->Buffer, &index);
    }

    critical_state = ws28xx_enter_critical(led);
    led->State = WS28XX_STATE_TX_BUSY;
    ws28xx_exit_critical(led, critical_state);

    if (!led->StartDma(led->TransportContext, led->Buffer, transfer_length))
    {
        bool stopped = led->StopDma(led->TransportContext);

        critical_state = ws28xx_enter_critical(led);
        led->State = stopped ? WS28XX_STATE_IDLE : WS28XX_STATE_TX_BUSY;
        led->LastStatus = WS28XX_STATUS_TRANSPORT_ERROR;
        ws28xx_exit_critical(led, critical_state);
        return false;
    }

    ws28xx_set_status(led, WS28XX_STATUS_OK);
    return true;
}

bool WS28XX_IsBusy(WS28XX_HandleTypeDef const *led)
{
    return (led != NULL) &&
           ((led->State == WS28XX_STATE_ENCODING) ||
            (led->State == WS28XX_STATE_TX_BUSY) ||
            (led->State == WS28XX_STATE_STOPPING));
}

WS28XX_State WS28XX_GetState(WS28XX_HandleTypeDef const *led)
{
    return (led == NULL) ? WS28XX_STATE_UNINITIALIZED : led->State;
}

WS28XX_Status WS28XX_GetLastStatus(WS28XX_HandleTypeDef const *led)
{
    return (led == NULL) ? WS28XX_STATUS_INVALID_ARGUMENT : led->LastStatus;
}

size_t WS28XX_GetTransferLength(WS28XX_HandleTypeDef const *led)
{
    if (!ws28xx_is_initialized(led))
    {
        return 0U;
    }

    return WS28XX_LEADING_LOW_SLOTS +
           ((size_t)led->MaxPixel * WS28XX_BITS_PER_PIXEL) +
           WS28XX_RESET_LOW_SLOTS;
}

bool WS28XX_NotifyTransferComplete(WS28XX_HandleTypeDef *led)
{
    return ws28xx_finish_transfer(led, WS28XX_STATUS_OK);
}

bool WS28XX_NotifyTransferError(WS28XX_HandleTypeDef *led)
{
    return ws28xx_finish_transfer(led, WS28XX_STATUS_TRANSPORT_ERROR);
}

bool WS28XX_Abort(WS28XX_HandleTypeDef *led)
{
    uintptr_t critical_state;
    WS28XX_State previous_state;
    bool stopped;

    if (!ws28xx_is_initialized(led))
    {
        ws28xx_set_status(led, WS28XX_STATUS_INVALID_ARGUMENT);
        return false;
    }

    critical_state = ws28xx_enter_critical(led);
    previous_state = led->State;
    if (previous_state != WS28XX_STATE_TX_BUSY)
    {
        if (previous_state == WS28XX_STATE_ENCODING)
        {
            led->LastStatus = WS28XX_STATUS_BUSY;
        }
        ws28xx_exit_critical(led, critical_state);
        return previous_state == WS28XX_STATE_IDLE;
    }
    led->State = WS28XX_STATE_STOPPING;
    ws28xx_exit_critical(led, critical_state);

    stopped = led->StopDma(led->TransportContext);

    critical_state = ws28xx_enter_critical(led);
    if (stopped)
    {
        led->State = WS28XX_STATE_IDLE;
        led->LastStatus = WS28XX_STATUS_ABORTED;
    }
    else
    {
        led->State = WS28XX_STATE_TX_BUSY;
        led->LastStatus = WS28XX_STATUS_TRANSPORT_ERROR;
    }
    ws28xx_exit_critical(led, critical_state);

    return stopped;
}
