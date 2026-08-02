#include "mai2led_app.h"

#include "main.h"
#include "mai2led.h"
#include "tusb.h"
#include <string.h>

#define IDLE_RAINBOW_UPDATE_MS         20U
#define IDLE_RAINBOW_STEP              10U
#define IDLE_RAINBOW_BUTTON_COUNT      MAI2LED_APP_DATA_BITS
#define IDLE_RAINBOW_DIM_SATURATION    240U
#define IDLE_RAINBOW_DIM_VALUE         128U
#define IDLE_RAINBOW_PRESS_SATURATION  64U
#define IDLE_RAINBOW_PRESS_VALUE       255U
#define LED_TX_TIMEOUT_MS               5U
#define LED_RESET_LATCH_MS              1U

#define MAI2LED_DUMMY_EEPROM_SIZE      8U

typedef struct
{
    uint8_t r;
    uint8_t g;
    uint8_t b;
} RGB_t;

typedef enum
{
    LED_TX_STATE_IDLE = 0,
    LED_TX_STATE_ACTIVE,
    LED_TX_STATE_LATCH_WAIT
} led_tx_state_t;

typedef enum
{
    LED_TX_KIND_NONE = 0,
    LED_TX_KIND_NORMAL,
    LED_TX_KIND_COUNT_CLEAR
} led_tx_kind_t;

typedef enum
{
    LED_FRAME_KIND_NONE = 0,
    LED_FRAME_KIND_NORMAL,
    LED_FRAME_KIND_IDLE,
    LED_FRAME_KIND_FADE_START,
    LED_FRAME_KIND_FADE_STEP,
    LED_FRAME_KIND_FADE_FINAL
} led_frame_kind_t;

typedef enum
{
    LED_FADE_STATE_IDLE = 0,
    LED_FADE_STATE_ARMED,
    LED_FADE_STATE_START_PENDING,
    LED_FADE_STATE_RUNNING,
    LED_FADE_STATE_FINAL_PENDING
} led_fade_state_t;

typedef struct
{
    mai2led_app_config_t config;
    PacketReq req;
    PacketAck ack;
    uint8_t dummy_eeprom[MAI2LED_DUMMY_EEPROM_SIZE];
    RGB_t staging_frame[MAI2LED_APP_DATA_BITS];
    RGB_t output_frame[MAI2LED_APP_DATA_BITS];
    RGB_t active_frame[MAI2LED_APP_DATA_BITS];
    RGB_t displayed_frame[MAI2LED_APP_DATA_BITS];
    uint32_t fade_started_tick;
    uint32_t fade_duration_ms;
    uint32_t fade_generation;
    uint32_t pending_frame_generation;
    uint32_t active_frame_generation;
    uint32_t tx_started_tick;
    uint32_t tx_latch_started_tick;
    uint8_t fade_start_logic;
    uint8_t fade_end_logic;
    uint8_t fade_progress;
    uint8_t configured_led_per_bit;
    uint8_t active_led_per_bit;
    RGB_t fade_start_color;
    RGB_t fade_end_color;
    RGB_t fade_now_color;
    uint32_t last_idle_update_tick;
    uint16_t idle_rainbow_loop;
    led_tx_state_t tx_state;
    led_tx_kind_t active_tx_kind;
    led_frame_kind_t pending_frame_kind;
    led_frame_kind_t active_frame_kind;
    led_fade_state_t fade_state;
    uint8_t staging_dirty_mask;
    bool normal_frame_dirty;
    bool count_clear_pending;
    bool idle_lights_enabled;
    bool idle_lights_dirty;
    bool rainbow_mode_enabled;
    bool initialized;
} mai2led_app_t;

static mai2led_app_t app;
static volatile bool led_tx_complete_event;
static volatile bool led_tx_error_event;

static bool set_pixels_rgb(RGB_t *pixels,
                           uint16_t start_pixel,
                           uint16_t end_pixel,
                           uint8_t red,
                           uint8_t green,
                           uint8_t blue)
{
    if ((pixels == NULL) ||
        (start_pixel > end_pixel) ||
        (end_pixel >= MAI2LED_APP_DATA_BITS))
    {
        return false;
    }

    for (uint16_t pixel = start_pixel; pixel <= end_pixel; pixel++)
    {
        pixels[pixel].r = red;
        pixels[pixel].g = green;
        pixels[pixel].b = blue;
    }

    return true;
}

