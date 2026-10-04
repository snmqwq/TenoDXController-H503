#include "usb_control.h"

#include "tusb.h"

#define USB_CONTROL_RESPONSE_FLUSH_MS    150U

uint32_t HAL_GetTick(void);

typedef enum
{
    USB_CONTROL_IDLE = 0,
    USB_CONTROL_REENUMERATE_DELAY,
    USB_CONTROL_REENUMERATE_WAIT
} usb_control_state_t;

static usb_control_state_t state;
static uint32_t action_tick;
static uint16_t disconnect_ms;

static void usb_control_disconnect(void)
{
    (void)tud_disconnect();
}

static void usb_control_connect(void)
{
    (void)tud_connect();
}

void usb_control_init(void)
{
    state = USB_CONTROL_IDLE;
    action_tick = 0U;
    disconnect_ms = 0U;
}

bool usb_control_request_reenumeration(uint16_t delay_ms_value)
{
    if (state != USB_CONTROL_IDLE)
    {
        return false;
    }

    disconnect_ms = (delay_ms_value < 20U) ? 20U : delay_ms_value;
    action_tick = HAL_GetTick();
    state = USB_CONTROL_REENUMERATE_DELAY;
    return true;
}

bool usb_control_is_busy(void)
{
    return state != USB_CONTROL_IDLE;
}

void usb_control_task(void)
{
    uint32_t now = HAL_GetTick();
    bool progressed;

    do
    {
        progressed = false;

        switch (state)
        {
            case USB_CONTROL_REENUMERATE_DELAY:
                if ((uint32_t)(now - action_tick) >=
                    USB_CONTROL_RESPONSE_FLUSH_MS)
                {
                    usb_control_disconnect();
                    action_tick = now;
                    state = USB_CONTROL_REENUMERATE_WAIT;
                    progressed = true;
                }
                break;

            case USB_CONTROL_REENUMERATE_WAIT:
                if ((uint32_t)(now - action_tick) >= disconnect_ms)
                {
                    usb_control_connect();
                    state = USB_CONTROL_IDLE;
                    progressed = true;
                }
                break;

            case USB_CONTROL_IDLE:
            default:
                break;
        }
    } while (progressed);
}
