#include "tenodata.h"

#include "cdc_manager.h"
#include "main.h"
#include "psoc_comm.h"
#include "tenodata_config.h"
#include "touch_pipeline.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define TENODATA_CDC_FRAME_LENGTH          70U
#define TENODATA_CDC_PERIOD_MS              5U
#define TENODATA_PROBE_PERIOD_MS           50U
#define TENODATA_SINGLE_DEVICE_WAIT_MS    500U
#define TENODATA_REPROBE_PERIOD_MS        500U
#define TENODATA_RECOVERY_BACKOFF_MIN_MS 5000U
#define TENODATA_RECOVERY_BACKOFF_MAX_MS 60000U
#define TENODATA_PSOC_REBOOT_WAIT_MS      150U
#define TENODATA_PSOC_REBOOT_TIMEOUT_MS  2000U
#define TENODATA_CALIB_CHECK_PERIOD_MS    100U
#define TENODATA_CALIBRATION_TIMEOUT_MS  5000U
#define TENODATA_I2C_POLL_PERIOD_MS         8U
#define TENODATA_I2C_TRANSACTION_TIMEOUT_MS 30U
#define TENODATA_I2C_FAILURE_LIMIT          3U
#define TENODATA_CONFIG_PAYLOAD_LENGTH \
    (TENODATA_TOTAL_CHANNELS * 4U)
#define TENODATA_ALL_DEVICE_MASK \
    ((uint8_t)((1U << PSOC_COMM_DEVICE_COUNT) - 1U))

#if defined(DEBUG) && defined(__GNUC__)
#define TENODATA_SIZE_OPTIMIZED __attribute__((optimize("Os")))
#else
#define TENODATA_SIZE_OPTIMIZED
#endif

typedef enum
{
    STATE_INIT_WAIT = 0,
    STATE_PREPARE_PSOC,
    STATE_WAIT_PSOC_REBOOT,
    STATE_VERIFY_PSOC_REBOOT,
    STATE_WRITE_CONFIG_TO_PSOC,
    STATE_WAIT_CALIBRATION,
    STATE_RUNNING,
    STATE_REINIT_DRAIN
} tenodata_state_t;

static tenodata_state_t state;
static uint8_t cdc_tx_frame[TENODATA_CDC_FRAME_LENGTH];
static uint8_t host_config_payload[TENODATA_CONFIG_PAYLOAD_LENGTH];
static uint8_t active_device;
static uint8_t pending_read_device;
static uint8_t init_target_mask;
static uint8_t operational_device_mask;
static uint8_t pipeline_device_mask;
static uint8_t unavailable_device_mask;
static uint8_t recovery_forced_mask;
static uint8_t reset_expected_mask;
static uint8_t reset_command_mask;
static uint8_t reset_status2_seen_mask;
static uint8_t reset_ready_mask;
static uint8_t calibrated_device_mask;
static uint8_t calibration_report_status;
static uint8_t discovery_candidate_mask;
static uint8_t locked_discovery_mask;
static uint8_t read_failure_count[PSOC_COMM_DEVICE_COUNT];
static uint8_t recovery_failure_count[PSOC_COMM_DEVICE_COUNT];
static uint8_t status_i2c_failure_count[PSOC_COMM_DEVICE_COUNT];
static uint8_t last_psoc_status[PSOC_COMM_DEVICE_COUNT];
static uint8_t status_valid_mask;
static uint8_t soft_reset_supported_mask;
static uint8_t legacy_firmware_mask;
static uint8_t power_cycle_required_mask;
static uint8_t firmware_detection_pending_mask;
static uint32_t next_cdc_tick;
static uint32_t last_probe_tick;
static uint32_t discovery_candidate_tick;
static uint32_t reset_wait_tick;
static uint32_t reset_cycle_tick;
static uint32_t calibration_start_tick;
static uint32_t last_calibration_tick;
static uint32_t last_i2c_poll_tick;
static uint32_t read_start_tick;
static uint32_t last_reprobe_tick;
static uint32_t recovery_retry_tick[PSOC_COMM_DEVICE_COUNT];
static uint32_t last_psoc_status_tick[PSOC_COMM_DEVICE_COUNT];
static uint32_t reinit_start_tick;
static bool read_inflight;
static bool reinit_requested;
static bool discovery_mask_locked;
static bool firmware_detection_pass_active;

_Static_assert(PSOC_COMM_DEVICE_COUNT == TENODATA_STATUS_DEVICE_COUNT,
               "PSoC status protocol requires exactly two devices");

static uint8_t device_bit(uint8_t device_index)
{
    return (uint8_t)(1U << device_index);
}

static bool elapsed_at_least(uint32_t now,
                             uint32_t start,
                             uint32_t period)
{
    return (uint32_t)(now - start) >= period;
}

