#include "aime_reader_app.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "tusb.h"
#include "usart.h"

#define AIME_CDC_ITF                      2U
#define AIME_CDC_RX_BUDGET                128U
#define AIME_REQUEST_MAX_LENGTH           64U
#define AIME_RESPONSE_MAX_LENGTH          64U

#define AIME_FRAME_START                  0xE0U
#define AIME_FRAME_ESCAPE                 0xD0U

#define PN532_RX_BUDGET                   128U
#define PN532_UART_RX_CHUNK_LENGTH         64U
#define PN532_UART_RX_RING_LENGTH         256U
#define PN532_PAYLOAD_MAX_LENGTH          64U
#define PN532_TX_MAX_LENGTH               32U
#define PN532_TX_TIMEOUT_MS               20U
#define PN532_UART_DEBUG_ENABLED            0U
#define PN532_DEBUG_TIMEOUT_MS            20U
#define PN532_DEBUG_BUFFER_LENGTH         224U
#define PN532_RAW_DEBUG_LENGTH            96U
#define PN532_RAW_DEBUG_IDLE_MS            2U
#define PN532_INIT_INTERVAL_MS            100U
#define PN532_WAKE_DELAY_MS               10U
#define PN532_POLL_INTERVAL_MS            33U
#define PN532_POLL_TIMEOUT_MS             150U
#define PN532_COMMAND_TIMEOUT_MS          500U
#define PN532_LINK_TIMEOUT_MS             1000U
#define PN532_CARD_DEBOUNCE_MS            2000U
#define AIME_CARD_PRESENCE_TIMEOUT_MS     2500U

#define PN532_TFI                         0xD4U
#define PN532_PN532_TO_HOST               0xD5U
#define PN532_IN_LIST_PASSIVE_TARGET      0x4AU
#define PN532_IN_LIST_PASSIVE_TARGET_ACK  0x4BU
#define PN532_IN_DATA_EXCHANGE            0x40U
#define PN532_IN_DATA_EXCHANGE_ACK        0x41U

typedef enum
{
    PN532_RX_WAIT_START,
    PN532_RX_LENGTH,
    PN532_RX_LENGTH_CHECKSUM,
    PN532_RX_PAYLOAD,
    PN532_RX_DATA_CHECKSUM,
    PN532_RX_ACK_POSTAMBLE,
    PN532_RX_NACK_POSTAMBLE
} pn532_rx_state_t;

typedef enum
{
    PN532_READ_IDLE,
    PN532_READ_POLLING,
    PN532_READ_AUTHING,
    PN532_READ_READING
} pn532_read_state_t;

typedef struct
{
    bool present;
    uint8_t block_data[16];
    uint32_t last_read_tick;
} aime_card_state_t;

typedef struct
{
    pn532_rx_state_t state;
    uint8_t length;
    uint8_t index;
    uint8_t payload_sum;
    uint8_t payload[PN532_PAYLOAD_MAX_LENGTH];
} pn532_rx_parser_t;

typedef struct
{
    bool initialized;
    uint8_t init_index;
    uint8_t poll_counter;
    pn532_read_state_t read_state;
    uint32_t next_action_tick;
    uint32_t state_tick;
    uint32_t last_physical_card_tick;
    uint32_t last_valid_rx_tick;
    uint32_t link_check_tick;
    uint32_t raw_debug_tick;
    bool link_connected;
    bool no_response_reported;
    uint8_t raw_debug_length;
    uint8_t raw_debug[PN532_RAW_DEBUG_LENGTH];
    pn532_rx_parser_t rx;
} pn532_state_t;

typedef struct
{
    bool active;
    bool escaped;
    uint8_t frame_length;
    uint8_t count;
    uint8_t data[AIME_REQUEST_MAX_LENGTH];
} aime_rx_parser_t;

typedef struct
{
    aime_rx_parser_t rx;
    bool tx_pending;
    uint8_t tx_length;
    uint8_t tx_data[AIME_RESPONSE_MAX_LENGTH];
} aime_host_state_t;

static aime_card_state_t card_state;
static pn532_state_t pn532;
static aime_host_state_t host;
static uint8_t pn532_uart_rx_chunk[PN532_UART_RX_CHUNK_LENGTH];
static uint8_t pn532_uart_rx_ring[PN532_UART_RX_RING_LENGTH];
static volatile uint16_t pn532_uart_rx_head;
static volatile uint16_t pn532_uart_rx_tail;
static volatile uint32_t pn532_uart_error;
static volatile bool pn532_uart_rx_overflow;
static volatile bool pn532_uart_restart_pending;

