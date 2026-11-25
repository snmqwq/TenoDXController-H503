#include "tenodata.h"

#include "cdc_manager.h"
#include "io_mode.h"
#include "main.h"
#include "psoc_comm.h"
#include "tenodata_config.h"
#include "touch_mode.h"
#include "touch_pipeline.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define TENODATA_CDC_FRAME_LENGTH          70U
#define TENODATA_CDC_PERIOD_MS              2U
#define TENODATA_PIPELINE_BATCH_PACKETS    32U
#define TENODATA_PROBE_PERIOD_MS           50U
#define TENODATA_SINGLE_DEVICE_WAIT_MS    500U
#define TENODATA_REPROBE_PERIOD_MS        500U
#define TENODATA_RECOVERY_BACKOFF_MIN_MS 5000U
#define TENODATA_RECOVERY_BACKOFF_MAX_MS 60000U
#define TENODATA_PSOC_REBOOT_WAIT_MS      150U
#define TENODATA_PSOC_REBOOT_TIMEOUT_MS  2000U
#define TENODATA_CALIB_CHECK_PERIOD_MS    100U
#define TENODATA_CALIBRATION_TIMEOUT_MS  5000U
/* 校准量 (Cp / 补偿 IDAC) 抓取重试间隔。
 * 必须允许在状态 0x02 时抓: PSoC 的 0x13/0x14 各只持续几毫秒, 而本状态机
 * 每 TENODATA_CALIB_CHECK_PERIOD_MS(100ms) 才轮询一次状态, 几乎不可能命中
 * 中间进度; 0x02 是持久状态, 且这几段数据是 PSoC 初始化时写一次、之后不再
 * 改动, 所以在 0x02 读到的就是校准结果。 */
#define TENODATA_CAL_CAPTURE_RETRY_MS     500U
/* 首轮抓到后再等一小段做一次"确认读"。
 * 实测: 刚看到 0x02 就立刻读, 有时会读到 PSoC 尚未写完的区域
 * (片0 前 7 个通道为 0), 稍后读则完全稳定; 因此 DONE 后再补读一次。 */
#define TENODATA_CAL_CONFIRM_DELAY_MS     250U
/* 2ms 轮询: 两片交替 → 每片 ~250Hz (单次 35B 读约 0.9ms, 总线占用 ~45%) */
#define TENODATA_I2C_POLL_PERIOD_MS         2U
#define TENODATA_I2C_TRANSACTION_TIMEOUT_MS 30U
#define TENODATA_I2C_FAILURE_LIMIT          3U
#define TENODATA_CONFIG_PAYLOAD_LENGTH \
    (TENODATA_TOTAL_CHANNELS * 4U)
#define TENODATA_ALL_DEVICE_MASK \
    ((uint8_t)((1U << PSOC_COMM_DEVICE_COUNT) - 1U))

/* run3 上位机扫描参数帧: 0xAA 0x01 + 136 字节 payload + 1 字节校验和
 * (校验和 = 帧头与 payload 全部字节累加的低 8 位)。 */
#define TENODATA_HOST_CFG_HEADER_A         0xAAU
#define TENODATA_HOST_CFG_HEADER_B         0x01U

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
    STATE_REINIT_DRAIN,
    /* 追加在末尾, 保持既有状态编号不变 (状态编号对 Magic 工具可见) */
    STATE_WAIT_HOST_CONFIG
} tenodata_state_t;

static tenodata_state_t state;
static uint8_t cdc_tx_frame[TENODATA_CDC_FRAME_LENGTH];
static uint8_t status_frame_seq;
static uint8_t host_config_payload[TENODATA_CONFIG_PAYLOAD_LENGTH];

/* 上位机下发的扫描参数 (覆盖本地固定表, 重新初始化后仍生效) */
static uint8_t host_config_override[TENODATA_CONFIG_PAYLOAD_LENGTH];
static bool host_config_override_valid;

