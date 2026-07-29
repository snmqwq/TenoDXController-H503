#include "psoc_comm.h"
#include <string.h>
#include "i2c.h"

// ================= 私有状态 =================
static PsocDevice devices[PSOC_COMM_DEVICE_COUNT];
static uint8_t connected_count = 0;
static volatile bool transfer_complete = false;
static volatile bool transfer_error = false;

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
    connected_count = 0;
    for (int i = 0; i < PSOC_COMM_DEVICE_COUNT; i++) {
        if (HAL_I2C_IsDeviceReady(&hi2c1, devices[i].address << 1, 3, 10) == HAL_OK) {
            devices[i].connected = true;
            connected_count++;
        } else {
            devices[i].connected = false;
        }
    }
    return connected_count;
}

uint8_t psoc_comm_connected_count(void)
{
    return connected_count;
}

const PsocDevice* psoc_comm_get_device(uint8_t index)
{
    if (index >= PSOC_COMM_DEVICE_COUNT) return NULL;
    return &devices[index];
}

void psoc_comm_write_config_and_calibrate(uint8_t device_index, const uint8_t *psoc_cfg_68)
{
    if (device_index >= PSOC_COMM_DEVICE_COUNT || !devices[device_index].connected) return;

    // 1. 写入 68 字节配置
    HAL_I2C_Mem_Write(&hi2c1, devices[device_index].address << 1,
                      PSOC_EZI2C_OFFSET_CONFIG, I2C_MEMADD_SIZE_8BIT,
                      (uint8_t *)psoc_cfg_68, PSOC_COMM_CONFIG_LENGTH, 100);

    // 2. 等待 PSoC 消化
    HAL_Delay(5);

    // 3. 触发校准
    uint8_t start_cmd = PSOC_STATUS_START_CALIBRATION;
    HAL_I2C_Mem_Write(&hi2c1, devices[device_index].address << 1,
                      PSOC_EZI2C_OFFSET_STATUS, I2C_MEMADD_SIZE_8BIT,
                      &start_cmd, 1, 10);
}

void psoc_comm_write_config_all(const uint8_t *config_payload_136)
{
    uint8_t psoc_cfg[PSOC_COMM_CONFIG_LENGTH];

    for (int i = 0; i < PSOC_COMM_DEVICE_COUNT; i++) {
        if (!devices[i].connected) continue;

        // 拆包：上位机 136 字节 = 34×Res + 34×Mod + 34×Sns + 34×Div
        // Device 0 取通道 0~16, Device 1 取通道 17~33
        int offset = (i == 0) ? 0 : 17;

        memcpy(&psoc_cfg[0],  &config_payload_136[0   + offset], 17); // Resolution
        memcpy(&psoc_cfg[17], &config_payload_136[34  + offset], 17); // Mod IDAC
        memcpy(&psoc_cfg[34], &config_payload_136[68  + offset], 17); // Sense Div
        memcpy(&psoc_cfg[51], &config_payload_136[102 + offset], 17); // Mod Div

        psoc_comm_write_config_and_calibrate(i, psoc_cfg);
    }
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
