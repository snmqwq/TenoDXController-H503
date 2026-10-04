/*
 * Host-side timing test for the firmware USB reconnect state machine.
 * The state machine owns only the sequencing; TinyUSB is stubbed here so the
 * 60 ms ACK flush, D+ disconnect, 250 ms hold and reconnect points are exact.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

static uint32_t tick;
static int disconnect_count;
static int connect_count;

uint32_t HAL_GetTick(void)
{
    return tick;
}

bool tud_disconnect(void)
{
    disconnect_count++;
    return true;
}

bool tud_connect(void)
{
    connect_count++;
    return true;
}

/* Pull only the logic under test; no STM32 headers or USB peripheral needed. */
#define main usb_control_test_main_entry
#include "../App/config/usb_control.c"
#undef main

static int failures;

static void check(bool condition, const char *name)
{
    printf("  %s %s\n", condition ? "OK  " : "FAIL", name);
    if (!condition)
    {
        failures++;
    }
}

static void advance(uint32_t ms)
{
    tick += ms;
    usb_control_task();
}

int main(void)
{
    printf("=== USB reconnect timing test ===\n");

    usb_control_init();
    check(!usb_control_is_busy(), "idle after init");

    check(usb_control_request_reenumeration(250U), "request accepted");
    check(usb_control_is_busy(), "busy after request");
    check(!usb_control_request_reenumeration(250U),
          "second request rejected while busy");

    advance(149U);
    check(disconnect_count == 0, "no disconnect before ACK flush window");

    advance(1U);
    check(disconnect_count == 1, "disconnect after 150 ms ACK flush");
    check(connect_count == 0, "connect waits during disconnect hold");

    advance(249U);
    check(connect_count == 0, "hold lasts the requested time");

    advance(1U);
    check(connect_count == 1, "reconnect after hold");
    check(!usb_control_is_busy(), "idle after reconnect");

    check(usb_control_request_reenumeration(0U), "zero delay is clamped");
    advance(150U);
    check(disconnect_count == 2, "clamped reconnect disconnects");
    advance(20U);
    check(connect_count == 2, "minimum hold is 20 ms");

    if (failures == 0)
    {
        printf("all USB reconnect timing checks passed\n");
        return 0;
    }

    printf("%d USB reconnect check(s) failed\n", failures);
    return 1;
}
