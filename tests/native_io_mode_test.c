/*
 * Host-side test for the CDC IO mode. It compiles the real cdc_manager.c and
 * io_mode.c with small stubs for the button, keyboard and touch pipeline.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint8_t g_rx[256];
static int g_rx_len;
static uint8_t g_tx[256];
static int g_tx_len;
static bool g_buttons[12];
static uint64_t g_touch;
static bool g_keyboard_io_mode;
static int g_reconfigure_count;
uint32_t native_test_tick;

uint8_t *tusb_stub_rx_buffer(void) { return g_rx; }
int *tusb_stub_rx_len(void) { return &g_rx_len; }
uint8_t *tusb_stub_tx_buffer(void) { return g_tx; }
int *tusb_stub_tx_len(void) { return &g_tx_len; }

void button_app_init(void) {}
void button_app_task(void) {}
bool button_app_is_pressed(uint8_t button_id)
{
    return (button_id < 12U) && g_buttons[button_id];
}
uint16_t button_app_read_main_mask8(void) { return 0U; }
bool button_app_set_long_press_callback(uint8_t button_id,
                                        void (*callback)(uint8_t, void *),
                                        void *context)
{
    (void)button_id; (void)callback; (void)context;
    return true;
}

void keyboard_app_init(void) {}
void keyboard_app_task(void) {}
void keyboard_app_set_io_mode(bool enabled) { g_keyboard_io_mode = enabled; }

void touch_pipeline_init(void) { g_touch = 0U; }
void touch_pipeline_set_device_masks(uint8_t connected, uint8_t forced)
{
    (void)connected; (void)forced;
}
bool touch_pipeline_is_ready(void) { return true; }
uint64_t touch_pipeline_get_touch_bits(void) { return g_touch; }
void touch_pipeline_commit_packet(const uint8_t *packet) { (void)packet; }
void touch_pipeline_commit_batch(const uint8_t *packet, uint16_t count)
{
    (void)packet; (void)count;
}

void tenodata_request_reconfigure(void) { g_reconfigure_count++; }
void tenodata_clear_host_config_override(void) {}

uint32_t HAL_GetTick(void)
{
    native_test_tick += 2U;
    return native_test_tick;
}

#include "cdc_manager.c"
#include "io_mode.c"

static int failures;

static void check(bool condition, const char *name)
{
    printf("  %s %s\n", condition ? "OK  " : "FAIL", name);
    if (!condition)
    {
        failures++;
    }
}

static void push_command(uint8_t enable)
{
    uint8_t frame[4] = {0xA6U, 0x02U, enable, 0U};
    frame[3] = (uint8_t)(frame[0] + frame[1] + frame[2]);
    memcpy(g_rx + g_rx_len, frame, sizeof(frame));
    g_rx_len += (int)sizeof(frame);
}

static void consume_command(void)
{
    while (g_rx_len > 0)
    {
        uint8_t byte = g_rx[0];
        memmove(g_rx, g_rx + 1, (size_t)(--g_rx_len));
        (void)io_mode_consume_rx_byte(byte);
    }
}

int main(void)
{
    printf("=== native IO mode test ===\n");

    cdc_manager_init();
    io_mode_init();
    check(!io_mode_is_enabled(), "power-on defaults to HID mode");
    check(!cdc_manager_is_io_mode(), "CDC0 still uses the raw route");

    push_command(1U);
    consume_command();
    check(io_mode_is_enabled(), "enable command switches to IO mode");
    check(cdc_manager_is_io_mode(), "CDC0 route is IO");
    check(g_keyboard_io_mode, "keyboard task is muted in IO mode");
    check(g_reconfigure_count == 1, "IO mode requests a PSoC reinitialize");
    check(!cdc_manager_try_send(false, (const uint8_t *)"x", 1U),
          "raw sender cannot share CDC0 in IO mode");

    g_buttons[0] = true;
    g_buttons[8] = true;
    g_buttons[9] = true;
    g_buttons[10] = true;
    g_touch = (1ULL << 0) | (1ULL << 17) | (1ULL << 33);
    g_tx_len = 0;
    io_mode_task();
    check(g_tx_len == 9, "state frame is 9 bytes");
    check(g_tx[0] == 0xA5U, "state header");
    check(g_tx[1] == 0x01U, "button 0 is in byte 1");
    check(g_tx[2] == 0x07U, "select, test and service are in byte 2");
    check(g_tx[3] == 0x01U && g_tx[4] == 0x00U && g_tx[5] == 0x02U &&
          g_tx[6] == 0x00U && g_tx[7] == 0x02U,
          "touch bits 0, 17 and 33 are packed into five bytes");

    push_command(0U);
    consume_command();
    check(!io_mode_is_enabled(), "disable command returns to HID mode");
    check(!cdc_manager_is_io_mode(), "CDC0 returns to the previous route");
    check(!g_keyboard_io_mode, "keyboard task resumes");

    g_tx_len = 0;
    io_mode_task();
    check(g_tx_len == 0, "no IO state is sent after disable");

    if (failures == 0)
    {
        printf("all native IO checks passed\n");
        return 0;
    }

    printf("%d native IO check(s) failed\n", failures);
    return 1;
}
