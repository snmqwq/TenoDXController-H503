#include "mai2led_app.h"

#include "flash_config.h"
#include "magic_config.h"
#include "main.h"
#include "mai2led.h"
#include "tusb.h"
#include <string.h>

#define SPECIAL_SEQ_LEN      8U
#define SPECIAL_MAGIC_CMD    0xB7U

#define IDLE_RAINBOW_UPDATE_MS         20U
#define IDLE_RAINBOW_STEP              10U
#define IDLE_RAINBOW_BUTTON_COUNT      MAI2LED_APP_DATA_BITS
#define IDLE_RAINBOW_DIM_SATURATION    240U
#define IDLE_RAINBOW_DIM_VALUE         128U
#define IDLE_RAINBOW_PRESS_SATURATION  64U
#define IDLE_RAINBOW_PRESS_VALUE       255U

#define MAI2LED_FLASH_CONFIG_MAGIC     0x324C414DUL
#define MAI2LED_FLASH_CONFIG_VERSION   2U
#define MAI2LED_PARAM_LED_PER_BIT      0x01U
#define MAI2LED_PARAM_RAINBOW_ENABLE   0x02U
#define MAI2LED_CONFIG_PAYLOAD_VERSION 1U

typedef struct
{
    uint8_t r;
    uint8_t g;
    uint8_t b;
} RGB_t;

typedef struct
{
    uint32_t magic;
    uint8_t version;
    uint8_t led_per_bit;
    uint8_t rainbow_mode_enable;
    uint8_t reserved0;
    uint8_t dummy_eeprom[8];
    uint8_t reserved[4];
} mai2led_flash_config_t;

typedef struct
{
    uint8_t version;
    uint8_t led_per_bit;
    uint8_t rainbow_mode_enable;
    uint8_t dummy_eeprom[8];
} mai2led_config_payload_t;

typedef struct
{
    mai2led_app_config_t config;
    PacketReq req;
    PacketAck ack;
    uint8_t dummy_eeprom[8];
    uint32_t fade_start_time;
    uint32_t fade_end_time;
    uint8_t fade_start_led;
    uint8_t fade_end_led;
    uint8_t fade_progress;
    RGB_t fade_start_color;
    RGB_t fade_end_color;
    RGB_t fade_now_color;
    uint32_t last_idle_update_tick;
    uint16_t idle_rainbow_loop;
    bool need_fade;
    bool fade_start;
    bool idle_lights_enabled;
    bool idle_lights_dirty;
    bool rainbow_mode_enabled;
    bool initialized;
} mai2led_app_t;

static mai2led_app_t app;

static const uint8_t special_seq[SPECIAL_SEQ_LEN] =
{
    0x91U,
    0x3eU,
    0xedU,
    0x20U,
    0x7cU,
    0x99U,
    0x58U,
    0xacU
};

static bool special_sequence_detect(uint8_t r)
{
    static uint8_t detector[SPECIAL_SEQ_LEN];

    memmove(&detector[0], &detector[1], SPECIAL_SEQ_LEN - 1U);
    detector[SPECIAL_SEQ_LEN - 1U] = r;

    return memcmp(detector, special_seq, SPECIAL_SEQ_LEN) == 0;
}

