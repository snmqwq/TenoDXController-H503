#ifndef AIME_PROTOCOL_H
#define AIME_PROTOCOL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "aime_host.h"

void aime_protocol_init(aime_host_tx_sink_t const *tx_sink);
void aime_protocol_reset(void);
void aime_protocol_task(void);
bool aime_protocol_is_active(void);
void aime_protocol_feed(uint8_t data);

#ifdef __cplusplus
}
#endif

#endif /* AIME_PROTOCOL_H */
