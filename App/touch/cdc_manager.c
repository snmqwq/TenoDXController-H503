#include "cdc_manager.h"
#include "tusb.h"

#define CDC_MANAGER_ITF    0U

/* 默认自动识别。上电时没有任何一方在说话, 谁是活跃模块由
 * touch_mode 依收到的帧特征决定; 这里先按 raw 待机,
 * 这样没接游戏时上位机的调试链路可以直接用。 */
static cdc_mode_t current_mode = CDC_MODE_AUTO_RAW;

// ================= 路由开关 =================

void cdc_manager_set_mai2touch_active(bool active) {
    /* 兼容旧接口 + 保持自动识别的语义:
     *   · 与当前工作模式一致 -> 保持自动模式 (touch_app_apply_config 会在
     *     每次配置变更时按"当前模式"调用本函数, 若无条件变成强制模式,
     *     自动识别就被永久关掉了)
     *   · 与当前模式不同 -> 视为显式切换, 退出自动识别
     * Magic 想强制固定模式时, 传一个与当前相反的值得以生效。 */
    bool mai2touch_now = cdc_manager_is_mai2touch_active();

    if (active == mai2touch_now) {
        return;     /* 无变化: 既不切模式也不放弃自动识别 */
    }

    current_mode = active ? CDC_MODE_FORCED_MAI2TOUCH : CDC_MODE_FORCED_RAW;
}

bool cdc_manager_is_mai2touch_active(void) {
    return (current_mode == CDC_MODE_AUTO_MAI2TOUCH) ||
           (current_mode == CDC_MODE_FORCED_MAI2TOUCH);
}

bool cdc_manager_is_auto_mode(void) {
    return (current_mode == CDC_MODE_AUTO_RAW) ||
           (current_mode == CDC_MODE_AUTO_MAI2TOUCH);
}

cdc_mode_t cdc_manager_get_mode(void) {
    return current_mode;
}

void cdc_manager_set_auto_detected(bool mai2touch) {
    /* 只有自动模式才允许被识别结果改写, Magic 的强制设定优先 */
    if (!cdc_manager_is_auto_mode()) {
        return;
    }

    current_mode = mai2touch ? CDC_MODE_AUTO_MAI2TOUCH
                             : CDC_MODE_AUTO_RAW;
}

void cdc_manager_set_auto_mode(bool mai2touch) {
    current_mode = mai2touch ? CDC_MODE_AUTO_MAI2TOUCH
                             : CDC_MODE_AUTO_RAW;
}

void cdc_manager_force_mode(bool mai2touch) {
    /* 无条件强制 (Magic 显式写入 0/1 时调用):
     * 即使当前输出侧本来就在这一侧, 也要把"强制"标记立起来,
     * 否则自动识别仍然会把模式带走。 */
    current_mode = mai2touch ? CDC_MODE_FORCED_MAI2TOUCH
                             : CDC_MODE_FORCED_RAW;
}

// ================= 初始化 =================

void cdc_manager_init(void) {
    // 路由开关的默认值由静态初始化决定，此处不做覆盖
}

// ================= 发送 =================

bool cdc_manager_try_send(bool from_mai2touch, const uint8_t *data, uint16_t len) {
    // 路由检查：仅活跃模块的发送请求被放行
    if (from_mai2touch != cdc_manager_is_mai2touch_active()) {
        return false;  // 被屏蔽
    }

    // 硬件检查
    if (!tud_cdc_n_ready(CDC_MANAGER_ITF)) return false;
    if (tud_cdc_n_write_available(CDC_MANAGER_ITF) < len) return false;

    uint32_t written = tud_cdc_n_write(CDC_MANAGER_ITF, data, len);
    if (written == len) {
        tud_cdc_n_write_flush(CDC_MANAGER_ITF);
        return true;
    }
    return false;
}

// ================= 接收 =================

uint32_t cdc_manager_rx_available(void) {
    return tud_cdc_n_available(CDC_MANAGER_ITF);
}

bool cdc_manager_rx_peek(uint8_t *byte_out) {
    if (tud_cdc_n_available(CDC_MANAGER_ITF) < 1) return false;
    return tud_cdc_n_peek(CDC_MANAGER_ITF, byte_out);
}

uint32_t cdc_manager_rx_read(uint8_t *buf, uint16_t len) {
    return tud_cdc_n_read(CDC_MANAGER_ITF, buf, len);
}
