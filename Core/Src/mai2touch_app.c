#include "mai2touch_app.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "i2c.h"
#include "tusb.h"

#define MAI2TOUCH_CDC_ITF                  0U
#define MAI2TOUCH_DEVICE_COUNT             2U
#define MAI2TOUCH_I2C_DATA_LENGTH          35U
#define MAI2TOUCH_DEVICE_PAYLOAD_LENGTH    34U
#define MAI2TOUCH_CDC_FRAME_LENGTH         70U
#define MAI2TOUCH_CDC_PERIOD_MS            5U

// 新增：适配上位机 34 通道下发的长度定义
#define MAI2TOUCH_HOST_RX_FRAME_LENGTH     139U
#define MAI2TOUCH_HOST_CONFIG_LENGTH       136U

// EZI2C 内存映射偏移
#define EZI2C_OFFSET_STATUS                0U
#define EZI2C_OFFSET_CONFIG                86U

// 内部状态机
typedef enum {
    APP_STATE_INIT_WAIT = 0,
    APP_STATE_WAIT_HOST_CONFIG,
    APP_STATE_WRITE_CONFIG_TO_PSOC,
    APP_STATE_WAIT_CALIBRATION,
    APP_STATE_RUNNING
} mai2touch_app_state_t;

typedef struct {
    uint8_t address;
    uint8_t raw[MAI2TOUCH_I2C_DATA_LENGTH];
    uint8_t rx_raw[MAI2TOUCH_I2C_DATA_LENGTH];
    bool connected;
} mai2touch_device_t;

static mai2touch_device_t devices[MAI2TOUCH_DEVICE_COUNT];
static mai2touch_app_state_t app_state;
static uint8_t cdc_tx_frame[MAI2TOUCH_CDC_FRAME_LENGTH];
// 扩大缓冲区以容纳 34 通道的配置数据 (34*4 = 136 字节)
static uint8_t host_config_payload[MAI2TOUCH_HOST_CONFIG_LENGTH];
static uint32_t next_cdc_tick;
static uint8_t active_device = 0;
static volatile bool i2c_transfer_complete = false;
static volatile bool i2c_transfer_error = false;

static bool tick_due(uint32_t now, uint32_t due) {
    return (int32_t)(now - due) >= 0;
}

void mai2touch_app_init(void) {
    uint32_t now = HAL_GetTick();
    memset(devices, 0, sizeof(devices));

    devices[0].address = 0x08U;
    devices[1].address = 0x09U;

    app_state = APP_STATE_INIT_WAIT;
    next_cdc_tick = now;
}

// 采用 Peek 预查法的安全流式解析，彻底解决缓冲溢出与数据错位
static void process_cdc_rx(void) {
    // 缓冲区加大到 139 字节
    uint8_t rx_buf[MAI2TOUCH_HOST_RX_FRAME_LENGTH];

    // 只要缓冲池里存在 >= 139 字节，就一直尝试提取完整的配置包
    while (tud_cdc_n_available(MAI2TOUCH_CDC_ITF) >= MAI2TOUCH_HOST_RX_FRAME_LENGTH) {
        uint8_t header;
        tud_cdc_n_peek(MAI2TOUCH_CDC_ITF, &header);

        // 找帧头，如果不是 0xAA 就扔掉这 1 个坏字节，继续下一轮 while
        if (header != 0xAA) {
            tud_cdc_n_read(MAI2TOUCH_CDC_ITF, &header, 1);
            continue;
        }

        // 到这里说明对齐了帧头，放心读出 139 字节
        tud_cdc_n_read(MAI2TOUCH_CDC_ITF, rx_buf, MAI2TOUCH_HOST_RX_FRAME_LENGTH);

        // 校验指令码和 Checksum
        if (rx_buf[1] == 0x01) {
            uint8_t sum = 0;
            for (int i = 0; i < MAI2TOUCH_HOST_RX_FRAME_LENGTH - 1; i++) {
                sum += rx_buf[i];
            }

            // 如果校验和正确，把 136 字节的配置载荷扣出来
            if (sum == rx_buf[MAI2TOUCH_HOST_RX_FRAME_LENGTH - 1]) {
                memcpy(host_config_payload, &rx_buf[2], MAI2TOUCH_HOST_CONFIG_LENGTH);
                app_state = APP_STATE_WRITE_CONFIG_TO_PSOC;
            }
        }
    }
}

