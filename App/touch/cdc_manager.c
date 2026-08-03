#include "cdc_manager.h"
#include "tusb.h"

#define CDC_MANAGER_ITF    0U

static bool is_mai2touch_active = true;

// ================= 路由开关 =================

void cdc_manager_set_mai2touch_active(bool active) {
    is_mai2touch_active = active;
}

bool cdc_manager_is_mai2touch_active(void) {
    return is_mai2touch_active;
}

// ================= 初始化 =================

void cdc_manager_init(void) {
    // 路由开关的默认值由静态初始化决定，此处不做覆盖
}

// ================= 发送 =================

bool cdc_manager_try_send(bool from_mai2touch, const uint8_t *data, uint16_t len) {
    // 路由检查：仅活跃模块的发送请求被放行
    if (from_mai2touch != is_mai2touch_active) {
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
