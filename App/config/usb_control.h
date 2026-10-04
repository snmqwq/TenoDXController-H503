#ifndef USB_CONTROL_H
#define USB_CONTROL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

/*
 * Software USB re-enumeration.
 *
 * The H503 has an internal D+ pull-up, so the application can make Windows
 * see a real detach/attach without external VBUS or D+ control hardware.
 */
void usb_control_init(void);
void usb_control_task(void);

/* Schedule a detach/attach cycle. The task performs it after a short delay,
 * so the USB stack has time to finish flushing the response first. */
bool usb_control_request_reenumeration(uint16_t disconnect_ms);

bool usb_control_is_busy(void);

#ifdef __cplusplus
}
#endif

#endif /* USB_CONTROL_H */
