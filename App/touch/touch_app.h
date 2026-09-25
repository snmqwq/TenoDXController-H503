#ifndef TOUCH_APP_H
#define TOUCH_APP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "tenodata.h"
#include "tenodata_config.h"

void touch_app_init(void);
void touch_app_task(void);

bool touch_app_is_mai2touch_active(void);

/* 是否处于自动识别模式 (未被 Magic 强制固定到某一侧) */
bool touch_app_is_auto_mode(void);

/* 摘掉"强制模式"标记, 回到自动识别 (Magic cdc_mode = 2) */
void touch_app_set_auto_mode(void);

/* 强制固定到某一侧 (Magic cdc_mode = 0/1); 优先级高于自动识别 */
void touch_app_force_cdc_mode(bool mai2touch);

bool touch_app_get_mapping(TenodataChannelMapping *mapping, uint8_t count);
bool touch_app_get_default_mapping(TenodataChannelMapping *mapping,
                                   uint8_t count);
bool touch_app_apply_config(TenodataChannelMapping const *mapping,
                            uint8_t count,
                            bool mai2touch_active,
                            bool force_host_config_resend);
bool touch_app_get_psoc_status(TenodataStatusSnapshot *snapshot);

/* Magic touch 0x05: 强制上位机重新下发扫描参数 (回 0x01 请求配置)。 */
void touch_app_force_host_config_resend(void);

/* Magic touch 0x06: 读取某片的寄生电容 Cp 原始字节 (17 通道 × 2 字节大端)。
 * 返回 false 表示该片没有有效快照 (尚未校准成功或刚重配)。 */
bool touch_app_get_cp(uint8_t device_index, uint8_t *cp_out);

/* Magic touch 0x06: 读取某片的补偿 IDAC (17 通道 × 1 字节)。 */
bool touch_app_get_comp_idac(uint8_t device_index, uint8_t *comp_out);

/* Magic touch 0x06: 某片校准量快照的年龄(ms)；无有效快照返回 false。 */
bool touch_app_get_cal_age_ms(uint8_t device_index, uint16_t *age_ms);

#ifdef __cplusplus
}
#endif

#endif /* TOUCH_APP_H */