static bool tick_due(uint32_t now, uint32_t due)
{
    return (int32_t)(now - due) >= 0;
}

static bool psoc_status_is_initializing(uint8_t status)
{
    return (status == PSOC_STATUS_START_CALIBRATION) ||
           ((status >= 0x11U) && (status <= 0x15U));
}

static void record_psoc_status(uint8_t device_index,
                               uint8_t status,
                               uint32_t now)
{
    if (device_index >= PSOC_COMM_DEVICE_COUNT)
    {
        return;
    }

    last_psoc_status[device_index] = status;
    last_psoc_status_tick[device_index] = now;
    status_i2c_failure_count[device_index] = 0U;
    status_valid_mask |= device_bit(device_index);
}

static void apply_pipeline_device_masks(void)
{
    touch_pipeline_set_device_masks(
        pipeline_device_mask,
        (uint8_t)((unavailable_device_mask | recovery_forced_mask) &
                  TENODATA_ALL_DEVICE_MASK));
}

static void schedule_recovery_backoff(uint8_t device_index, uint32_t now)
{
    uint32_t delay = TENODATA_RECOVERY_BACKOFF_MIN_MS;

    if (device_index >= PSOC_COMM_DEVICE_COUNT)
    {
        return;
    }

    if (recovery_failure_count[device_index] < UINT8_MAX)
    {
        recovery_failure_count[device_index]++;
    }

    for (uint8_t step = 1U;
         step < recovery_failure_count[device_index];
         step++)
    {
        if (delay >= (TENODATA_RECOVERY_BACKOFF_MAX_MS / 2U))
        {
            delay = TENODATA_RECOVERY_BACKOFF_MAX_MS;
            break;
        }
        delay *= 2U;
    }

    recovery_retry_tick[device_index] = now + delay;
}

static void set_pipeline_device_mask(uint8_t connected_mask)
{
    pipeline_device_mask =
        (uint8_t)(connected_mask & TENODATA_ALL_DEVICE_MASK);
    unavailable_device_mask =
        (uint8_t)(TENODATA_ALL_DEVICE_MASK &
                  (uint8_t)~pipeline_device_mask);
    apply_pipeline_device_masks();
}

static void set_first_active_device(void)
{
    active_device = 0U;

    for (uint8_t index = 0U; index < PSOC_COMM_DEVICE_COUNT; index++)
    {
        if ((operational_device_mask & device_bit(index)) != 0U)
        {
            active_device = index;
            return;
        }
    }
}

static void advance_active_device(uint8_t previous_device)
{
    for (uint8_t step = 1U; step <= PSOC_COMM_DEVICE_COUNT; step++)
    {
        uint8_t candidate =
            (uint8_t)((previous_device + step) % PSOC_COMM_DEVICE_COUNT);

        if ((operational_device_mask & device_bit(candidate)) != 0U)
        {
            active_device = candidate;
            return;
        }
    }

    active_device = 0U;
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

            if (((operational_device_mask & device_bit(index)) != 0U) &&
                (device != NULL) &&
                device->connected)
            {
                memcpy(&cdc_tx_frame[1U +
                                     index * PSOC_COMM_PAYLOAD_LENGTH],
                       &device->raw[1],
                       PSOC_COMM_PAYLOAD_LENGTH);
            }
        }
    }

    for (uint8_t index = 0U;
         index < (TENODATA_CDC_FRAME_LENGTH - 1U);
         index++)
    {
        checksum = (uint8_t)(checksum + cdc_tx_frame[index]);
    }
    cdc_tx_frame[TENODATA_CDC_FRAME_LENGTH - 1U] = checksum;

    (void)cdc_manager_try_send(false,
                               cdc_tx_frame,
                               TENODATA_CDC_FRAME_LENGTH);
}

static void start_initialization(uint32_t now)
{
    psoc_comm_init();
    touch_pipeline_init();
    apply_pipeline_device_masks();
    tenodata_config_get_payload(host_config_payload);

    operational_device_mask = 0U;
    init_target_mask = 0U;
    reset_expected_mask = 0U;
    reset_command_mask = 0U;
    reset_status2_seen_mask = 0U;
    reset_ready_mask = 0U;
    calibrated_device_mask = 0U;
    calibration_report_status = PSOC_STATUS_START_CALIBRATION;
    active_device = 0U;
    pending_read_device = 0U;
    read_inflight = false;
    memset(read_failure_count, 0, sizeof(read_failure_count));
    psoc_comm_clear_read_flags();

    last_probe_tick = now - TENODATA_PROBE_PERIOD_MS;
    discovery_candidate_mask = 0xffU;
    discovery_candidate_tick = now;
    last_calibration_tick = now;
    last_i2c_poll_tick = now;
    last_reprobe_tick = now;
    reinit_requested = false;
    state = STATE_INIT_WAIT;
}

