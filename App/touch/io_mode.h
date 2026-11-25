#ifndef __IO_MODE_H__
#define __IO_MODE_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * CDC0 game input mode.
 *
 * This is deliberately separate from touch_mode. touch_mode selects the
 * wire format for the raw/Mai2Touch paths; io_mode is an upper-computer
 * controlled switch that makes CDC0 carry button + touch state instead.
 *
 * Power-on defaults to disabled. The setting is RAM-only: unplugging USB
 * resets the board and therefore resets IO mode.
 */
void io_mode_init(void);
void io_mode_set_enabled(bool enabled);
bool io_mode_is_enabled(void);

/* Observe a command-stream byte. Only a complete IO control frame returns
 * true. Other bytes are still available to the active protocol parser. */
bool io_mode_observe_rx_byte(uint8_t byte);

/* Called from the main loop. */
void io_mode_task(void);

#ifdef __cplusplus
}
#endif

#endif /* __IO_MODE_H__ */