static RGB_t rgb_blend(RGB_t c1, RGB_t c2, uint8_t amount)
{
    RGB_t out;

    out.r = (uint8_t)(((uint16_t)c1.r * (255U - amount) +
                       (uint16_t)c2.r * amount) / 255U);
    out.g = (uint8_t)(((uint16_t)c1.g * (255U - amount) +
                       (uint16_t)c2.g * amount) / 255U);
    out.b = (uint8_t)(((uint16_t)c1.b * (255U - amount) +
                       (uint16_t)c2.b * amount) / 255U);

    return out;
}

bool mai2led_app_is_led_per_bit_valid(uint8_t led_per_bit)
{
    if ((led_per_bit == 0U) ||
        (led_per_bit > MAI2LED_APP_MAX_LED_PER_BIT))
    {
        return false;
    }

    if ((app.config.led != NULL) &&
        ((uint16_t)led_per_bit * MAI2LED_APP_DATA_BITS >
         WS2812B_GetPixelCount(app.config.led)))
    {
        return false;
    }

    return true;
}

static uint8_t packet_read(void)
{
    static uint8_t r_len = 0;
    static uint8_t checksum = 0;
    static bool escape = false;
    uint8_t r;

    while (tud_cdc_n_available(MAI2LED_APP_CDC_ITF))
    {
        tud_cdc_n_read(MAI2LED_APP_CDC_ITF, &r, 1);

        if (r == Sync)
        {
            r_len = 0;
            checksum = 0;
            escape = false;
            memset(&app.req, 0, sizeof(app.req));
            continue;
        }

        if (r == Marker)
        {
            escape = true;
            continue;
        }

        if (escape)
        {
            r++;
            escape = false;
        }

        if (r_len == app.req.length + 3U)
        {
            uint8_t ret = (checksum == r) ? app.req.command : AckStatus_SumError;

            r_len = 0;
            checksum = 0;
            escape = false;

            return ret;
        }

        if (r_len >= sizeof(app.req.buffer))
        {
            r_len = 0;
            checksum = 0;
            escape = false;
            memset(&app.req, 0, sizeof(app.req));

            return AckStatus_RecvBfOverFlow;
        }

        app.req.buffer[r_len++] = r;
        checksum += r;
    }

    return 0;
}

static void packet_write(void)
{
    if (app.ack.command == 0)
    {
        return;
    }

    uint8_t checksum = 0;
    uint8_t w_len = 0;
    uint8_t data = Sync;

    tud_cdc_n_write(MAI2LED_APP_CDC_ITF, &data, 1);

    while (w_len < app.ack.length + 3U)
    {
        uint8_t w = app.ack.buffer[w_len++];
        checksum += w;

        if ((w == Sync) || (w == Marker))
        {
            data = Marker;
            tud_cdc_n_write(MAI2LED_APP_CDC_ITF, &data, 1);
            data = --w;
            tud_cdc_n_write(MAI2LED_APP_CDC_ITF, &data, 1);
        }
        else
        {
            data = w;
            tud_cdc_n_write(MAI2LED_APP_CDC_ITF, &data, 1);
        }
    }

    data = checksum;
    tud_cdc_n_write(MAI2LED_APP_CDC_ITF, &data, 1);
    tud_cdc_n_write_flush(MAI2LED_APP_CDC_ITF);

    app.ack.command = 0;
}

static void ack_init(uint8_t length, uint8_t status, uint8_t report)
{
    app.ack.dstNodeID = app.req.srcNodeID;
    app.ack.srcNodeID = app.req.dstNodeID;
    app.ack.length = 3U + length;
    app.ack.status = status;
    app.ack.command = app.req.command;
    app.ack.report = report;
}

static void clear_logical_frame(RGB_t *frame)
{
    if (frame != NULL)
    {
        memset(frame, 0, sizeof(RGB_t) * MAI2LED_APP_DATA_BITS);
    }
}

static bool led_frame_kind_is_fade(led_frame_kind_t kind)
{
    return (kind == LED_FRAME_KIND_FADE_START) ||
           (kind == LED_FRAME_KIND_FADE_STEP) ||
           (kind == LED_FRAME_KIND_FADE_FINAL);
}

static void queue_output_frame(led_frame_kind_t kind)
{
    app.normal_frame_dirty = true;
    app.pending_frame_kind = kind;
    app.pending_frame_generation = led_frame_kind_is_fade(kind) ?
                                   app.fade_generation : 0U;
}

static void commit_staging_frame(led_frame_kind_t kind)
{
    memcpy(app.output_frame,
           app.staging_frame,
           sizeof(app.output_frame));
    app.staging_dirty_mask = 0U;
    queue_output_frame(kind);
}