static const uint8_t pn532_init_commands[][6] =
{
    { PN532_TFI, 0x14U, 0x01U },
    { PN532_TFI, 0x14U, 0x01U, 0x14U, 0x01U },
    { PN532_TFI, 0x32U, 0x05U, 0xFFU, 0x01U, 0x01U },
    { PN532_TFI, 0x32U, 0x01U, 0x01U }
};

static const uint8_t pn532_init_lengths[] = { 3U, 5U, 6U, 4U };

static bool tick_due(uint32_t now, uint32_t due);
static bool elapsed_at_least(uint32_t now, uint32_t start, uint32_t period);
#if PN532_UART_DEBUG_ENABLED
static void pn532_debug_text(const char *text);
static void pn532_debug_hex(const char *prefix,
                            const uint8_t *data,
                            uint8_t length);
static void pn532_debug_status(const char *prefix, uint8_t status);
#else
#define pn532_debug_text(...)       ((void)0)
#define pn532_debug_hex(...)        ((void)0)
#define pn532_debug_status(...)     ((void)0)
#endif
static void pn532_note_valid_rx(uint32_t now);
static void pn532_link_task(uint32_t now);
static void pn532_raw_debug_add(uint8_t data, uint32_t now);
static void pn532_raw_debug_task(uint32_t now);
static void card_update(const uint8_t block_data[16], uint32_t now);
static bool card_is_present(uint32_t now);
static void pn532_reset_rx_parser(void);
static void pn532_flush_uart_rx(void);
static bool pn532_start_uart_receive(void);
static void pn532_uart_service_task(void);
static void pn532_uart_rx_task(void);
static void pn532_parse_byte(uint8_t data);
static void pn532_handle_payload(const uint8_t *payload, uint8_t length);
static bool pn532_send_payload(const uint8_t *payload, uint8_t length);
static void pn532_set_read_state(pn532_read_state_t state, uint32_t now);
static void pn532_set_idle(uint32_t now);
static void pn532_task(void);
static void pn532_handle_poll_response(const uint8_t *payload,
                                       uint8_t length,
                                       uint32_t now);
static void pn532_handle_data_response(const uint8_t *payload,
                                       uint8_t length,
                                       uint32_t now);
static void pn532_store_felica_idm(const uint8_t idm[8], uint32_t now);
static void aime_reset_rx_parser(void);
static void aime_host_task(void);
static void aime_host_rx_task(void);
static void aime_host_tx_task(void);
static void aime_parse_byte(uint8_t data);
static void aime_handle_request(const uint8_t *packet, uint8_t length);
static void aime_queue_response(uint8_t address,
                                uint8_t sequence,
                                uint8_t command,
                                uint8_t status,
                                const uint8_t *payload,
                                uint8_t payload_length);
static bool aime_encode_byte(uint8_t data, uint8_t *output, uint8_t *length);

void aime_reader_app_init(void)
{
    static const uint8_t wake_sequence[] =
    {
        0x55U, 0x55U, 0x00U, 0x00U, 0x00U
    };
    uint32_t now = HAL_GetTick();

    memset(&card_state, 0, sizeof(card_state));
    memset(&pn532, 0, sizeof(pn532));
    memset(&host, 0, sizeof(host));

    pn532.read_state = PN532_READ_IDLE;
    pn532.last_physical_card_tick = now - PN532_CARD_DEBOUNCE_MS;
    pn532.link_check_tick = now;
    pn532_reset_rx_parser();
    aime_reset_rx_parser();
    pn532_flush_uart_rx();

    pn532_debug_text("[PN532] DEBUG USART1 115200 8N1\r\n");
    if (!pn532_start_uart_receive())
    {
        pn532_debug_text("[PN532 ERR] RX INTERRUPT START FAILED\r\n");
    }
    else
    {
        pn532_debug_text("[PN532] RX INTERRUPT READY\r\n");
    }

    pn532_debug_text("[PN532] INIT START\r\n");
    if (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_5) == GPIO_PIN_SET)
    {
        pn532_debug_text("[PN532] USART2 RX PB5 IDLE HIGH\r\n");
    }
    else
    {
        pn532_debug_text("[PN532] USART2 RX PB5 IDLE LOW\r\n");
    }
    pn532_debug_hex("[PN532 TX WAKE] ",
                    wake_sequence,
                    sizeof(wake_sequence));
    if (HAL_UART_Transmit(&huart2,
                          (uint8_t *)wake_sequence,
                          sizeof(wake_sequence),
                          PN532_TX_TIMEOUT_MS) != HAL_OK)
    {
        pn532_debug_text("[PN532 ERR] WAKE TX FAILED\r\n");
    }
    pn532.next_action_tick = now + PN532_WAKE_DELAY_MS;
}

