#include "config_app.h"

#include "keyboard_config.h"
#include "led_config.h"
#include "magic_config.h"
#include "touch_config.h"

bool config_app_init(void)
{
    bool keyboard_ok;
    bool led_ok;

    magic_config_init();
    touch_config_init();
    keyboard_ok = keyboard_config_init();
    led_ok = led_config_init();

    return keyboard_ok && led_ok;
}

void config_app_task(void)
{
    magic_config_task();
}
