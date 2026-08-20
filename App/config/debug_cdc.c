#include "debug_cdc.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "magic_config.h"
#include "magic_protocol.h"
#include "tusb.h"

#define DEBUG_CDC_ITF             3U
#define DEBUG_CDC_RX_BUDGET     128U
#define DEBUG_CDC_TX_MAX_LENGTH (MAGIC_CONFIG_MAX_PAYLOAD + 7U)

typedef struct
{
    bool tx_pending;
    uint8_t tx_length;
    uint8_t tx_data[DEBUG_CDC_TX_MAX_LENGTH];
} debug_cdc_state_t;

static debug_cdc_state_t debug_cdc;

static bool debug_cdc_tx_begin(void *context,
                               uint16_t required_length,
                               uint8_t **buffer);
static void debug_cdc_tx_commit(void *context, uint8_t length);
static void debug_cdc_rx_task(void);
static void debug_cdc_tx_task(void);

void debug_cdc_init(void)
{
    magic_protocol_tx_sink_t tx_sink =
    {
        .begin = debug_cdc_tx_begin,
        .commit = debug_cdc_tx_commit,
        .context = &debug_cdc
    };

    memset(&debug_cdc, 0, sizeof(debug_cdc));
    magic_protocol_init(&tx_sink);
}

void debug_cdc_task(void)
{
    if (!tud_cdc_n_ready(DEBUG_CDC_ITF))
    {
        magic_protocol_reset();
        debug_cdc.tx_pending = false;
        debug_cdc.tx_length = 0U;
        return;
    }

    debug_cdc_tx_task();
    magic_protocol_task();
    if (!debug_cdc.tx_pending)
    {
        debug_cdc_rx_task();
    }
    debug_cdc_tx_task();
}

static bool debug_cdc_tx_begin(void *context,
                               uint16_t required_length,
                               uint8_t **buffer)
{
    debug_cdc_state_t *state = context;

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

static void debug_cdc_tx_commit(void *context, uint8_t length)
{
    debug_cdc_state_t *state = context;

    if ((state == NULL) || (length > sizeof(state->tx_data)))
    {
        return;
    }

    state->tx_length = length;
    state->tx_pending = true;
}

static void debug_cdc_rx_task(void)
{
    uint16_t count = 0U;
    uint8_t data;

    while (!debug_cdc.tx_pending &&
           (tud_cdc_n_available(DEBUG_CDC_ITF) != 0U) &&
           (count < DEBUG_CDC_RX_BUDGET))
    {
        if (tud_cdc_n_read(DEBUG_CDC_ITF, &data, 1U) != 1U)
        {
            break;
        }

        if (magic_protocol_is_active())
        {
            magic_protocol_feed(data);
        }
        else
        {
            (void)magic_protocol_probe(data);
        }
        count++;
    }
}

static void debug_cdc_tx_task(void)
{
    if (!debug_cdc.tx_pending ||
        (tud_cdc_n_write_available(DEBUG_CDC_ITF) < debug_cdc.tx_length))
    {
        return;
    }

    if (tud_cdc_n_write(DEBUG_CDC_ITF,
                        debug_cdc.tx_data,
                        debug_cdc.tx_length) == debug_cdc.tx_length)
    {
        tud_cdc_n_write_flush(DEBUG_CDC_ITF);
        debug_cdc.tx_pending = false;
    }
}