void aime_reader_app_task(void)
{
    pn532_uart_service_task();
    pn532_uart_rx_task();
    pn532_raw_debug_task(HAL_GetTick());
    pn532_task();
    aime_host_task();
}

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t size)
{
    uint16_t index;

    if (huart->Instance != USART2)
    {
        return;
    }

    if (size > sizeof(pn532_uart_rx_chunk))
    {
        size = sizeof(pn532_uart_rx_chunk);
    }

    for (index = 0U; index < size; index++)
    {
        uint16_t next_head =
            (uint16_t)((pn532_uart_rx_head + 1U) &
                       (PN532_UART_RX_RING_LENGTH - 1U));

        if (next_head == pn532_uart_rx_tail)
        {
            pn532_uart_rx_overflow = true;
            break;
        }

        pn532_uart_rx_ring[pn532_uart_rx_head] =
            pn532_uart_rx_chunk[index];
        pn532_uart_rx_head = next_head;
    }

    if (HAL_UARTEx_ReceiveToIdle_IT(&huart2,
                                    pn532_uart_rx_chunk,
                                    sizeof(pn532_uart_rx_chunk)) != HAL_OK)
    {
        pn532_uart_restart_pending = true;
    }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance != USART2)
    {
        return;
    }

    pn532_uart_error |= huart->ErrorCode;
    pn532_uart_restart_pending = true;
}

static bool tick_due(uint32_t now, uint32_t due)
{
    return (int32_t)(now - due) >= 0;
}

static bool elapsed_at_least(uint32_t now, uint32_t start, uint32_t period)
{
    return (uint32_t)(now - start) >= period;
}

#if PN532_UART_DEBUG_ENABLED
static void pn532_debug_text(const char *text)
{
    if (text == NULL)
    {
        return;
    }

    (void)HAL_UART_Transmit(&huart1,
                            (uint8_t *)text,
                            (uint16_t)strlen(text),
                            PN532_DEBUG_TIMEOUT_MS);
}

static void pn532_debug_hex(const char *prefix,
                            const uint8_t *data,
                            uint8_t length)
{
    static const uint8_t hex[] = "0123456789ABCDEF";
    uint8_t output[PN532_DEBUG_BUFFER_LENGTH];
    uint16_t output_length = 0U;
    uint16_t prefix_length;
    uint8_t index;

    if ((prefix == NULL) || ((data == NULL) && (length != 0U)))
    {
        return;
    }

    prefix_length = (uint16_t)strlen(prefix);
    if ((uint32_t)prefix_length + (uint32_t)length * 3U + 2U >
        sizeof(output))
    {
        return;
    }

    memcpy(output, prefix, prefix_length);
    output_length = prefix_length;

    for (index = 0U; index < length; index++)
    {
        output[output_length++] = hex[data[index] >> 4U];
        output[output_length++] = hex[data[index] & 0x0FU];
        output[output_length++] = ' ';
    }

    output[output_length++] = '\r';
    output[output_length++] = '\n';
    (void)HAL_UART_Transmit(&huart1,
                            output,
                            output_length,
                            PN532_DEBUG_TIMEOUT_MS);
}

static void pn532_debug_status(const char *prefix, uint8_t status)
{
    pn532_debug_hex(prefix, &status, 1U);
}
#endif

static void pn532_note_valid_rx(uint32_t now)
{
    pn532.last_valid_rx_tick = now;
    pn532.link_check_tick = now;
    pn532.no_response_reported = false;

    if (!pn532.link_connected)
    {
        pn532.link_connected = true;
        pn532_debug_text("[PN532] CONNECTED\r\n");
    }
}

static void pn532_link_task(uint32_t now)
{
    if (pn532.link_connected)
    {
        if (elapsed_at_least(now,
                             pn532.last_valid_rx_tick,
                             PN532_LINK_TIMEOUT_MS))
        {
            pn532.link_connected = false;
            pn532.no_response_reported = true;
            pn532.link_check_tick = now;
            pn532_debug_text("[PN532] DISCONNECTED: 1S NO VALID RX\r\n");
        }
    }
    else if (!pn532.no_response_reported &&
             elapsed_at_least(now,
                              pn532.link_check_tick,
                              PN532_LINK_TIMEOUT_MS))
    {
        pn532.no_response_reported = true;
        pn532_debug_text("[PN532] NO RESPONSE\r\n");
    }
}

