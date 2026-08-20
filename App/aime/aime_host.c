#include "aime_host.h"

#include <stddef.h>
#include <string.h>

#include "aime_protocol.h"
#include "tusb.h"

#define AIME_CDC_ITF           2U
#define AIME_CDC_RX_BUDGET   128U
#define AIME_HOST_TX_MAX_LENGTH 64U

typedef struct
{
    bool tx_pending;
    uint8_t tx_length;
    uint8_t tx_data[AIME_HOST_TX_MAX_LENGTH];
} aime_host_state_t;

static aime_host_state_t host;

static bool aime_host_tx_begin(void *context,
                               uint16_t required_length,
                               uint8_t **buffer);
static void aime_host_tx_commit(void *context, uint8_t length);
static void aime_host_rx_task(void);
static void aime_host_tx_task(void);

void aime_host_init(void)
{
    aime_host_tx_sink_t tx_sink =
    {
        .begin = aime_host_tx_begin,
        .commit = aime_host_tx_commit,
        .context = &host
    };

    memset(&host, 0, sizeof(host));
    aime_protocol_init(&tx_sink);
}

void aime_host_task(void)
{
    if (!tud_cdc_n_ready(AIME_CDC_ITF))
    {
        aime_protocol_reset();
        host.tx_pending = false;
        host.tx_length = 0U;
        return;
    }

    aime_host_tx_task();
    aime_protocol_task();
    if (!host.tx_pending)
    {
        aime_host_rx_task();
    }
    aime_host_tx_task();
}

static bool aime_host_tx_begin(void *context,
                               uint16_t required_length,
                               uint8_t **buffer)
{
    aime_host_state_t *state = context;

    if ((state == NULL) ||
        (buffer == NULL) ||
        state->tx_pending ||
        (required_length > sizeof(state->tx_data)))
    {
        return false;
    }

    *buffer = state->tx_data;
    return true;
}

static void aime_host_tx_commit(void *context, uint8_t length)
{
    aime_host_state_t *state = context;

    if ((state == NULL) || (length > sizeof(state->tx_data)))
    {
        return;
    }

    state->tx_length = length;
    state->tx_pending = true;
}

static void aime_host_rx_task(void)
{
    uint16_t count = 0U;
    uint8_t data;

    while (!host.tx_pending &&
           (tud_cdc_n_available(AIME_CDC_ITF) != 0U) &&
           (count < AIME_CDC_RX_BUDGET))
    {
        if (tud_cdc_n_read(AIME_CDC_ITF, &data, 1U) != 1U)
        {
            break;
        }
        aime_protocol_feed(data);
        count++;
    }
}

static void aime_host_tx_task(void)
{
    if (!host.tx_pending ||
        (tud_cdc_n_write_available(AIME_CDC_ITF) < host.tx_length))
    {
        return;
    }

    if (tud_cdc_n_write(AIME_CDC_ITF,
                        host.tx_data,
                        host.tx_length) == host.tx_length)
    {
        tud_cdc_n_write_flush(AIME_CDC_ITF);
        host.tx_pending = false;
    }
}