static void enter_reinitialization_drain(void)
{
    /* Keep already-confirmed missing/recovering regions asserted while the
     * current I2C transaction drains and the full initialization restarts.
     */
    operational_device_mask = 0U;
    touch_pipeline_init();
    apply_pipeline_device_masks();
    reinit_requested = true;
    reinit_start_tick = HAL_GetTick();
    state = STATE_REINIT_DRAIN;
}

static void request_initialization(void)
{
    firmware_detection_pending_mask = 0U;
    firmware_detection_pass_active = false;
    discovery_mask_locked = false;
    enter_reinitialization_drain();
}

static void request_initialization_for_mask(uint8_t device_mask)
{
    locked_discovery_mask =
        (uint8_t)(device_mask & TENODATA_ALL_DEVICE_MASK);
    discovery_mask_locked = true;
    enter_reinitialization_drain();
}

static void begin_running(uint8_t device_mask, uint32_t now)
{
    firmware_detection_pending_mask = 0U;
    firmware_detection_pass_active = false;
    operational_device_mask =
        (uint8_t)(device_mask & TENODATA_ALL_DEVICE_MASK);
    init_target_mask = operational_device_mask;
    set_pipeline_device_mask(operational_device_mask);

    memset(read_failure_count, 0, sizeof(read_failure_count));
    for (uint8_t index = 0U; index < PSOC_COMM_DEVICE_COUNT; index++)
    {
        if ((operational_device_mask & device_bit(index)) != 0U)
        {
            recovery_failure_count[index] = 0U;
            recovery_retry_tick[index] = now;
        }
    }
    psoc_comm_clear_read_flags();
    read_inflight = false;
    set_first_active_device();
    last_i2c_poll_tick = now;
    last_reprobe_tick = now;
    state = STATE_RUNNING;
}

static void finish_successful_calibration(uint8_t device_mask, uint32_t now)
{
    uint8_t detection_mask =
        (uint8_t)(firmware_detection_pending_mask & device_mask);

    if (!firmware_detection_pass_active && (detection_mask != 0U))
    {
        /* Status 0 is shared by both firmware generations, so probing 0xAD
         * there would be unsafe for legacy firmware. After the first cold
         * calibration reaches status 2, repeat the normal initialization
         * path once. Its existing reset verification can now distinguish a
         * software-reset-capable PSoC from a legacy one.
         */
        firmware_detection_pass_active = true;
        request_initialization_for_mask(device_mask);
        return;
    }

    begin_running(device_mask, now);
}

static void finish_device_discovery(uint8_t device_mask, uint32_t now)
{
    if (discovery_mask_locked)
    {
        uint8_t lost_locked_mask =
            (uint8_t)(locked_discovery_mask & (uint8_t)~device_mask);

        for (uint8_t index = 0U;
             index < PSOC_COMM_DEVICE_COUNT;
             index++)
        {
            if ((lost_locked_mask & device_bit(index)) != 0U)
            {
                schedule_recovery_backoff(index, now);
            }
        }
    }

    discovery_mask_locked = false;
    init_target_mask =
        (uint8_t)(device_mask & TENODATA_ALL_DEVICE_MASK);
    set_pipeline_device_mask(init_target_mask);

    if (init_target_mask == 0U)
    {
        begin_running(0U, now);
    }
    else
    {
        state = STATE_PREPARE_PSOC;
    }
}

static void finish_reboot_verification(uint8_t ready_mask, uint32_t now)
{
    ready_mask &= reset_expected_mask;

    for (uint8_t index = 0U; index < PSOC_COMM_DEVICE_COUNT; index++)
    {
        if ((ready_mask & device_bit(index)) == 0U)
        {
            psoc_comm_mark_disconnected(index);
            if ((reset_expected_mask & device_bit(index)) != 0U)
            {
                schedule_recovery_backoff(index, now);
            }
        }
    }

    init_target_mask = ready_mask;
    set_pipeline_device_mask(ready_mask);

    if (ready_mask == 0U)
    {
        begin_running(0U, now);
    }
    else
    {
        state = STATE_WRITE_CONFIG_TO_PSOC;
    }
}

static void mark_device_unavailable(uint8_t device_index, uint32_t now)
{
    uint8_t mask;

    if (device_index >= PSOC_COMM_DEVICE_COUNT)
    {
        return;
    }

    mask = device_bit(device_index);
    psoc_comm_mark_disconnected(device_index);
    operational_device_mask &= (uint8_t)~mask;
    recovery_forced_mask |= mask;
    read_failure_count[device_index] = 0U;
    last_reprobe_tick = now;
    recovery_retry_tick[device_index] = now + TENODATA_REPROBE_PERIOD_MS;
    set_pipeline_device_mask(operational_device_mask);
    set_first_active_device();
}

