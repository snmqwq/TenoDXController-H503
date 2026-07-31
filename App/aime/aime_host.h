#ifndef AIME_HOST_H
#define AIME_HOST_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

typedef bool (*aime_host_tx_begin_cb_t)(void *context,
                                        uint16_t required_length,
                                        uint8_t **buffer);
typedef void (*aime_host_tx_commit_cb_t)(void *context, uint8_t length);

typedef struct
{
    aime_host_tx_begin_cb_t begin;
    aime_host_tx_commit_cb_t commit;
    void *context;
} aime_host_tx_sink_t;

void aime_host_init(void);
void aime_host_task(void);

#ifdef __cplusplus
}
#endif

#endif /* AIME_HOST_H */