static void sync_unstaged_pixels_from(RGB_t const *frame)
{
    if (frame == NULL)
    {
        return;
    }

    for (uint8_t logical = 0U;
         logical < MAI2LED_APP_DATA_BITS;
         logical++)
    {
        if ((app.staging_dirty_mask & (uint8_t)(1U << logical)) == 0U)
        {
            app.staging_frame[logical] = frame[logical];
        }
    }
}

static void restore_displayed_output(void)
{
    memcpy(app.output_frame,
           app.displayed_frame,
           sizeof(app.output_frame));
    sync_unstaged_pixels_from(app.displayed_frame);
}

static void cancel_fade(void)
{
    app.fade_generation++;
    app.fade_state = LED_FADE_STATE_IDLE;

    if (app.normal_frame_dirty &&
        led_frame_kind_is_fade(app.pending_frame_kind))
    {
        app.normal_frame_dirty = false;
        app.pending_frame_kind = LED_FRAME_KIND_NONE;
        app.pending_frame_generation = 0U;
        restore_displayed_output();
    }
}

static bool led_transport_channel_ready(void)
{
    WS2812B_HandleTypeDef const *led = app.config.led;

    return (led != NULL) &&
           (WS2812B_GetState(led) == WS2812B_STATE_IDLE);
}

static bool led_transport_render_frame(led_tx_kind_t kind)
{
    WS2812B_HandleTypeDef *led = app.config.led;
    uint16_t active_total;
    uint16_t pixel_count = WS2812B_GetPixelCount(led);

    if ((led == NULL) ||
        (pixel_count == 0U) ||
        (pixel_count > MAI2LED_APP_MAX_LED_TOTAL))
    {
        return false;
    }

    active_total = (uint16_t)app.active_led_per_bit *
                   MAI2LED_APP_DATA_BITS;

    for (uint16_t pixel = 0U; pixel < pixel_count; pixel++)
    {
        RGB_t color = {0U, 0U, 0U};

        if ((kind == LED_TX_KIND_NORMAL) &&
            (app.active_led_per_bit > 0U) &&
            (pixel < active_total))
        {
            uint8_t logical = (uint8_t)(pixel / app.active_led_per_bit);

            color = app.active_frame[logical];
        }

        if (!WS2812B_SetPixel_RGB(led,
                                 pixel,
                                 color.r,
                                 color.g,
                                 color.b))
        {
            return false;
        }
    }

    return true;
}

static void led_transport_take_events(bool *complete, bool *error)
{
    uint32_t primask;

    if ((complete == NULL) || (error == NULL))
    {
        return;
    }

    primask = __get_PRIMASK();
    __disable_irq();
    *complete = led_tx_complete_event;
    *error = led_tx_error_event;
    led_tx_complete_event = false;
    led_tx_error_event = false;
    if (primask == 0U)
    {
        __enable_irq();
    }
}

static bool led_frame_can_retry(led_frame_kind_t kind,
                                uint32_t generation)
{
    if (kind == LED_FRAME_KIND_NORMAL)
    {
        return true;
    }

    if (kind == LED_FRAME_KIND_IDLE)
    {
        return app.idle_lights_enabled;
    }

    if (!led_frame_kind_is_fade(kind) ||
        (generation != app.fade_generation))
    {
        return false;
    }

    switch (kind)
    {
        case LED_FRAME_KIND_FADE_START:
            return app.fade_state == LED_FADE_STATE_START_PENDING;

        case LED_FRAME_KIND_FADE_STEP:
            return app.fade_state == LED_FADE_STATE_RUNNING;

        case LED_FRAME_KIND_FADE_FINAL:
            return app.fade_state == LED_FADE_STATE_FINAL_PENDING;

        default:
            return false;
    }
}

static void led_transport_enter_latch_wait(uint32_t now, bool success)
{
    if (!success)
    {
        if ((app.active_tx_kind == LED_TX_KIND_NORMAL) &&
            !app.normal_frame_dirty)
        {
            if (led_frame_can_retry(app.active_frame_kind,
                                    app.active_frame_generation))
            {
                memcpy(app.output_frame,
                       app.active_frame,
                       sizeof(app.output_frame));
                app.normal_frame_dirty = true;
                app.pending_frame_kind = app.active_frame_kind;
                app.pending_frame_generation =
                    app.active_frame_generation;
            }
            else if (led_frame_kind_is_fade(app.active_frame_kind) &&
                     (app.active_frame_generation != app.fade_generation))
            {
                restore_displayed_output();
            }
        }

        app.active_tx_kind = LED_TX_KIND_NONE;
        app.active_frame_kind = LED_FRAME_KIND_NONE;
        app.active_frame_generation = 0U;
    }

    app.tx_state = LED_TX_STATE_LATCH_WAIT;
    app.tx_latch_started_tick = now;
}

