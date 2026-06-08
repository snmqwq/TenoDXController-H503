#include "mai2touch_app.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "i2c.h"
#include "tusb.h"

#define MAI2TOUCH_CDC_ITF                 0U
#define MAI2TOUCH_DEVICE_COUNT            2U
#define MAI2TOUCH_CHANNEL_COUNT           17U
#define MAI2TOUCH_ZONE_COUNT              34U
#define MAI2TOUCH_I2C_REGISTER            0x00U
#define MAI2TOUCH_I2C_DATA_LENGTH         35U
#define MAI2TOUCH_I2C_VALID_STATUS        0x01U
#define MAI2TOUCH_POLL_PERIOD_MS          10U
#define MAI2TOUCH_DISCONNECTED_PERIOD_MS  500U
#define MAI2TOUCH_TRANSFER_TIMEOUT_MS     20U
#define MAI2TOUCH_FAILURE_LIMIT           4U
#define MAI2TOUCH_SETUP_SAMPLE_COUNT      30U
#define MAI2TOUCH_HISTORY_LENGTH          16U
#define MAI2TOUCH_REPORT_PERIOD_MS        10U
#define MAI2TOUCH_REPORT_LENGTH           9U
#define MAI2TOUCH_COMMAND_MAX_LENGTH      8U
#define MAI2TOUCH_RESPONSE_MAX_LENGTH     10U
#define MAI2TOUCH_ERROR_TIMEOUT           0x80000000UL

typedef enum
{
    MAI2TOUCH_REGION_A = 0,
    MAI2TOUCH_REGION_B,
    MAI2TOUCH_REGION_C,
    MAI2TOUCH_REGION_D,
    MAI2TOUCH_REGION_E
} mai2touch_region_t;

typedef enum
{
    MAI2TOUCH_I2C_EVENT_NONE = 0,
    MAI2TOUCH_I2C_EVENT_COMPLETE,
    MAI2TOUCH_I2C_EVENT_ERROR
} mai2touch_i2c_event_t;

typedef struct
{
    bool is_pressed;
    bool lock_releasing;
    int8_t direction;
    int8_t deriv_down_count;
} mai2touch_detector_t;

typedef struct
{
    uint16_t setup_raw[MAI2TOUCH_CHANNEL_COUNT];
    uint32_t setup_sum[MAI2TOUCH_CHANNEL_COUNT];
    uint16_t history[MAI2TOUCH_CHANNEL_COUNT][MAI2TOUCH_HISTORY_LENGTH];
    mai2touch_detector_t detector[MAI2TOUCH_CHANNEL_COUNT];
    uint32_t next_poll_tick;
    uint8_t setup_count;
    uint8_t failure_count;
    bool connected;
    bool setup_complete;
} mai2touch_device_t;

static const uint8_t mai2touch_i2c_addresses[MAI2TOUCH_DEVICE_COUNT] =
{
    0x08U,
    0x09U
};

/* Zone bits are ordered A1-A8, B1-B8, C1-C2, D1-D8, E1-E8. */
static const uint8_t mai2touch_zone_map[MAI2TOUCH_DEVICE_COUNT][MAI2TOUCH_CHANNEL_COUNT] =
{
    {
        12U, 23U, 31U, 5U, 13U, 24U, 32U, 17U, 6U,
        14U, 25U, 33U, 7U, 15U, 18U, 26U, 0U
    },
    {
        4U, 30U, 22U, 11U, 3U, 29U, 21U, 10U, 2U,
        16U, 28U, 20U, 9U, 1U, 27U, 19U, 8U
    }
};

static mai2touch_device_t mai2touch_devices[MAI2TOUCH_DEVICE_COUNT];
static uint8_t mai2touch_i2c_data[MAI2TOUCH_I2C_DATA_LENGTH];
static uint8_t mai2touch_active_device;
static uint8_t mai2touch_schedule_cursor;
static uint32_t mai2touch_transfer_start_tick;
static uint32_t mai2touch_next_report_tick;
static volatile bool mai2touch_transfer_active;
static volatile mai2touch_i2c_event_t mai2touch_i2c_event;
static volatile uint32_t mai2touch_i2c_error;

static uint8_t mai2touch_command[MAI2TOUCH_COMMAND_MAX_LENGTH];
static uint8_t mai2touch_command_length;
static bool mai2touch_command_receiving;
static bool mai2touch_conditioning;