static void pn532_raw_debug_add(uint8_t data, uint32_t now)
{
    if (pn532.raw_debug_length < sizeof(pn532.raw_debug))
    {
        pn532.raw_debug[pn532.raw_debug_length++] = data;
    }
    pn532.raw_debug_tick = now;
}

static void pn532_raw_debug_task(uint32_t now)
{
    if ((pn532.raw_debug_length != 0U) &&
        elapsed_at_least(now,
                         pn532.raw_debug_tick,
                         PN532_RAW_DEBUG_IDLE_MS))
    {
        pn532_debug_hex("[PN532 RAW RX] ",
                        pn532.raw_debug,
                        pn532.raw_debug_length);
        pn532.raw_debug_length = 0U;
    }
}

static void card_update(const uint8_t block_data[16], uint32_t now)
{
    memcpy(card_state.block_data, block_data, sizeof(card_state.block_data));
    card_state.present = true;
    card_state.last_read_tick = now;
    pn532_debug_hex("[PN532 CARD BLOCK] ",
                    block_data,
                    sizeof(card_state.block_data));
}

static bool card_is_present(uint32_t now)
{
    if (card_state.present &&
        elapsed_at_least(now,
                         card_state.last_read_tick,
                         AIME_CARD_PRESENCE_TIMEOUT_MS))
    {
        card_state.present = false;
    }

    return card_state.present;
}

static void pn532_reset_rx_parser(void)
{
    pn532.rx.state = PN532_RX_WAIT_START;
    pn532.rx.length = 0U;
    pn532.rx.index = 0U;
    pn532.rx.payload_sum = 0U;
}

static void pn532_flush_uart_rx(void)
{
    uint16_t count = 0U;

    pn532_uart_rx_head = 0U;
    pn532_uart_rx_tail = 0U;
    pn532_uart_error = HAL_UART_ERROR_NONE;
    pn532_uart_rx_overflow = false;
    pn532_uart_restart_pending = false;

    while ((__HAL_UART_GET_FLAG(&huart2, UART_FLAG_RXNE) != RESET) &&
           (count < PN532_RX_BUDGET))
    {
        (void)huart2.Instance->RDR;
        count++;
    }

    __HAL_UART_CLEAR_OREFLAG(&huart2);
    __HAL_UART_CLEAR_NEFLAG(&huart2);
    __HAL_UART_CLEAR_FEFLAG(&huart2);

    if (count != 0U)
    {
        pn532_debug_text("[PN532] RX FLUSHED\r\n");
    }
}

static bool pn532_start_uart_receive(void)
{
    HAL_StatusTypeDef result;

    result = HAL_UARTEx_ReceiveToIdle_IT(&huart2,
                                         pn532_uart_rx_chunk,
                                         sizeof(pn532_uart_rx_chunk));
    if (result != HAL_OK)
    {
        pn532_uart_restart_pending = true;
        return false;
    }

    pn532_uart_restart_pending = false;
    return true;
}

static void pn532_uart_service_task(void)
{
    uint32_t error;

    if (pn532_uart_rx_overflow)
    {
        pn532_uart_rx_overflow = false;
        pn532_debug_text("[PN532 ERR] RX RING OVERFLOW\r\n");
    }

    error = pn532_uart_error;
    if (error != HAL_UART_ERROR_NONE)
    {
        pn532_uart_error = HAL_UART_ERROR_NONE;
        pn532_reset_rx_parser();
        if ((error & HAL_UART_ERROR_ORE) != 0U)
        {
            pn532_debug_text("[PN532 ERR] UART ORE\r\n");
        }
        if ((error & HAL_UART_ERROR_NE) != 0U)
        {
            pn532_debug_text("[PN532 ERR] UART NE\r\n");
        }
        if ((error & HAL_UART_ERROR_FE) != 0U)
        {
            pn532_debug_text("[PN532 ERR] UART FE\r\n");
        }
        if ((error & HAL_UART_ERROR_PE) != 0U)
        {
            pn532_debug_text("[PN532 ERR] UART PE\r\n");
        }
    }

    if (pn532_uart_restart_pending)
    {
        (void)HAL_UART_AbortReceive(&huart2);
        if (pn532_start_uart_receive())
        {
            pn532_debug_text("[PN532] RX INTERRUPT RESTARTED\r\n");
        }
    }
}