static void led_transport_start_frame(uint32_t now)
{
    WS2812B_HandleTypeDef *led = app.config.led;
    led_tx_kind_t kind;

    if (!led_transport_channel_ready())
    {
        return;
    }

    if (app.count_clear_pending)
    {
        kind = LED_TX_KIND_COUNT_CLEAR;
    }
    else if (app.normal_frame_dirty)
    {
        kind = LED_TX_KIND_NORMAL;
    }
    else
    {
        return;
    }

    if (kind == LED_TX_KIND_NORMAL)
    {
        memcpy(app.active_frame,
               app.output_frame,
               sizeof(app.active_frame));
        app.active_frame_kind = app.pending_frame_kind;
        app.active_frame_generation = app.pending_frame_generation;
    }
    else
    {
        app.active_frame_kind = LED_FRAME_KIND_NONE;
        app.active_frame_generation = 0U;
    }

    if (!led_transport_render_frame(kind) || !WS2812B_Update(led))
    {
        (void)WS2812B_Abort(led);
        app.active_tx_kind = LED_TX_KIND_NONE;
        app.active_frame_kind = LED_FRAME_KIND_NONE;
        app.active_frame_generation = 0U;
        app.tx_state = LED_TX_STATE_LATCH_WAIT;
        app.tx_latch_started_tick = now;
        return;
    }

    app.active_tx_kind = kind;
    app.tx_state = LED_TX_STATE_ACTIVE;
    app.tx_started_tick = now;
    if (kind == LED_TX_KIND_NORMAL)
    {
        app.normal_frame_dirty = false;
        app.pending_frame_kind = LED_FRAME_KIND_NONE;
        app.pending_frame_generation = 0U;
    }
}

static void led_transport_finish_normal_frame(uint32_t now)
{
    memcpy(app.displayed_frame,
           app.active_frame,
           sizeof(app.displayed_frame));

    if (!app.normal_frame_dirty &&
        ((app.active_frame_kind != LED_FRAME_KIND_IDLE) ||
         app.idle_lights_enabled))
    {
        memcpy(app.output_frame,
               app.active_frame,
               sizeof(app.output_frame));
        sync_unstaged_pixels_from(app.active_frame);
    }

    if (app.active_frame_generation != app.fade_generation)
    {
        return;
    }

    if ((app.active_frame_kind == LED_FRAME_KIND_FADE_START) &&
        (app.fade_state == LED_FADE_STATE_START_PENDING) &&
        !(app.normal_frame_dirty &&
          (app.pending_frame_kind == LED_FRAME_KIND_FADE_START) &&
          (app.pending_frame_generation == app.fade_generation)))
    {
        app.fade_started_tick = now;
        app.fade_progress = 0U;
        app.fade_state = LED_FADE_STATE_RUNNING;
    }
    else if ((app.active_frame_kind == LED_FRAME_KIND_FADE_FINAL) &&
             (app.fade_state == LED_FADE_STATE_FINAL_PENDING))
    {
        app.fade_state = LED_FADE_STATE_IDLE;
    }
}

