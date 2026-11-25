/* Run the firmware from PSoC I2C registers and GPIO pins through CDC0 and HID. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "main.h"
#include "i2c.h"
#include "tusb.h"
#include "button/button_app.h"
#include "kb/keyboard_app.h"
#include "cdc_manager.h"
#include "psoc_comm.h"
#include "touch_app.h"
#include "touch_pipeline.h"

static const uint8_t enable_io[] = {0xa6, 0x02, 0x01, 0xa9};
static const uint8_t disable_io[] = {0xa6, 0x02, 0x00, 0xa8};
static const uint8_t sens[] = {'{', 'L', 'A', 'k', 0xa6, '}'};
static const uint8_t sens_echo[] = {'(', 'L', 'A', 'k', 0xa6, ')'};

GPIO_TypeDef native_gpio_a, native_gpio_b;
I2C_HandleTypeDef hi2c1 = {I2C1};
uint32_t native_test_tick;
static uint16_t pa_high, pb_low;
static struct {
    uint8_t status, config[PSOC_COMM_CONFIG_LENGTH];
    unsigned config_writes, samples;
    bool touched;
} chips[PSOC_COMM_DEVICE_COUNT];

static uint8_t usb_rx[4096], usb_tx[4096], last_cdc[4096], hid[14];
static int rx_count, tx_count, last_cdc_len;
uint8_t *tusb_stub_rx_buffer(void) { return usb_rx; }
uint8_t *tusb_stub_tx_buffer(void) { return usb_tx; }
int *tusb_stub_rx_len(void) { return &rx_count; }
int *tusb_stub_tx_len(void) { return &tx_count; }
bool tud_hid_n_ready(uint8_t instance) { return instance == 0; }
bool tud_hid_n_report(uint8_t instance, uint8_t id, const void *report, uint16_t len)
{
    assert(instance == 0 && id == 0 && len == sizeof(hid));
    memcpy(hid, report, len);
    return true;
}

uint32_t HAL_GetTick(void) { return native_test_tick; }
void HAL_Delay(uint32_t ms) { native_test_tick += ms; }
GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef *port, uint16_t pin)
{
    return ((port == GPIOA ? pa_high : (uint16_t)~pb_low) & pin) ? GPIO_PIN_SET : GPIO_PIN_RESET;
}

static unsigned chip_index(uint16_t addr)
{
    assert(addr == (0x08u << 1) || addr == (0x09u << 1));
    return (addr >> 1) - 0x08u;
}
HAL_StatusTypeDef HAL_I2C_IsDeviceReady(I2C_HandleTypeDef *bus, uint16_t addr,
                                        uint32_t trials, uint32_t timeout)
{
    return bus == &hi2c1 && chip_index(addr) < PSOC_COMM_DEVICE_COUNT ? HAL_OK : HAL_ERROR;
}
HAL_StatusTypeDef HAL_I2C_Mem_Write(I2C_HandleTypeDef *bus, uint16_t addr, uint16_t reg,
                                    uint16_t width, uint8_t *data, uint16_t len, uint32_t timeout)
{
    unsigned i = chip_index(addr);
    assert(bus == &hi2c1 && width == I2C_MEMADD_SIZE_8BIT);
    if (reg == PSOC_EZI2C_OFFSET_CONFIG && len == sizeof(chips[i].config)) {
        memcpy(chips[i].config, data, len);
        chips[i].config_writes++;
    } else if (reg == PSOC_EZI2C_OFFSET_STATUS && len == 1) {
        if (*data == PSOC_STATUS_START_CALIBRATION) {
            assert(chips[i].config_writes > 0);
            chips[i].status = PSOC_STATUS_CALIBRATION_DONE;
        } else {
            assert(*data == PSOC_COMMAND_SOFT_RESET);
            chips[i].status = PSOC_STATUS_CRASH;
        }
    } else {
        return HAL_ERROR;
    }
    return HAL_OK;
}
HAL_StatusTypeDef HAL_I2C_Mem_Read(I2C_HandleTypeDef *bus, uint16_t addr, uint16_t reg,
                                   uint16_t width, uint8_t *data, uint16_t len, uint32_t timeout)
{
    unsigned i = chip_index(addr);
    assert(bus == &hi2c1 && width == I2C_MEMADD_SIZE_8BIT);
    if (reg == PSOC_EZI2C_OFFSET_STATUS && len == 1) {
        *data = chips[i].status;
    } else if (reg == PSOC_EZI2C_OFFSET_CP && len == PSOC_COMM_CAL_LENGTH) {
        memset(data, 1, len);
    } else {
        return HAL_ERROR;
    }
    return HAL_OK;
}
HAL_StatusTypeDef HAL_I2C_Mem_Read_IT(I2C_HandleTypeDef *bus, uint16_t addr, uint16_t reg,
                                      uint16_t width, uint8_t *data, uint16_t len)
{
    unsigned i = chip_index(addr);
    assert(bus == &hi2c1 && reg == 0 && width == I2C_MEMADD_SIZE_8BIT &&
           len == PSOC_COMM_I2C_DATA_LENGTH);
    data[0] = chips[i].status;
    /* A one-count idle variation represents fresh, distinct electrode scans. */
    uint16_t idle = 1000u + (++chips[i].samples & 1u);
    for (unsigned ch = 0; ch < PSOC_COMM_CHANNELS_PER_DEVICE; ch++) {
        uint16_t raw = idle + ((ch == 0 && chips[i].touched) ? 300u : 0u);
        data[1 + 2 * ch] = (uint8_t)raw;
        data[2 + 2 * ch] = (uint8_t)(raw >> 8);
    }
    HAL_I2C_MemRxCpltCallback(bus);
    return HAL_OK;
}
unsigned HAL_I2C_GetState(I2C_HandleTypeDef *bus) { return HAL_I2C_STATE_READY; }
HAL_StatusTypeDef HAL_I2C_DeInit(I2C_HandleTypeDef *bus) { return HAL_OK; }
void MX_I2C1_Init(void) {}