static void mark_initializing_device_unavailable(uint8_t device_index,
                                                 uint32_t now)
{
    uint8_t mask;

    if (device_index >= PSOC_COMM_DEVICE_COUNT)
    {
        return;
    }

    mask = device_bit(device_index);
    psoc_comm_mark_disconnected(device_index);
    init_target_mask &= (uint8_t)~mask;
    firmware_detection_pending_mask &= (uint8_t)~mask;
    recovery_forced_mask |= mask;
    read_failure_count[device_index] = 0U;
    schedule_recovery_backoff(device_index, now);
    set_pipeline_device_mask(init_target_mask);
}

static bool record_device_read_failure(uint8_t device_index, uint32_t now)
{
    if (device_index >= PSOC_COMM_DEVICE_COUNT)
    {
        return false;
    }

    if (read_failure_count[device_index] < UINT8_MAX)
    {
        read_failure_count[device_index]++;
    }
    status_i2c_failure_count[device_index] =
        read_failure_count[device_index];

    if (read_failure_count[device_index] < TENODATA_I2C_FAILURE_LIMIT)
    {
        return false;
    }

    mark_device_unavailable(device_index, now);
    return true;
}

static bool probe_for_reconnected_devices(uint32_t now)
{
    uint8_t found_mask = 0U;
    uint8_t missing_mask =
        (uint8_t)(TENODATA_ALL_DEVICE_MASK &
                  (uint8_t)~operational_device_mask);

    if (missing_mask == 0U)
    {
        return false;
    }

    if (!elapsed_at_least(now,
                          last_reprobe_tick,
                          TENODATA_REPROBE_PERIOD_MS))
    {
        return false;
    }

    /* Advance the timer for both successful and failed attempts. */
    last_reprobe_tick = now;

    for (uint8_t index = 0U; index < PSOC_COMM_DEVICE_COUNT; index++)
    {
        uint8_t mask = device_bit(index);
        uint8_t psoc_status = 0xffU;
        bool status_read;

        if (((missing_mask & mask) == 0U) ||
            !tick_due(now, recovery_retry_tick[index]))
        {
            continue;
        }

        status_read = psoc_comm_probe_device_quick(index) &&
                      psoc_comm_read_status(index, &psoc_status);
        if (status_read)
        {
            record_psoc_status(index, psoc_status, now);
        }

        if (status_read &&
            ((psoc_status == PSOC_STATUS_CRASH) ||
             (psoc_status == PSOC_STATUS_CALIBRATION_DONE)))
        {
            found_mask |= mask;
        }
        else
        {
            /* An ACK alone is insufficient. In particular, do not interrupt
             * the healthy PSoC while the missing one is still in 0x11-0x15
             * calibration progress or otherwise cannot report a stable state.
             */
            psoc_comm_mark_disconnected(index);
        }
    }

    if (found_mask == 0U)
    {
        return false;
    }

    /* Address ACK is only discovery. Keep these devices forced until the
     * complete reset/config/hardware/software calibration path succeeds.
     */
    recovery_forced_mask |= found_mask;
    request_initialization_for_mask(
        (uint8_t)(operational_device_mask | found_mask));
    return true;
}

void tenodata_init(void)
{
    uint32_t now = HAL_GetTick();

    next_cdc_tick = now;
    operational_device_mask = 0U;
    pipeline_device_mask = TENODATA_ALL_DEVICE_MASK;
    unavailable_device_mask = 0U;
    recovery_forced_mask = 0U;
    locked_discovery_mask = 0U;
    discovery_mask_locked = false;
    memset(recovery_failure_count, 0, sizeof(recovery_failure_count));
    memset(recovery_retry_tick, 0, sizeof(recovery_retry_tick));
    memset(status_i2c_failure_count,
           0,
           sizeof(status_i2c_failure_count));
    memset(last_psoc_status, 0xff, sizeof(last_psoc_status));
    memset(last_psoc_status_tick, 0, sizeof(last_psoc_status_tick));
    status_valid_mask = 0U;
    soft_reset_supported_mask = 0U;
    legacy_firmware_mask = 0U;
    power_cycle_required_mask = 0U;
    firmware_detection_pending_mask = 0U;
    firmware_detection_pass_active = false;
    reinit_requested = false;
    touch_pipeline_set_device_masks(pipeline_device_mask, 0U);
    start_initialization(now);
}

void tenodata_request_reconfigure(void)
{
    request_initialization();
}

