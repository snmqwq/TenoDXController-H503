#ifndef TOUCH_APP_H
#define TOUCH_APP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "tenodata_config.h"

void touch_app_init(void);
void touch_app_task(void);

bool touch_app_is_mai2touch_active(void);
bool touch_app_get_mapping(TenodataChannelMapping *mapping, uint8_t count);
bool touch_app_get_default_mapping(TenodataChannelMapping *mapping,
                                   uint8_t count);
bool touch_app_apply_config(TenodataChannelMapping const *mapping,
                            uint8_t count,
                            bool mai2touch_active);

#ifdef __cplusplus
}
#endif

#endif /* TOUCH_APP_H */
