#include "config_app.h"

#include "debug_cdc.h"
#include "keyboard_config.h"
#include "led_config.h"
#include "magic_config.h"
#include "touch_config.h"

bool config_app_init(void)
{
    bool keyboard_ok;
    bool led_ok;
    bool touch_ok;

    magic_config_init();
    touch_ok = touch_config_init();
    keyboard_ok = keyboard_config_init();
    led_ok = led_config_init();
    debug_cdc_init();

    return touch_ok && keyboard_ok && led_ok;
}

void config_app_task(void)
{
    debug_cdc_task();
    magic_config_task();
}