static void led_transport_task(void)
{
    WS2812B_HandleTypeDef *led = app.config.led;
    uint32_t now = HAL_GetTick();
    bool complete = false;
    bool error = false;

    led_transport_take_events(&complete, &error);

    if (led == NULL)
    {
        return;
    }

    if (app.tx_state == LED_TX_STATE_ACTIVE)
    {
        if (error)
        {
            led_transport_enter_latch_wait(now, false);
        }
        else if (complete)
        {
            led_transport_enter_latch_wait(now, true);
        }
        else if ((uint32_t)(now - app.tx_started_tick) >=
                 LED_TX_TIMEOUT_MS)
        {
            (void)WS2812B_Abort(led);
            led_transport_enter_latch_wait(
                now,
                WS2812B_GetLastStatus(led) == WS2812B_STATUS_OK);
        }
    }

    if ((app.tx_state == LED_TX_STATE_LATCH_WAIT) &&
        ((uint32_t)(now - app.tx_latch_started_tick) >=
         LED_RESET_LATCH_MS))
    {
        if (app.active_tx_kind == LED_TX_KIND_NORMAL)
        {
            led_transport_finish_normal_frame(now);
        }
        else if (app.active_tx_kind == LED_TX_KIND_COUNT_CLEAR)
        {
            app.active_led_per_bit = app.configured_led_per_bit;
            app.count_clear_pending = false;
            if (app.idle_lights_enabled)
            {
                app.normal_frame_dirty = false;
                app.pending_frame_kind = LED_FRAME_KIND_NONE;
                app.pending_frame_generation = 0U;
                app.idle_lights_dirty = true;
                app.last_idle_update_tick = now;
            }
            else if (!app.normal_frame_dirty)
            {
                queue_output_frame(LED_FRAME_KIND_NORMAL);
            }
        }

        app.active_tx_kind = LED_TX_KIND_NONE;
        app.active_frame_kind = LED_FRAME_KIND_NONE;
        app.active_frame_generation = 0U;
        app.tx_state = LED_TX_STATE_IDLE;
    }

    if (app.tx_state == LED_TX_STATE_IDLE)
    {
        led_transport_start_frame(now);
    }
}

void mai2led_app_mark_io_active(void)
{
    if (app.idle_lights_enabled)
    {
        clear_logical_frame(app.staging_frame);
        clear_logical_frame(app.output_frame);
        app.staging_dirty_mask = 0U;
        app.normal_frame_dirty = false;
        app.pending_frame_kind = LED_FRAME_KIND_NONE;
        app.pending_frame_generation = 0U;
    }

    app.idle_lights_enabled = false;
    app.idle_lights_dirty = true;
}

bool mai2led_app_io_is_active(void)
{
    return !app.idle_lights_enabled;
}

void mai2led_app_restore_idle_lights(void)
{
    cancel_fade();
    app.idle_lights_enabled = true;
    app.idle_lights_dirty = true;
    app.last_idle_update_tick = HAL_GetTick() - IDLE_RAINBOW_UPDATE_MS;
}

static bool aime_is_active(void)
{
    return false;
}

