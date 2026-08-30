#include "magic_protocol.h"

#include <stddef.h>
#include <string.h>

#include "config/magic_config.h"
#include "main.h"

#define MAGIC_SEQUENCE_LENGTH         8U
#define MAGIC_REQUEST_HEADER_LENGTH   4U
#define MAGIC_REQUEST_TIMEOUT_MS    100U
#define MAGIC_RESPONSE_SYNC         0xACU

typedef enum
{
    MAGIC_RX_IDLE,
    MAGIC_RX_HEADER,
    MAGIC_RX_PAYLOAD,
    MAGIC_RX_CHECKSUM
} magic_rx_state_t;

typedef struct
{
    magic_rx_state_t state;
    uint8_t sequence_index;
    uint8_t header_index;
    uint8_t payload_index;
    uint8_t header[MAGIC_REQUEST_HEADER_LENGTH];
    uint8_t payload[MAGIC_CONFIG_MAX_PAYLOAD];
    uint8_t sum;
    uint32_t last_byte_tick;
} magic_rx_parser_t;

typedef struct
{
    magic_rx_parser_t rx;
    aime_host_tx_sink_t tx_sink;
} magic_protocol_state_t;

static magic_protocol_state_t protocol;
static uint8_t magic_response[MAGIC_CONFIG_MAX_PAYLOAD];

static const uint8_t magic_sequence[MAGIC_SEQUENCE_LENGTH] =
{
    0x91U,
    0x3EU,
    0xEDU,
    0x20U,
    0x7CU,
    0x99U,
    0x58U,
    0xACU
};

static bool elapsed_at_least(uint32_t now, uint32_t start, uint32_t period)
{
    return (uint32_t)(now - start) >= period;
}

static void magic_process_request(void);
static void magic_queue_response(uint8_t status,
                                 uint8_t module,
                                 uint8_t cmd,
                                 uint8_t param,
                                 const uint8_t *payload,
                                 uint8_t payload_length);

void magic_protocol_init(aime_host_tx_sink_t const *tx_sink)
{
    memset(&protocol, 0, sizeof(protocol));

    if (tx_sink != NULL)
    {
        protocol.tx_sink = *tx_sink;
    }

    magic_protocol_reset();
}

void magic_protocol_reset(void)
{
    protocol.rx.state = MAGIC_RX_IDLE;
    protocol.rx.sequence_index = 0U;
    protocol.rx.header_index = 0U;
    protocol.rx.payload_index = 0U;
    protocol.rx.sum = 0U;
    protocol.rx.last_byte_tick = 0U;
}

void magic_protocol_task(void)
{
    uint8_t module = 0U;
    uint8_t cmd = 0U;
    uint8_t param = 0U;

    if ((protocol.rx.state == MAGIC_RX_IDLE) ||
        !elapsed_at_least(HAL_GetTick(),
                          protocol.rx.last_byte_tick,
                          MAGIC_REQUEST_TIMEOUT_MS))
    {
        return;
    }

    if (protocol.rx.header_index > 0U)
    {
        module = protocol.rx.header[0];
    }
    if (protocol.rx.header_index > 1U)
    {
        cmd = protocol.rx.header[1];
    }
    if (protocol.rx.header_index > 2U)
    {
        param = protocol.rx.header[2];
    }

    magic_queue_response(MAGIC_CONFIG_STATUS_IO_ERROR,
                         module,
                         cmd,
                         param,
                         NULL,
                         0U);
    magic_protocol_reset();
}

bool magic_protocol_is_active(void)
{
    return protocol.rx.state != MAGIC_RX_IDLE;
}

bool magic_protocol_probe(uint8_t data)
{
    if (data == magic_sequence[protocol.rx.sequence_index])
    {
        protocol.rx.sequence_index++;
    }
    else
    {
        protocol.rx.sequence_index =
            (data == magic_sequence[0]) ? 1U : 0U;
    }

    if (protocol.rx.sequence_index < MAGIC_SEQUENCE_LENGTH)
    {
        return false;
    }

    protocol.rx.state = MAGIC_RX_HEADER;
    protocol.rx.sequence_index = 0U;
    protocol.rx.header_index = 0U;
    protocol.rx.payload_index = 0U;
    protocol.rx.sum = 0U;
    protocol.rx.last_byte_tick = HAL_GetTick();
    return true;
}