void mai2touch_app_task(void) {
    uint32_t now = HAL_GetTick();

    // 1. 监控上位机配置下发
    process_cdc_rx();

    // 2. 主控制逻辑
    switch (app_state) {
        case APP_STATE_INIT_WAIT:
        {
            // 防跳过保护：必须有至少 1 颗 PSoC 回应，才进入下一阶段
            bool any_found = false;
            for(int i = 0; i < MAI2TOUCH_DEVICE_COUNT; i++) {
                if (HAL_I2C_IsDeviceReady(&hi2c1, devices[i].address << 1, 3, 10) == HAL_OK) {
                    devices[i].connected = true;
                    any_found = true;
                } else {
                    devices[i].connected = false;
                }
            }
            if (any_found) {
                app_state = APP_STATE_WAIT_HOST_CONFIG;
            } else {
                HAL_Delay(50); // 若都没准备好，略微延时等待硬件复位完成
            }
            break;
        }

        case APP_STATE_WAIT_HOST_CONFIG:
            // 此时由下方的 60Hz 推流器不断向 Python 发送 0x01
            break;

        case APP_STATE_WRITE_CONFIG_TO_PSOC:
        {
            uint8_t psoc_cfg[68];

            for(int i = 0; i < MAI2TOUCH_DEVICE_COUNT; i++) {
                if(devices[i].connected) {
                    // ==============================================================
                    // 【关键修复：数据拆包与路由】
                    // 上位机数组排列为：34个Res + 34个Mod + 34个Sns + 34个Div
                    // Device 0 只需要 0~16 号通道数据；Device 1 只需要 17~33 号通道数据
                    // ==============================================================
                    int offset = (i == 0) ? 0 : 17;

                    // 提取属于当前 PSoC 的 17 个字节
                    memcpy(&psoc_cfg[0],  &host_config_payload[0 + offset], 17);   // Resolution
                    memcpy(&psoc_cfg[17], &host_config_payload[34 + offset], 17);  // Mod IDAC
                    memcpy(&psoc_cfg[34], &host_config_payload[68 + offset], 17);  // Sense Div
                    memcpy(&psoc_cfg[51], &host_config_payload[102 + offset], 17); // Mod Div

                    // 1. 写入拆解后的 68 字节配置
                    HAL_I2C_Mem_Write(&hi2c1, devices[i].address << 1, EZI2C_OFFSET_CONFIG, I2C_MEMADD_SIZE_8BIT, psoc_cfg, 68, 100);

                    // 增加 5ms 延时，给 PSoC 留出消化这 68 字节的缓冲时间
                    HAL_Delay(5);

                    // 2. 将 PSoC 的状态字改写为 0x01，触发 PSoC 启动校准
                    uint8_t start_cmd = 0x01;
                    HAL_I2C_Mem_Write(&hi2c1, devices[i].address << 1, EZI2C_OFFSET_STATUS, I2C_MEMADD_SIZE_8BIT, &start_cmd, 1, 10);
                }
            }
            app_state = APP_STATE_WAIT_CALIBRATION;
            break;
        }

        case APP_STATE_WAIT_CALIBRATION:
        {
            // 防 DDOS 攻击：加入 100ms 轮询间隔，防止过快的 I2C 查询把 PSoC 处理器卡死
            static uint32_t last_calib_check = 0;
            if (now - last_calib_check > 100) {
                last_calib_check = now;
                bool all_calibrated = true;

                for(int i = 0; i < MAI2TOUCH_DEVICE_COUNT; i++) {
                    if(devices[i].connected) {
                        uint8_t psoc_status = 0xFF;
                        if(HAL_I2C_Mem_Read(&hi2c1, devices[i].address << 1, EZI2C_OFFSET_STATUS, I2C_MEMADD_SIZE_8BIT, &psoc_status, 1, 10) == HAL_OK) {

                            // 记录 PSoC 的实时进度！
                            devices[i].raw[0] = psoc_status;

                            if(psoc_status == 0x00) {
                                // PSoC 发生了重启崩溃，立刻退回重走流程
                                app_state = APP_STATE_WAIT_HOST_CONFIG;
                                return;
                            } else if(psoc_status != 0x02) {
                                // 还在 0x11 ~ 0x15 之间，继续等
                                all_calibrated = false;
                            }
                        } else {
                            all_calibrated = false;
                        }
                    }
                }
                if(all_calibrated) {
                    app_state = APP_STATE_RUNNING;
                }
            }
            break;
        }

        case APP_STATE_RUNNING:
            if (HAL_I2C_GetState(&hi2c1) == HAL_I2C_STATE_READY) {
                // 1. 处理上一次接收的结果
                if (i2c_transfer_complete || i2c_transfer_error) {
                    if (i2c_transfer_complete) {
                        memcpy(devices[active_device].raw, devices[active_device].rx_raw, MAI2TOUCH_I2C_DATA_LENGTH);

                        // 依然保留神级自愈机制
                        if (devices[active_device].raw[0] == 0x00) {
                            app_state = APP_STATE_WAIT_HOST_CONFIG;
                            return;
                        }
                    }
                    i2c_transfer_complete = false;
                    i2c_transfer_error = false;
                    active_device = (active_device + 1) % MAI2TOUCH_DEVICE_COUNT;
                }

                // 2. 每隔 8ms 发起下一次通讯 (防 DDOS)
                static uint32_t last_i2c_poll = 0;
                if (now - last_i2c_poll >= 8) {
                    last_i2c_poll = now;
                    if (devices[active_device].connected) {

                        // 改用 Mem_Read_IT，一次性安全完成指针跳转与数据读取
                        if(HAL_I2C_Mem_Read_IT(&hi2c1, devices[active_device].address << 1, 0x00, I2C_MEMADD_SIZE_8BIT, devices[active_device].rx_raw, MAI2TOUCH_I2C_DATA_LENGTH) != HAL_OK) {
                            i2c_transfer_error = true;
                        }

                    } else {
                        active_device = (active_device + 1) % MAI2TOUCH_DEVICE_COUNT;
                    }
                }
            }
            break;
    }

    // 3. CDC 60Hz 定时推流
    if (tick_due(now, next_cdc_tick)) {
        next_cdc_tick = now + MAI2TOUCH_CDC_PERIOD_MS;
        memset(cdc_tx_frame, 0, MAI2TOUCH_CDC_FRAME_LENGTH);

        if (app_state == APP_STATE_WAIT_HOST_CONFIG) {
            cdc_tx_frame[0] = 0x01;
        } else if (app_state == APP_STATE_WRITE_CONFIG_TO_PSOC) {
            cdc_tx_frame[0] = 0x02;
        } else if (app_state == APP_STATE_WAIT_CALIBRATION) {
            // 将 T0 内部的真实执行状态透传给 Python 看板
            cdc_tx_frame[0] = devices[0].raw[0];
        } else {
            cdc_tx_frame[0] = 0x00;
            for (int i = 0; i < MAI2TOUCH_DEVICE_COUNT; i++) {
                if (devices[i].connected) {
                    memcpy(&cdc_tx_frame[1 + i * MAI2TOUCH_DEVICE_PAYLOAD_LENGTH],
                           &devices[i].raw[1],
                           MAI2TOUCH_DEVICE_PAYLOAD_LENGTH);
                }
            }
        }

        uint8_t checksum = 0;
        for (int i = 0; i < MAI2TOUCH_CDC_FRAME_LENGTH - 1; i++) {
            checksum += cdc_tx_frame[i];
        }
        cdc_tx_frame[MAI2TOUCH_CDC_FRAME_LENGTH - 1] = checksum;

        if (tud_cdc_n_ready(MAI2TOUCH_CDC_ITF) && tud_cdc_n_write_available(MAI2TOUCH_CDC_ITF) >= MAI2TOUCH_CDC_FRAME_LENGTH) {
            if (tud_cdc_n_write(MAI2TOUCH_CDC_ITF, cdc_tx_frame, MAI2TOUCH_CDC_FRAME_LENGTH) == MAI2TOUCH_CDC_FRAME_LENGTH) {
                tud_cdc_n_write_flush(MAI2TOUCH_CDC_ITF);
            }
        }
    }
}

// ----------------------------------------------------
// 使用 Mem_Read 专用回调
// ----------------------------------------------------
void HAL_I2C_MemRxCpltCallback(I2C_HandleTypeDef *hi2c) {
    if (hi2c->Instance == I2C1 && app_state == APP_STATE_RUNNING) {
        i2c_transfer_complete = true;
    }
}

void HAL_I2C_ErrorCallback(I2C_HandleTypeDef *hi2c) {
    if (hi2c->Instance == I2C1 && app_state == APP_STATE_RUNNING) {
        i2c_transfer_error = true;
    }
}
