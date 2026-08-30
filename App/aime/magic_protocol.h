#ifndef MAGIC_PROTOCOL_H
#define MAGIC_PROTOCOL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "aime_host.h"

void magic_protocol_init(aime_host_tx_sink_t const *tx_sink);
void magic_protocol_reset(void);
void magic_protocol_task(void);
bool magic_protocol_is_active(void);
bool magic_protocol_probe(uint8_t data);
void magic_protocol_cancel_probe(void);
void magic_protocol_feed(uint8_t data);

#ifdef __cplusplus
}
#endif

#endif /* MAGIC_PROTOCOL_H */
