#ifndef WS2812B_H
#define WS2812B_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tim.h"

/*
 * STM32 HAL WS2812B PWM/DMA driver.
 *
 * The application initializes a timer and a DMA channel, then passes both
 * handles and the selected PWM channel to WS2812B_Init. No middleware pack or
 * CubeMX component-specific configuration is required.
 */

#ifndef WS2812B_PIXEL_MAX
#define WS2812B_PIXEL_MAX              32U
#endif

#ifndef WS2812B_LEADING_LOW_SLOTS
#define WS2812B_LEADING_LOW_SLOTS      2U
#endif

#ifndef WS2812B_RESET_LOW_SLOTS
#define WS2812B_RESET_LOW_SLOTS        64U
#endif

#define WS2812B_BITS_PER_PIXEL         24U

#ifndef WS2812B_DEFAULT_BIT_PERIOD_NS
#define WS2812B_DEFAULT_BIT_PERIOD_NS  1250U
#endif

#ifndef WS2812B_DEFAULT_T0H_NS
#define WS2812B_DEFAULT_T0H_NS         400U
#endif

#ifndef WS2812B_DEFAULT_T1H_NS
#define WS2812B_DEFAULT_T1H_NS         800U
#endif

#define WS2812B_HANDLE_INITIALIZER     {0}

#define WS2812B_DMA_BUFFER_CAPACITY \
    (WS2812B_LEADING_LOW_SLOTS + \
     (WS2812B_PIXEL_MAX * WS2812B_BITS_PER_PIXEL) + \
     WS2812B_RESET_LOW_SLOTS)

#if (WS2812B_PIXEL_MAX == 0U)
#error "WS2812B_PIXEL_MAX must be greater than zero"
#endif

#if (WS2812B_PIXEL_MAX > UINT16_MAX)
#error "WS2812B_PIXEL_MAX must fit in uint16_t"
#endif

#if defined(__cplusplus)
#define WS2812B_ALIGNAS_4 alignas(4)
#elif defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
#define WS2812B_ALIGNAS_4 _Alignas(4)
#elif defined(__GNUC__) || defined(__clang__)
#define WS2812B_ALIGNAS_4 __attribute__((aligned(4)))
#else
#define WS2812B_ALIGNAS_4
#endif

/* STM32H5 HAL TIM DMA length is expressed in source bytes. */
typedef uint8_t WS2812B_DmaSample;

typedef enum
{
    WS2812B_COLOR_ORDER_RGB = 0,
    WS2812B_COLOR_ORDER_RBG,
    WS2812B_COLOR_ORDER_GRB,
    WS2812B_COLOR_ORDER_GBR,
    WS2812B_COLOR_ORDER_BRG,
    WS2812B_COLOR_ORDER_BGR
} WS2812B_ColorOrder;

typedef enum
{
    WS2812B_STATE_UNINITIALIZED = 0,
    WS2812B_STATE_IDLE,
    WS2812B_STATE_ENCODING,
    WS2812B_STATE_TX_BUSY,
    WS2812B_STATE_STOPPING
} WS2812B_State;

typedef enum
{
    WS2812B_STATUS_OK = 0,
    WS2812B_STATUS_BUSY,
    WS2812B_STATUS_INVALID_ARGUMENT,
    WS2812B_STATUS_OUT_OF_RANGE,
    WS2812B_STATUS_TRANSPORT_ERROR,
    WS2812B_STATUS_ABORTED
} WS2812B_Status;

typedef struct
{
    /* Dma->Instance identifies the physical DMA channel. TimerChannel selects
       the PWM channel; the driver derives and validates its TIM DMA slot. */
    TIM_HandleTypeDef *Timer;
    DMA_HandleTypeDef *Dma;
    uint32_t TimerChannel;
    uint32_t TimerFrequencyHz;
    uint32_t BitPeriodNs;
    uint32_t T0HNs;
    uint32_t T1HNs;
    uint16_t PixelCount;
    WS2812B_ColorOrder ColorOrder;
    bool GammaEnabled;
    uint8_t const *GammaTable;
} WS2812B_Config;

typedef struct
{
    uint32_t Magic;
    TIM_HandleTypeDef *Timer;
    DMA_HandleTypeDef *Dma;
    uint32_t TimerChannel;
    uint32_t TimerDmaId;
    uint32_t TimerDmaRequest;
    HAL_TIM_ActiveChannel TimerActiveChannel;
    uint32_t PeriodTicks;
    WS2812B_DmaSample Pulse0;
    WS2812B_DmaSample Pulse1;
    uint16_t MaxPixel;
    WS2812B_ColorOrder ColorOrder;
    bool GammaEnabled;
    uint8_t const *GammaTable;
    volatile WS2812B_State State;
    volatile WS2812B_Status LastStatus;
    uint8_t Pixel[WS2812B_PIXEL_MAX][3];
    WS2812B_ALIGNAS_4 WS2812B_DmaSample Buffer[WS2812B_DMA_BUFFER_CAPACITY];
} WS2812B_HandleTypeDef;

bool WS2812B_Init(WS2812B_HandleTypeDef *led, WS2812B_Config const *config);

bool WS2812B_SetPixel_RGB(WS2812B_HandleTypeDef *led,
                         uint16_t pixel,
                         uint8_t red,
                         uint8_t green,
                         uint8_t blue);
bool WS2812B_SetPixel_RGB_565(WS2812B_HandleTypeDef *led,
                             uint16_t pixel,
                             uint16_t color);
bool WS2812B_SetPixel_RGB_888(WS2812B_HandleTypeDef *led,
                             uint16_t pixel,
                             uint32_t color);
bool WS2812B_SetPixel_RGBW_565(WS2812B_HandleTypeDef *led,
                              uint16_t pixel,
                              uint16_t color,
                              uint8_t brightness);
bool WS2812B_SetPixel_RGBW_888(WS2812B_HandleTypeDef *led,
                              uint16_t pixel,
                              uint32_t color,
                              uint8_t brightness);

/* Starts a non-blocking transfer. Returns false while DMA owns Buffer. */
bool WS2812B_Update(WS2812B_HandleTypeDef *led);
bool WS2812B_IsBusy(WS2812B_HandleTypeDef const *led);
WS2812B_State WS2812B_GetState(WS2812B_HandleTypeDef const *led);
WS2812B_Status WS2812B_GetLastStatus(WS2812B_HandleTypeDef const *led);
uint16_t WS2812B_GetPixelCount(WS2812B_HandleTypeDef const *led);
size_t WS2812B_GetTransferLength(WS2812B_HandleTypeDef const *led);

/* Filters shared HAL timer callbacks using the Init timer/channel selection. */
bool WS2812B_IsTimerCallback(WS2812B_HandleTypeDef const *led,
                             TIM_HandleTypeDef const *timer);
bool WS2812B_HasDmaError(WS2812B_HandleTypeDef const *led);

/* Forward the active transport's completion/error callbacks to these APIs. */
bool WS2812B_NotifyTransferComplete(WS2812B_HandleTypeDef *led);
bool WS2812B_NotifyTransferError(WS2812B_HandleTypeDef *led);

/* Synchronously stops an active transfer before releasing the DMA lock. */
bool WS2812B_Abort(WS2812B_HandleTypeDef *led);

#ifdef __cplusplus
}
#endif

#endif /* WS2812B_H */