static void pn532_uart_rx_task(void)
{
    uint16_t count = 0U;

    while ((pn532_uart_rx_tail != pn532_uart_rx_head) &&
           (count < PN532_RX_BUDGET))
    {
        uint8_t data = pn532_uart_rx_ring[pn532_uart_rx_tail];

        pn532_uart_rx_tail =
            (uint16_t)((pn532_uart_rx_tail + 1U) &
                       (PN532_UART_RX_RING_LENGTH - 1U));
        pn532_raw_debug_add(data, HAL_GetTick());
        pn532_parse_byte(data);
        count++;
    }
}

static void pn532_parse_byte(uint8_t data)
{
    switch (pn532.rx.state)
    {
        case PN532_RX_WAIT_START:
            if (data == 0xFFU)
            {
                pn532.rx.state = PN532_RX_LENGTH;
            }
            break;

        case PN532_RX_LENGTH:
            pn532.rx.length = data;
            pn532.rx.state = PN532_RX_LENGTH_CHECKSUM;
            break;

        case PN532_RX_LENGTH_CHECKSUM:
            if ((pn532.rx.length == 0U) && (data == 0xFFU))
            {
                pn532.rx.state = PN532_RX_ACK_POSTAMBLE;
            }
            else if ((pn532.rx.length == 0xFFU) && (data == 0x00U))
            {
                pn532.rx.state = PN532_RX_NACK_POSTAMBLE;
            }
            else if ((((uint16_t)pn532.rx.length + data) & 0xFFU) != 0U ||
                     (pn532.rx.length > PN532_PAYLOAD_MAX_LENGTH))
            {
#if PN532_UART_DEBUG_ENABLED
                uint8_t invalid_length[2] =
                {
                    pn532.rx.length, data
                };
                pn532_debug_hex("[PN532 ERR LCS] ",
                                invalid_length,
                                sizeof(invalid_length));
#endif
                pn532_reset_rx_parser();
            }
            else
            {
                pn532.rx.index = 0U;
                pn532.rx.payload_sum = 0U;
                pn532.rx.state = (pn532.rx.length == 0U) ?
                                 PN532_RX_DATA_CHECKSUM :
                                 PN532_RX_PAYLOAD;
            }
            break;

        case PN532_RX_PAYLOAD:
            pn532.rx.payload[pn532.rx.index++] = data;
            pn532.rx.payload_sum =
                (uint8_t)(pn532.rx.payload_sum + data);
            if (pn532.rx.index >= pn532.rx.length)
            {
                pn532.rx.state = PN532_RX_DATA_CHECKSUM;
            }
            break;

        case PN532_RX_DATA_CHECKSUM:
            if ((uint8_t)(pn532.rx.payload_sum + data) == 0U)
            {
                pn532_handle_payload(pn532.rx.payload, pn532.rx.length);
            }
            else
            {
                pn532_debug_hex("[PN532 ERR DCS PAYLOAD] ",
                                pn532.rx.payload,
                                pn532.rx.length);
                pn532_debug_status("[PN532 ERR DCS BYTE] ", data);
            }
            pn532_reset_rx_parser();
            break;

        case PN532_RX_ACK_POSTAMBLE:
            if (data == 0x00U)
            {
                pn532_note_valid_rx(HAL_GetTick());
                pn532_debug_text("[PN532 RX] ACK\r\n");
            }
            else
            {
                pn532_debug_status("[PN532 ERR ACK POSTAMBLE] ", data);
            }
            pn532_reset_rx_parser();
            break;

        case PN532_RX_NACK_POSTAMBLE:
            if (data == 0x00U)
            {
                pn532_note_valid_rx(HAL_GetTick());
                pn532_debug_text("[PN532 RX] NACK\r\n");
            }
            else
            {
                pn532_debug_status("[PN532 ERR NACK POSTAMBLE] ", data);
            }
            pn532_reset_rx_parser();
            break;

        default:
            pn532_reset_rx_parser();
            break;
    }
}

static void pn532_handle_payload(const uint8_t *payload, uint8_t length)
{
    uint32_t now = HAL_GetTick();

    pn532_note_valid_rx(now);
    pn532_debug_hex("[PN532 RX] ", payload, length);

    if ((length >= 2U) &&
        (payload[0] == PN532_PN532_TO_HOST) &&
        (payload[1] == PN532_IN_LIST_PASSIVE_TARGET_ACK))
    {
        pn532_handle_poll_response(payload, length, now);
    }
    else if ((length >= 2U) &&
             (payload[0] == PN532_PN532_TO_HOST) &&
             (payload[1] == PN532_IN_DATA_EXCHANGE_ACK))
    {
        pn532_handle_data_response(payload, length, now);
    }
}