static uint8_t mai2touch_response[MAI2TOUCH_RESPONSE_MAX_LENGTH];
static uint8_t mai2touch_response_length;
static bool mai2touch_response_pending;

static void mai2touch_reset_device_setup(uint8_t device_index);
static void mai2touch_reset_all_setup(void);
static void mai2touch_process_i2c(void);
static bool mai2touch_start_due_read(void);
static void mai2touch_start_read(uint8_t device_index);
static void mai2touch_finish_success(void);
static void mai2touch_finish_failure(uint32_t error);
static void mai2touch_recover_i2c(void);
static void mai2touch_process_raw_data(uint8_t device_index);
static bool mai2touch_detect(mai2touch_region_t region,
                             mai2touch_detector_t *detector,
                             uint16_t const *history,
                             int32_t diff,
                             int32_t deriv,
                             uint16_t setup_raw);
static mai2touch_region_t mai2touch_region_from_zone(uint8_t zone);
static void mai2touch_shift_history(uint16_t *history, uint16_t raw);
static void mai2touch_process_commands(void);
static void mai2touch_handle_command(uint8_t const *command, uint8_t length);
static void mai2touch_queue_echo(uint8_t const *command, uint8_t length);
static void mai2touch_send_response(void);
static void mai2touch_send_report(void);
static uint64_t mai2touch_build_zone_mask(void);
static bool mai2touch_tick_due(uint32_t now, uint32_t due);
static uint32_t mai2touch_next_periodic_tick(uint32_t previous,
                                             uint32_t now,
                                             uint32_t period);

void mai2touch_app_init(void)
{
    uint32_t now = HAL_GetTick();

    memset(mai2touch_devices, 0, sizeof(mai2touch_devices));
    memset(mai2touch_i2c_data, 0, sizeof(mai2touch_i2c_data));
    memset(mai2touch_command, 0, sizeof(mai2touch_command));
    memset(mai2touch_response, 0, sizeof(mai2touch_response));

    for (uint8_t device = 0U; device < MAI2TOUCH_DEVICE_COUNT; device++)
    {
        mai2touch_devices[device].next_poll_tick = now;
    }

    mai2touch_active_device = 0U;
    mai2touch_schedule_cursor = 0U;
    mai2touch_transfer_start_tick = 0U;
    mai2touch_next_report_tick = now;
    mai2touch_transfer_active = false;
    mai2touch_i2c_event = MAI2TOUCH_I2C_EVENT_NONE;
    mai2touch_i2c_error = HAL_I2C_ERROR_NONE;
    mai2touch_command_length = 0U;
    mai2touch_command_receiving = false;
    mai2touch_conditioning = true;
    mai2touch_response_length = 0U;
    mai2touch_response_pending = false;
}

void mai2touch_app_task(void)
{
    mai2touch_process_i2c();
    mai2touch_process_commands();
    mai2touch_send_response();
    mai2touch_send_report();

    if (!mai2touch_transfer_active)
    {
        (void)mai2touch_start_due_read();
    }
}

static void mai2touch_reset_device_setup(uint8_t device_index)
{
    mai2touch_device_t *device = &mai2touch_devices[device_index];

    memset(device->setup_raw, 0, sizeof(device->setup_raw));
    memset(device->setup_sum, 0, sizeof(device->setup_sum));
    memset(device->history, 0, sizeof(device->history));
    memset(device->detector, 0, sizeof(device->detector));

    for (uint8_t channel = 0U; channel < MAI2TOUCH_CHANNEL_COUNT; channel++)
    {
        device->detector[channel].deriv_down_count = -1;
    }

    device->setup_count = 0U;
    device->setup_complete = false;
}

static void mai2touch_reset_all_setup(void)
{
    for (uint8_t device = 0U; device < MAI2TOUCH_DEVICE_COUNT; device++)
    {
        mai2touch_reset_device_setup(device);
    }
}

