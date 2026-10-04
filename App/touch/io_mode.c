#include "io_mode.h"

#include "button/button_app.h"
#include "cdc_manager.h"
#include "kb/keyboard_app.h"
#include "main.h"
#include "tenodata.h"
#include "touch_pipeline.h"
#include <string.h>

/*
 * IO protocol (host -> device command):
 *
 *   A6 02 01 A9
 *
 * The payload byte is currently reserved and must be 0x01 to enable IO.
 * The trailing byte is an 8-bit sum of the preceding bytes.
 *
 * IO protocol (device -> host state packet):
 *
 *   A5 <buttons_lo> <buttons_hi> <touch0> ... <touch4> <seq>
 *
 * buttons: bit0..7 = BTN0..BTN7, bit8 = BTN8 (select),
 *          bit9 = BTN9 (test), bit10 = BTN10 (service).
 *          BTN11 is deliberately excluded: it remains a normal HID key.
 * touch:   bit0 = A1 ... bit33 = E8, mapping to the game's area order.
 */
#define IO_MODE_CMD_HEADER_0       0xA6U
#define IO_MODE_CMD_HEADER_1       0x02U
#define IO_MODE_CMD_ENABLE         0x01U
#define IO_MODE_CMD_LENGTH         4U
#define IO_MODE_STATE_HEADER       0xA5U
#define IO_MODE_STATE_LENGTH       9U
#define IO_MODE_REPORT_PERIOD_MS   2U

static bool enabled;
static uint8_t rx_frame[IO_MODE_CMD_LENGTH];
static uint8_t rx_length;
static uint32_t next_report_tick;
static uint8_t sequence;

static uint16_t io_mode_button_bits(void)
{
    uint16_t bits = 0U;

    for (uint8_t button = 0U; button < 8U; button++)
    {
        if (button_app_is_pressed(button))
        {
            bits |= (uint16_t)(1U << button);
        }
    }

    if (button_app_is_pressed(8U))
    {
        bits |= (uint16_t)(1U << 8);
    }
    if (button_app_is_pressed(9U))
    {
        bits |= (uint16_t)(1U << 9);
    }
    if (button_app_is_pressed(10U))
    {
        bits |= (uint16_t)(1U << 10);
    }

    return bits;
}

static void io_mode_send_state(void)
{
    uint8_t frame[IO_MODE_STATE_LENGTH];
    uint64_t touch_bits = touch_pipeline_is_ready()
                              ? touch_pipeline_get_touch_bits()
                              : 0U;
    uint16_t button_bits = io_mode_button_bits();

    frame[0] = IO_MODE_STATE_HEADER;
    frame[1] = (uint8_t)(button_bits & 0xFFU);
    frame[2] = (uint8_t)(button_bits >> 8);
    frame[3] = (uint8_t)(touch_bits & 0xFFU);
    frame[4] = (uint8_t)((touch_bits >> 8) & 0xFFU);
    frame[5] = (uint8_t)((touch_bits >> 16) & 0xFFU);
    frame[6] = (uint8_t)((touch_bits >> 24) & 0xFFU);
    frame[7] = (uint8_t)((touch_bits >> 32) & 0xFFU);
    frame[8] = sequence++;

    (void)cdc_manager_try_send_io(frame, sizeof(frame));
}

void io_mode_init(void)
{
    enabled = false;
    rx_length = 0U;
    next_report_tick = 0U;
    sequence = 0U;
}

void io_mode_set_enabled(bool value)
{
    if (enabled == value)
    {
        return;
    }

    cdc_manager_set_io_mode(value);
    enabled = value;
    rx_length = 0U;
    next_report_tick = HAL_GetTick();

    if (value)
    {
        /*
         * IO mode is a CDC0 consumer, not the HID path. Stop the HID task
         * from reporting so a connected host cannot see duplicate input.
         */
        keyboard_app_set_io_mode(true);
        touch_pipeline_init();
        tenodata_request_reconfigure();
    }
    else
    {
        keyboard_app_set_io_mode(false);
        touch_pipeline_init();
        tenodata_request_reconfigure();
    }
}

bool io_mode_is_enabled(void)
{
    return enabled;
}

bool io_mode_consume_rx_byte(uint8_t byte)
{
    uint8_t sum;

    if (rx_length == 0U)
    {
        if (byte != IO_MODE_CMD_HEADER_0)
        {
            return false;
        }

        rx_frame[0] = byte;
        rx_length = 1U;
        return true;
    }

    if (rx_length == 1U)
    {
        if (byte != IO_MODE_CMD_HEADER_1)
        {
            rx_length = 0U;
            return true;
        }

        rx_frame[1] = byte;
        rx_length = 2U;
        return true;
    }

    if (rx_length < IO_MODE_CMD_LENGTH)
    {
        rx_frame[rx_length++] = byte;
    }

    if (rx_length < IO_MODE_CMD_LENGTH)
    {
        return true;
    }

    sum = (uint8_t)(rx_frame[0] + rx_frame[1] + rx_frame[2]);
    if ((rx_frame[0] == IO_MODE_CMD_HEADER_0) &&
        (rx_frame[1] == IO_MODE_CMD_HEADER_1) &&
        (rx_frame[3] == sum))
    {
        io_mode_set_enabled(rx_frame[2] != 0U);
    }

    rx_length = 0U;
    return true;
}

void io_mode_task(void)
{
    uint32_t now;

    if (!enabled)
    {
        return;
    }

    now = HAL_GetTick();
    if ((int32_t)(now - next_report_tick) >= 0)
    {
        next_report_tick = now + IO_MODE_REPORT_PERIOD_MS;
        io_mode_send_state();
    }
}
