#include "mai2touch_app.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "i2c.h"
#include "tusb.h"

#define MAI2TOUCH_CDC_ITF                  0U
#define MAI2TOUCH_DEVICE_COUNT             2U
#define MAI2TOUCH_I2C_REGISTER             0x00U
#define MAI2TOUCH_I2C_DATA_LENGTH          35U
#define MAI2TOUCH_DEVICE_PAYLOAD_LENGTH    34U
#define MAI2TOUCH_CDC_FRAME_LENGTH         69U
#define MAI2TOUCH_CONNECTED_POLL_MS         5U
#define MAI2TOUCH_DISCONNECTED_POLL_MS    500U
#define MAI2TOUCH_CDC_PERIOD_MS             5U
#define MAI2TOUCH_TRANSFER_TIMEOUT_MS      50U
#define MAI2TOUCH_READY_TRIALS              3U
#define MAI2TOUCH_READY_TIMEOUT_MS         10U
#define MAI2TOUCH_FAILURE_LIMIT             4U
#define MAI2TOUCH_NO_ACTIVE_DEVICE        0xFFU

typedef enum
{
    MAI2TOUCH_TRANSFER_IDLE,
    MAI2TOUCH_TRANSFER_TX_REGISTER,
    MAI2TOUCH_TRANSFER_RX_RAW
} mai2touch_transfer_state_t;

typedef enum
{
    MAI2TOUCH_EVENT_NONE,
    MAI2TOUCH_EVENT_COMPLETE,
    MAI2TOUCH_EVENT_ERROR
} mai2touch_transfer_event_t;

typedef struct
{
    uint8_t address;
    uint8_t raw[MAI2TOUCH_I2C_DATA_LENGTH];
    uint8_t rx_raw[MAI2TOUCH_I2C_DATA_LENGTH];
    uint32_t next_poll_tick;
    uint8_t failure_count;
    bool connected;
} mai2touch_device_t;

static mai2touch_device_t mai2touch_devices[MAI2TOUCH_DEVICE_COUNT];
static uint8_t mai2touch_cdc_frame[MAI2TOUCH_CDC_FRAME_LENGTH];
static uint32_t mai2touch_next_cdc_tick;
static uint32_t mai2touch_transfer_deadline;
static uint8_t mai2touch_register_address;
static uint8_t mai2touch_active_device;
static uint8_t mai2touch_next_device;
static volatile mai2touch_transfer_state_t mai2touch_transfer_state;
static volatile mai2touch_transfer_event_t mai2touch_transfer_event;

static bool mai2touch_tick_due(uint32_t now, uint32_t due);
static void mai2touch_recover_i2c(void);
static bool mai2touch_device_ready(mai2touch_device_t *device);
static bool mai2touch_start_read(uint8_t device_index, uint32_t now);
static void mai2touch_schedule_read(uint32_t now);
static void mai2touch_service_transfer(uint32_t now);
static void mai2touch_finish_transfer(bool success, uint32_t now);
static void mai2touch_record_failure(mai2touch_device_t *device,
                                     uint32_t now);
static void mai2touch_send_cdc_frame(uint32_t now);

void mai2touch_app_init(void)
{
    uint32_t now = HAL_GetTick();
    uint8_t index;

    memset(mai2touch_devices, 0, sizeof(mai2touch_devices));
    memset(mai2touch_cdc_frame, 0, sizeof(mai2touch_cdc_frame));

    mai2touch_devices[0].address = 0x08U;
    mai2touch_devices[1].address = 0x09U;
    mai2touch_next_cdc_tick = now;
    mai2touch_transfer_deadline = 0U;
    mai2touch_register_address = MAI2TOUCH_I2C_REGISTER;
    mai2touch_active_device = MAI2TOUCH_NO_ACTIVE_DEVICE;
    mai2touch_next_device = 0U;
    mai2touch_transfer_state = MAI2TOUCH_TRANSFER_IDLE;
    mai2touch_transfer_event = MAI2TOUCH_EVENT_NONE;

    if ((HAL_I2C_GetState(&hi2c1) != HAL_I2C_STATE_READY) ||
        (__HAL_I2C_GET_FLAG(&hi2c1, I2C_FLAG_BUSY) != RESET))
    {
        mai2touch_recover_i2c();
    }

    for (index = 0U; index < MAI2TOUCH_DEVICE_COUNT; index++)
    {
        mai2touch_device_t *device = &mai2touch_devices[index];

        if (mai2touch_device_ready(device))
        {
            device->connected = true;
            device->failure_count = 0U;
            device->next_poll_tick = now;
        }
        else
        {
            device->connected = false;
            device->failure_count = MAI2TOUCH_FAILURE_LIMIT;
            device->next_poll_tick =
                now + MAI2TOUCH_DISCONNECTED_POLL_MS;
        }
    }
}