TENODATA_SIZE_OPTIMIZED
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
            /* Never clear HAL buffers while an interrupt read owns them. */
            if (psoc_comm_is_bus_ready())
            {
                psoc_comm_clear_read_flags();
                read_inflight = false;
                start_initialization(now);
            }
            else if (elapsed_at_least(
                         now,
                         reinit_start_tick,
                         TENODATA_I2C_TRANSACTION_TIMEOUT_MS))
            {
                psoc_comm_recover_bus();
                read_inflight = false;
                start_initialization(HAL_GetTick());
            }
            break;

        case STATE_INIT_WAIT:
            if (elapsed_at_least(now,
                                 last_probe_tick,
                                 TENODATA_PROBE_PERIOD_MS))
            {
                uint8_t detected_mask;

                last_probe_tick = now;
                if (discovery_mask_locked)
                {
                    for (uint8_t index = 0U;
                         index < PSOC_COMM_DEVICE_COUNT;
                         index++)
                    {
                        if ((locked_discovery_mask & device_bit(index)) != 0U)
                        {
                            (void)psoc_comm_probe_device(index);
                        }
                    }
                    detected_mask =
                        (uint8_t)(psoc_comm_get_connected_mask() &
                                  locked_discovery_mask);
                }
                else
                {
                    (void)psoc_comm_probe();
                    detected_mask = psoc_comm_get_connected_mask();
                }

                if ((!discovery_mask_locked &&
                     (detected_mask == TENODATA_ALL_DEVICE_MASK)) ||
                    (discovery_mask_locked &&
                     (detected_mask == locked_discovery_mask)))
                {
                    finish_device_discovery(detected_mask, now);
                    break;
                }

                if (detected_mask != discovery_candidate_mask)
                {
                    discovery_candidate_mask = detected_mask;
                    discovery_candidate_tick = now;
                }
                else if (elapsed_at_least(
                             now,
                             discovery_candidate_tick,
                             TENODATA_SINGLE_DEVICE_WAIT_MS))
                {
                    finish_device_discovery(detected_mask, now);
                }
            }
            break;

        case STATE_PREPARE_PSOC:
        {
            /* Read status before sending 0xAD. Legacy firmware can ACK the
             * command without implementing it, and writing 0xAD while it is
             * waiting at status 0 can make it leave the startup wait path.
             */
            reset_expected_mask = init_target_mask;
            reset_command_mask = 0U;
            reset_status2_seen_mask = 0U;
            reset_ready_mask = 0U;
            reset_cycle_tick = HAL_GetTick();
            reset_wait_tick = reset_cycle_tick;

            for (uint8_t index = 0U;
                 index < PSOC_COMM_DEVICE_COUNT;
                 index++)
            {
                uint8_t mask = device_bit(index);
                uint8_t psoc_status = 0xffU;

                if ((reset_expected_mask & mask) == 0U)
                {
                    continue;
                }

                if (!psoc_comm_probe_device(index) ||
                    !psoc_comm_read_status(index, &psoc_status))
                {
                    continue;
                }

                record_psoc_status(index, psoc_status, reset_cycle_tick);

                if (psoc_status == PSOC_STATUS_CRASH)
                {
                    /* Both firmware generations use status 0 while waiting
                     * for their initial configuration. No reset is needed.
                     */
                    if (((soft_reset_supported_mask |
                          legacy_firmware_mask) & mask) == 0U)
                    {
                        firmware_detection_pending_mask |= mask;
                    }
                    reset_ready_mask |= mask;
                    power_cycle_required_mask &= (uint8_t)~mask;
                }
                else if ((psoc_status == PSOC_STATUS_CALIBRATION_DONE) &&
                         psoc_comm_soft_reset(index))
                {
                    reset_command_mask |= mask;
                }
                else if (psoc_status_is_initializing(psoc_status))
                {
                    /* 0xAD is intentionally ignored by new firmware during
                     * 0x01/0x11-0x15, so wait for a stable state first.
                     */
                }
            }

            if ((reset_ready_mask & reset_expected_mask) ==
                reset_expected_mask)
            {
                finish_reboot_verification(reset_ready_mask,
                                           reset_cycle_tick);
            }
            else
            {
                state = STATE_WAIT_PSOC_REBOOT;
            }
            break;
        }

        case STATE_WAIT_PSOC_REBOOT:
            if (elapsed_at_least(now,
                                 reset_wait_tick,
                                 TENODATA_PSOC_REBOOT_WAIT_MS))
            {
                last_probe_tick = now - TENODATA_PROBE_PERIOD_MS;
                state = STATE_VERIFY_PSOC_REBOOT;
            }
            break;

        case STATE_VERIFY_PSOC_REBOOT:
            if (elapsed_at_least(now,
                                 last_probe_tick,
                                 TENODATA_PROBE_PERIOD_MS))
            {
                bool reset_retried = false;
                bool reset_timed_out = elapsed_at_least(
                    now,
                    reset_cycle_tick,
                    TENODATA_PSOC_REBOOT_TIMEOUT_MS);

                last_probe_tick = now;
                for (uint8_t index = 0U;
                     index < PSOC_COMM_DEVICE_COUNT;
                     index++)
                {
                    uint8_t mask = device_bit(index);
                    uint8_t psoc_status = 0xffU;

                    if (((reset_expected_mask & mask) == 0U) ||
                        ((reset_ready_mask & mask) != 0U))
                    {
                        continue;
                    }

                    if (!psoc_comm_probe_device(index) ||
                        !psoc_comm_read_status(index, &psoc_status))
                    {
                        continue;
                    }

                    record_psoc_status(index, psoc_status, now);

                    if (psoc_status == PSOC_STATUS_CRASH)
                    {
                        reset_ready_mask |= mask;
                        power_cycle_required_mask &= (uint8_t)~mask;
                        if ((reset_command_mask & mask) != 0U)
                        {
                            soft_reset_supported_mask |= mask;
                            legacy_firmware_mask &= (uint8_t)~mask;
                            firmware_detection_pending_mask &=
                                (uint8_t)~mask;
                        }
                        continue;
                    }

                    if (psoc_status_is_initializing(psoc_status))
                    {
                        /* A reset attempt made before initialization started
                         * is no longer evidence for legacy detection.
                         */
                        reset_command_mask &= (uint8_t)~mask;
                        reset_status2_seen_mask &= (uint8_t)~mask;
                        continue;
                    }

                    if (psoc_status == PSOC_STATUS_CALIBRATION_DONE)
                    {
                        if (((reset_command_mask & mask) != 0U) &&
                            ((reset_status2_seen_mask & mask) != 0U))
                        {
                            /* Two completed reset writes both left status 2:
                             * keep legacy firmware online. Its new settings
                             * take effect after the next full power cycle.
                             */
                            reset_ready_mask |= mask;
                            legacy_firmware_mask |= mask;
                            soft_reset_supported_mask &= (uint8_t)~mask;
                            if (firmware_detection_pass_active &&
                                ((firmware_detection_pending_mask & mask) !=
                                 0U))
                            {
                                /* The first cold-start pass already applied
                                 * this configuration successfully. This
                                 * second pass only identifies the firmware,
                                 * so legacy hardware does not need another
                                 * power cycle.
                                 */
                                power_cycle_required_mask &= (uint8_t)~mask;
                            }
                            else
                            {
                                power_cycle_required_mask |= mask;
                            }
                            firmware_detection_pending_mask &=
                                (uint8_t)~mask;
                        }
                        else if (reset_timed_out)
                        {
                            continue;
                        }
                        else if ((reset_command_mask & mask) == 0U)
                        {
                            if (psoc_comm_soft_reset(index))
                            {
                                reset_command_mask |= mask;
                                reset_retried = true;
                            }
                        }
                        else if ((reset_status2_seen_mask & mask) == 0U)
                        {
                            /* The first command left the device at status 2.
                             * Retry once to distinguish a delayed new-firmware
                             * reset from legacy firmware that overwrites 0xAD.
                             */
                            if (psoc_comm_soft_reset(index))
                            {
                                reset_status2_seen_mask |= mask;
                                reset_retried = true;
                            }
                        }
                    }
                }

                if ((reset_ready_mask & reset_expected_mask) ==
                    reset_expected_mask)
                {
                    finish_reboot_verification(reset_ready_mask, now);
                }
                else if (reset_timed_out)
                {
                    finish_reboot_verification(reset_ready_mask, now);
                }
                else if (reset_retried)
                {
                    reset_wait_tick = HAL_GetTick();
                    state = STATE_WAIT_PSOC_REBOOT;
                }
            }
            break;

        case STATE_WRITE_CONFIG_TO_PSOC:
        {
            uint8_t configured_mask =
                psoc_comm_write_config_mask(host_config_payload,
                                            init_target_mask);
            uint8_t failed_mask =
                (uint8_t)(init_target_mask &
                          (uint8_t)~configured_mask);

            for (uint8_t index = 0U;
                 index < PSOC_COMM_DEVICE_COUNT;
                 index++)
            {
                if ((failed_mask & device_bit(index)) != 0U)
                {
                    mark_initializing_device_unavailable(index,
                                                         HAL_GetTick());
                }
            }

            init_target_mask &= configured_mask;
            calibrated_device_mask = 0U;
            calibration_report_status = PSOC_STATUS_START_CALIBRATION;
            memset(read_failure_count, 0, sizeof(read_failure_count));
            calibration_start_tick = HAL_GetTick();
            last_calibration_tick = calibration_start_tick;

            if (init_target_mask == 0U)
            {
                begin_running(0U, calibration_start_tick);
            }
            else
            {
                state = STATE_WAIT_CALIBRATION;
            }
            break;
        }

        case STATE_WAIT_CALIBRATION:
            if (elapsed_at_least(now,
                                 last_calibration_tick,
                                 TENODATA_CALIB_CHECK_PERIOD_MS))
            {
                bool calibration_timed_out = elapsed_at_least(
                    now,
                    calibration_start_tick,
                    TENODATA_CALIBRATION_TIMEOUT_MS);
                uint8_t confirmed_calibrated_mask = 0U;

                last_calibration_tick = now;

                for (uint8_t index = 0U;
                     index < PSOC_COMM_DEVICE_COUNT;
                     index++)
                {
                    uint8_t mask = device_bit(index);
                    uint8_t psoc_status = 0xffU;

                    if ((init_target_mask & mask) == 0U)
                    {
                        continue;
                    }

                    if (!psoc_comm_read_status(index, &psoc_status))
                    {
                        if (read_failure_count[index] < UINT8_MAX)
                        {
                            read_failure_count[index]++;
                        }
                        status_i2c_failure_count[index] =
                            read_failure_count[index];
                        if (read_failure_count[index] >=
                            TENODATA_I2C_FAILURE_LIMIT)
                        {
                            mark_initializing_device_unavailable(index, now);
                        }
                        continue;
                    }

                    read_failure_count[index] = 0U;
                    record_psoc_status(index, psoc_status, now);
                    calibration_report_status = psoc_status;

                    if (psoc_status == PSOC_STATUS_CRASH)
                    {
                        mark_initializing_device_unavailable(index, now);
                        continue;
                    }

                    if (psoc_status == PSOC_STATUS_CALIBRATION_DONE)
                    {
                        calibrated_device_mask |= mask;
                        confirmed_calibrated_mask |= mask;
                    }
                    else if ((calibrated_device_mask & mask) != 0U)
                    {
                        /* A device may not regress after reporting done. */
                        mark_initializing_device_unavailable(index, now);
                    }
                }

                if (calibration_timed_out)
                {
                    uint8_t timed_out_mask =
                        (uint8_t)(init_target_mask &
                                  (uint8_t)~calibrated_device_mask);

                    for (uint8_t index = 0U;
                         index < PSOC_COMM_DEVICE_COUNT;
                         index++)
                    {
                        if ((timed_out_mask & device_bit(index)) != 0U)
                        {
                            mark_initializing_device_unavailable(index, now);
                        }
                    }
                }

                if (init_target_mask == 0U)
                {
                    begin_running(0U, now);
                }
                else if ((confirmed_calibrated_mask & init_target_mask) ==
                         init_target_mask)
                {
                    finish_successful_calibration(init_target_mask, now);
                }
            }
            break;

        case STATE_RUNNING:
        {
            bool reprobe_attempted = false;
            bool bus_recovered = false;

            if (touch_pipeline_is_ready())
            {
                uint8_t recovered_mask =
                    (uint8_t)(recovery_forced_mask &
                              operational_device_mask);

                if (recovered_mask != 0U)
                {
                    recovery_forced_mask &= (uint8_t)~recovered_mask;
                    apply_pipeline_device_masks();
                }
            }

            if (read_inflight && psoc_comm_is_bus_ready() &&
                (psoc_comm_is_read_complete() ||
                 psoc_comm_is_read_error()))
            {
                uint8_t completed_device = pending_read_device;
                bool read_error = psoc_comm_is_read_error();

                if (!read_error && psoc_comm_is_read_complete())
                {
                    PsocDevice const *device;

                    psoc_comm_commit_read(completed_device);
                    device = psoc_comm_get_device(completed_device);
                    read_failure_count[completed_device] = 0U;

                    if (device != NULL)
                    {
                        record_psoc_status(completed_device,
                                           device->raw[0],
                                           now);
                    }

                    psoc_comm_clear_read_flags();
                    read_inflight = false;
                    advance_active_device(completed_device);

                    if ((device != NULL) &&
                        device->connected &&
                        (device->raw[0] !=
                         PSOC_STATUS_CALIBRATION_DONE))
                    {
                        mark_device_unavailable(completed_device, now);
                    }

                    if (cdc_manager_is_mai2touch_active() &&
                        (device != NULL) &&
                        device->connected &&
                        (device->raw[0] ==
                         PSOC_STATUS_CALIBRATION_DONE))
                    {
                        touch_pipeline_feed(completed_device, device->raw);
                    }
                }
                else
                {
                    psoc_comm_clear_read_flags();
                    read_inflight = false;
                    (void)record_device_read_failure(completed_device, now);
                    advance_active_device(completed_device);
                }
            }

            /* Consume completion/error flags before considering a timeout.
             * This avoids classifying a completed read as failed when the
             * cooperative main loop was delayed for more than 30 ms.
             */
            if (read_inflight &&
                elapsed_at_least(now,
                                 read_start_tick,
                                 TENODATA_I2C_TRANSACTION_TIMEOUT_MS))
            {
                uint8_t timed_out_device = pending_read_device;

                psoc_comm_recover_bus();
                psoc_comm_clear_read_flags();
                read_inflight = false;
                (void)record_device_read_failure(timed_out_device, now);
                advance_active_device(timed_out_device);
                bus_recovered = true;
            }

            if (!bus_recovered &&
                !read_inflight && psoc_comm_is_bus_ready() &&
                (operational_device_mask != TENODATA_ALL_DEVICE_MASK) &&
                elapsed_at_least(now,
                                 last_reprobe_tick,
                                 TENODATA_REPROBE_PERIOD_MS))
            {
                reprobe_attempted = true;
                if (probe_for_reconnected_devices(now))
                {
                    return;
                }
            }

            if (!bus_recovered &&
                !reprobe_attempted &&
                !read_inflight &&
                psoc_comm_is_bus_ready() &&
                elapsed_at_least(now,
                                 last_i2c_poll_tick,
                                 TENODATA_I2C_POLL_PERIOD_MS))
            {
                uint8_t mask = device_bit(active_device);

                last_i2c_poll_tick = now;

                if ((operational_device_mask & mask) == 0U)
                {
                    set_first_active_device();
                    mask = device_bit(active_device);
                }

                if ((operational_device_mask & mask) != 0U)
                {
                    psoc_comm_clear_read_flags();
                    if (psoc_comm_start_async_read(active_device))
                    {
                        pending_read_device = active_device;
                        read_start_tick = HAL_GetTick();
                        read_inflight = true;
                    }
                    else
                    {
                        uint8_t failed_device = active_device;

                        (void)record_device_read_failure(failed_device, now);
                        advance_active_device(failed_device);
                    }
                }
            }
            break;
        }

        default:
            request_initialization();
            break;
    }

    if (tick_due(now, next_cdc_tick))
    {
        next_cdc_tick = now + TENODATA_CDC_PERIOD_MS;

        switch (state)
        {
            case STATE_RUNNING:
                cdc_send_frame(0x00U);
                break;

            case STATE_WAIT_CALIBRATION:
                cdc_send_frame(calibration_report_status);
                break;

            case STATE_INIT_WAIT:
            case STATE_PREPARE_PSOC:
            case STATE_WAIT_PSOC_REBOOT:
            case STATE_VERIFY_PSOC_REBOOT:
            case STATE_WRITE_CONFIG_TO_PSOC:
            case STATE_REINIT_DRAIN:
                cdc_send_frame(0x02U);
                break;

            default:
                break;
        }
    }
}