static uint32_t rgb_from_hsv(uint8_t h, uint8_t s, uint8_t v)
{
    uint8_t region;
    uint8_t remainder;
    uint8_t p;
    uint8_t q;
    uint8_t t;
    uint8_t r;
    uint8_t g;
    uint8_t b;

    if (s == 0U)
    {
        return ((uint32_t)v << 16) | ((uint32_t)v << 8) | v;
    }

    region = h / 43U;
    remainder = (uint8_t)((h - (region * 43U)) * 6U);
    p = (uint8_t)(((uint16_t)v * (255U - s)) / 255U);
    q = (uint8_t)(((uint16_t)v * (255U - (((uint16_t)s * remainder) / 255U))) / 255U);
    t = (uint8_t)(((uint16_t)v * (255U - (((uint16_t)s * (255U - remainder)) / 255U))) / 255U);

    switch (region)
    {
        case 0:
            r = v;
            g = t;
            b = p;
            break;

        case 1:
            r = q;
            g = v;
            b = p;
            break;

        case 2:
            r = p;
            g = v;
            b = t;
            break;

        case 3:
            r = p;
            g = q;
            b = v;
            break;

        case 4:
            r = t;
            g = p;
            b = v;
            break;

        default:
            r = v;
            g = p;
            b = q;
            break;
    }

    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static void set_button_rgb(uint8_t button_index, uint32_t color)
{
    uint8_t red = (uint8_t)((color >> 16) & 0xffU);
    uint8_t green = (uint8_t)((color >> 8) & 0xffU);
    uint8_t blue = (uint8_t)(color & 0xffU);

    if (button_index >= MAI2LED_APP_DATA_BITS)
    {
        return;
    }

    app.staging_frame[button_index].r = red;
    app.staging_frame[button_index].g = green;
    app.staging_frame[button_index].b = blue;
}

static void set_idle_white_lights(void)
{
    (void)set_pixels_rgb(app.staging_frame,
                         0U,
                         MAI2LED_APP_DATA_BITS - 1U,
                         255U,
                         255U,
                         255U);
}

static void update_button_rainbow(void)
{
    uint16_t buttons = 0;

    if (app.config.button_read != NULL)
    {
        buttons = app.config.button_read();
    }

    app.idle_rainbow_loop += IDLE_RAINBOW_STEP;

    for (uint8_t i = 0; i < IDLE_RAINBOW_BUTTON_COUNT; i++)
    {
        uint8_t phase = (uint8_t)((((uint16_t)i * 256U) + app.idle_rainbow_loop) / IDLE_RAINBOW_BUTTON_COUNT);
        uint32_t color;

        if ((buttons & (uint16_t)(1U << i)) != 0U)
        {
            color = rgb_from_hsv(phase, IDLE_RAINBOW_PRESS_SATURATION, IDLE_RAINBOW_PRESS_VALUE);
        }
        else
        {
            color = rgb_from_hsv(phase, IDLE_RAINBOW_DIM_SATURATION, IDLE_RAINBOW_DIM_VALUE);
        }

        set_button_rgb(i, color);
    }
}

static void update_idle_button_lights(void)
{
    uint32_t now = HAL_GetTick();

    if (!app.idle_lights_enabled ||
        app.count_clear_pending ||
        aime_is_active())
    {
        return;
    }

    if ((uint32_t)(now - app.last_idle_update_tick) < IDLE_RAINBOW_UPDATE_MS)
    {
        return;
    }

    app.last_idle_update_tick = now;

    if (!app.rainbow_mode_enabled)
    {
        if (app.idle_lights_dirty)
        {
            set_idle_white_lights();
            commit_staging_frame(LED_FRAME_KIND_IDLE);
            app.idle_lights_dirty = false;
        }

        return;
    }

    update_button_rainbow();
    commit_staging_frame(LED_FRAME_KIND_IDLE);
    app.idle_lights_dirty = false;
}

static void begin_external_io_control(void)
{
    mai2led_app_mark_io_active();
}

static void set_led_gs_8bit(void)
{
    if (app.req.index < MAI2LED_APP_DATA_BITS)
    {
        app.staging_frame[app.req.index].r = app.req.color[0];
        app.staging_frame[app.req.index].g = app.req.color[1];
        app.staging_frame[app.req.index].b = app.req.color[2];
        app.staging_dirty_mask |= (uint8_t)(1U << app.req.index);
    }

    ack_init(0, AckStatus_Ok, AckReport_Ok);
}

static bool get_logic_range(uint8_t start,
                            uint8_t end,
                            uint8_t *range_start,
                            uint8_t *range_end)
{
    if ((range_start == NULL) || (range_end == NULL))
    {
        return false;
    }

    if (end == 0x20U)
    {
        end = MAI2LED_APP_DATA_BITS;
    }

    if ((start >= MAI2LED_APP_DATA_BITS) ||
        (end > MAI2LED_APP_DATA_BITS) ||
        (start >= end))
    {
        return false;
    }

    *range_start = start;
    *range_end = end;
    return true;
}

static void mark_staging_range_dirty(uint8_t start, uint8_t end)
{
    for (uint8_t logical = start; logical < end; logical++)
    {
        app.staging_dirty_mask |= (uint8_t)(1U << logical);
    }
}

static void set_led_gs_8bit_multi(void)
{
    uint8_t range_start;
    uint8_t range_end;

    app.fade_start_color.r = app.req.Multi_color[0];
    app.fade_start_color.g = app.req.Multi_color[1];
    app.fade_start_color.b = app.req.Multi_color[2];

    if (get_logic_range(app.req.start,
                        app.req.end,
                        &range_start,
                        &range_end))
    {
        (void)set_pixels_rgb(app.staging_frame,
                             range_start,
                             (uint16_t)(range_end - 1U),
                             app.fade_start_color.r,
                             app.fade_start_color.g,
                             app.fade_start_color.b);
        mark_staging_range_dirty(range_start, range_end);
    }

    cancel_fade();
    ack_init(0, AckStatus_Ok, AckReport_Ok);
}

static void set_led_gs_8bit_multi_fade(void)
{
    uint8_t speed = (app.req.speed == 0U) ? 1U : app.req.speed;
    uint8_t range_start;
    uint8_t range_end;

    app.fade_end_color.r = app.req.Multi_color[0];
    app.fade_end_color.g = app.req.Multi_color[1];
    app.fade_end_color.b = app.req.Multi_color[2];

    cancel_fade();
    if (get_logic_range(app.req.start,
                        app.req.end,
                        &range_start,
                        &range_end))
    {
        app.fade_start_logic = range_start;
        app.fade_end_logic = range_end;
        app.fade_duration_ms = (4095U / speed) * 8U;
        app.fade_state = LED_FADE_STATE_ARMED;
    }

    ack_init(0, AckStatus_Ok, AckReport_Ok);
}

static void set_led_gs_update(void)
{
    if (app.fade_state == LED_FADE_STATE_ARMED)
    {
        app.fade_progress = 0U;
        commit_staging_frame(LED_FRAME_KIND_FADE_START);
        app.fade_state = LED_FADE_STATE_START_PENDING;
    }
    else if (app.fade_state == LED_FADE_STATE_IDLE)
    {
        commit_staging_frame(LED_FRAME_KIND_NORMAL);
    }

    ack_init(0, AckStatus_Ok, AckReport_Ok);
}

static void set_led_fet(void)
{
    ack_init(0, AckStatus_Ok, AckReport_Ok);
}

static void get_board_info(void)
{
    memcpy(app.ack.boardNo, "15070-04\xFF\x90\x00", 10);
    app.ack.firmRevision = 144;
    ack_init(10, AckStatus_Ok, AckReport_Ok);
}

static void get_board_status(void)
{
    app.ack.timeoutStat = 0;
    app.ack.timeoutSec = 1;
    app.ack.pwmIo = 0;
    app.ack.fetTimeout = 0;
    ack_init(4, AckStatus_Ok, AckReport_Ok);
}

static void get_firm_sum(void)
{
    app.ack.sum_upper = 0;
    app.ack.sum_lower = 0;
    ack_init(2, AckStatus_Ok, AckReport_Ok);
}

static void get_protocol_version(void)
{
    app.ack.appliMode = 1;
    app.ack.major = 1;
    app.ack.minor = 1;
    ack_init(3, AckStatus_Ok, AckReport_Ok);
}

static bool fade_boundary_frame_pending(void)
{
    if (app.normal_frame_dirty)
    {
        if (app.pending_frame_kind == LED_FRAME_KIND_NORMAL)
        {
            return true;
        }

        if ((app.pending_frame_kind == LED_FRAME_KIND_FADE_START) &&
            (app.pending_frame_generation == app.fade_generation))
        {
            return true;
        }
    }

    if (app.active_tx_kind == LED_TX_KIND_NORMAL)
    {
        if (app.active_frame_kind == LED_FRAME_KIND_NORMAL)
        {
            return true;
        }

        if ((app.active_frame_kind == LED_FRAME_KIND_FADE_START) &&
            (app.active_frame_generation == app.fade_generation))
        {
            return true;
        }
    }

    return false;
}

static void fade_task(void)
{
    uint32_t elapsed;
    uint32_t now;

    if ((app.fade_state != LED_FADE_STATE_RUNNING) ||
        fade_boundary_frame_pending())
    {
        return;
    }

    now = HAL_GetTick();
    elapsed = (uint32_t)(now - app.fade_started_tick);

    if ((app.fade_duration_ms == 0U) ||
        (elapsed >= app.fade_duration_ms))
    {
        app.fade_progress = 255U;
    }
    else
    {
        app.fade_progress = (uint8_t)((elapsed * 255U) /
                                      app.fade_duration_ms);
    }

    app.fade_now_color = rgb_blend(app.fade_start_color,
                                   app.fade_end_color,
                                   app.fade_progress);

    (void)set_pixels_rgb(app.output_frame,
                         app.fade_start_logic,
                         (uint16_t)(app.fade_end_logic - 1U),
                         app.fade_now_color.r,
                         app.fade_now_color.g,
                         app.fade_now_color.b);
    for (uint8_t logical = app.fade_start_logic;
         logical < app.fade_end_logic;
         logical++)
    {
        if ((app.staging_dirty_mask & (uint8_t)(1U << logical)) == 0U)
        {
            app.staging_frame[logical] = app.fade_now_color;
        }
    }

    if (app.fade_progress == 255U)
    {
        queue_output_frame(LED_FRAME_KIND_FADE_FINAL);
        app.fade_state = LED_FADE_STATE_FINAL_PENDING;
    }
    else
    {
        queue_output_frame(LED_FRAME_KIND_FADE_STEP);
    }
}

void mai2led_app_init(mai2led_app_config_t const *config)
{
    uint8_t initial_led_per_bit;
    uint32_t now = HAL_GetTick();

    memset(&app, 0, sizeof(app));
    led_tx_complete_event = false;
    led_tx_error_event = false;

    if (config != NULL)
    {
        app.config = *config;
    }

    initial_led_per_bit = app.config.led_per_bit;
    if (!mai2led_app_is_led_per_bit_valid(initial_led_per_bit))
    {
        initial_led_per_bit = MAI2LED_APP_DEFAULT_LED_PER_BIT;
    }

    app.config.led_per_bit = initial_led_per_bit;
    app.configured_led_per_bit = initial_led_per_bit;
    app.active_led_per_bit = initial_led_per_bit;
    app.idle_lights_enabled = true;
    app.idle_lights_dirty = true;
    app.rainbow_mode_enabled = false;
    app.normal_frame_dirty = true;
    app.pending_frame_kind = LED_FRAME_KIND_IDLE;
    app.tx_state = LED_TX_STATE_LATCH_WAIT;
    app.tx_latch_started_tick = now;
    app.initialized = true;
    app.last_idle_update_tick = now - IDLE_RAINBOW_UPDATE_MS;
}

void mai2led_app_task(void)
{
    uint8_t command;

    if (!app.initialized)
    {
        return;
    }

    command = packet_read();

    if ((command != 0U) &&
        (command != AckStatus_SumError) &&
        (command != AckStatus_RecvBfOverFlow))
    {
        begin_external_io_control();
    }

    switch (command)
    {
        case AckStatus_SumError:
            ack_init(0, AckStatus_SumError, 0);
            break;

        case AckStatus_RecvBfOverFlow:
            ack_init(0, AckStatus_RecvBfOverFlow, 0);
            break;

        case SetLedGs8Bit:
            set_led_gs_8bit();
            break;

        case SetLedGs8BitMulti:
            set_led_gs_8bit_multi();
            break;

        case SetLedGs8BitMultiFade:
            set_led_gs_8bit_multi_fade();
            break;

        case SetLedFet:
            set_led_fet();
            break;

        case SetLedGsUpdate:
            set_led_gs_update();
            break;

        case SetEEPRom:
            if (app.req.Set_adress < sizeof(app.dummy_eeprom))
            {
                app.dummy_eeprom[app.req.Set_adress] =
                    app.req.writeData;
            }
            ack_init(0, AckStatus_Ok, AckReport_Ok);
            break;

        case GetEEPRom:
            app.ack.eepData =
                (app.req.Get_adress < sizeof(app.dummy_eeprom)) ?
                app.dummy_eeprom[app.req.Get_adress] : 0U;
            ack_init(1, AckStatus_Ok, AckReport_Ok);
            break;

        case GetBoardInfo:
            get_board_info();
            break;

        case GetBoardStatus:
            get_board_status();
            break;

        case GetFirmSum:
            get_firm_sum();
            break;

        case GetProtocolVersion:
            get_protocol_version();
            break;

        case SetEnableResponse:
        case SetDisableResponse:
        case 0:
            break;

        default:
            ack_init(0, AckStatus_Ok, AckReport_Ok);
            break;
    }

    packet_write();
    fade_task();
    update_idle_button_lights();
    led_transport_task();
}

void mai2led_app_notify_tx_complete(void)
{
    led_tx_complete_event = true;
}

void mai2led_app_notify_tx_error(void)
{
    led_tx_error_event = true;
}

bool mai2led_app_set_led_per_bit(uint8_t led_per_bit)
{
    if (!mai2led_app_is_led_per_bit_valid(led_per_bit))
    {
        return false;
    }

    if (led_per_bit == mai2led_app_get_led_per_bit())
    {
        return true;
    }

    cancel_fade();
    app.configured_led_per_bit = led_per_bit;
    app.config.led_per_bit = led_per_bit;
    app.count_clear_pending = true;
    app.idle_lights_dirty = true;
    app.last_idle_update_tick = HAL_GetTick();
    return true;
}

uint8_t mai2led_app_get_led_per_bit(void)
{
    return (app.configured_led_per_bit == 0U) ?
           MAI2LED_APP_DEFAULT_LED_PER_BIT :
           app.configured_led_per_bit;
}

uint16_t mai2led_app_get_led_total(void)
{
    return MAI2LED_APP_DATA_BITS * (uint16_t)mai2led_app_get_led_per_bit();
}

void mai2led_app_set_rainbow_mode(bool enabled)
{
    app.rainbow_mode_enabled = enabled;
    app.idle_lights_dirty = true;
    app.last_idle_update_tick = HAL_GetTick() - IDLE_RAINBOW_UPDATE_MS;
}

bool mai2led_app_get_rainbow_mode(void)
{
    return app.rainbow_mode_enabled;
}

bool mai2led_app_reset_light_config(void)
{
    if (!mai2led_app_set_led_per_bit(MAI2LED_APP_DEFAULT_LED_PER_BIT))
    {
        return false;
    }

    mai2led_app_set_rainbow_mode(false);
    return true;
}