void mai2touch_app_task(void)
{
    uint32_t now = HAL_GetTick();

    mai2touch_send_cdc_frame(now);
    mai2touch_service_transfer(now);
    mai2touch_schedule_read(now);
}

void HAL_I2C_MasterTxCpltCallback(I2C_HandleTypeDef *hi2c)
{
    mai2touch_device_t *device;

    if ((hi2c->Instance != I2C1) ||
        (mai2touch_transfer_state !=
         MAI2TOUCH_TRANSFER_TX_REGISTER) ||
        (mai2touch_active_device >= MAI2TOUCH_DEVICE_COUNT))
    {
        return;
    }

    device = &mai2touch_devices[mai2touch_active_device];
    mai2touch_transfer_state = MAI2TOUCH_TRANSFER_RX_RAW;

    if (HAL_I2C_Master_Receive_IT(
            &hi2c1,
            (uint16_t)(device->address << 1U),
            device->rx_raw,
            MAI2TOUCH_I2C_DATA_LENGTH) != HAL_OK)
    {
        mai2touch_transfer_event = MAI2TOUCH_EVENT_ERROR;
    }
}

void HAL_I2C_MasterRxCpltCallback(I2C_HandleTypeDef *hi2c)
{
    if ((hi2c->Instance == I2C1) &&
        (mai2touch_transfer_state == MAI2TOUCH_TRANSFER_RX_RAW))
    {
        mai2touch_transfer_event = MAI2TOUCH_EVENT_COMPLETE;
    }
}

void HAL_I2C_ErrorCallback(I2C_HandleTypeDef *hi2c)
{
    if ((hi2c->Instance == I2C1) &&
        (mai2touch_transfer_state != MAI2TOUCH_TRANSFER_IDLE))
    {
        mai2touch_transfer_event = MAI2TOUCH_EVENT_ERROR;
    }
}

static bool mai2touch_tick_due(uint32_t now, uint32_t due)
{
    return (int32_t)(now - due) >= 0;
}

static void mai2touch_recover_i2c(void)
{
    (void)HAL_I2C_DeInit(&hi2c1);
    __HAL_RCC_I2C1_FORCE_RESET();
    __NOP();
    __HAL_RCC_I2C1_RELEASE_RESET();
    MX_I2C1_Init();
}

static bool mai2touch_device_ready(mai2touch_device_t *device)
{
    return HAL_I2C_IsDeviceReady(
               &hi2c1,
               (uint16_t)(device->address << 1U),
               MAI2TOUCH_READY_TRIALS,
               MAI2TOUCH_READY_TIMEOUT_MS) == HAL_OK;
}

static bool mai2touch_start_read(uint8_t device_index, uint32_t now)
{
    mai2touch_device_t *device = &mai2touch_devices[device_index];

    if ((HAL_I2C_GetState(&hi2c1) != HAL_I2C_STATE_READY) ||
        (__HAL_I2C_GET_FLAG(&hi2c1, I2C_FLAG_BUSY) != RESET))
    {
        return false;
    }

    if (!device->connected && !mai2touch_device_ready(device))
    {
        return false;
    }

    mai2touch_active_device = device_index;
    mai2touch_transfer_state = MAI2TOUCH_TRANSFER_TX_REGISTER;
    mai2touch_transfer_event = MAI2TOUCH_EVENT_NONE;
    mai2touch_transfer_deadline =
        now + MAI2TOUCH_TRANSFER_TIMEOUT_MS;

    if (HAL_I2C_Master_Transmit_IT(
            &hi2c1,
            (uint16_t)(device->address << 1U),
            &mai2touch_register_address,
            1U) != HAL_OK)
    {
        mai2touch_active_device = MAI2TOUCH_NO_ACTIVE_DEVICE;
        mai2touch_transfer_state = MAI2TOUCH_TRANSFER_IDLE;
        return false;
    }

    return true;
}

static void mai2touch_schedule_read(uint32_t now)
{
    uint8_t offset;

    if (mai2touch_transfer_state != MAI2TOUCH_TRANSFER_IDLE)
    {
        return;
    }

    for (offset = 0U; offset < MAI2TOUCH_DEVICE_COUNT; offset++)
    {
        uint8_t device_index =
            (uint8_t)((mai2touch_next_device + offset) %
                      MAI2TOUCH_DEVICE_COUNT);
        mai2touch_device_t *device =
            &mai2touch_devices[device_index];

        if (!mai2touch_tick_due(now, device->next_poll_tick))
        {
            continue;
        }

        mai2touch_next_device =
            (uint8_t)((device_index + 1U) %
                      MAI2TOUCH_DEVICE_COUNT);

        if (!mai2touch_start_read(device_index, now))
        {
            if (HAL_I2C_GetState(&hi2c1) ==
                HAL_I2C_STATE_READY)
            {
                mai2touch_record_failure(device, now);
            }
        }
        return;
    }
}