static bool pn532_send_payload(const uint8_t *payload, uint8_t length)
{
    uint8_t packet[PN532_TX_MAX_LENGTH];
    uint8_t payload_sum = 0U;
    uint8_t index;
    HAL_StatusTypeDef result;

    if ((payload == NULL) ||
        (length == 0U) ||
        ((uint16_t)length + 7U > sizeof(packet)))
    {
        return false;
    }

    packet[0] = 0x00U;
    packet[1] = 0x00U;
    packet[2] = 0xFFU;
    packet[3] = length;
    packet[4] = (uint8_t)(0U - length);

    for (index = 0U; index < length; index++)
    {
        packet[5U + index] = payload[index];
        payload_sum = (uint8_t)(payload_sum + payload[index]);
    }

    packet[5U + length] = (uint8_t)(0U - payload_sum);
    packet[6U + length] = 0x00U;

    pn532_debug_hex("[PN532 TX] ",
                    packet,
                    (uint8_t)(length + 7U));
    result = HAL_UART_Transmit(&huart2,
                               packet,
                               (uint16_t)length + 7U,
                               PN532_TX_TIMEOUT_MS);
    if (result == HAL_OK)
    {
        return true;
    }

    if (result == HAL_BUSY)
    {
        pn532_debug_text("[PN532 ERR] TX HAL_BUSY\r\n");
    }
    else if (result == HAL_TIMEOUT)
    {
        pn532_debug_text("[PN532 ERR] TX HAL_TIMEOUT\r\n");
    }
    else
    {
        pn532_debug_text("[PN532 ERR] TX HAL_ERROR\r\n");
    }

    return false;
}

static void pn532_set_read_state(pn532_read_state_t state, uint32_t now)
{
    pn532.read_state = state;
    pn532.state_tick = now;
}

static void pn532_set_idle(uint32_t now)
{
    pn532_set_read_state(PN532_READ_IDLE, now);
    pn532.next_action_tick = now + PN532_POLL_INTERVAL_MS;
}

static void pn532_task(void)
{
    static const uint8_t poll_short[] =
    {
        PN532_TFI, PN532_IN_LIST_PASSIVE_TARGET, 0x01U, 0x00U
    };
    static const uint8_t poll_long[] =
    {
        PN532_TFI, PN532_IN_LIST_PASSIVE_TARGET, 0x01U, 0x01U,
        0x00U, 0xFFU, 0xFFU, 0x01U, 0x00U
    };
    uint32_t now = HAL_GetTick();
    const uint8_t *command;
    uint8_t command_length;
    uint32_t timeout;

    pn532_link_task(now);

    if (!pn532.initialized)
    {
        if (!tick_due(now, pn532.next_action_tick))
        {
            return;
        }

        if (pn532.init_index < sizeof(pn532_init_lengths))
        {
            if (pn532_send_payload(
                    pn532_init_commands[pn532.init_index],
                    pn532_init_lengths[pn532.init_index]))
            {
                pn532.init_index++;
            }
            pn532.next_action_tick = now + PN532_INIT_INTERVAL_MS;
            return;
        }

        pn532.initialized = true;
        pn532_set_read_state(PN532_READ_IDLE, now);
        pn532.next_action_tick = now;
        pn532_debug_text("[PN532] INIT DONE\r\n");
    }

    if (pn532.read_state != PN532_READ_IDLE)
    {
        timeout = (pn532.read_state == PN532_READ_POLLING) ?
                  PN532_POLL_TIMEOUT_MS :
                  PN532_COMMAND_TIMEOUT_MS;
        if (elapsed_at_least(now, pn532.state_tick, timeout))
        {
            if (pn532.read_state == PN532_READ_POLLING)
            {
                pn532_debug_text("[PN532 TIMEOUT] POLLING\r\n");
            }
            else if (pn532.read_state == PN532_READ_AUTHING)
            {
                pn532_debug_text("[PN532 TIMEOUT] AUTHING\r\n");
            }
            else
            {
                pn532_debug_text("[PN532 TIMEOUT] READING\r\n");
            }
            pn532_set_idle(now);
        }
        return;
    }

    if (!tick_due(now, pn532.next_action_tick))
    {
        return;
    }

    if ((pn532.poll_counter % 6U) == 0U)
    {
        command = poll_short;
        command_length = sizeof(poll_short);
    }
    else
    {
        command = poll_long;
        command_length = sizeof(poll_long);
    }

    pn532.poll_counter++;
    if (pn532_send_payload(command, command_length))
    {
        pn532_set_read_state(PN532_READ_POLLING, now);
    }
    else
    {
        pn532.next_action_tick = now + PN532_POLL_INTERVAL_MS;
    }
}

