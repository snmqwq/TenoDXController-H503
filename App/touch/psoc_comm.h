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
#define PSOC_COMMAND_SOFT_RESET         0xADU

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

// 探测指定 PSoC，并同步连接状态和已连接设备数
bool psoc_comm_probe_device(uint8_t device_index);

// 运行中快速探测指定 PSoC，避免长时间阻塞在线设备采样
bool psoc_comm_probe_device_quick(uint8_t device_index);

// 获取已连接设备数
uint8_t psoc_comm_connected_count(void);

// 获取连接状态位掩码，bit n 对应设备 n
uint8_t psoc_comm_get_connected_mask(void);

// 标记指定 PSoC 离线，并清空该设备的接收数据
void psoc_comm_mark_disconnected(uint8_t device_index);

// 获取指定设备的指针 (供外部直接读取 raw 数据)
const PsocDevice* psoc_comm_get_device(uint8_t index);

// 将 68 字节配置写入指定 PSoC，并触发校准
bool psoc_comm_write_config_and_calibrate(uint8_t device_index,
                                          const uint8_t *psoc_cfg_68);

// 将所有 34 通道配置拆分写入两个 PSoC
// config_payload_136: 34×Res + 34×Mod + 34×Sns + 34×Div
bool psoc_comm_write_config_all(const uint8_t *config_payload_136);

// 仅配置 device_mask 指定的设备，返回成功设备位掩码
uint8_t psoc_comm_write_config_mask(const uint8_t *config_payload_136,
                                    uint8_t device_mask);

// 向指定 PSoC 的状态偏移写入软复位命令
bool psoc_comm_soft_reset(uint8_t device_index);

// 尝试复位所有当前已连接设备，返回成功设备位掩码
uint8_t psoc_comm_soft_reset_all(void);

// 读取 PSoC 状态 (阻塞)
bool psoc_comm_read_status(uint8_t device_index, uint8_t *status_out);

// 检查 I2C 总线是否空闲
bool psoc_comm_is_bus_ready(void);

// 中止异常事务并重新初始化 I2C1 外设
void psoc_comm_recover_bus(void);

// 启动异步 I2C 数据读取 (交替轮询用)
bool psoc_comm_start_async_read(uint8_t device_index);

// 异步读取是否完成 (由 I2C 回调设置)
bool psoc_comm_is_read_complete(void);
bool psoc_comm_is_read_error(void);
void psoc_comm_clear_read_flags(void);

// 将异步读取的数据拷贝到设备 raw 缓冲区
void psoc_comm_commit_read(uint8_t device_index);

#endif /* __PSOC_COMM_H__ */