static void mai2touch_process_i2c(void)
{
    mai2touch_i2c_event_t event;
    uint32_t error;
    bool timed_out = false;

    if (!mai2touch_transfer_active)
    {
        return;
    }

    __disable_irq();
    event = mai2touch_i2c_event;
    error = mai2touch_i2c_error;
    mai2touch_i2c_event = MAI2TOUCH_I2C_EVENT_NONE;

    if (event != MAI2TOUCH_I2C_EVENT_NONE)
    {
        mai2touch_transfer_active = false;
    }
    else if ((uint32_t)(HAL_GetTick() - mai2touch_transfer_start_tick) >=
             MAI2TOUCH_TRANSFER_TIMEOUT_MS)
    {
        mai2touch_transfer_active = false;
        timed_out = true;
    }

    __enable_irq();

    if (event == MAI2TOUCH_I2C_EVENT_COMPLETE)
    {
        if (mai2touch_i2c_data[0] == MAI2TOUCH_I2C_VALID_STATUS)
        {
            mai2touch_finish_success();
        }
        else
        {
            mai2touch_finish_failure(HAL_I2C_ERROR_INVALID_PARAM);
        }
    }
    else if (event == MAI2TOUCH_I2C_EVENT_ERROR)
    {
        mai2touch_finish_failure(error);
    }
    else if (timed_out)
    {
        mai2touch_recover_i2c();
        mai2touch_finish_failure(MAI2TOUCH_ERROR_TIMEOUT);
    }
}

static bool mai2touch_start_due_read(void)
{
    uint32_t now = HAL_GetTick();

    for (uint8_t offset = 0U; offset < MAI2TOUCH_DEVICE_COUNT; offset++)
    {
        uint8_t device_index =
            (uint8_t)((mai2touch_schedule_cursor + offset) % MAI2TOUCH_DEVICE_COUNT);

        if (mai2touch_tick_due(now, mai2touch_devices[device_index].next_poll_tick))
        {
            mai2touch_schedule_cursor =
                (uint8_t)((device_index + 1U) % MAI2TOUCH_DEVICE_COUNT);
            mai2touch_start_read(device_index);
            return true;
        }
    }

    return false;
}

static void mai2touch_start_read(uint8_t device_index)
{
    HAL_StatusTypeDef result;

    if ((HAL_I2C_GetState(&hi2c1) != HAL_I2C_STATE_READY) ||
        (__HAL_I2C_GET_FLAG(&hi2c1, I2C_FLAG_BUSY) != RESET))
    {
        mai2touch_recover_i2c();
    }

    mai2touch_active_device = device_index;
    memset(mai2touch_i2c_data, 0, sizeof(mai2touch_i2c_data));
    mai2touch_i2c_event = MAI2TOUCH_I2C_EVENT_NONE;
    mai2touch_i2c_error = HAL_I2C_ERROR_NONE;
    mai2touch_transfer_start_tick = HAL_GetTick();
    mai2touch_transfer_active = true;

    result = HAL_I2C_Mem_Read_IT(&hi2c1,
                                 (uint16_t)(mai2touch_i2c_addresses[device_index] << 1U),
                                 MAI2TOUCH_I2C_REGISTER,
                                 I2C_MEMADD_SIZE_8BIT,
                                 mai2touch_i2c_data,
                                 MAI2TOUCH_I2C_DATA_LENGTH);

    if (result != HAL_OK)
    {
        mai2touch_transfer_active = false;
        mai2touch_recover_i2c();
        mai2touch_finish_failure((uint32_t)result);
    }
}

static void mai2touch_finish_success(void)
{
    mai2touch_device_t *device = &mai2touch_devices[mai2touch_active_device];
    uint32_t now = HAL_GetTick();

    if (!device->connected)
    {
        device->connected = true;
        mai2touch_reset_device_setup(mai2touch_active_device);
    }

    device->failure_count = 0U;
    mai2touch_process_raw_data(mai2touch_active_device);
    device->next_poll_tick =
        mai2touch_next_periodic_tick(device->next_poll_tick,
                                     now,
                                     MAI2TOUCH_POLL_PERIOD_MS);
}

static void mai2touch_finish_failure(uint32_t error)
{
    mai2touch_device_t *device = &mai2touch_devices[mai2touch_active_device];
    uint32_t now = HAL_GetTick();

    (void)error;

    if (device->failure_count < MAI2TOUCH_FAILURE_LIMIT)
    {
        device->failure_count++;
    }

    if (device->failure_count >= MAI2TOUCH_FAILURE_LIMIT)
    {
        device->connected = false;
        mai2touch_reset_device_setup(mai2touch_active_device);
        device->failure_count = MAI2TOUCH_FAILURE_LIMIT;
        device->next_poll_tick = now + MAI2TOUCH_DISCONNECTED_PERIOD_MS;
    }
    else
    {
        device->next_poll_tick =
            mai2touch_next_periodic_tick(device->next_poll_tick,
                                         now,
                                         MAI2TOUCH_POLL_PERIOD_MS);
    }
}

