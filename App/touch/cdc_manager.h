#ifndef __CDC_MANAGER_H__
#define __CDC_MANAGER_H__

#include <stdbool.h>
#include <stdint.h>

// ================= 路由开关 =================

// 设置为 true 时: mai2touch 独占 CDC, tenodata 发送被丢弃
// 设置为 false 时: tenodata 独占 CDC, mai2touch 发送被丢弃
void cdc_manager_set_mai2touch_active(bool active);
bool cdc_manager_is_mai2touch_active(void);

// ================= 初始化 =================

void cdc_manager_init(void);

// ================= 发送 =================

// 尝试发送数据。仅当 caller 是当前活跃模块时才实际发送。
// from_mai2touch: true=来自 mai2touch, false=来自 tenodata
// 返回 true 表示数据已写入 CDC 缓冲区, false 表示被路由屏蔽或硬件未就绪
bool cdc_manager_try_send(bool from_mai2touch, const uint8_t *data, uint16_t len);

// ================= 接收 (无路由, 活跃模块自行调用) =================

// CDC 接收缓冲区中可读取的字节数
uint32_t cdc_manager_rx_available(void);

// 预读一个字节但不消费 (用于帧头检测)
bool cdc_manager_rx_peek(uint8_t *byte_out);

// 从 CDC 接收缓冲区读取数据
uint32_t cdc_manager_rx_read(uint8_t *buf, uint16_t len);

#endif /* __CDC_MANAGER_H__ */