static void pn532_handle_poll_response(const uint8_t *payload,
                                       uint8_t length,
                                       uint32_t now)
{
    static const uint8_t mifare_key[] =
    {
        0x57U, 0x43U, 0x43U, 0x46U, 0x76U, 0x32U
    };
    uint8_t auth_command[21];
    uint8_t uid_length;

    if (pn532.read_state != PN532_READ_POLLING)
    {
        return;
    }

    if ((length < 3U) || (payload[2] == 0U))
    {
        pn532_set_idle(now);
        return;
    }

    if (!elapsed_at_least(now,
                          pn532.last_physical_card_tick,
                          PN532_CARD_DEBOUNCE_MS))
    {
        pn532_set_idle(now);
        return;
    }

    if ((length >= 14U) && (payload[4] == 0x14U))
    {
        pn532_debug_text("[PN532 CARD] FELICA\r\n");
        pn532_debug_hex("[PN532 FELICA IDM] ", &payload[6], 8U);
        pn532.last_physical_card_tick = now;
        pn532_store_felica_idm(&payload[6], now);
        pn532_set_idle(now);
        return;
    }

    if (length < 8U)
    {
        pn532_set_idle(now);
        return;
    }

    uid_length = payload[7];
    if ((uid_length == 0U) ||
        (uid_length > 10U) ||
        ((uint16_t)8U + uid_length > length))
    {
        pn532_set_idle(now);
        return;
    }

    auth_command[0] = PN532_TFI;
    auth_command[1] = PN532_IN_DATA_EXCHANGE;
    auth_command[2] = 0x01U;
    auth_command[3] = 0x60U;
    auth_command[4] = 0x02U;
    memcpy(&auth_command[5], mifare_key, sizeof(mifare_key));
    memcpy(&auth_command[11], &payload[8], uid_length);

    pn532_debug_text("[PN532 CARD] MIFARE\r\n");
    pn532_debug_hex("[PN532 MIFARE UID] ", &payload[8], uid_length);
    if (pn532_send_payload(auth_command, (uint8_t)(11U + uid_length)))
    {
        pn532_debug_text("[PN532] AUTH START BLOCK 2\r\n");
        pn532_set_read_state(PN532_READ_AUTHING, now);
    }
    else
    {
        pn532_set_idle(now);
    }
}

static void pn532_handle_data_response(const uint8_t *payload,
                                       uint8_t length,
                                       uint32_t now)
{
    static const uint8_t read_command[] =
    {
        PN532_TFI, PN532_IN_DATA_EXCHANGE, 0x01U, 0x30U, 0x02U
    };
    uint8_t status = (length >= 3U) ? payload[2] : 0xFFU;

    if (pn532.read_state == PN532_READ_AUTHING)
    {
        if (status == 0U)
        {
            if (pn532_send_payload(read_command, sizeof(read_command)))
            {
                pn532_debug_text("[PN532] AUTH OK, READ BLOCK 2\r\n");
                pn532_set_read_state(PN532_READ_READING, now);
                return;
            }
            pn532_debug_text("[PN532] AUTH OK, READ TX FAILED\r\n");
        }
        else
        {
            pn532_debug_status("[PN532] AUTH FAILED STATUS: ", status);
        }

        pn532.last_physical_card_tick = now;
        pn532_set_idle(now);
    }
    else if (pn532.read_state == PN532_READ_READING)
    {
        if ((status == 0U) && (length >= 19U))
        {
            pn532_debug_text("[PN532] READ BLOCK 2 OK\r\n");
            pn532.last_physical_card_tick = now;
            card_update(&payload[3], now);
        }
        else if (status != 0U)
        {
            pn532_debug_status("[PN532] READ FAILED STATUS: ", status);
        }
        else
        {
            pn532_debug_text("[PN532] READ RESPONSE TOO SHORT\r\n");
        }
        pn532_set_idle(now);
    }
}

