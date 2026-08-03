#include "tenodata.h"

#include "cdc_manager.h"
#include "main.h"
#include "psoc_comm.h"
#include "tenodata_config.h"
#include "touch_pipeline.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define TENODATA_CDC_FRAME_LENGTH       70U
#define TENODATA_CDC_PERIOD_MS          5U
#define TENODATA_PROBE_PERIOD_MS        50U
#define TENODATA_CALIB_CHECK_PERIOD_MS  100U
#define TENODATA_I2C_POLL_PERIOD_MS     8U
#define TENODATA_CONFIG_PAYLOAD_LENGTH  (TENODATA_TOTAL_CHANNELS * 4U)

typedef enum
{
    STATE_INIT_WAIT = 0,
    STATE_WRITE_CONFIG_TO_PSOC,
    STATE_WAIT_CALIBRATION,
    STATE_RUNNING,
    STATE_REINIT_DRAIN
} tenodata_state_t;

static tenodata_state_t state;
static uint8_t cdc_tx_frame[TENODATA_CDC_FRAME_LENGTH];
static uint8_t host_config_payload[TENODATA_CONFIG_PAYLOAD_LENGTH];
static uint8_t active_device;
static uint32_t next_cdc_tick;
static uint32_t last_probe_tick;
static uint32_t last_calibration_tick;
static uint32_t last_i2c_poll_tick;
static bool reinit_requested;

static bool tick_due(uint32_t now, uint32_t due)
{
    return (int32_t)(now - due) >= 0;
}

static void cdc_send_frame(uint8_t status)
{
    uint8_t checksum = 0U;

    memset(cdc_tx_frame, 0, sizeof(cdc_tx_frame));
    cdc_tx_frame[0] = status;

    if (status == 0x00U)
    {
        for (uint8_t index = 0U; index < PSOC_COMM_DEVICE_COUNT; index++)
        {
            PsocDevice const *device = psoc_comm_get_device(index);

            if ((device != NULL) && device->connected)
            {
                memcpy(&cdc_tx_frame[1U + index * PSOC_COMM_PAYLOAD_LENGTH],
                       &device->raw[1],
                       PSOC_COMM_PAYLOAD_LENGTH);
            }
        }
    }

    for (uint8_t index = 0U; index < (TENODATA_CDC_FRAME_LENGTH - 1U); index++)
    {
        checksum = (uint8_t)(checksum + cdc_tx_frame[index]);
    }
    cdc_tx_frame[TENODATA_CDC_FRAME_LENGTH - 1U] = checksum;

    (void)cdc_manager_try_send(false,
                               cdc_tx_frame,
                               TENODATA_CDC_FRAME_LENGTH);
}

/*
 * Re-enter exactly the same initialization path used at boot. Runtime callers
 * reach this helper only after STATE_REINIT_DRAIN has observed an idle I2C bus.
 */
static void start_initialization(uint32_t now)
{
    psoc_comm_init();
    touch_pipeline_init();
    tenodata_config_get_payload(host_config_payload);

    active_device = 0U;
    last_probe_tick = now - TENODATA_PROBE_PERIOD_MS;
    last_calibration_tick = now;
    last_i2c_poll_tick = now;
    reinit_requested = false;
    state = STATE_INIT_WAIT;
}

static void request_initialization(void)
{
    /* Stop exposing outputs derived from the old table while an outstanding
     * I2C transaction is being drained.
     */
    touch_pipeline_init();
    reinit_requested = true;
    state = STATE_REINIT_DRAIN;
}

void tenodata_init(void)
{
    uint32_t now = HAL_GetTick();

    next_cdc_tick = now;
    reinit_requested = false;
    start_initialization(now);
}

void tenodata_request_reconfigure(void)
{
    request_initialization();
}