static void mai2touch_recover_i2c(void)
{
    (void)HAL_I2C_DeInit(&hi2c1);
    __HAL_RCC_I2C1_FORCE_RESET();
    __NOP();
    __HAL_RCC_I2C1_RELEASE_RESET();
    MX_I2C1_Init();
    mai2touch_i2c_event = MAI2TOUCH_I2C_EVENT_NONE;
    mai2touch_i2c_error = HAL_I2C_ERROR_NONE;
}

static void mai2touch_process_raw_data(uint8_t device_index)
{
    mai2touch_device_t *device = &mai2touch_devices[device_index];

    for (uint8_t channel = 0U; channel < MAI2TOUCH_CHANNEL_COUNT; channel++)
    {
        uint8_t raw_index = (uint8_t)(1U + (channel * 2U));
        uint16_t raw =
            (uint16_t)(((uint16_t)mai2touch_i2c_data[raw_index] << 8U) |
                       mai2touch_i2c_data[raw_index + 1U]);

        if (!device->setup_complete)
        {
            device->setup_sum[channel] += raw;
            mai2touch_shift_history(device->history[channel], raw);
        }
        else
        {
            uint8_t zone = mai2touch_zone_map[device_index][channel];
            int32_t diff = (int32_t)raw - (int32_t)device->setup_raw[channel];
            int32_t deriv =
                (int32_t)raw - (int32_t)device->history[channel][0];

            device->detector[channel].is_pressed =
                mai2touch_detect(mai2touch_region_from_zone(zone),
                                 &device->detector[channel],
                                 device->history[channel],
                                 diff,
                                 deriv,
                                 device->setup_raw[channel]);
            mai2touch_shift_history(device->history[channel], raw);
        }
    }

    if (!device->setup_complete)
    {
        device->setup_count++;

        if (device->setup_count >= MAI2TOUCH_SETUP_SAMPLE_COUNT)
        {
            for (uint8_t channel = 0U;
                 channel < MAI2TOUCH_CHANNEL_COUNT;
                 channel++)
            {
                device->setup_raw[channel] =
                    (uint16_t)((device->setup_sum[channel] +
                                (MAI2TOUCH_SETUP_SAMPLE_COUNT / 2U)) /
                               MAI2TOUCH_SETUP_SAMPLE_COUNT);
            }

            device->setup_complete = true;
        }
    }
}

static bool mai2touch_detect(mai2touch_region_t region,
                             mai2touch_detector_t *detector,
                             uint16_t const *history,
                             int32_t diff,
                             int32_t deriv,
                             uint16_t setup_raw)
{
    bool on = false;

    switch (region)
    {
        case MAI2TOUCH_REGION_C:
            if ((diff > 18) || (deriv > 7))
            {
                on = deriv >= -8;
            }
            else if (diff < 10)
            {
                on = false;
            }
            break;

        case MAI2TOUCH_REGION_E:
            if (diff > 24)
            {
                on = true;
            }
            if (deriv < -16)
            {
                on = false;
            }
            break;

        case MAI2TOUCH_REGION_B:
            if (diff > 4)
            {
                on = true;
            }
            if (deriv < -2)
            {
                on = false;
            }
            break;

        case MAI2TOUCH_REGION_D:
            if (diff > 3)
            {
                on = true;
            }
            if (deriv < -4)
            {
                on = false;
            }
            break;

        case MAI2TOUCH_REGION_A:
        {
            int32_t on_diff;
            int32_t last_diff;
            int32_t last3_diff;
            int32_t deriv_down = -250;

            on = detector->is_pressed;

            if ((diff > 950) || (diff < 150))
            {
                detector->direction = 0;
            }

            if (detector->direction > 0)
            {
                on_diff = 350;
            }
            else if (detector->direction < 0)
            {
                on_diff = 700;
            }
            else
            {
                on_diff = 550;
            }

            last_diff = (int32_t)history[0] - (int32_t)setup_raw;
            if ((last_diff < 550) && (diff >= 550))
            {
                detector->direction = 1;
            }
            else if ((last_diff >= 550) && (diff < 550))
            {
                detector->direction = -1;
            }

            if (diff < 200)
            {
                detector->lock_releasing = false;
            }

            if (detector->lock_releasing && (deriv > 150) && (diff > on_diff))
            {
                detector->lock_releasing = false;
            }

            if (diff > on_diff)
            {
                if (detector->deriv_down_count > 0)
                {
                    detector->deriv_down_count--;
                }
                else if (!detector->lock_releasing)
                {
                    if (detector->is_pressed || (deriv >= 15) || (diff >= 1000))
                    {
                        on = true;
                    }
                }
            }
            else
            {
                if (detector->deriv_down_count > 0)
                {
                    detector->deriv_down_count--;
                }

                if (detector->is_pressed && (diff > 200))
                {
                    detector->lock_releasing = true;
                }

                on = false;
            }

            last3_diff = (int32_t)history[2] - (int32_t)setup_raw;
            if (last3_diff > 2700)
            {
                deriv_down = -400;
            }

            if (deriv < deriv_down)
            {
                if (deriv < -800)
                {
                    if (diff < 1000)
                    {
                        on = false;
                        detector->deriv_down_count = 3;
                        if (diff > 500)
                        {
                            detector->lock_releasing = true;
                        }
                    }
                }
                else if (diff < 1200)
                {
                    on = false;
                    detector->deriv_down_count = 3;
                    if (diff > 200)
                    {
                        detector->lock_releasing = true;
                    }
                }
            }
            break;
        }

        default:
            break;
    }

    return on;
}

