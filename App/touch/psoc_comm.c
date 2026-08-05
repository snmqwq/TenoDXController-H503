#include "psoc_comm.h"
#include <string.h>
#include "i2c.h"

// ================= 私有状态 =================
static PsocDevice devices[PSOC_COMM_DEVICE_COUNT];
static uint8_t connected_count = 0;
static volatile bool transfer_complete = false;
static volatile bool transfer_error = false;

static bool probe_device(uint8_t device_index,
                         uint32_t trials,
                         uint32_t timeout_ms)
{
    if (device_index >= PSOC_COMM_DEVICE_COUNT)
    {
        return false;
    }

    if (HAL_I2C_IsDeviceReady(&hi2c1,
                              devices[device_index].address << 1,
                              trials,
                              timeout_ms) == HAL_OK)
    {
        if (!devices[device_index].connected)
        {
            devices[device_index].connected = true;
            connected_count++;
        }
        return true;
    }

    psoc_comm_mark_disconnected(device_index);
    return false;
}

static void build_device_config(const uint8_t *config_payload_136,
                                uint8_t device_index,
                                uint8_t *psoc_cfg)
{
    uint8_t offset =
        (uint8_t)(device_index * PSOC_COMM_CHANNELS_PER_DEVICE);

    memcpy(&psoc_cfg[0],
           &config_payload_136[0U + offset],
           PSOC_COMM_CHANNELS_PER_DEVICE);
    memcpy(&psoc_cfg[17],
           &config_payload_136[34U + offset],
           PSOC_COMM_CHANNELS_PER_DEVICE);
    memcpy(&psoc_cfg[34],
           &config_payload_136[68U + offset],
           PSOC_COMM_CHANNELS_PER_DEVICE);
    memcpy(&psoc_cfg[51],
           &config_payload_136[102U + offset],
           PSOC_COMM_CHANNELS_PER_DEVICE);
}

// ================= I2C 回调 =================
void HAL_I2C_MemRxCpltCallback(I2C_HandleTypeDef *hi2c)
{
    if (hi2c->Instance == I2C1) {
        transfer_complete = true;
    }
}

void HAL_I2C_ErrorCallback(I2C_HandleTypeDef *hi2c)
{
    if (hi2c->Instance == I2C1) {
        transfer_error = true;
    }
}

// ================= 公开 API =================

void psoc_comm_init(void)
{
    memset(devices, 0, sizeof(devices));
    devices[0].address = 0x08U;
    devices[1].address = 0x09U;
    connected_count = 0;
    transfer_complete = false;
    transfer_error = false;
}

uint8_t psoc_comm_probe(void)
{
    for (int i = 0; i < PSOC_COMM_DEVICE_COUNT; i++) {
        (void)psoc_comm_probe_device((uint8_t)i);
    }
    return connected_count;
}

bool psoc_comm_probe_device(uint8_t device_index)
{
    return probe_device(device_index, 3U, 10U);
}

bool psoc_comm_probe_device_quick(uint8_t device_index)
{
    return probe_device(device_index, 1U, 2U);
}

uint8_t psoc_comm_connected_count(void)
{
    return connected_count;
}

uint8_t psoc_comm_get_connected_mask(void)
{
    uint8_t connected_mask = 0U;

    for (uint8_t i = 0U; i < PSOC_COMM_DEVICE_COUNT; i++)
    {
        if (devices[i].connected)
        {
            connected_mask |= (uint8_t)(1U << i);
        }
    }

    return connected_mask;
}

void psoc_comm_mark_disconnected(uint8_t device_index)
{
    if (device_index >= PSOC_COMM_DEVICE_COUNT)
    {
        return;
    }

    if (devices[device_index].connected)
    {
        devices[device_index].connected = false;
        if (connected_count > 0U)
        {
            connected_count--;
        }
    }

    memset(devices[device_index].raw, 0, sizeof(devices[device_index].raw));
    memset(devices[device_index].rx_raw, 0, sizeof(devices[device_index].rx_raw));
}

const PsocDevice* psoc_comm_get_device(uint8_t index)
{
    if (index >= PSOC_COMM_DEVICE_COUNT) return NULL;
    return &devices[index];
}