typedef enum
{
    HOST_CFG_WAIT_AA = 0,
    HOST_CFG_WAIT_01,
    HOST_CFG_WAIT_PAYLOAD
} host_cfg_rx_state_t;

static host_cfg_rx_state_t host_cfg_rx_state;
static uint8_t host_cfg_rx_buffer[TENODATA_CONFIG_PAYLOAD_LENGTH];
static uint16_t host_cfg_rx_count;
static uint8_t host_cfg_rx_checksum;
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
/* 每片 17 通道寄生电容 Cp (原始字节, 每通道 2 字节大端, 与 EZI2C 一致)。
 * PSoC 只在校准流程的状态 0x13 写一次, 软复位后清零, 因此必须在校准
 * 进行中抓取; 之后作为静态量缓存, 供 Magic 按需读取。 */
static uint8_t cp_raw[PSOC_COMM_DEVICE_COUNT][PSOC_COMM_CP_LENGTH];
static bool cp_valid[PSOC_COMM_DEVICE_COUNT];
/* 每片 17 通道补偿 IDAC (每通道 1 字节)。PSoC 在状态 0x14 完成 SAR
 * 校准后写入, 与 Cp 在同一次事务里读出。 */
static uint8_t comp_idac[PSOC_COMM_DEVICE_COUNT][PSOC_COMM_COMP_IDAC_LENGTH];
static bool comp_idac_valid[PSOC_COMM_DEVICE_COUNT];
/* 校准量补抓的时间戳 (每个设备独立) */
static uint32_t last_cal_capture_tick[PSOC_COMM_DEVICE_COUNT];
/* 首轮抓取成功后, 是否还需要做一次确认读 */
static bool cal_capture_confirm_pending[PSOC_COMM_DEVICE_COUNT];
/* 校准量真正抓取成功的时刻 (HAL tick), 供 Magic 上报"快照年龄" */
static uint32_t cal_captured_tick[PSOC_COMM_DEVICE_COUNT];
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
static uint8_t pipeline_batch[TENODATA_PIPELINE_BATCH_PACKETS *
                              TOUCH_PIPELINE_PACKET_BYTES];
static uint16_t pipeline_batch_count;
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

/* 校准进行中抓取校准量。PSoC 分两个阶段写入:
 *   状态 0x13 → Cp 区有效 (35..68)
 *   状态 0x14 → SAR 校准完成, 补偿 IDAC 区有效 (69..85)
 * 两者在同一次 I2C 事务里返回, 因此各自到点后读一次即可。
 *
 * 注意: 本状态机每 100ms 才轮询一次状态, 而 0x13/0x14 只持续几毫秒, 实际上
 * 几乎不可能命中; 好在 0x02(校准完成) 是持久状态, 而且这几段数据由 PSoC 在
 * 初始化时写一次、之后不再改动 —— 所以在 0x02 读到的一样是校准结果。
 * 因此这里把"已校准完成"也算作可读时机, 未抓到时由 RUNNING 定期重试。 */
static void capture_cal_if_ready(uint8_t device_index, uint8_t psoc_status)
{
    bool calibrated = (psoc_status == PSOC_STATUS_CALIBRATION_DONE);
    bool need_cp;
    bool need_comp;

    if (device_index >= PSOC_COMM_DEVICE_COUNT)
    {
        return;
    }

    need_cp = !cp_valid[device_index] &&
              ((psoc_status >= PSOC_STATUS_CP_READY) || calibrated);

    need_comp = !comp_idac_valid[device_index] &&
                ((psoc_status >= PSOC_STATUS_COMP_READY) || calibrated);

    if (!need_cp && !need_comp)
    {
        return;
    }

    if (psoc_comm_read_cal(device_index,
                           cp_raw[device_index],
                           comp_idac[device_index]))
    {
        if (need_cp)
        {
            cp_valid[device_index] = true;
        }

        if (need_comp)
        {
            comp_idac_valid[device_index] = true;
        }

        /* 刚看到校准完成时读到的可能是"写了一半"的区域, 稍后再确认读一次
         * (时间戳保持不动, 由 confirm_cal_capture 负责延迟后重读) */
        cal_capture_confirm_pending[device_index] = true;
        cal_captured_tick[device_index] = HAL_GetTick();
    }
}

