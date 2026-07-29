#ifndef __TENODATA_CONFIG_H__
#define __TENODATA_CONFIG_H__

#include <stdint.h>

#define TENODATA_TOTAL_CHANNELS        34U
#define TENODATA_CHANNELS_PER_DEVICE   17U

// 单个通道硬件扫描参数
// 与 C# 端 HardwareConfig.ScanParams 对应
typedef struct {
    uint8_t res;   // 扫描分辨率
    uint8_t mod;   // 调制电流 (Mod IDAC)
    uint8_t sns;   // 感应分频 (Sense Div)
    uint8_t div;   // 调制分频 (Mod Div)
} TenodataChannelConfig;

// 读取单通道配置
TenodataChannelConfig tenodata_config_get_channel(uint8_t channel);

// 生成 136 字节完整配置载荷
// 格式: 34×Res + 34×Mod + 34×Sns + 34×Div
// 与上位机下发的 host_config_payload 结构一致
void tenodata_config_get_payload(uint8_t *payload);

#endif /* __TENODATA_CONFIG_H__ */
