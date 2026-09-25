#ifndef __CDC_MANAGER_H__
#define __CDC_MANAGER_H__

#include <stdbool.h>
#include <stdint.h>

// ================= 路由开关 =================

// 输出模式。AUTO 模式下由 touch_mode 根据收到的帧特征自动切换:
//   · 收到 `0xAA 0x01`(raw 调试帧头) -> 切到 RAW
//   · 收到 `{`(Mai2Touch 命令起始符) -> 切到 MAI2TOUCH
typedef enum
{
    CDC_MODE_AUTO_RAW = 0,      // 自动识别, 当前按 raw 工作
    CDC_MODE_AUTO_MAI2TOUCH,    // 自动识别, 当前按 Mai2Touch 工作
    CDC_MODE_FORCED_RAW,        // Magic 强制 raw
    CDC_MODE_FORCED_MAI2TOUCH   // Magic 强制 Mai2Touch
} cdc_mode_t;

// output 侧切换请求。值没变时是 no-op —— 保持 AUTO 不被误关
// (touch_app_apply_config 每次配置变更都会按"当前模式"调它)。
void cdc_manager_set_mai2touch_active(bool active);

// 当前是否按 Mai2Touch 工作 (AUTO 与 FORCED 两种来源都算)
bool cdc_manager_is_mai2touch_active(void);

// 是否处于自动识别模式
bool cdc_manager_is_auto_mode(void);

// 当前模式 (含来源)
cdc_mode_t cdc_manager_get_mode(void);

// 由 touch_mode 自动切换时调用 (只在 AUTO 模式下生效)
void cdc_manager_set_auto_detected(bool mai2touch);

// 显式回到自动识别模式 (上电默认; 也用于解除 Magic 的强制设定)
void cdc_manager_set_auto_mode(bool mai2touch);

// 无条件强制到某一侧 (Magic 显式写入 cdc_mode 0/1 时调用):
// 即使输出侧本来就在这一侧, 也要把"强制"标记立起来, 关掉自动识别。
void cdc_manager_force_mode(bool mai2touch);

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