static void pn532_store_felica_idm(const uint8_t idm[8], uint32_t now)
{
    uint64_t value = 0U;
    uint8_t decimal_digits[20];
    uint8_t block_data[16] = { 0U };
    uint8_t index;

    for (index = 0U; index < 8U; index++)
    {
        value = (value << 8U) | idm[index];
    }

    for (index = 20U; index > 0U; index--)
    {
        decimal_digits[index - 1U] = (uint8_t)(value % 10U);
        value /= 10U;
    }

    for (index = 0U; index < 10U; index++)
    {
        block_data[6U + index] =
            (uint8_t)((decimal_digits[index * 2U] << 4U) |
                      decimal_digits[index * 2U + 1U]);
    }

    card_update(block_data, now);
}

static void aime_reset_rx_parser(void)
{
    host.rx.active = false;
    host.rx.escaped = false;
    host.rx.frame_length = 0U;
    host.rx.count = 0U;
}

static void aime_host_task(void)
{
    if (!tud_cdc_n_ready(AIME_CDC_ITF))
    {
        aime_reset_rx_parser();
        host.tx_pending = false;
        return;
    }

    aime_host_tx_task();
    aime_host_rx_task();
    aime_host_tx_task();
}

static void aime_host_rx_task(void)
{
    uint16_t count = 0U;
    uint8_t data;

    while ((tud_cdc_n_available(AIME_CDC_ITF) != 0U) &&
           (count < AIME_CDC_RX_BUDGET))
    {
        if (tud_cdc_n_read(AIME_CDC_ITF, &data, 1U) != 1U)
        {
            break;
        }
        aime_parse_byte(data);
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

static void aime_parse_byte(uint8_t data)
{
    uint8_t checksum = 0U;
    uint8_t index;

    if (data == AIME_FRAME_START)
    {
        host.rx.active = true;
        host.rx.escaped = false;
        host.rx.frame_length = 0U;
        host.rx.count = 0U;
        return;
    }

    if (!host.rx.active)
    {
        return;
    }

    if (data == AIME_FRAME_ESCAPE)
    {
        host.rx.escaped = true;
        return;
    }

    if (host.rx.escaped)
    {
        data = (uint8_t)(data + 1U);
        host.rx.escaped = false;
    }

    if (host.rx.count >= sizeof(host.rx.data))
    {
        aime_reset_rx_parser();
        return;
    }

    host.rx.data[host.rx.count++] = data;
    if (host.rx.count == 1U)
    {
        host.rx.frame_length = data;
        if ((host.rx.frame_length < 5U) ||
            ((uint16_t)host.rx.frame_length + 1U >
             sizeof(host.rx.data)))
        {
            aime_reset_rx_parser();
        }
        return;
    }

    if (host.rx.count < (uint8_t)(host.rx.frame_length + 1U))
    {
        return;
    }

    if (host.rx.count == (uint8_t)(host.rx.frame_length + 1U))
    {
        for (index = 0U; index + 1U < host.rx.count; index++)
        {
            checksum = (uint8_t)(checksum + host.rx.data[index]);
        }

        if (checksum == host.rx.data[host.rx.count - 1U])
        {
            aime_handle_request(host.rx.data, host.rx.count);
        }
    }

    aime_reset_rx_parser();
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
    static const uint8_t card_present_response[] =
    {
        0x01U, 0x10U, 0x04U, 0x01U, 0x02U, 0x03U, 0x04U
    };
    static const uint8_t card_absent_response[] = { 0x00U };
    static const uint8_t empty_block[16] = { 0U };
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

        case 0x42U:
            if (card_is_present(now))
            {
                response_payload = card_present_response;
                response_length = sizeof(card_present_response);
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
                response_payload = card_state.block_data;
            }
            else
            {
                response_payload = empty_block;
            }
            response_length = sizeof(card_state.block_data);
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
    uint8_t packet_length;
    uint8_t checksum = 0U;
    uint8_t output_length = 0U;
    uint8_t index;

    if (host.tx_pending ||
        ((uint16_t)payload_length + 7U > sizeof(packet)))
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

    host.tx_data[output_length++] = AIME_FRAME_START;
    for (index = 0U; index < packet_length; index++)
    {
        if (!aime_encode_byte(packet[index],
                              host.tx_data,
                              &output_length))
        {
            return;
        }
    }
    if (!aime_encode_byte(checksum, host.tx_data, &output_length))
    {
        return;
    }

    host.tx_length = output_length;
    host.tx_pending = true;
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
