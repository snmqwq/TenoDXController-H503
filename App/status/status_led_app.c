#include "status_led_app.h"

#include "main.h"

#define STATUS_LED_APP_RUNNING_PERIOD_MS         1000U
#define STATUS_LED_APP_RUNNING_ON_MS              100U
#define STATUS_LED_APP_CONFIG_PHASE_MS             80U
#define STATUS_LED_APP_CONFIG_FEEDBACK_MS         480U

typedef enum
{
    STATUS_LED_APP_STATE_INITIALIZING = 0,
    STATUS_LED_APP_STATE_RUNNING,
    STATUS_LED_APP_STATE_CONFIG_WRITE,
    STATUS_LED_APP_STATE_ERROR
} status_led_app_state_t;

static status_led_app_state_t status_led_state = STATUS_LED_APP_STATE_INITIALIZING;
static uint32_t status_led_phase_started_tick;
static bool status_led_config_write_complete;
static bool status_led_output_valid;
static bool status_led_output_on;

static void status_led_app_write(bool on)
{
    if (!status_led_output_valid || (status_led_output_on != on))
    {
        HAL_GPIO_WritePin(STATUS_LED_GPIO_Port,
                          STATUS_LED_Pin,
                          on ? GPIO_PIN_SET : GPIO_PIN_RESET);
        status_led_output_on = on;
        status_led_output_valid = true;
    }
}

static void status_led_app_configure_output(bool on)
{
    GPIO_InitTypeDef gpio_init = {0};

    __HAL_RCC_GPIOC_CLK_ENABLE();
    HAL_GPIO_WritePin(STATUS_LED_GPIO_Port,
                      STATUS_LED_Pin,
                      on ? GPIO_PIN_SET : GPIO_PIN_RESET);

    gpio_init.Pin = STATUS_LED_Pin;
    gpio_init.Mode = GPIO_MODE_OUTPUT_PP;
    gpio_init.Pull = GPIO_NOPULL;
    gpio_init.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(STATUS_LED_GPIO_Port, &gpio_init);

    status_led_output_on = on;
    status_led_output_valid = true;
}

static void status_led_app_enter_running(uint32_t now, bool pulse_now)
{
    status_led_state = STATUS_LED_APP_STATE_RUNNING;
    status_led_config_write_complete = false;

    if (pulse_now)
    {
        status_led_phase_started_tick = now;
        status_led_app_write(true);
    }
    else
    {
        status_led_phase_started_tick = now - STATUS_LED_APP_RUNNING_ON_MS;
        status_led_app_write(false);
    }
}

void status_led_app_init(void)
{
    if (status_led_state == STATUS_LED_APP_STATE_ERROR)
    {
        status_led_app_configure_output(true);
        return;
    }

    status_led_app_configure_output(true);
    status_led_state = STATUS_LED_APP_STATE_INITIALIZING;
    status_led_phase_started_tick = HAL_GetTick();
    status_led_config_write_complete = false;
}

void status_led_app_task(void)
{
    uint32_t now = HAL_GetTick();
    uint32_t elapsed;

    switch (status_led_state)
    {
        case STATUS_LED_APP_STATE_INITIALIZING:
        case STATUS_LED_APP_STATE_ERROR:
            status_led_app_write(true);
            break;

        case STATUS_LED_APP_STATE_RUNNING:
            elapsed = (uint32_t)(now - status_led_phase_started_tick);
            if (elapsed >= STATUS_LED_APP_RUNNING_PERIOD_MS)
            {
                elapsed %= STATUS_LED_APP_RUNNING_PERIOD_MS;
                status_led_phase_started_tick = now - elapsed;
            }
            status_led_app_write(elapsed < STATUS_LED_APP_RUNNING_ON_MS);
            break;

        case STATUS_LED_APP_STATE_CONFIG_WRITE:
            elapsed = (uint32_t)(now - status_led_phase_started_tick);
            if (status_led_config_write_complete &&
                (elapsed >= STATUS_LED_APP_CONFIG_FEEDBACK_MS))
            {
                status_led_app_enter_running(now, false);
                break;
            }

            status_led_app_write(((elapsed / STATUS_LED_APP_CONFIG_PHASE_MS) & 1U) == 0U);
            break;

        default:
            status_led_app_set_error();
            break;
    }
}

void status_led_app_set_running(void)
{
    if (status_led_state != STATUS_LED_APP_STATE_ERROR)
    {
        status_led_app_enter_running(HAL_GetTick(), true);
    }
}

void status_led_app_config_write_begin(void)
{
    if (status_led_state == STATUS_LED_APP_STATE_ERROR)
    {
        return;
    }

    status_led_state = STATUS_LED_APP_STATE_CONFIG_WRITE;
    status_led_phase_started_tick = HAL_GetTick();
    status_led_config_write_complete = false;
    status_led_app_write(true);
}

void status_led_app_config_write_end(bool success)
{
    if (status_led_state == STATUS_LED_APP_STATE_ERROR)
    {
        return;
    }

    if (!success)
    {
        status_led_app_set_error();
        return;
    }

    status_led_state = STATUS_LED_APP_STATE_CONFIG_WRITE;
    status_led_phase_started_tick = HAL_GetTick();
    status_led_config_write_complete = true;
    status_led_app_write(true);
}

void status_led_app_set_error(void)
{
    status_led_state = STATUS_LED_APP_STATE_ERROR;
    status_led_config_write_complete = false;
    status_led_app_configure_output(true);
}