static mai2touch_region_t mai2touch_region_from_zone(uint8_t zone)
{
    if (zone < 8U)
    {
        return MAI2TOUCH_REGION_A;
    }
    if (zone < 16U)
    {
        return MAI2TOUCH_REGION_B;
    }
    if (zone < 18U)
    {
        return MAI2TOUCH_REGION_C;
    }
    if (zone < 26U)
    {
        return MAI2TOUCH_REGION_D;
    }

    return MAI2TOUCH_REGION_E;
}

static void mai2touch_shift_history(uint16_t *history, uint16_t raw)
{
    for (uint8_t index = MAI2TOUCH_HISTORY_LENGTH - 1U; index > 0U; index--)
    {
        history[index] = history[index - 1U];
    }

    history[0] = raw;
}

static void mai2touch_process_commands(void)
{
    while (tud_cdc_n_available(MAI2TOUCH_CDC_ITF) > 0U)
    {
        uint8_t data;

        if (tud_cdc_n_read(MAI2TOUCH_CDC_ITF, &data, 1U) != 1U)
        {
            break;
        }

        if (data == (uint8_t)'{')
        {
            mai2touch_command_length = 0U;
            mai2touch_command_receiving = true;
        }
        else if (mai2touch_command_receiving && (data == (uint8_t)'}'))
        {
            mai2touch_handle_command(mai2touch_command,
                                     mai2touch_command_length);
            mai2touch_command_length = 0U;
            mai2touch_command_receiving = false;
        }
        else if (mai2touch_command_receiving)
        {
            if (mai2touch_command_length < MAI2TOUCH_COMMAND_MAX_LENGTH)
            {
                mai2touch_command[mai2touch_command_length++] = data;
            }
            else
            {
                mai2touch_command_length = 0U;
                mai2touch_command_receiving = false;
            }
        }
    }
}

static void mai2touch_handle_command(uint8_t const *command, uint8_t length)
{
    if ((length == 4U) && (memcmp(command, "RSET", 4U) == 0))
    {
        mai2touch_reset_all_setup();
    }
    else if ((length == 4U) && (memcmp(command, "HALT", 4U) == 0))
    {
        mai2touch_conditioning = true;
    }
    else if ((length == 4U) && (memcmp(command, "STAT", 4U) == 0))
    {
        mai2touch_conditioning = false;
        mai2touch_next_report_tick = HAL_GetTick();
    }
    else if ((length == 4U) &&
             ((command[0] == (uint8_t)'L') || (command[0] == (uint8_t)'R')) &&
             ((command[2] == (uint8_t)'r') || (command[2] == (uint8_t)'k')))
    {
        mai2touch_queue_echo(command, length);
    }
}

