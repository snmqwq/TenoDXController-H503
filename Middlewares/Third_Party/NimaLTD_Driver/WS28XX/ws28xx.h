#ifndef WS28XX_H
#define WS28XX_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Portable WS2812B/WS28XX driver core.
 *
 * The driver does not include a vendor HAL, generated timer header, RTOS, or
 * CubeMX configuration. The application supplies three transport callbacks:
 * configure PWM timing, start DMA, and synchronously stop DMA.
 */

#ifndef WS28XX_PIXEL_MAX
#define WS28XX_PIXEL_MAX              32U
#endif

#ifndef WS28XX_DMA_SAMPLE_TYPE
#define WS28XX_DMA_SAMPLE_TYPE        uint8_t
#endif

#ifndef WS28XX_LEADING_LOW_SLOTS
#define WS28XX_LEADING_LOW_SLOTS      2U
#endif

#ifndef WS28XX_RESET_LOW_SLOTS
#define WS28XX_RESET_LOW_SLOTS        64U
#endif

#define WS28XX_BITS_PER_PIXEL         24U

#ifndef WS28XX_DEFAULT_BIT_PERIOD_NS
#define WS28XX_DEFAULT_BIT_PERIOD_NS  1250U
#endif

#ifndef WS28XX_DEFAULT_T0H_NS
#define WS28XX_DEFAULT_T0H_NS         400U
#endif

#ifndef WS28XX_DEFAULT_T1H_NS
#define WS28XX_DEFAULT_T1H_NS         800U
#endif

#define WS28XX_HANDLE_INITIALIZER     {0}

#define WS28XX_DMA_BUFFER_CAPACITY \
    (WS28XX_LEADING_LOW_SLOTS + \
     (WS28XX_PIXEL_MAX * WS28XX_BITS_PER_PIXEL) + \
     WS28XX_RESET_LOW_SLOTS)

#if (WS28XX_PIXEL_MAX == 0U)
#error "WS28XX_PIXEL_MAX must be greater than zero"
#endif

#if (WS28XX_PIXEL_MAX > UINT16_MAX)
#error "WS28XX_PIXEL_MAX must fit in uint16_t"
#endif

#if defined(__cplusplus)
#define WS28XX_ALIGNAS_4 alignas(4)
#elif defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
#define WS28XX_ALIGNAS_4 _Alignas(4)
#elif defined(__GNUC__) || defined(__clang__)
#define WS28XX_ALIGNAS_4 __attribute__((aligned(4)))
#else
#define WS28XX_ALIGNAS_4
#endif

typedef WS28XX_DMA_SAMPLE_TYPE WS28XX_DmaSample;

typedef enum
{
    WS28XX_COLOR_ORDER_RGB = 0,
    WS28XX_COLOR_ORDER_RBG,
    WS28XX_COLOR_ORDER_GRB,
    WS28XX_COLOR_ORDER_GBR,
    WS28XX_COLOR_ORDER_BRG,
    WS28XX_COLOR_ORDER_BGR
} WS28XX_ColorOrder;

typedef enum
{
    WS28XX_STATE_UNINITIALIZED = 0,
    WS28XX_STATE_IDLE,
    WS28XX_STATE_ENCODING,
    WS28XX_STATE_TX_BUSY,
    WS28XX_STATE_STOPPING
} WS28XX_State;

typedef enum
{
    WS28XX_STATUS_OK = 0,
    WS28XX_STATUS_BUSY,
    WS28XX_STATUS_INVALID_ARGUMENT,
    WS28XX_STATUS_OUT_OF_RANGE,
    WS28XX_STATUS_TRANSPORT_ERROR,
    WS28XX_STATUS_ABORTED
} WS28XX_Status;

typedef bool (*WS28XX_ConfigurePwmFn)(void *context,
                                     uint32_t period_ticks,
                                     WS28XX_DmaSample pulse0_ticks,
                                     WS28XX_DmaSample pulse1_ticks);

typedef bool (*WS28XX_StartDmaFn)(void *context,
                                  WS28XX_DmaSample const *samples,
                                  size_t sample_count);