static void queue(const uint8_t *bytes, size_t len)
{
    assert((size_t)rx_count + len <= sizeof(usb_rx));
    memcpy(usb_rx + rx_count, bytes, len);
    rx_count += (int)len;
}
static void advance(unsigned ticks)
{
    last_cdc_len = 0;
    for (unsigned n = 0; n < ticks; n++) {
        native_test_tick++;
        button_app_task();
        keyboard_app_task();
        touch_app_task();
        if (tx_count > 0) {
            memcpy(last_cdc, usb_tx, tx_count);
            last_cdc_len = tx_count;
            tx_count = 0;
        }
    }
}
static bool io_frame(uint16_t buttons, uint64_t touch)
{
    uint64_t bits = 0;
    if (last_cdc_len != 9 || last_cdc[0] != 0xa5) return false;
    for (unsigned i = 0; i < 5; i++) bits |= (uint64_t)last_cdc[3 + i] << (8 * i);
    return (last_cdc[1] | (last_cdc[2] << 8)) == buttons && bits == touch;
}
static bool configs_match(const uint8_t *payload)
{
    for (unsigned chip = 0; chip < PSOC_COMM_DEVICE_COUNT; chip++) {
        if (!chips[chip].config_writes || chips[chip].status != PSOC_STATUS_CALIBRATION_DONE)
            return false;
        for (unsigned group = 0; group < 4; group++) {
            if (memcmp(chips[chip].config + group * 17, payload + group * 34 + chip * 17, 17))
                return false;
        }
    }
    return true;
}
static uint8_t checksum(const uint8_t *data, size_t len)
{
    uint8_t sum = 0;
    while (len--) sum += *data++;
    return sum;
}
static bool saw_echo(void)
{
    return last_cdc_len == sizeof(sens_echo) && !memcmp(last_cdc, sens_echo, sizeof(sens_echo));
}

#define CHECK(condition, name) do { \
    if (!(condition)) { fprintf(stderr, "FAIL: %s (tick %u, CDC length %d)\n", \
                                name, (unsigned)native_test_tick, last_cdc_len); return 1; } \
    puts("  OK  " name); \
} while (0)