/* 首轮抓取后延迟确认读: 用总线空闲窗口把快照刷新成稳定值。 */
static void confirm_cal_capture(uint32_t now)
{
    for (uint8_t index = 0U; index < PSOC_COMM_DEVICE_COUNT; index++)
    {
        uint8_t mask = device_bit(index);

        if (!cal_capture_confirm_pending[index] ||
            ((operational_device_mask & mask) == 0U))
        {
            continue;
        }

        if (!elapsed_at_least(now,
                              last_cal_capture_tick[index],
                              TENODATA_CAL_CONFIRM_DELAY_MS))
        {
            continue;
        }

        if (psoc_comm_read_cal(index,
                               cp_raw[index],
                               comp_idac[index]))
        {
            cal_capture_confirm_pending[index] = false;
            last_cal_capture_tick[index] = now;
            cal_captured_tick[index] = now;
        }
    }
}

/* RUNNING 状态下若校准量还没抓到, 定期补抓一次。
 * 覆盖"轮询恰好错过 0x13/0x14, 首轮 0x02 时读取又失败"等情况。 */
static void retry_cal_capture(uint32_t now)
{
    for (uint8_t index = 0U; index < PSOC_COMM_DEVICE_COUNT; index++)
    {
        uint8_t mask = device_bit(index);

        if ((operational_device_mask & mask) == 0U)
        {
            continue;
        }

        if (cp_valid[index] && comp_idac_valid[index])
        {
            continue;
        }

        if (!elapsed_at_least(now,
                              last_cal_capture_tick[index],
                              TENODATA_CAL_CAPTURE_RETRY_MS))
        {
            continue;
        }

        last_cal_capture_tick[index] = now;
        capture_cal_if_ready(index, (uint8_t)PSOC_STATUS_CALIBRATION_DONE);
    }
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
    else
    {
        /* 状态帧载荷恒为 0, 上位机按载荷去重会连状态字节的变化
         * (0x02→0x01) 一起丢弃, 导致 0x01 请求配置永远不被看到。
         * 放入递增序号使每帧载荷不同, 保证状态字节总被解析。 */
        cdc_tx_frame[1U] = status_frame_seq;
        cdc_tx_frame[1U + PSOC_COMM_PAYLOAD_LENGTH] = status_frame_seq;
        status_frame_seq++;
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
    if (host_config_override_valid)
    {
        /* 上位机下发的扫描参数优先于本地固定表 */
        memcpy(host_config_payload,
               host_config_override,
               sizeof(host_config_payload));
    }
    else
    {
        tenodata_config_get_payload(host_config_payload);
    }

    operational_device_mask = 0U;
    init_target_mask = 0U;
    reset_expected_mask = 0U;
    reset_command_mask = 0U;
    reset_status2_seen_mask = 0U;
    reset_ready_mask = 0U;
    calibrated_device_mask = 0U;
    calibration_report_status = PSOC_STATUS_START_CALIBRATION;
    pipeline_batch_count = 0U;
    /* 重配会让 PSoC 重新测量 Cp 并重跑 SAR 校准, 旧快照作废 */
    memset(cp_valid, 0, sizeof(cp_valid));
    memset(comp_idac_valid, 0, sizeof(comp_idac_valid));
    memset(last_cal_capture_tick, 0, sizeof(last_cal_capture_tick));
    memset(cal_capture_confirm_pending, 0, sizeof(cal_capture_confirm_pending));
    memset(cal_captured_tick, 0, sizeof(cal_captured_tick));
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
    else if (!cdc_manager_uses_touch_pipeline() &&
             !host_config_override_valid)
    {
        /* tenodata 模式: 等待上位机下发扫描参数后再写 PSoC, 无超时 */
        state = STATE_WAIT_HOST_CONFIG;
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

static void consume_host_config_frame(void)
{
    while (cdc_manager_rx_available() > 0U)
    {
        uint8_t byte;

        (void)cdc_manager_rx_read(&byte, 1U);

        /* A raw frame owns its entire payload and checksum, including any
         * bytes resembling a mode command. Between frames CDC0 can switch. */
        if (host_cfg_rx_state != HOST_CFG_WAIT_PAYLOAD || io_mode_is_enabled())
        {
            if (io_mode_observe_rx_byte(byte))
            {
                /* A disable may restore Mai2Touch. Leave trailing bytes
                 * queued for the restored route's normal command parser. */
                return;
            }
            if (io_mode_is_enabled())
            {
                continue;
            }
            touch_mode_observe_byte(byte);
            if (touch_mode_pending_to_mai2touch())
            {
                return;
            }
        }

        switch (host_cfg_rx_state)
        {
        case HOST_CFG_WAIT_AA:
            if (byte == TENODATA_HOST_CFG_HEADER_A)
            {
                host_cfg_rx_state = HOST_CFG_WAIT_01;
            }
            break;

        case HOST_CFG_WAIT_01:
            if (byte == TENODATA_HOST_CFG_HEADER_B)
            {
                host_cfg_rx_state = HOST_CFG_WAIT_PAYLOAD;
                host_cfg_rx_count = 0U;
                host_cfg_rx_checksum =
                    (uint8_t)(TENODATA_HOST_CFG_HEADER_A +
                              TENODATA_HOST_CFG_HEADER_B);
            }
            else
            {
                host_cfg_rx_state = HOST_CFG_WAIT_AA;
            }
            break;

        case HOST_CFG_WAIT_PAYLOAD:
            if (host_cfg_rx_count < TENODATA_CONFIG_PAYLOAD_LENGTH)
            {
                host_cfg_rx_checksum =
                    (uint8_t)(host_cfg_rx_checksum + byte);
                host_cfg_rx_buffer[host_cfg_rx_count] = byte;
                host_cfg_rx_count++;
            }
            else
            {
                /* 最后一个字节是校验和 */
                host_cfg_rx_state = HOST_CFG_WAIT_AA;

                if (byte == host_cfg_rx_checksum)
                {
                    /* 上位机按 1 秒节流重复下发相同配置,
                     * 去重避免无限重新初始化。 */
                    if (!host_config_override_valid ||
                        (memcmp(host_config_override,
                                host_cfg_rx_buffer,
                                TENODATA_CONFIG_PAYLOAD_LENGTH) != 0))
                    {
                        memcpy(host_config_override,
                               host_cfg_rx_buffer,
                               TENODATA_CONFIG_PAYLOAD_LENGTH);
                        host_config_override_valid = true;
                        tenodata_request_reconfigure();
                    }
                }
            }
            break;
        }
    }
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

void tenodata_clear_host_config_override(void)
{
    /* 只清除"上位机扫描参数覆盖"标记, 不触发重配。
     * 用于模式切换等场景: 清掉之后, 若仍处于 tenodata(raw) 模式,
     * 下一次完整初始化会走 STATE_WAIT_HOST_CONFIG 重新向上位机索要参数,
     * 而不是继续沿用上一次的参数。 */
    host_config_override_valid = false;
}

void tenodata_force_host_config_resend(void)
{
    /* 强制上位机重新下发扫描参数:
     * 清掉本地覆盖 + 重走完整初始化, 从而回到 STATE_WAIT_HOST_CONFIG
     * 并重新上报 0x01 请求配置。 */
    host_config_override_valid = false;
    request_initialization();
}

bool tenodata_get_cp(uint8_t device_index, uint8_t *cp_out)
{
    if ((device_index >= PSOC_COMM_DEVICE_COUNT) ||
        (cp_out == NULL) ||
        !cp_valid[device_index])
    {
        return false;
    }

    memcpy(cp_out, cp_raw[device_index], PSOC_COMM_CP_LENGTH);
    return true;
}

bool tenodata_get_comp_idac(uint8_t device_index, uint8_t *comp_out)
{
    if ((device_index >= PSOC_COMM_DEVICE_COUNT) ||
        (comp_out == NULL) ||
        !comp_idac_valid[device_index])
    {
        return false;
    }

    memcpy(comp_out, comp_idac[device_index], PSOC_COMM_COMP_IDAC_LENGTH);
    return true;
}

bool tenodata_get_cal_age_ms(uint8_t device_index, uint16_t *age_ms)
{
    uint32_t age;

    if ((device_index >= PSOC_COMM_DEVICE_COUNT) ||
        (age_ms == NULL) ||
        !cp_valid[device_index])
    {
        return false;
    }

    age = HAL_GetTick() - cal_captured_tick[device_index];
    *age_ms = (age > UINT16_MAX) ? UINT16_MAX : (uint16_t)age;
    return true;
}

TENODATA_SIZE_OPTIMIZED
void tenodata_task(void)
{
    uint32_t now = HAL_GetTick();

    /* mai2touch 模式独占 CDC RX; raw/IO 共用一个接收分发器 */
    if (!cdc_manager_is_mai2touch_active())
    {
        consume_host_config_frame();
    }

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

                    /* 校准进行中顺带抓取 Cp / 补偿 IDAC (各自到点才有效) */
                    capture_cal_if_ready(index, psoc_status);

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

            /* 校准量 (Cp / 补偿 IDAC): 先补抓未拿到的, 再做延迟确认读 */
            if (!read_inflight && psoc_comm_is_bus_ready())
            {
                retry_cal_capture(now);
                confirm_cal_capture(now);
            }

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

                    if (cdc_manager_uses_touch_pipeline() &&
                        (device != NULL) &&
                        device->connected &&
                        (device->raw[0] ==
                         PSOC_STATUS_CALIBRATION_DONE) &&
                        (pipeline_batch_count <
                         TENODATA_PIPELINE_BATCH_PACKETS))
                    {
                        uint8_t *packet =
                            &pipeline_batch[
                                pipeline_batch_count *
                                TOUCH_PIPELINE_PACKET_BYTES];

                        memset(packet,
                               0,
                               TOUCH_PIPELINE_PACKET_BYTES);
                        packet[0] = (uint8_t)(1U << completed_device);
                        memcpy(packet +
                                   TOUCH_PIPELINE_PACKET_MASK_BYTES +
                                   ((size_t)completed_device *
                                    PSOC_COMM_PAYLOAD_LENGTH),
                               &device->raw[1],
                               PSOC_COMM_PAYLOAD_LENGTH);
                        pipeline_batch_count++;
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

        case STATE_WAIT_HOST_CONFIG:
            /* Raw waits for host parameters. Both bitmap routes can use
             * the local scan profile prepared by start_initialization(). */
            if (host_config_override_valid ||
                cdc_manager_uses_touch_pipeline())
            {
                state = STATE_WRITE_CONFIG_TO_PSOC;
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
            case STATE_RUNNING:
                if (cdc_manager_uses_touch_pipeline())
                {
                    if (pipeline_batch_count > 0U)
                    {
                        touch_pipeline_commit_batch(
                            pipeline_batch,
                            pipeline_batch_count);
                        pipeline_batch_count = 0U;
                    }
                    else
                    {
                        touch_pipeline_commit_packet(NULL);
                    }
                }
                cdc_send_frame(0x00U);
                break;

            case STATE_WAIT_CALIBRATION:
                cdc_send_frame(calibration_report_status);
                break;

            case STATE_WAIT_HOST_CONFIG:
                /* 报 0x01 提示上位机下发扫描参数帧 */
                cdc_send_frame(PSOC_STATUS_START_CALIBRATION);
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