void tenodata_task(void)
{
    uint32_t now = HAL_GetTick();

    if (reinit_requested && (state != STATE_REINIT_DRAIN))
    {
        state = STATE_REINIT_DRAIN;
    }

    switch (state)
    {
        case STATE_REINIT_DRAIN:
            /* Do not reset the HAL or overwrite buffers during an async read. */
            if (psoc_comm_is_bus_ready())
            {
                start_initialization(now);
            }
            break;

        case STATE_INIT_WAIT:
            if ((uint32_t)(now - last_probe_tick) >= TENODATA_PROBE_PERIOD_MS)
            {
                last_probe_tick = now;
                if (psoc_comm_probe() > 0U)
                {
                    state = STATE_WRITE_CONFIG_TO_PSOC;
                }
            }
            break;

        case STATE_WRITE_CONFIG_TO_PSOC:
            if (psoc_comm_write_config_all(host_config_payload))
            {
                last_calibration_tick = now;
                state = STATE_WAIT_CALIBRATION;
            }
            else
            {
                request_initialization();
            }
            break;

        case STATE_WAIT_CALIBRATION:
            if ((uint32_t)(now - last_calibration_tick) >
                TENODATA_CALIB_CHECK_PERIOD_MS)
            {
                bool all_calibrated = true;
                uint8_t checked_count = 0U;

                last_calibration_tick = now;

                for (uint8_t index = 0U;
                     index < PSOC_COMM_DEVICE_COUNT;
                     index++)
                {
                    PsocDevice const *device = psoc_comm_get_device(index);
                    uint8_t psoc_status = 0xffU;

                    if ((device == NULL) || !device->connected)
                    {
                        continue;
                    }

                    checked_count++;
                    if (!psoc_comm_read_status(index, &psoc_status))
                    {
                        all_calibrated = false;
                    }
                    else if (psoc_status == PSOC_STATUS_CRASH)
                    {
                        request_initialization();
                        return;
                    }
                    else if (psoc_status != PSOC_STATUS_CALIBRATION_DONE)
                    {
                        all_calibrated = false;
                    }
                }

                if ((checked_count > 0U) && all_calibrated)
                {
                    active_device = 0U;
                    last_i2c_poll_tick = now;
                    state = STATE_RUNNING;
                }
                else if (checked_count == 0U)
                {
                    request_initialization();
                }
            }
            break;

        case STATE_RUNNING:
            if (!psoc_comm_is_bus_ready())
            {
                break;
            }

            if (psoc_comm_is_read_complete() || psoc_comm_is_read_error())
            {
                if (psoc_comm_is_read_complete())
                {
                    PsocDevice const *device;

                    psoc_comm_commit_read(active_device);
                    device = psoc_comm_get_device(active_device);

                    if ((device != NULL) &&
                        device->connected &&
                        (device->raw[0] == PSOC_STATUS_CRASH))
                    {
                        psoc_comm_clear_read_flags();
                        request_initialization();
                        return;
                    }

                    if (cdc_manager_is_mai2touch_active() &&
                        (device != NULL) &&
                        device->connected)
                    {
                        touch_pipeline_feed(active_device, device->raw);
                    }
                }

                psoc_comm_clear_read_flags();
                active_device =
                    (uint8_t)((active_device + 1U) % PSOC_COMM_DEVICE_COUNT);
            }

            if ((uint32_t)(now - last_i2c_poll_tick) >=
                TENODATA_I2C_POLL_PERIOD_MS)
            {
                PsocDevice const *device;

                last_i2c_poll_tick = now;
                device = psoc_comm_get_device(active_device);

                if ((device != NULL) && device->connected)
                {
                    (void)psoc_comm_start_async_read(active_device);
                }
                else
                {
                    active_device =
                        (uint8_t)((active_device + 1U) %
                                  PSOC_COMM_DEVICE_COUNT);
                }
            }
            break;

        default:
            request_initialization();
            break;
    }

    if (tick_due(now, next_cdc_tick))
    {
        next_cdc_tick = now + TENODATA_CDC_PERIOD_MS;

        switch (state)
        {
            case STATE_REINIT_DRAIN:
            case STATE_WRITE_CONFIG_TO_PSOC:
                cdc_send_frame(0x02U);
                break;

            case STATE_WAIT_CALIBRATION:
            {
                PsocDevice const *device = psoc_comm_get_device(0U);
                cdc_send_frame(((device != NULL) && device->connected) ?
                               device->raw[0] : 0x00U);
                break;
            }

            case STATE_RUNNING:
                cdc_send_frame(0x00U);
                break;

            default:
                break;
        }
    }
}