void magic_protocol_cancel_probe(void)
{
    protocol.rx.sequence_index = 0U;
}

void magic_protocol_feed(uint8_t data)
{
    uint8_t payload_length;

    protocol.rx.last_byte_tick = HAL_GetTick();

    switch (protocol.rx.state)
    {
        case MAGIC_RX_HEADER:
            protocol.rx.header[protocol.rx.header_index++] = data;
            protocol.rx.sum = (uint8_t)(protocol.rx.sum + data);

            if (protocol.rx.header_index < MAGIC_REQUEST_HEADER_LENGTH)
            {
                return;
            }

            payload_length = protocol.rx.header[3];
            if (payload_length > MAGIC_CONFIG_MAX_PAYLOAD)
            {
                magic_queue_response(MAGIC_CONFIG_STATUS_LENGTH_ERROR,
                                     protocol.rx.header[0],
                                     protocol.rx.header[1],
                                     protocol.rx.header[2],
                                     NULL,
                                     0U);
                magic_protocol_reset();
                return;
            }

            protocol.rx.state = (payload_length == 0U) ?
                                MAGIC_RX_CHECKSUM :
                                MAGIC_RX_PAYLOAD;
            return;

        case MAGIC_RX_PAYLOAD:
            payload_length = protocol.rx.header[3];
            protocol.rx.payload[protocol.rx.payload_index++] = data;
            protocol.rx.sum = (uint8_t)(protocol.rx.sum + data);

            if (protocol.rx.payload_index >= payload_length)
            {
                protocol.rx.state = MAGIC_RX_CHECKSUM;
            }
            return;

        case MAGIC_RX_CHECKSUM:
            if (data == protocol.rx.sum)
            {
                magic_process_request();
            }
            else
            {
                magic_queue_response(MAGIC_CONFIG_STATUS_SUM_ERROR,
                                     protocol.rx.header[0],
                                     protocol.rx.header[1],
                                     protocol.rx.header[2],
                                     NULL,
                                     0U);
            }
            magic_protocol_reset();
            return;

        case MAGIC_RX_IDLE:
        default:
            magic_protocol_reset();
            return;
    }
}

static void magic_process_request(void)
{
    uint8_t response_length = 0U;
    uint8_t module = protocol.rx.header[0];
    uint8_t cmd = protocol.rx.header[1];
    uint8_t param = protocol.rx.header[2];
    uint8_t payload_length = protocol.rx.header[3];
    uint8_t status;

    status = magic_config_handle(module,
                                 cmd,
                                 param,
                                 protocol.rx.payload,
                                 payload_length,
                                 magic_response,
                                 (uint8_t)sizeof(magic_response),
                                 &response_length);

    magic_queue_response(status,
                         module,
                         cmd,
                         param,
                         magic_response,
                         response_length);
}

static void magic_queue_response(uint8_t status,
                                 uint8_t module,
                                 uint8_t cmd,
                                 uint8_t param,
                                 const uint8_t *payload,
                                 uint8_t payload_length)
{
    uint8_t *output;
    uint8_t sum = 0U;
    uint8_t index;
    uint8_t output_length;

    if ((payload_length > MAGIC_CONFIG_MAX_PAYLOAD) ||
        ((payload == NULL) && (payload_length != 0U)) ||
        (protocol.tx_sink.begin == NULL) ||
        (protocol.tx_sink.commit == NULL) ||
        !protocol.tx_sink.begin(protocol.tx_sink.context,
                                (uint16_t)payload_length + 7U,
                                &output))
    {
        return;
    }

    output[0] = MAGIC_RESPONSE_SYNC;
    output[1] = status;
    output[2] = module;
    output[3] = cmd;
    output[4] = param;
    output[5] = payload_length;

    for (index = 0U; index < 6U; index++)
    {
        sum = (uint8_t)(sum + output[index]);
    }

    if (payload_length != 0U)
    {
        memcpy(&output[6], payload, payload_length);
        for (index = 0U; index < payload_length; index++)
        {
            sum = (uint8_t)(sum + payload[index]);
        }
    }

    output_length = (uint8_t)(6U + payload_length);
    output[output_length++] = sum;
    protocol.tx_sink.commit(protocol.tx_sink.context, output_length);
}
