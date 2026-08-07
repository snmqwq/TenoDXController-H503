#include "aime_protocol.h"

#include <stddef.h>
#include <string.h>

#include "main.h"
#include "pn532_reader.h"

#define AIME_REQUEST_MAX_LENGTH    64U
#define AIME_RESPONSE_MAX_LENGTH   64U
#define AIME_REQUEST_TIMEOUT_MS   100U

#define AIME_FRAME_START  0xE0U
#define AIME_FRAME_ESCAPE 0xD0U

typedef struct
{
    bool active;
    bool escaped;
    uint8_t frame_length;
    uint8_t count;
    uint8_t data[AIME_REQUEST_MAX_LENGTH];
    uint32_t last_byte_tick;
} aime_rx_parser_t;

typedef struct
{
    aime_rx_parser_t rx;
    aime_host_tx_sink_t tx_sink;
} aime_protocol_state_t;

static aime_protocol_state_t protocol;

static bool elapsed_at_least(uint32_t now, uint32_t start, uint32_t period)
{
    return (uint32_t)(now - start) >= period;
}

static void aime_handle_request(const uint8_t *packet, uint8_t length)
    __attribute__((optimize("Os")));
static void aime_queue_response(uint8_t address,
                                uint8_t sequence,
                                uint8_t command,
                                uint8_t status,
                                const uint8_t *payload,
                                uint8_t payload_length)
    __attribute__((optimize("Os")));
static bool aime_encode_byte(uint8_t data, uint8_t *output, uint8_t *length)
    __attribute__((optimize("Os")));

void aime_protocol_init(aime_host_tx_sink_t const *tx_sink)
{
    memset(&protocol, 0, sizeof(protocol));

    if (tx_sink != NULL)
    {
        protocol.tx_sink = *tx_sink;
    }

    aime_protocol_reset();
}

void aime_protocol_reset(void)
{
    protocol.rx.active = false;
    protocol.rx.escaped = false;
    protocol.rx.frame_length = 0U;
    protocol.rx.count = 0U;
    protocol.rx.last_byte_tick = 0U;
}

void aime_protocol_task(void)
{
    if (protocol.rx.active &&
        elapsed_at_least(HAL_GetTick(),
                         protocol.rx.last_byte_tick,
                         AIME_REQUEST_TIMEOUT_MS))
    {
        aime_protocol_reset();
    }
}

bool aime_protocol_is_active(void)
{
    return protocol.rx.active;
}

void aime_protocol_feed(uint8_t data)
{
    uint8_t checksum = 0U;
    uint8_t index;

    if (data == AIME_FRAME_START)
    {
        protocol.rx.active = true;
        protocol.rx.escaped = false;
        protocol.rx.frame_length = 0U;
        protocol.rx.count = 0U;
        protocol.rx.last_byte_tick = HAL_GetTick();
        return;
    }

    if (!protocol.rx.active)
    {
        return;
    }

    protocol.rx.last_byte_tick = HAL_GetTick();

    if (data == AIME_FRAME_ESCAPE)
    {
        protocol.rx.escaped = true;
        return;
    }

    if (protocol.rx.escaped)
    {
        data = (uint8_t)(data + 1U);
        protocol.rx.escaped = false;
    }

    if (protocol.rx.count >= sizeof(protocol.rx.data))
    {
        aime_protocol_reset();
        return;
    }

    protocol.rx.data[protocol.rx.count++] = data;
    if (protocol.rx.count == 1U)
    {
        protocol.rx.frame_length = data;
        if ((protocol.rx.frame_length < 5U) ||
            ((uint16_t)protocol.rx.frame_length + 1U >
             sizeof(protocol.rx.data)))
        {
            aime_protocol_reset();
        }
        return;
    }

    if (protocol.rx.count < (uint8_t)(protocol.rx.frame_length + 1U))
    {
        return;
    }

    if (protocol.rx.count == (uint8_t)(protocol.rx.frame_length + 1U))
    {
        for (index = 0U; index + 1U < protocol.rx.count; index++)
        {
            checksum = (uint8_t)(checksum + protocol.rx.data[index]);
        }

        if (checksum == protocol.rx.data[protocol.rx.count - 1U])
        {
            aime_handle_request(protocol.rx.data, protocol.rx.count);
        }
    }

    aime_protocol_reset();
}