static void mai2touch_service_transfer(uint32_t now)
{
    mai2touch_transfer_event_t event;
    uint32_t primask;

    if (mai2touch_transfer_state == MAI2TOUCH_TRANSFER_IDLE)
    {
        return;
    }

    primask = __get_PRIMASK();
    __disable_irq();
    event = mai2touch_transfer_event;
    mai2touch_transfer_event = MAI2TOUCH_EVENT_NONE;
    if (primask == 0U)
    {
        __enable_irq();
    }

    if (event == MAI2TOUCH_EVENT_COMPLETE)
    {
        mai2touch_finish_transfer(true, now);
    }
    else if (event == MAI2TOUCH_EVENT_ERROR)
    {
        mai2touch_finish_transfer(false, now);
    }
    else if (mai2touch_tick_due(now, mai2touch_transfer_deadline))
    {
        uint8_t timed_out_device;

        primask = __get_PRIMASK();
        __disable_irq();
        if (mai2touch_transfer_event != MAI2TOUCH_EVENT_NONE)
        {
            if (primask == 0U)
            {
                __enable_irq();
            }
            return;
        }
        timed_out_device = mai2touch_active_device;
        mai2touch_active_device = MAI2TOUCH_NO_ACTIVE_DEVICE;
        mai2touch_transfer_state = MAI2TOUCH_TRANSFER_IDLE;
        if (primask == 0U)
        {
            __enable_irq();
        }

        mai2touch_recover_i2c();

        if (timed_out_device < MAI2TOUCH_DEVICE_COUNT)
        {
            mai2touch_record_failure(
                &mai2touch_devices[timed_out_device],
                now);
        }
    }
}

static void mai2touch_finish_transfer(bool success, uint32_t now)
{
    uint8_t device_index = mai2touch_active_device;
    mai2touch_device_t *device;

    mai2touch_active_device = MAI2TOUCH_NO_ACTIVE_DEVICE;
    mai2touch_transfer_state = MAI2TOUCH_TRANSFER_IDLE;
    mai2touch_transfer_event = MAI2TOUCH_EVENT_NONE;

    if (device_index >= MAI2TOUCH_DEVICE_COUNT)
    {
        return;
    }

    device = &mai2touch_devices[device_index];
    if (success)
    {
        memcpy(device->raw,
               device->rx_raw,
               sizeof(device->raw));
        device->connected = true;
        device->failure_count = 0U;
        device->next_poll_tick =
            now + MAI2TOUCH_CONNECTED_POLL_MS;
    }
    else
    {
        mai2touch_record_failure(device, now);
    }
}

static void mai2touch_record_failure(mai2touch_device_t *device,
                                     uint32_t now)
{
    if (device->failure_count < MAI2TOUCH_FAILURE_LIMIT)
    {
        device->failure_count++;
    }

    if (device->failure_count >= MAI2TOUCH_FAILURE_LIMIT)
    {
        device->connected = false;
        device->failure_count = MAI2TOUCH_FAILURE_LIMIT;
        memset(device->raw, 0, sizeof(device->raw));
        device->next_poll_tick =
            now + MAI2TOUCH_DISCONNECTED_POLL_MS;
    }
    else
    {
        device->next_poll_tick =
            now + MAI2TOUCH_CONNECTED_POLL_MS;
    }
}

static void mai2touch_send_cdc_frame(uint32_t now)
{
    uint8_t index;

    if (!mai2touch_tick_due(now, mai2touch_next_cdc_tick))
    {
        return;
    }
    mai2touch_next_cdc_tick = now + MAI2TOUCH_CDC_PERIOD_MS;

    mai2touch_cdc_frame[0] = 0x00U;
    for (index = 0U; index < MAI2TOUCH_DEVICE_COUNT; index++)
    {
        uint8_t *destination =
            &mai2touch_cdc_frame[
                1U + index * MAI2TOUCH_DEVICE_PAYLOAD_LENGTH];

        if (mai2touch_devices[index].connected)
        {
            memcpy(destination,
                   &mai2touch_devices[index].raw[1],
                   MAI2TOUCH_DEVICE_PAYLOAD_LENGTH);
        }
        else
        {
            memset(destination, 0, MAI2TOUCH_DEVICE_PAYLOAD_LENGTH);
        }
    }

    if (!tud_cdc_n_ready(MAI2TOUCH_CDC_ITF) ||
        (tud_cdc_n_write_available(MAI2TOUCH_CDC_ITF) <
         MAI2TOUCH_CDC_FRAME_LENGTH))
    {
        return;
    }

    if (tud_cdc_n_write(MAI2TOUCH_CDC_ITF,
                        mai2touch_cdc_frame,
                        MAI2TOUCH_CDC_FRAME_LENGTH) ==
        MAI2TOUCH_CDC_FRAME_LENGTH)
    {
        tud_cdc_n_write_flush(MAI2TOUCH_CDC_ITF);
    }
}
