#ifndef STATUS_LED_APP_H
#define STATUS_LED_APP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

void status_led_app_init(void);
void status_led_app_task(void);
void status_led_app_set_running(void);
void status_led_app_config_write_begin(void);
void status_led_app_config_write_end(bool success);
void status_led_app_set_error(void);

#ifdef __cplusplus
}
#endif

#endif /* STATUS_LED_APP_H */