bool psoc_comm_write_config_and_calibrate(uint8_t device_index,
                                          const uint8_t *psoc_cfg_68)
{
    uint8_t start_cmd = PSOC_STATUS_START_CALIBRATION;

    if ((device_index >= PSOC_COMM_DEVICE_COUNT) ||
        !devices[device_index].connected ||
        (psoc_cfg_68 == NULL))
    {
        return false;
    }

    // 1. 写入 68 字节配置
    if (HAL_I2C_Mem_Write(&hi2c1, devices[device_index].address << 1,
                          PSOC_EZI2C_OFFSET_CONFIG, I2C_MEMADD_SIZE_8BIT,
                          (uint8_t *)psoc_cfg_68,
                          PSOC_COMM_CONFIG_LENGTH, 100) != HAL_OK)
    {
        return false;
    }

    // 2. 等待 PSoC 消化
    HAL_Delay(5);

    // 3. 触发校准
    return HAL_I2C_Mem_Write(&hi2c1, devices[device_index].address << 1,
                             PSOC_EZI2C_OFFSET_STATUS,
                             I2C_MEMADD_SIZE_8BIT,
                             &start_cmd, 1, 10) == HAL_OK;
}

bool psoc_comm_write_config_all(const uint8_t *config_payload_136)
{
    uint8_t target_mask = psoc_comm_get_connected_mask();

    return (target_mask != 0U) &&
           (psoc_comm_write_config_mask(config_payload_136, target_mask) ==
            target_mask);
}

uint8_t psoc_comm_write_config_mask(const uint8_t *config_payload_136,
                                    uint8_t device_mask)
{
    uint8_t psoc_cfg[PSOC_COMM_CONFIG_LENGTH];
    uint8_t written_mask = 0U;

    if (config_payload_136 == NULL)
    {
        return 0U;
    }

    for (uint8_t index = 0U;
         index < PSOC_COMM_DEVICE_COUNT;
         index++)
    {
        uint8_t mask = (uint8_t)(1U << index);

        if (((device_mask & mask) == 0U) || !devices[index].connected)
        {
            continue;
        }

        build_device_config(config_payload_136, index, psoc_cfg);
        if (psoc_comm_write_config_and_calibrate(index, psoc_cfg))
        {
            written_mask |= mask;
        }
    }

    return written_mask;
}

bool psoc_comm_soft_reset(uint8_t device_index)
{
    uint8_t soft_reset_cmd = PSOC_COMMAND_SOFT_RESET;

    if ((device_index >= PSOC_COMM_DEVICE_COUNT) ||
        !devices[device_index].connected)
    {
        return false;
    }

    return HAL_I2C_Mem_Write(&hi2c1,
                             devices[device_index].address << 1,
                             PSOC_EZI2C_OFFSET_STATUS,
                             I2C_MEMADD_SIZE_8BIT,
                             &soft_reset_cmd,
                             1,
                             10) == HAL_OK;
}

uint8_t psoc_comm_soft_reset_all(void)
{
    uint8_t connected_mask = psoc_comm_get_connected_mask();
    uint8_t reset_mask = 0U;

    for (uint8_t i = 0U; i < PSOC_COMM_DEVICE_COUNT; i++)
    {
        uint8_t device_mask = (uint8_t)(1U << i);

        if (((connected_mask & device_mask) != 0U) &&
            psoc_comm_soft_reset(i))
        {
            reset_mask |= device_mask;
        }
    }

    return reset_mask;
}

bool psoc_comm_read_status(uint8_t device_index, uint8_t *status_out)
{
    if (device_index >= PSOC_COMM_DEVICE_COUNT || !devices[device_index].connected)
        return false;

    return HAL_I2C_Mem_Read(&hi2c1, devices[device_index].address << 1,
                            PSOC_EZI2C_OFFSET_STATUS, I2C_MEMADD_SIZE_8BIT,
                            status_out, 1, 10) == HAL_OK;
}

bool psoc_comm_is_bus_ready(void)
{
    return HAL_I2C_GetState(&hi2c1) == HAL_I2C_STATE_READY;
}

void psoc_comm_recover_bus(void)
{
    (void)HAL_I2C_DeInit(&hi2c1);
    MX_I2C1_Init();
    transfer_complete = false;
    transfer_error = false;
}

bool psoc_comm_start_async_read(uint8_t device_index)
{
    if (device_index >= PSOC_COMM_DEVICE_COUNT || !devices[device_index].connected)
        return false;

    return HAL_I2C_Mem_Read_IT(&hi2c1, devices[device_index].address << 1,
                               0x00, I2C_MEMADD_SIZE_8BIT,
                               devices[device_index].rx_raw,
                               PSOC_COMM_I2C_DATA_LENGTH) == HAL_OK;
}

bool psoc_comm_is_read_complete(void)  { return transfer_complete; }
bool psoc_comm_is_read_error(void)     { return transfer_error; }

void psoc_comm_clear_read_flags(void)
{
    transfer_complete = false;
    transfer_error = false;
}

void psoc_comm_commit_read(uint8_t device_index)
{
    if (device_index >= PSOC_COMM_DEVICE_COUNT) return;
    memcpy(devices[device_index].raw, devices[device_index].rx_raw, PSOC_COMM_I2C_DATA_LENGTH);
}
