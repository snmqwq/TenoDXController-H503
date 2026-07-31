#ifndef KEYBOARD_APP_H
#define KEYBOARD_APP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#define KEYBOARD_APP_KEY_COUNT         11U
#define KEYBOARD_APP_CONFIG_KEY_FIRST   8U
#define KEYBOARD_APP_CONFIG_KEY_COUNT   3U

void keyboard_app_init(void);
void keyboard_app_task(void);

bool keyboard_app_get_keycode(uint8_t index, uint8_t *out_keycode);
bool keyboard_app_set_keycode(uint8_t index, uint8_t keycode);
bool keyboard_app_get_config_keycodes(uint8_t *data, uint8_t length);
bool keyboard_app_set_config_keycodes(uint8_t const *data, uint8_t length);
void keyboard_app_reset_keycodes(void);

#ifdef __cplusplus
}
#endif

#endif /* KEYBOARD_APP_H */
