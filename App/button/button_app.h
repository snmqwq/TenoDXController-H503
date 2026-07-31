#ifndef BUTTON_APP_H
#define BUTTON_APP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#define BUTTON_APP_COUNT       11U
#define BUTTON_APP_MAIN_COUNT   8U

typedef void (*button_app_callback_t)(uint8_t button_id, void *context);

void button_app_init(void);
void button_app_task(void);
bool button_app_is_pressed(uint8_t button_id);
uint16_t button_app_read_main_mask8(void);
bool button_app_set_long_press_callback(uint8_t button_id,
                                        button_app_callback_t callback,
                                        void *context);

#ifdef __cplusplus
}
#endif

#endif /* BUTTON_APP_H */