bool tenodata_get_status(TenodataStatusSnapshot *snapshot)
{
    uint32_t now;
    uint8_t connected_mask;

    if (snapshot == NULL)
    {
        return false;
    }

    now = HAL_GetTick();
    connected_mask = psoc_comm_get_connected_mask();
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->state = (uint8_t)state;
    snapshot->device_count = PSOC_COMM_DEVICE_COUNT;
    if (reinit_requested)
    {
        snapshot->flags |= TENODATA_STATUS_FLAG_REINIT_REQUESTED;
    }
    if (read_inflight)
    {
        snapshot->flags |= TENODATA_STATUS_FLAG_READ_INFLIGHT;
    }

    for (uint8_t index = 0U; index < PSOC_COMM_DEVICE_COUNT; index++)
    {
        uint8_t mask = device_bit(index);
        TenodataPsocStatus *device = &snapshot->devices[index];

        device->address = (uint8_t)(0x08U + index);
        device->status = last_psoc_status[index];
        device->consecutive_failures =
            status_i2c_failure_count[index];
        device->status_age_ms = UINT16_MAX;

        if ((connected_mask & mask) != 0U)
        {
            device->flags |= TENODATA_DEVICE_FLAG_CONNECTED;
        }
        if ((operational_device_mask & mask) != 0U)
        {
            device->flags |= TENODATA_DEVICE_FLAG_OPERATIONAL;
        }
        if (((unavailable_device_mask | recovery_forced_mask) & mask) != 0U)
        {
            device->flags |= TENODATA_DEVICE_FLAG_UNAVAILABLE;
        }
        if ((status_valid_mask & mask) != 0U)
        {
            uint32_t age = now - last_psoc_status_tick[index];

            device->flags |= TENODATA_DEVICE_FLAG_STATUS_VALID;
            device->status_age_ms =
                (age > UINT16_MAX) ? UINT16_MAX : (uint16_t)age;
        }
        if ((soft_reset_supported_mask & mask) != 0U)
        {
            device->flags |= TENODATA_DEVICE_FLAG_SOFT_RESET_SUPPORTED;
        }
        if ((legacy_firmware_mask & mask) != 0U)
        {
            device->flags |= TENODATA_DEVICE_FLAG_LEGACY_FIRMWARE;
        }
        if ((power_cycle_required_mask & mask) != 0U)
        {
            device->flags |= TENODATA_DEVICE_FLAG_POWER_CYCLE_REQUIRED;
        }
    }

    return true;
}
