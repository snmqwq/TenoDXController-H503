#ifndef __PSOC_COMM_H__
#define __PSOC_COMM_H__

#include <stdbool.h>
#include <stdint.h>

// ================= 常量 =================
#define PSOC_COMM_DEVICE_COUNT          2U
#define PSOC_COMM_I2C_DATA_LENGTH       35U    // Status(1) + 17ch×2byte(34)
#define PSOC_COMM_PAYLOAD_LENGTH        34U    // 仅数据载荷 (不含 Status)
#define PSOC_COMM_CHANNELS_PER_DEVICE   17U
#define PSOC_COMM_CONFIG_LENGTH         68U    // 17ch × 4 参数

// EZI2C 内存映射
#define PSOC_EZI2C_OFFSET_STATUS        0x00U
#define PSOC_EZI2C_OFFSET_CONFIG        0x56U  // = 86

// PSoC 状态码
#define PSOC_STATUS_CRASH               0x00U
#define PSOC_STATUS_START_CALIBRATION   0x01U
#define PSOC_STATUS_CALIBRATION_DONE    0x02U

// ================= 类型 =================
typedef struct {
    uint8_t address;                              // I2C 7-bit 地址
    uint8_t raw[PSOC_COMM_I2C_DATA_LENGTH];       // 最新有效数据 (含 Status)
    uint8_t rx_raw[PSOC_COMM_I2C_DATA_LENGTH];    // DMA 接收缓冲
    bool connected;
} PsocDevice;

// ================= API =================

// 初始化 PSoC 设备列表 (设置地址等), 不访问 I2C
void psoc_comm_init(void);

// 探测 I2C 总线，返回已发现的 PSoC 数量
uint8_t psoc_comm_probe(void);

// 获取已连接设备数
uint8_t psoc_comm_connected_count(void);

// 获取指定设备的指针 (供外部直接读取 raw 数据)
const PsocDevice* psoc_comm_get_device(uint8_t index);

// 将 68 字节配置写入指定 PSoC，并触发校准
void psoc_comm_write_config_and_calibrate(uint8_t device_index, const uint8_t *psoc_cfg_68);

// 将所有 34 通道配置拆分写入两个 PSoC
// config_payload_136: 34×Res + 34×Mod + 34×Sns + 34×Div
void psoc_comm_write_config_all(const uint8_t *config_payload_136);

// 读取 PSoC 状态 (阻塞)
bool psoc_comm_read_status(uint8_t device_index, uint8_t *status_out);

// 检查 I2C 总线是否空闲
bool psoc_comm_is_bus_ready(void);

// 启动异步 I2C 数据读取 (交替轮询用)
bool psoc_comm_start_async_read(uint8_t device_index);

// 异步读取是否完成 (由 I2C 回调设置)
bool psoc_comm_is_read_complete(void);
bool psoc_comm_is_read_error(void);
void psoc_comm_clear_read_flags(void);

// 将异步读取的数据拷贝到设备 raw 缓冲区
void psoc_comm_commit_read(uint8_t device_index);

#endif /* __PSOC_COMM_H__ */
