#ifndef __KEYBOARD_APP_H__
#define __KEYBOARD_APP_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define KEYBOARD_APP_KEY_COUNT          11U
#define KEYBOARD_APP_MAIN_BUTTON_COUNT  8U

typedef void (*keyboard_app_event_cb_t)(void);

void keyboard_app_init(keyboard_app_event_cb_t btn8_long_press_cb);
void keyboard_app_poll(void);
uint16_t keyboard_app_button_read_mask8(void);

uint8_t *keyboard_app_get_key_map(void);
uint8_t const *keyboard_app_get_default_key_map(void);
uint8_t keyboard_app_get_key_count(void);

#ifdef __cplusplus
}
#endif

#endif /* __KEYBOARD_APP_H__ */
