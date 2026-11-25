#ifndef BUTTON_APP_H
#define BUTTON_APP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#define BUTTON_APP_COUNT       12U
#define BUTTON_APP_MAIN_COUNT   8U

/*
 * BTN0..BTN7 (main keys) are active-high; BTN8..BTN11 (config keys) are
 * active-low. See button_app.c for the GPIO wiring behind this choice.
 *
 * BTN0..BTN7 are driven by the key panel, so the pin already carries a clean
 * digital level; going through MultiButton's debounce would only add up to
 * DEBOUNCE_TICKS * BUTTON_APP_TICK_INTERVAL_MS of input lag. The latency
 * critical consumers (HID keyboard, CDC0 IO mode, idle key lights) therefore
 * read the main keys immediately. BTN8..BTN11 are bare switches and keep the
 * debounced state.
 *
 * A main key that reads high at power-up is held pending until a stable low
 * (released) level confirms its presence. Config keys are enabled at boot.
 */

typedef void (*button_app_callback_t)(uint8_t button_id, void *context);

void button_app_init(void);
void button_app_task(void);

/* Debounced MultiButton state: config keys and long-press handling. */
bool button_app_is_pressed(uint8_t button_id);

/*
 * Game input state. Main keys (BTN0..BTN7) are read straight from the GPIO so
 * a press is visible on the very next scan; config keys (BTN8..BTN11) fall
 * back to the debounced state.
 */
bool button_app_is_pressed_immediate(uint8_t button_id);

bool button_app_is_enabled(uint8_t button_id);

/* Immediate mask of the installed main keys (bit N = BTN N). */
uint16_t button_app_read_main_mask8(void);

bool button_app_set_long_press_callback(uint8_t button_id,
                                        button_app_callback_t callback,
                                        void *context);

#ifdef __cplusplus
}
#endif

#endif /* BUTTON_APP_H */