static void aime_handle_request(const uint8_t *packet, uint8_t length)
{
    static const uint8_t firmware_version[] = { 0x94U };
    static const uint8_t hardware_version[] = "837-15396";
    static const uint8_t extension_info[] =
    {
        '0', '0', '0', '-', '0', '0', '0', '0', '0',
        0xFFU, 0x11U, 0x40U
    };
    static const uint8_t card_absent_response[] = { 0x00U };
    static const uint8_t empty_block[PN532_READER_CARD_BLOCK_LENGTH] = { 0U };
    pn532_reader_card_info_t card_info;
    uint8_t card_present_response[3U + PN532_READER_CARD_ID_MAX_LENGTH];
    uint8_t last_block[PN532_READER_CARD_BLOCK_LENGTH];
    uint8_t frame_length;
    uint8_t address;
    uint8_t sequence;
    uint8_t command;
    uint8_t payload_length;
    const uint8_t *payload;
    uint8_t status = 0U;
    const uint8_t *response_payload = NULL;
    uint8_t response_length = 0U;
    bool send_reply = true;
    uint32_t now = HAL_GetTick();

    if ((packet == NULL) || (length < 6U))
    {
        return;
    }

    frame_length = packet[0];
    address = packet[1];
    sequence = packet[2];
    command = packet[3];
    payload_length = packet[4];

    if (((uint16_t)frame_length + 1U != length) ||
        ((uint16_t)payload_length + 5U != frame_length))
    {
        return;
    }

    payload = &packet[5];

    switch (command)
    {
        case 0x30U:
            response_payload = firmware_version;
            response_length = sizeof(firmware_version);
            break;

        case 0x32U:
            response_payload = hardware_version;
            response_length = sizeof(hardware_version) - 1U;
            break;

        case 0xF0U:
            response_payload = extension_info;
            response_length = sizeof(extension_info);
            break;

        case 0x40U:
            pn532_reader_start_polling();
            break;

        case 0x41U:
            pn532_reader_stop_polling();
            break;

        case 0x42U:
            if (pn532_reader_copy_card_info(now, &card_info))
            {
                card_present_response[0] = 0x01U;
                if (((card_info.type == PN532_READER_CARD_TYPE_MIFARE) ||
                     (card_info.type == PN532_READER_CARD_TYPE_FELICA)) &&
                    (card_info.identifier_length != 0U) &&
                    (card_info.identifier_length <=
                     PN532_READER_CARD_ID_MAX_LENGTH))
                {
                    card_present_response[1] = card_info.type;
                    card_present_response[2] = card_info.identifier_length;
                    memcpy(&card_present_response[3],
                           card_info.identifier,
                           card_info.identifier_length);
                    response_payload = card_present_response;
                    response_length =
                        (uint8_t)(3U + card_info.identifier_length);
                }
                else
                {
                    response_payload = card_absent_response;
                    response_length = sizeof(card_absent_response);
                }
            }
            else
            {
                response_payload = card_absent_response;
                response_length = sizeof(card_absent_response);
            }
            break;

        case 0x52U:
            if ((payload_length >= 5U) && (payload[4] == 0x02U))
            {
                pn532_reader_copy_last_block(last_block);
                response_payload = last_block;
            }
            else
            {
                response_payload = empty_block;
            }
            response_length = PN532_READER_CARD_BLOCK_LENGTH;
            break;

        case 0x81U:
        case 0x82U:
            send_reply = false;
            break;

        case 0x61U:
            status = 0x20U;
            break;

        case 0x64U:
            status = 0x08U;
            break;

        default:
            break;
    }

    if (send_reply)
    {
        aime_queue_response(address,
                            sequence,
                            command,
                            status,
                            response_payload,
                            response_length);
    }
}

static void aime_queue_response(uint8_t address,
                                uint8_t sequence,
                                uint8_t command,
                                uint8_t status,
                                const uint8_t *payload,
                                uint8_t payload_length)
{
    uint8_t packet[32];
    uint8_t *output;
    uint8_t packet_length;
    uint8_t checksum = 0U;
    uint8_t output_length = 0U;
    uint8_t index;

    if (((uint16_t)payload_length + 7U > sizeof(packet)) ||
        (protocol.tx_sink.begin == NULL) ||
        (protocol.tx_sink.commit == NULL) ||
        !protocol.tx_sink.begin(protocol.tx_sink.context,
                                AIME_RESPONSE_MAX_LENGTH,
                                &output))
    {
        return;
    }

    packet[0] = (uint8_t)(6U + payload_length);
    packet[1] = address;
    packet[2] = sequence;
    packet[3] = command;
    packet[4] = status;
    packet[5] = payload_length;
    if ((payload != NULL) && (payload_length != 0U))
    {
        memcpy(&packet[6], payload, payload_length);
    }
    packet_length = (uint8_t)(6U + payload_length);

    for (index = 0U; index < packet_length; index++)
    {
        checksum = (uint8_t)(checksum + packet[index]);
    }

    output[output_length++] = AIME_FRAME_START;
    for (index = 0U; index < packet_length; index++)
    {
        if (!aime_encode_byte(packet[index], output, &output_length))
        {
            return;
        }
    }
    if (!aime_encode_byte(checksum, output, &output_length))
    {
        return;
    }

    protocol.tx_sink.commit(protocol.tx_sink.context, output_length);
}

static bool aime_encode_byte(uint8_t data, uint8_t *output, uint8_t *length)
{
    if ((data == AIME_FRAME_START) || (data == AIME_FRAME_ESCAPE))
    {
        if ((uint16_t)*length + 2U > AIME_RESPONSE_MAX_LENGTH)
        {
            return false;
        }
        output[(*length)++] = AIME_FRAME_ESCAPE;
        output[(*length)++] = (uint8_t)(data - 1U);
    }
    else
    {
        if ((uint16_t)*length + 1U > AIME_RESPONSE_MAX_LENGTH)
        {
            return false;
        }
        output[(*length)++] = data;
    }

    return true;
}
