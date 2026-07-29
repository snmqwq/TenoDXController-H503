#include "tenodata.h"
#include "tenodata_config.h"
#include "psoc_comm.h"
#include "cdc_manager.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// ================= CDC 通讯常量 =================
#define TENODATA_CDC_FRAME_LENGTH      70U
#define TENODATA_CDC_PERIOD_MS         5U

// ================= 内部状态机 =================
typedef enum {
    STATE_INIT_WAIT = 0,
    STATE_WRITE_CONFIG_TO_PSOC,
    STATE_WAIT_CALIBRATION,
    STATE_RUNNING
} tenodata_state_t;

// ================= 私有变量 =================
static tenodata_state_t state;
static uint8_t cdc_tx_frame[TENODATA_CDC_FRAME_LENGTH];
static uint32_t next_cdc_tick;
static uint8_t active_device = 0;
static uint8_t host_config_payload[136];  // 从本地配置读取

// ================= 工具函数 =================
static bool tick_due(uint32_t now, uint32_t due) {
    return (int32_t)(now - due) >= 0;
}

// 组装 CDC 上行帧并发送
static void cdc_send_frame(uint8_t status) {
    memset(cdc_tx_frame, 0, TENODATA_CDC_FRAME_LENGTH);
    cdc_tx_frame[0] = status;

    if (status == 0x00) {
        // RUNNING: 打包两个 PSoC 的数据
        for (int i = 0; i < PSOC_COMM_DEVICE_COUNT; i++) {
            const PsocDevice *dev = psoc_comm_get_device(i);
            if (dev && dev->connected) {
                // 跳过 Status 字节 (raw[0])，拷贝 34 字节有效载荷
                memcpy(&cdc_tx_frame[1 + i * PSOC_COMM_PAYLOAD_LENGTH],
                       &dev->raw[1],
                       PSOC_COMM_PAYLOAD_LENGTH);
            }
        }
    }

    // 计算 checksum
    uint8_t checksum = 0;
    for (int i = 0; i < TENODATA_CDC_FRAME_LENGTH - 1; i++) {
        checksum += cdc_tx_frame[i];
    }
    cdc_tx_frame[TENODATA_CDC_FRAME_LENGTH - 1] = checksum;

    // 通过 CDC 管理层发送 (from_mai2touch=false 表示来自 tenodata)
    cdc_manager_try_send(false, cdc_tx_frame, TENODATA_CDC_FRAME_LENGTH);
}

// ================= 公开 API =================

void tenodata_init(void) {
    uint32_t now = HAL_GetTick();

    // 初始化子模块
    psoc_comm_init();

    // 从本地配置生成 136 字节硬件参数载荷 (无需等待上位机下发)
    tenodata_config_get_payload(host_config_payload);

    state = STATE_INIT_WAIT;
    next_cdc_tick = now;
}

void tenodata_task(void) {
    uint32_t now = HAL_GetTick();

    // ---- 状态机 ----
    switch (state) {

        case STATE_INIT_WAIT: {
            if (psoc_comm_probe() > 0) {
                // 直接使用本地配置，跳过等待上位机下发
                state = STATE_WRITE_CONFIG_TO_PSOC;
            } else {
                HAL_Delay(50);
            }
            break;
        }

        case STATE_WRITE_CONFIG_TO_PSOC: {
            // 将本地配置写入所有 PSoC
            psoc_comm_write_config_all(host_config_payload);
            state = STATE_WAIT_CALIBRATION;
            break;
        }

        case STATE_WAIT_CALIBRATION: {
            static uint32_t last_calib_check = 0;
            if (now - last_calib_check > 100) {
                last_calib_check = now;
                bool all_calibrated = true;

                for (int i = 0; i < PSOC_COMM_DEVICE_COUNT; i++) {
                    const PsocDevice *dev = psoc_comm_get_device(i);
                    if (!dev || !dev->connected) continue;

                    uint8_t psoc_status = 0xFF;
                    if (psoc_comm_read_status(i, &psoc_status)) {
                        if (psoc_status == PSOC_STATUS_CRASH) {
                            // PSoC 崩溃，退回重配
                            state = STATE_WRITE_CONFIG_TO_PSOC;
                            return;
                        } else if (psoc_status != PSOC_STATUS_CALIBRATION_DONE) {
                            all_calibrated = false;
                        }
                    } else {
                        all_calibrated = false;
                    }
                }

                if (all_calibrated) {
                    state = STATE_RUNNING;
                }
            }
            break;
        }

        case STATE_RUNNING: {
            if (!psoc_comm_is_bus_ready()) break;

            // 1. 处理上一次异步读取的结果
            if (psoc_comm_is_read_complete() || psoc_comm_is_read_error()) {
                if (psoc_comm_is_read_complete()) {
                    psoc_comm_commit_read(active_device);

                    // 自愈：PSoC 崩溃则退回重配
                    const PsocDevice *dev = psoc_comm_get_device(active_device);
                    if (dev && dev->raw[0] == PSOC_STATUS_CRASH) {
                        state = STATE_WRITE_CONFIG_TO_PSOC;
                        psoc_comm_clear_read_flags();
                        return;
                    }
                }
                psoc_comm_clear_read_flags();
                active_device = (active_device + 1) % PSOC_COMM_DEVICE_COUNT;
            }

            // 2. 对当前设备发起异步读取 (8ms 间隔)
            static uint32_t last_i2c_poll = 0;
            if (now - last_i2c_poll >= 8) {
                last_i2c_poll = now;
                const PsocDevice *dev = psoc_comm_get_device(active_device);
                if (dev && dev->connected) {
                    psoc_comm_start_async_read(active_device);
                } else {
                    active_device = (active_device + 1) % PSOC_COMM_DEVICE_COUNT;
                }
            }
            break;
        }
    }

    // ---- CDC 60Hz 定时推流 ----
    if (tick_due(now, next_cdc_tick)) {
        next_cdc_tick = now + TENODATA_CDC_PERIOD_MS;

        switch (state) {
            case STATE_WRITE_CONFIG_TO_PSOC:
                cdc_send_frame(0x02);
                break;
            case STATE_WAIT_CALIBRATION: {
                // 透传 PSoC 0 的校准进度
                const PsocDevice *dev = psoc_comm_get_device(0);
                cdc_send_frame((dev && dev->connected) ? dev->raw[0] : 0x00);
                break;
            }
            case STATE_RUNNING:
                cdc_send_frame(0x00);  // 正常数据帧
                break;
            default:
                // STATE_INIT_WAIT: 不发数据
                break;
        }
    }
}