static void mai2touch_queue_echo(uint8_t const *command, uint8_t length)
{
    if (mai2touch_response_pending ||
        ((uint8_t)(length + 2U) > MAI2TOUCH_RESPONSE_MAX_LENGTH))
    {
        return;
    }

    mai2touch_response[0] = (uint8_t)'(';
    memcpy(&mai2touch_response[1], command, length);
    mai2touch_response[length + 1U] = (uint8_t)')';
    mai2touch_response_length = (uint8_t)(length + 2U);
    mai2touch_response_pending = true;
}

static void mai2touch_send_response(void)
{
    if (!mai2touch_response_pending ||
        !tud_cdc_n_connected(MAI2TOUCH_CDC_ITF) ||
        (tud_cdc_n_write_available(MAI2TOUCH_CDC_ITF) <
         mai2touch_response_length))
    {
        return;
    }

    if (tud_cdc_n_write(MAI2TOUCH_CDC_ITF,
                        mai2touch_response,
                        mai2touch_response_length) == mai2touch_response_length)
    {
        mai2touch_response_pending = false;
        tud_cdc_n_write_flush(MAI2TOUCH_CDC_ITF);
    }
}

static void mai2touch_send_report(void)
{
    uint8_t frame[MAI2TOUCH_REPORT_LENGTH];
    uint64_t zone_mask;
    uint32_t now = HAL_GetTick();

    if (mai2touch_conditioning ||
        mai2touch_response_pending ||
        !mai2touch_tick_due(now, mai2touch_next_report_tick) ||
        !tud_cdc_n_connected(MAI2TOUCH_CDC_ITF) ||
        (tud_cdc_n_write_available(MAI2TOUCH_CDC_ITF) <
         MAI2TOUCH_REPORT_LENGTH))
    {
        return;
    }

    zone_mask = mai2touch_build_zone_mask();
    frame[0] = (uint8_t)'(';
    for (uint8_t index = 0U; index < 7U; index++)
    {
        frame[index + 1U] = (uint8_t)((zone_mask >> (index * 5U)) & 0x1fU);
    }
    frame[MAI2TOUCH_REPORT_LENGTH - 1U] = (uint8_t)')';

    if (tud_cdc_n_write(MAI2TOUCH_CDC_ITF,
                        frame,
                        MAI2TOUCH_REPORT_LENGTH) == MAI2TOUCH_REPORT_LENGTH)
    {
        tud_cdc_n_write_flush(MAI2TOUCH_CDC_ITF);
        mai2touch_next_report_tick =
            mai2touch_next_periodic_tick(mai2touch_next_report_tick,
                                         now,
                                         MAI2TOUCH_REPORT_PERIOD_MS);
    }
}

static uint64_t mai2touch_build_zone_mask(void)
{
    uint64_t mask = 0U;

    for (uint8_t device_index = 0U;
         device_index < MAI2TOUCH_DEVICE_COUNT;
         device_index++)
    {
        mai2touch_device_t const *device = &mai2touch_devices[device_index];

        for (uint8_t channel = 0U;
             channel < MAI2TOUCH_CHANNEL_COUNT;
             channel++)
        {
            bool pressed = !device->connected;

            if (device->connected && device->setup_complete)
            {
                pressed = device->detector[channel].is_pressed;
            }

            if (pressed)
            {
                uint8_t zone = mai2touch_zone_map[device_index][channel];
                mask |= ((uint64_t)1U << zone);
            }
        }
    }

    return mask & ((((uint64_t)1U) << MAI2TOUCH_ZONE_COUNT) - 1U);
}

static bool mai2touch_tick_due(uint32_t now, uint32_t due)
{
    return (int32_t)(now - due) >= 0;
}

static uint32_t mai2touch_next_periodic_tick(uint32_t previous,
                                             uint32_t now,
                                             uint32_t period)
{
    uint32_t next = previous + period;

    if (mai2touch_tick_due(now, next))
    {
        next = now + period;
    }

    return next;
}

void HAL_I2C_MemRxCpltCallback(I2C_HandleTypeDef *hi2c)
{
    if ((hi2c->Instance == I2C1) && mai2touch_transfer_active)
    {
        mai2touch_i2c_event = MAI2TOUCH_I2C_EVENT_COMPLETE;
    }
}

void HAL_I2C_ErrorCallback(I2C_HandleTypeDef *hi2c)
{
    if ((hi2c->Instance == I2C1) && mai2touch_transfer_active)
    {
        mai2touch_i2c_error = HAL_I2C_GetError(hi2c);
        mai2touch_i2c_event = MAI2TOUCH_I2C_EVENT_ERROR;
    }
}
