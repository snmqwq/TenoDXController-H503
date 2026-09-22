#ifndef MAGIC_PROTOCOL_H
#define MAGIC_PROTOCOL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

typedef bool (*magic_protocol_tx_begin_cb_t)(void *context,
                                             uint16_t required_length,
                                             uint8_t **buffer);
typedef void (*magic_protocol_tx_commit_cb_t)(void *context, uint8_t length);

typedef struct
{
    magic_protocol_tx_begin_cb_t begin;
    magic_protocol_tx_commit_cb_t commit;
    void *context;
} magic_protocol_tx_sink_t;

void magic_protocol_init(magic_protocol_tx_sink_t const *tx_sink);
void magic_protocol_reset(void);
void magic_protocol_task(void);
bool magic_protocol_is_active(void);
bool magic_protocol_probe(uint8_t data);
void magic_protocol_feed(uint8_t data);

#ifdef __cplusplus
}
#endif

#endif /* MAGIC_PROTOCOL_H */