int main(void)
{
    const uint64_t e4 = UINT64_C(1) << 29, e8 = UINT64_C(1) << 33;
    uint8_t raw_frame[2 + TENODATA_TOTAL_CHANNELS * 4 + 1] = {0xaa, 0x01};
    uint8_t *payload = raw_frame + 2;
    button_app_init();
    keyboard_app_init();
    touch_app_init();
    tenodata_config_get_payload(payload);
    CHECK(keyboard_app_set_keycode(11, 0x3a), "fixture assigns F1 to BTN11");

    queue(enable_io, 2);
    advance(1);
    CHECK(cdc_manager_get_mode() == CDC_MODE_AUTO_RAW, "partial command preserves Raw");
    queue(enable_io + 2, 2);
    advance(3000);
    CHECK(configs_match(payload), "cold IO calibrates both chips with local scan parameters");
    CHECK(touch_pipeline_is_ready() && io_frame(0, 0), "both chips sampled through idle IO output");
    chips[0].touched = true;
    advance(100);
    CHECK(io_frame(0, e4), "PSoC 0 presses E4 on CDC0");
    chips[1].touched = true;
    advance(100);
    CHECK(io_frame(0, e4 | e8), "PSoC 1 adds E8 on CDC0");
    chips[0].touched = false;
    advance(100);
    CHECK(io_frame(0, e8), "releasing E4 leaves E8 held");
    chips[1].touched = false;
    advance(100);
    CHECK(io_frame(0, 0), "releasing E8 clears the CDC0 touch bitmap");

    pa_high = 0xff;
    pb_low = BTN8_Pin | BTN9_Pin | BTN10_Pin | BTN11_Pin;
    advance(30);
    CHECK(io_frame(0x7ff, 0), "BTN0 through BTN10 reach IO and BTN11 is excluded");
    const uint8_t io_hid[14] = {0, 0, 0x3a};
    CHECK(!memcmp(hid, io_hid, sizeof(hid)), "only BTN11 is present in the IO-mode HID report");
    queue(disable_io, sizeof(disable_io));
    advance(1800);
    CHECK(cdc_manager_get_mode() == CDC_MODE_AUTO_RAW && last_cdc_len == 70 && last_cdc[0] == 1,
          "leaving cold IO restores auto Raw and its host-config request");
    const uint8_t full_hid[14] = {0,0,0x1a,0x08,0x07,0x06,0x1b,0x1d,0x04,0x14,0x20,0x26,0x25,0x3a};
    CHECK(!memcmp(hid, full_hid, sizeof(hid)), "all twelve keycodes return to HID");
    pa_high = pb_low = 0;

    /* The opaque parameter payload contains a complete IO command, and its
     * checksum is itself 0xa6. Only the surrounding stream may switch routes. */
    memcpy(payload + 34, enable_io, sizeof(enable_io));
    payload[68] += (uint8_t)(0xa6 - checksum(raw_frame, sizeof(raw_frame) - 1));
    raw_frame[sizeof(raw_frame) - 1] = 0xa6;
    queue(raw_frame, 40);
    advance(3);
    queue(raw_frame + 40, sizeof(raw_frame) - 40);
    advance(1800);
    CHECK(cdc_manager_get_mode() == CDC_MODE_AUTO_RAW && configs_match(payload) &&
          last_cdc_len == 70 && last_cdc[0] == 0 && checksum(last_cdc, 69) == last_cdc[69],
          "fragmented binary parameters reach both chips and Raw emits valid samples");
    unsigned before = chips[0].config_writes;
    queue(enable_io, sizeof(enable_io));
    advance(1800);
    CHECK(chips[0].config_writes > before && configs_match(payload) &&
          touch_pipeline_is_ready() && io_frame(0, 0),
          "IO reuses the accepted Raw scan parameters and completes warm-up");
    chips[0].touched = true;
    advance(100);
    CHECK(io_frame(0, e4), "cached Raw to IO reports a fresh E4 press");
    chips[0].touched = false;
    advance(100);
    CHECK(io_frame(0, 0), "cached Raw to IO reports the E4 release");
    queue(disable_io, sizeof(disable_io));
    payload[34] = 50;
    raw_frame[sizeof(raw_frame) - 1] = checksum(raw_frame, sizeof(raw_frame) - 1);
    queue(raw_frame, sizeof(raw_frame));
    advance(1800);
    CHECK(cdc_manager_get_mode() == CDC_MODE_AUTO_RAW && configs_match(payload),
          "a Raw frame adjacent to IO disable reaches its restored receiver");

    queue((const uint8_t *)"{STAT}", 6);
    advance(2000);
    CHECK(cdc_manager_get_mode() == CDC_MODE_AUTO_MAI2TOUCH && last_cdc_len == 9 &&
          last_cdc[0] == '(' && last_cdc[8] == ')', "auto Mai2Touch streams after STAT");
    chips[1].touched = true;
    advance(100);
    CHECK(last_cdc_len == 9 && last_cdc[7] == 8, "Mai2Touch reports E8 in its own wire format");
    chips[1].touched = false;
    advance(100);
    queue((const uint8_t *)"{HALT}", 6);
    queue(sens, sizeof(sens));
    advance(2);
    CHECK(saw_echo(), "Mai2Touch echoes sensitivity value 0xa6");
    const uint8_t bad[] = {0xa6, 2, 1, 0xa8};
    queue(bad, sizeof(bad));
    advance(1);
    CHECK(cdc_manager_get_mode() == CDC_MODE_AUTO_MAI2TOUCH, "bad IO checksum preserves Mai2Touch");
    queue(enable_io, 3);
    advance(1);
    queue(enable_io + 3, 1);
    advance(4);
    CHECK(cdc_manager_get_mode() == CDC_MODE_IO && io_frame(0, 0), "split IO command works in Mai2Touch");
    queue(disable_io, sizeof(disable_io));
    queue(sens, sizeof(sens));
    advance(3);
    CHECK(cdc_manager_get_mode() == CDC_MODE_AUTO_MAI2TOUCH && saw_echo(),
          "IO disable restores auto Mai2Touch and dispatches the adjacent command");

    queue((const uint8_t *)"{STAT}", 6);
    advance(5);
    queue(enable_io, sizeof(enable_io));
    advance(8);
    CHECK(io_frame(0, 0), "IO takes over CDC0 while Mai2Touch is streaming");
    queue(disable_io, sizeof(disable_io));
    advance(8);
    CHECK(last_cdc_len == 9 && last_cdc[0] == '(' && last_cdc[8] == ')',
          "the restored Mai2Touch stream resumes without another STAT");

    for (unsigned mai = 0; mai <= 1; mai++) {
        touch_app_force_cdc_mode(mai != 0);
        queue(enable_io, sizeof(enable_io));
        queue(enable_io, sizeof(enable_io));
        queue(disable_io, sizeof(disable_io));
        advance(5);
        CHECK(!rx_count && cdc_manager_get_mode() == (mai ? CDC_MODE_FORCED_MAI2TOUCH : CDC_MODE_FORCED_RAW),
              "adjacent IO commands restore the original forced route");
    }
    return 0;
}