/*
 * Stop must be idempotent. A successful call must guarantee that DMA no longer
 * reads samples before it returns. This contract also covers cleanup after a
 * failed StartDma call and is what makes Abort safe for buffer reuse.
 */
typedef bool (*WS28XX_StopDmaFn)(void *context);

typedef uintptr_t (*WS28XX_EnterCriticalFn)(void *context);
typedef void (*WS28XX_ExitCriticalFn)(void *context, uintptr_t state);

typedef struct
{
    void *TransportContext;
    WS28XX_ConfigurePwmFn ConfigurePwm;
    WS28XX_StartDmaFn StartDma;
    WS28XX_StopDmaFn StopDma;
    WS28XX_EnterCriticalFn EnterCritical;
    WS28XX_ExitCriticalFn ExitCritical;
    uint32_t TimerFrequencyHz;
    uint32_t BitPeriodNs;
    uint32_t T0HNs;
    uint32_t T1HNs;
    uint16_t PixelCount;
    WS28XX_ColorOrder ColorOrder;
    bool GammaEnabled;
    uint8_t const *GammaTable;
} WS28XX_Config;

typedef struct
{
    uint32_t Magic;
    void *TransportContext;
    WS28XX_ConfigurePwmFn ConfigurePwm;
    WS28XX_StartDmaFn StartDma;
    WS28XX_StopDmaFn StopDma;
    WS28XX_EnterCriticalFn EnterCritical;
    WS28XX_ExitCriticalFn ExitCritical;
    uint32_t PeriodTicks;
    WS28XX_DmaSample Pulse0;
    WS28XX_DmaSample Pulse1;
    uint16_t MaxPixel;
    WS28XX_ColorOrder ColorOrder;
    bool GammaEnabled;
    uint8_t const *GammaTable;
    volatile WS28XX_State State;
    volatile WS28XX_Status LastStatus;
    uint8_t Pixel[WS28XX_PIXEL_MAX][3];
    WS28XX_ALIGNAS_4 WS28XX_DmaSample Buffer[WS28XX_DMA_BUFFER_CAPACITY];
} WS28XX_HandleTypeDef;

bool WS28XX_Init(WS28XX_HandleTypeDef *led, WS28XX_Config const *config);

bool WS28XX_SetPixel_RGB(WS28XX_HandleTypeDef *led,
                         uint16_t pixel,
                         uint8_t red,
                         uint8_t green,
                         uint8_t blue);
bool WS28XX_SetPixel_RGB_565(WS28XX_HandleTypeDef *led,
                             uint16_t pixel,
                             uint16_t color);
bool WS28XX_SetPixel_RGB_888(WS28XX_HandleTypeDef *led,
                             uint16_t pixel,
                             uint32_t color);
bool WS28XX_SetPixel_RGBW_565(WS28XX_HandleTypeDef *led,
                              uint16_t pixel,
                              uint16_t color,
                              uint8_t brightness);
bool WS28XX_SetPixel_RGBW_888(WS28XX_HandleTypeDef *led,
                              uint16_t pixel,
                              uint32_t color,
                              uint8_t brightness);

/* Starts a non-blocking transfer. Returns false while DMA owns Buffer. */
bool WS28XX_Update(WS28XX_HandleTypeDef *led);
bool WS28XX_IsBusy(WS28XX_HandleTypeDef const *led);
WS28XX_State WS28XX_GetState(WS28XX_HandleTypeDef const *led);
WS28XX_Status WS28XX_GetLastStatus(WS28XX_HandleTypeDef const *led);
size_t WS28XX_GetTransferLength(WS28XX_HandleTypeDef const *led);

/* Forward the active transport's completion/error callbacks to these APIs. */
bool WS28XX_NotifyTransferComplete(WS28XX_HandleTypeDef *led);
bool WS28XX_NotifyTransferError(WS28XX_HandleTypeDef *led);

/* Synchronously stops an active transfer before releasing the DMA lock. */
bool WS28XX_Abort(WS28XX_HandleTypeDef *led);

#ifdef __cplusplus
}
#endif

#endif /* WS28XX_H */
