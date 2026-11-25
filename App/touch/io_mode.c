#include "io_mode.h"

#include "button/button_app.h"
#include "cdc_manager.h"
#include "kb/keyboard_app.h"
#include "main.h"
#include "tenodata.h"
#include "touch_pipeline.h"

/*
 * IO protocol (host -> device command):
 *
 *   A6 02 01 A9 (enable), A6 02 00 A8 (disable).
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
#define IO_MODE_STATE_HEADER       0xA5U
#define IO_MODE_STATE_LENGTH       9U
#define IO_MODE_REPORT_PERIOD_MS   2U

static bool enabled;
static uint32_t rx_command;
static uint32_t next_report_tick;
static uint8_t sequence;

static uint16_t io_mode_button_bits(void)
{
    uint16_t bits = 0U;

    for (uint8_t button = 0U; button < 8U; button++)
    {
        /* Main keys drive the game buttons: read them without the debounce
         * that the config switches need. */
        if (button_app_is_pressed_immediate(button))
        {
            bits |= (uint16_t)(1U << button);
        }
    }

    if (button_app_is_pressed_immediate(8U))
    {
        bits |= (uint16_t)(1U << 8);
    }
    if (button_app_is_pressed_immediate(9U))
    {
        bits |= (uint16_t)(1U << 9);
    }
    if (button_app_is_pressed_immediate(10U))
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
    rx_command = 0U;
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
    rx_command = 0U;
    next_report_tick = HAL_GetTick();

    keyboard_app_set_io_mode(value);
    /* Reconfiguration also clears the previous bitmap before I2C drains. */
    tenodata_request_reconfigure();
}

bool io_mode_is_enabled(void)
{
    return enabled;
}

bool io_mode_observe_rx_byte(uint8_t byte)
{
    /* Match the last four wire bytes, also when split across USB reads.
     * A stray A6 cannot consume the next Raw header or Mai2Touch command. */
    rx_command = (rx_command << 8) | byte;
    uint8_t mode = (uint8_t)(rx_command >> 8);
    if ((rx_command >> 16) != ((IO_MODE_CMD_HEADER_0 << 8) | IO_MODE_CMD_HEADER_1) ||
        byte != (uint8_t)(IO_MODE_CMD_HEADER_0 + IO_MODE_CMD_HEADER_1 + mode))
    {
        return false;
    }
    io_mode_set_enabled(mode != 0U);
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
