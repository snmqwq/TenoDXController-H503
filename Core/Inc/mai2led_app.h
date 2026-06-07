#ifndef __MAI2LED_APP_H__
#define __MAI2LED_APP_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include "ws28xx.h"

#define MAI2LED_APP_CDC_ITF                 1U
#define MAI2LED_APP_DATA_BITS               8U
#define MAI2LED_APP_DEFAULT_LED_PER_BIT     2U
#define MAI2LED_APP_MAX_LED_PER_BIT         4U
#define MAI2LED_APP_DEFAULT_LED_TOTAL       (MAI2LED_APP_DATA_BITS * MAI2LED_APP_DEFAULT_LED_PER_BIT)
#define MAI2LED_APP_MAX_LED_TOTAL           (MAI2LED_APP_DATA_BITS * MAI2LED_APP_MAX_LED_PER_BIT)

typedef struct
{
    WS28XX_HandleTypeDef *led;
    uint8_t led_per_bit;
    uint16_t (*button_read)(void);
} mai2led_app_config_t;

void mai2led_app_init(mai2led_app_config_t const *config);
void mai2led_app_task(void);

void mai2led_app_mark_io_active(void);
bool mai2led_app_io_is_active(void);
void mai2led_app_restore_idle_lights(void);

void mai2led_app_set_led_per_bit(uint8_t led_per_bit);
uint8_t mai2led_app_get_led_per_bit(void);
uint16_t mai2led_app_get_led_total(void);
void mai2led_app_set_rainbow_mode(bool enabled);
bool mai2led_app_get_rainbow_mode(void);

bool mai2led_app_load_config_from_flash(void);
bool mai2led_app_save_config_to_flash(void);

#ifdef __cplusplus
}
#endif

#endif /* __MAI2LED_APP_H__ */
