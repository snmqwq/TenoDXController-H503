#include "mai2touch_app.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "i2c.h"
#include "tusb.h"

#define MAI2TOUCH_CDC_ITF                 0U
#define MAI2TOUCH_PROBE_ADDRESS           0x08U
#define MAI2TOUCH_RAW_ADDRESS             0x09U
#define MAI2TOUCH_I2C_REGISTER            0x00U
#define MAI2TOUCH_I2C_DATA_LENGTH         35U
#define MAI2TOUCH_POLL_PERIOD_MS          10U
#define MAI2TOUCH_DISCONNECTED_PERIOD_MS  500U
#define MAI2TOUCH_TRANSFER_TIMEOUT_MS     50U
#define MAI2TOUCH_READY_TRIALS            3U
#define MAI2TOUCH_READY_TIMEOUT_MS        10U
#define MAI2TOUCH_FAILURE_LIMIT           4U

static uint8_t mai2touch_raw[MAI2TOUCH_I2C_DATA_LENGTH];
static uint32_t mai2touch_next_poll_tick;
static uint32_t mai2touch_probe_error;
static uint8_t mai2touch_failure_count;
static bool mai2touch_probe_connected;
static bool mai2touch_raw_connected;
static bool mai2touch_raw_pending;

static void mai2touch_recover_i2c(void);
static bool mai2touch_read_raw(void);
static void mai2touch_record_failure(void);
static void mai2touch_send_raw(void);
static bool mai2touch_tick_due(uint32_t now, uint32_t due);

void mai2touch_app_init(void)
{
    HAL_StatusTypeDef result;

    memset(mai2touch_raw, 0, sizeof(mai2touch_raw));
    mai2touch_next_poll_tick = HAL_GetTick();
    mai2touch_probe_error = HAL_I2C_ERROR_NONE;
    mai2touch_failure_count = 0U;
    mai2touch_probe_connected = false;
    mai2touch_raw_connected = false;
    mai2touch_raw_pending = false;

    if ((HAL_I2C_GetState(&hi2c1) != HAL_I2C_STATE_READY) ||
        (__HAL_I2C_GET_FLAG(&hi2c1, I2C_FLAG_BUSY) != RESET))
    {
        mai2touch_recover_i2c();
    }

    result = HAL_I2C_IsDeviceReady(
        &hi2c1,
        (uint16_t)(MAI2TOUCH_PROBE_ADDRESS << 1U),
        MAI2TOUCH_READY_TRIALS,
        MAI2TOUCH_READY_TIMEOUT_MS);
    mai2touch_probe_connected = (result == HAL_OK);
    mai2touch_probe_error = HAL_I2C_GetError(&hi2c1);
}

void mai2touch_app_task(void)
{
    uint32_t now = HAL_GetTick();

    mai2touch_send_raw();

    if (mai2touch_raw_pending ||
        !mai2touch_tick_due(now, mai2touch_next_poll_tick))
    {
        return;
    }

    if (mai2touch_read_raw())
    {
        mai2touch_raw_connected = true;
        mai2touch_failure_count = 0U;
        mai2touch_raw_pending = true;
        mai2touch_next_poll_tick = HAL_GetTick() + MAI2TOUCH_POLL_PERIOD_MS;
    }
    else
    {
        mai2touch_record_failure();
    }
}

static bool mai2touch_read_raw(void)
{
    HAL_StatusTypeDef result;
    uint8_t register_address = MAI2TOUCH_I2C_REGISTER;
    uint16_t device_address =
        (uint16_t)(MAI2TOUCH_RAW_ADDRESS << 1U);

    if ((HAL_I2C_GetState(&hi2c1) != HAL_I2C_STATE_READY) ||
        (__HAL_I2C_GET_FLAG(&hi2c1, I2C_FLAG_BUSY) != RESET))
    {
        mai2touch_recover_i2c();
    }

    if (!mai2touch_raw_connected)
    {
        result = HAL_I2C_IsDeviceReady(&hi2c1,
                                       device_address,
                                       MAI2TOUCH_READY_TRIALS,
                                       MAI2TOUCH_READY_TIMEOUT_MS);
        if (result != HAL_OK)
        {
            return false;
        }
    }

    result = HAL_I2C_Master_Transmit(&hi2c1,
                                     device_address,
                                     &register_address,
                                     1U,
                                     MAI2TOUCH_TRANSFER_TIMEOUT_MS);
    if (result != HAL_OK)
    {
        return false;
    }

    result = HAL_I2C_Master_Receive(&hi2c1,
                                    device_address,
                                    mai2touch_raw,
                                    MAI2TOUCH_I2C_DATA_LENGTH,
                                    MAI2TOUCH_TRANSFER_TIMEOUT_MS);
    return result == HAL_OK;
}

static void mai2touch_record_failure(void)
{
    uint32_t now = HAL_GetTick();

    if (mai2touch_failure_count < MAI2TOUCH_FAILURE_LIMIT)
    {
        mai2touch_failure_count++;
    }

    if (mai2touch_failure_count >= MAI2TOUCH_FAILURE_LIMIT)
    {
        mai2touch_raw_connected = false;
        mai2touch_failure_count = MAI2TOUCH_FAILURE_LIMIT;
        mai2touch_next_poll_tick =
            now + MAI2TOUCH_DISCONNECTED_PERIOD_MS;
    }
    else
    {
        mai2touch_next_poll_tick = now + MAI2TOUCH_POLL_PERIOD_MS;
    }
}

static void mai2touch_send_raw(void)
{
    if (!mai2touch_raw_pending ||
        !tud_cdc_n_connected(MAI2TOUCH_CDC_ITF) ||
        (tud_cdc_n_write_available(MAI2TOUCH_CDC_ITF) <
         MAI2TOUCH_I2C_DATA_LENGTH))
    {
        return;
    }

    if (tud_cdc_n_write(MAI2TOUCH_CDC_ITF,
                        mai2touch_raw,
                        MAI2TOUCH_I2C_DATA_LENGTH) ==
        MAI2TOUCH_I2C_DATA_LENGTH)
    {
        tud_cdc_n_write_flush(MAI2TOUCH_CDC_ITF);
        mai2touch_raw_pending = false;
    }
}

static void mai2touch_recover_i2c(void)
{
    (void)HAL_I2C_DeInit(&hi2c1);
    __HAL_RCC_I2C1_FORCE_RESET();
    __NOP();
    __HAL_RCC_I2C1_RELEASE_RESET();
    MX_I2C1_Init();
}

static bool mai2touch_tick_due(uint32_t now, uint32_t due)
{
    return (int32_t)(now - due) >= 0;
}