static bool set_pixels_rgb(WS28XX_HandleTypeDef *led,
                           uint16_t start_pixel,
                           uint16_t end_pixel,
                           uint8_t red,
                           uint8_t green,
                           uint8_t blue)
{
    if ((led == NULL) || (start_pixel > end_pixel) || (end_pixel >= led->MaxPixel))
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

static int32_t map_int32(int32_t x,
                         int32_t in_min,
                         int32_t in_max,
                         int32_t out_min,
                         int32_t out_max)
{
    if (in_max == in_min)
    {
        return out_min;
    }

    return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

static RGB_t rgb_blend(RGB_t c1, RGB_t c2, uint8_t amount)
{
    RGB_t out;

    out.r = ((uint16_t)c1.r * (255U - amount) + (uint16_t)c2.r * amount) / 255U;
    out.g = ((uint16_t)c1.g * (255U - amount) + (uint16_t)c2.g * amount) / 255U;
    out.b = ((uint16_t)c1.b * (255U - amount) + (uint16_t)c2.b * amount) / 255U;

    return out;
}

static bool led_per_bit_is_valid(uint8_t led_per_bit)
{
    if (led_per_bit == 0U)
    {
        return false;
    }

    if ((app.config.led != NULL) &&
        ((uint16_t)led_per_bit * MAI2LED_APP_DATA_BITS > app.config.led->MaxPixel))
    {
        return false;
    }

    return true;
}

static bool light_magic_read(uint8_t param,
                             uint8_t *data,
                             uint8_t max_length,
                             uint8_t *out_length)
{
    if ((data == NULL) || (out_length == NULL) || (max_length < 1U))
    {
        return false;
    }

    switch (param)
    {
        case MAI2LED_PARAM_LED_PER_BIT:
            data[0] = mai2led_app_get_led_per_bit();
            *out_length = 1U;
            return true;

        case MAI2LED_PARAM_RAINBOW_ENABLE:
            data[0] = mai2led_app_get_rainbow_mode() ? 1U : 0U;
            *out_length = 1U;
            return true;

        default:
            return false;
    }
}

static bool light_magic_write(uint8_t param, uint8_t const *data, uint8_t length)
{
    if ((data == NULL) || (length != 1U))
    {
        return false;
    }

    switch (param)
    {
        case MAI2LED_PARAM_LED_PER_BIT:
            if (!led_per_bit_is_valid(data[0]))
            {
                return false;
            }

            mai2led_app_set_led_per_bit(data[0]);
            return true;

        case MAI2LED_PARAM_RAINBOW_ENABLE:
            mai2led_app_set_rainbow_mode(data[0] != 0U);
            return true;

        default:
            return false;
    }
}

static bool light_magic_save(uint8_t param)
{
    (void)param;
    return mai2led_app_save_config_to_flash();
}

static bool light_magic_load_default(uint8_t param)
{
    (void)param;
    mai2led_app_set_led_per_bit(MAI2LED_APP_DEFAULT_LED_PER_BIT);
    mai2led_app_set_rainbow_mode(false);
    memset(app.dummy_eeprom, 0, sizeof(app.dummy_eeprom));
    return true;
}

static bool light_magic_info(uint8_t param,
                             uint8_t *data,
                             uint8_t max_length,
                             uint8_t *out_length)
{
    (void)param;

    if ((data == NULL) || (out_length == NULL) || (max_length < 2U))
    {
        return false;
    }

    data[0] = MAI2LED_PARAM_LED_PER_BIT;
    data[1] = MAI2LED_PARAM_RAINBOW_ENABLE;
    *out_length = 2U;
    return true;
}

static bool light_magic_read_all(uint8_t *data, uint8_t max_length, uint8_t *out_length)
{
    mai2led_config_payload_t payload;

    if ((data == NULL) || (out_length == NULL) || (max_length < sizeof(payload)))
    {
        return false;
    }

    payload.version = MAI2LED_CONFIG_PAYLOAD_VERSION;
    payload.led_per_bit = mai2led_app_get_led_per_bit();
    payload.rainbow_mode_enable = app.rainbow_mode_enabled ? 1U : 0U;
    memcpy(payload.dummy_eeprom, app.dummy_eeprom, sizeof(payload.dummy_eeprom));

    memcpy(data, &payload, sizeof(payload));
    *out_length = sizeof(payload);

    return true;
}

static bool light_magic_write_all(uint8_t const *data, uint8_t length)
{
    mai2led_config_payload_t payload;

    if ((data == NULL) || (length != sizeof(payload)))
    {
        return false;
    }

    memcpy(&payload, data, sizeof(payload));

    if ((payload.version != MAI2LED_CONFIG_PAYLOAD_VERSION) ||
        !led_per_bit_is_valid(payload.led_per_bit))
    {
        return false;
    }

    mai2led_app_set_led_per_bit(payload.led_per_bit);
    mai2led_app_set_rainbow_mode(payload.rainbow_mode_enable != 0U);
    memcpy(app.dummy_eeprom, payload.dummy_eeprom, sizeof(app.dummy_eeprom));

    return true;
}

static void register_light_magic(void)
{
    static const magic_config_module_t module =
    {
        .module = MAGIC_CONFIG_MODULE_LIGHT,
        .read = light_magic_read,
        .write = light_magic_write,
        .save = light_magic_save,
        .load_default = light_magic_load_default,
        .get_info = light_magic_info,
        .read_all = light_magic_read_all,
        .write_all = light_magic_write_all
    };

    (void)magic_config_register(&module);
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

        if (special_sequence_detect(r))
        {
            r_len = 0;
            checksum = 0;
            escape = false;
            memset(&app.req, 0, sizeof(app.req));

            return SPECIAL_MAGIC_CMD;
        }

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

static bool magic_read_bytes(uint8_t *data, uint8_t length)
{
    uint8_t count = 0;
    uint32_t start = HAL_GetTick();

    while (count < length)
    {
        tud_task();

        if (tud_cdc_n_available(MAI2LED_APP_CDC_ITF))
        {
            tud_cdc_n_read(MAI2LED_APP_CDC_ITF, &data[count], 1);
            count++;
        }

        if ((uint32_t)(HAL_GetTick() - start) > 100U)
        {
            return false;
        }
    }

    return true;
}

static void magic_send_response(uint8_t status,
                                uint8_t module,
                                uint8_t cmd,
                                uint8_t param,
                                uint8_t const *payload,
                                uint8_t payload_length)
{
    uint8_t header[6];
    uint8_t sum = 0;

    header[0] = 0xACU;
    header[1] = status;
    header[2] = module;
    header[3] = cmd;
    header[4] = param;
    header[5] = payload_length;

    for (uint8_t i = 0; i < sizeof(header); i++)
    {
        sum += header[i];
    }

    for (uint8_t i = 0; i < payload_length; i++)
    {
        sum += payload[i];
    }

    tud_cdc_n_write(MAI2LED_APP_CDC_ITF, header, sizeof(header));

    if ((payload != NULL) && (payload_length > 0U))
    {
        tud_cdc_n_write(MAI2LED_APP_CDC_ITF, payload, payload_length);
    }

    tud_cdc_n_write(MAI2LED_APP_CDC_ITF, &sum, 1);
    tud_cdc_n_write_flush(MAI2LED_APP_CDC_ITF);
}

static void magic_process_from_cdc(void)
{
    uint8_t header[4];
    uint8_t payload[MAGIC_CONFIG_MAX_PAYLOAD];
    uint8_t response[MAGIC_CONFIG_MAX_PAYLOAD];
    uint8_t checksum;
    uint8_t sum = 0;
    uint8_t response_length = 0;
    uint8_t status;
    uint8_t module;
    uint8_t cmd;
    uint8_t param;
    uint8_t length;

    if (!magic_read_bytes(header, sizeof(header)))
    {
        magic_send_response(MAGIC_CONFIG_STATUS_IO_ERROR, 0, 0, 0, NULL, 0);
        return;
    }

    module = header[0];
    cmd = header[1];
    param = header[2];
    length = header[3];

    for (uint8_t i = 0; i < sizeof(header); i++)
    {
        sum += header[i];
    }

    if (length > MAGIC_CONFIG_MAX_PAYLOAD)
    {
        magic_send_response(MAGIC_CONFIG_STATUS_LENGTH_ERROR, module, cmd, param, NULL, 0);
        return;
    }

    if ((length > 0U) && !magic_read_bytes(payload, length))
    {
        magic_send_response(MAGIC_CONFIG_STATUS_IO_ERROR, module, cmd, param, NULL, 0);
        return;
    }

    for (uint8_t i = 0; i < length; i++)
    {
        sum += payload[i];
    }

    if (!magic_read_bytes(&checksum, 1U))
    {
        magic_send_response(MAGIC_CONFIG_STATUS_IO_ERROR, module, cmd, param, NULL, 0);
        return;
    }

    if (checksum != sum)
    {
        magic_send_response(MAGIC_CONFIG_STATUS_SUM_ERROR, module, cmd, param, NULL, 0);
        return;
    }

    status = magic_config_handle(module,
                                 cmd,
                                 param,
                                 payload,
                                 length,
                                 response,
                                 sizeof(response),
                                 &response_length);

    magic_send_response(status, module, cmd, param, response, response_length);
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

static void logic_to_physical(uint8_t start_logic,
                              uint8_t end_logic,
                              uint8_t *start_phy,
                              uint8_t *end_phy)
{
    uint8_t led_per_bit = mai2led_app_get_led_per_bit();

    if ((start_phy == NULL) || (end_phy == NULL))
    {
        return;
    }

    if (led_per_bit == 0)
    {
        led_per_bit = 1;
    }

    if (start_logic >= MAI2LED_APP_DATA_BITS)
    {
        start_logic = MAI2LED_APP_DATA_BITS - 1U;
    }

    if (end_logic >= MAI2LED_APP_DATA_BITS)
    {
        end_logic = MAI2LED_APP_DATA_BITS - 1U;
    }

    if (start_logic > end_logic)
    {
        uint8_t temp = start_logic;
        start_logic = end_logic;
        end_logic = temp;
    }

    *start_phy = start_logic * led_per_bit;
    *end_phy = ((uint8_t)(end_logic + 1U) * led_per_bit) - 1U;
}

static void clear_button_lights(void);
static bool led_flush(void);

void mai2led_app_mark_io_active(void)
{
    if (app.idle_lights_enabled)
    {
        clear_button_lights();
        led_flush();
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
    app.idle_lights_enabled = true;
    app.idle_lights_dirty = true;
    app.last_idle_update_tick = HAL_GetTick() - IDLE_RAINBOW_UPDATE_MS;
}

static bool aime_is_active(void)
{
    return false;
}

static bool led_flush(void)
{
    if (app.config.led != NULL)
    {
        return WS28XX_Update(app.config.led);
    }

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
    uint8_t start_phy;
    uint8_t end_phy;
    uint8_t red = (uint8_t)((color >> 16) & 0xffU);
    uint8_t green = (uint8_t)((color >> 8) & 0xffU);
    uint8_t blue = (uint8_t)(color & 0xffU);

    if ((app.config.led == NULL) || (button_index >= MAI2LED_APP_DATA_BITS))
    {
        return;
    }

    logic_to_physical(button_index, button_index, &start_phy, &end_phy);
    set_pixels_rgb(app.config.led, start_phy, end_phy, red, green, blue);
}

static void clear_button_lights(void)
{
    if (app.config.led == NULL)
    {
        return;
    }

    if (app.config.led->MaxPixel > 0U)
    {
        set_pixels_rgb(app.config.led, 0, app.config.led->MaxPixel - 1U, 0, 0, 0);
    }
}

static void set_idle_white_lights(void)
{
    uint16_t led_total;

    if (app.config.led == NULL)
    {
        return;
    }

    led_total = mai2led_app_get_led_total();
    if (led_total > app.config.led->MaxPixel)
    {
        led_total = app.config.led->MaxPixel;
    }

    if (led_total > 0U)
    {
        set_pixels_rgb(app.config.led, 0, led_total - 1U, 255, 255, 255);
    }
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

    if (!app.idle_lights_enabled || aime_is_active())
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
            if (led_flush())
            {
                app.idle_lights_dirty = false;
            }
        }

        return;
    }

    update_button_rainbow();
    if (led_flush())
    {
        app.idle_lights_dirty = false;
    }
    else
    {
        app.idle_lights_dirty = true;
    }
}

static void begin_external_io_control(void)
{
    mai2led_app_mark_io_active();
}

static void set_led_gs_8bit(void)
{
    uint8_t start_phy;
    uint8_t end_phy;

    logic_to_physical(app.req.index, app.req.index, &start_phy, &end_phy);
    set_pixels_rgb(app.config.led, start_phy, end_phy, app.req.color[0], app.req.color[1], app.req.color[2]);
    WS28XX_Update(app.config.led);

    ack_init(0, AckStatus_Ok, AckReport_Ok);
}

static void set_led_gs_8bit_multi(void)
{
    if (app.req.end == 0x20U)
    {
        app.req.end = (uint8_t)mai2led_app_get_led_total();
    }

    logic_to_physical(app.req.start, app.req.end - 1U, &app.fade_start_led, &app.fade_end_led);

    app.fade_start_color.r = app.req.Multi_color[0];
    app.fade_start_color.g = app.req.Multi_color[1];
    app.fade_start_color.b = app.req.Multi_color[2];

    set_pixels_rgb(app.config.led,
                   app.fade_start_led,
                   app.fade_end_led,
                   app.fade_start_color.r,
                   app.fade_start_color.g,
                   app.fade_start_color.b);

    app.need_fade = false;
    app.fade_start = false;
    ack_init(0, AckStatus_Ok, AckReport_Ok);
}

static void set_led_gs_8bit_multi_fade(void)
{
    uint8_t speed = (app.req.speed == 0U) ? 1U : app.req.speed;

    app.fade_end_color.r = app.req.Multi_color[0];
    app.fade_end_color.g = app.req.Multi_color[1];
    app.fade_end_color.b = app.req.Multi_color[2];

    app.fade_start_time = HAL_GetTick();
    app.fade_end_time = app.fade_start_time + (4095U / speed * 8U);

    logic_to_physical(app.req.start, app.req.end - 1U, &app.fade_start_led, &app.fade_end_led);
    app.need_fade = true;

    ack_init(0, AckStatus_Ok, AckReport_Ok);
}

static void set_led_gs_update(void)
{
    if (!app.need_fade)
    {
        WS28XX_Update(app.config.led);
    }
    else
    {
        app.fade_start = true;
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

static void fade_task(void)
{
    if (!app.need_fade || !app.fade_start)
    {
        return;
    }

    uint32_t now = HAL_GetTick();

    if (now >= app.fade_end_time)
    {
        app.need_fade = false;
        app.fade_start = false;
        app.fade_progress = 255;
    }
    else
    {
        app.fade_progress = (uint8_t)map_int32((int32_t)now,
                                               (int32_t)app.fade_start_time,
                                               (int32_t)app.fade_end_time,
                                               0,
                                               255);
    }

    app.fade_now_color = rgb_blend(app.fade_start_color, app.fade_end_color, app.fade_progress);

    set_pixels_rgb(app.config.led,
                   app.fade_start_led,
                   app.fade_end_led,
                   app.fade_now_color.r,
                   app.fade_now_color.g,
                   app.fade_now_color.b);
    WS28XX_Update(app.config.led);
}

void mai2led_app_init(mai2led_app_config_t const *config)
{
    memset(&app, 0, sizeof(app));

    if (config != NULL)
    {
        app.config = *config;
    }

    if (app.config.led_per_bit == 0U)
    {
        app.config.led_per_bit = MAI2LED_APP_DEFAULT_LED_PER_BIT;
    }

    app.idle_lights_enabled = true;
    app.idle_lights_dirty = true;
    app.rainbow_mode_enabled = false;
    app.initialized = true;
    app.last_idle_update_tick = HAL_GetTick() - IDLE_RAINBOW_UPDATE_MS;

    (void)mai2led_app_load_config_from_flash();
    register_light_magic();
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
        (command != SPECIAL_MAGIC_CMD) &&
        (command != AckStatus_SumError) &&
        (command != AckStatus_RecvBfOverFlow))
    {
        begin_external_io_control();
    }

    switch (command)
    {
        case SPECIAL_MAGIC_CMD:
            magic_process_from_cdc();
            app.ack.command = 0;
            break;

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
                app.dummy_eeprom[app.req.Set_adress] = app.req.writeData;
            }
            ack_init(0, AckStatus_Ok, AckReport_Ok);
            break;

        case GetEEPRom:
            app.ack.eepData = (app.req.Get_adress < sizeof(app.dummy_eeprom)) ? app.dummy_eeprom[app.req.Get_adress] : 0;
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
}

void mai2led_app_set_led_per_bit(uint8_t led_per_bit)
{
    if (!led_per_bit_is_valid(led_per_bit))
    {
        led_per_bit = MAI2LED_APP_DEFAULT_LED_PER_BIT;
    }

    app.config.led_per_bit = led_per_bit;
    app.idle_lights_dirty = true;
    app.last_idle_update_tick = HAL_GetTick() - IDLE_RAINBOW_UPDATE_MS;
}

uint8_t mai2led_app_get_led_per_bit(void)
{
    return (app.config.led_per_bit == 0U) ? MAI2LED_APP_DEFAULT_LED_PER_BIT : app.config.led_per_bit;
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

bool mai2led_app_load_config_from_flash(void)
{
    mai2led_flash_config_t flash_config;
    uint16_t length = 0;

    memset(&flash_config, 0, sizeof(flash_config));
    if (!flash_config_read(FLASH_CONFIG_SLOT_LIGHT,
                           &flash_config,
                           sizeof(flash_config),
                           &length))
    {
        return false;
    }

    if ((length != sizeof(flash_config)) ||
        (flash_config.magic != MAI2LED_FLASH_CONFIG_MAGIC) ||
        (flash_config.version != MAI2LED_FLASH_CONFIG_VERSION))
    {
        return false;
    }

    if (led_per_bit_is_valid(flash_config.led_per_bit))
    {
        app.config.led_per_bit = flash_config.led_per_bit;
    }

    app.rainbow_mode_enabled = flash_config.rainbow_mode_enable != 0U;
    memcpy(app.dummy_eeprom, flash_config.dummy_eeprom, sizeof(app.dummy_eeprom));
    app.idle_lights_dirty = true;
    app.last_idle_update_tick = HAL_GetTick() - IDLE_RAINBOW_UPDATE_MS;

    return true;
}

bool mai2led_app_save_config_to_flash(void)
{
    mai2led_flash_config_t flash_config;

    memset(&flash_config, 0xff, sizeof(flash_config));
    flash_config.magic = MAI2LED_FLASH_CONFIG_MAGIC;
    flash_config.version = MAI2LED_FLASH_CONFIG_VERSION;
    flash_config.led_per_bit = mai2led_app_get_led_per_bit();
    flash_config.rainbow_mode_enable = app.rainbow_mode_enabled ? 1U : 0U;
    memcpy(flash_config.dummy_eeprom, app.dummy_eeprom, sizeof(app.dummy_eeprom));

    return flash_config_write(FLASH_CONFIG_SLOT_LIGHT,
                              &flash_config,
                              sizeof(flash_config));
}
