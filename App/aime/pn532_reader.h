#ifndef PN532_READER_H
#define PN532_READER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#define PN532_READER_CARD_BLOCK_LENGTH          16U
#define PN532_READER_MIFARE_UID_MAX_LENGTH      10U
#define PN532_READER_FELICA_IDM_LENGTH           8U
#define PN532_READER_FELICA_PMM_LENGTH           8U
#define PN532_READER_CARD_ID_MAX_LENGTH          16U

#define PN532_READER_CARD_TYPE_MIFARE           0x10U
#define PN532_READER_CARD_TYPE_FELICA            0x20U

typedef struct
{
    uint8_t type;
    uint8_t identifier_length;
    uint8_t identifier[PN532_READER_CARD_ID_MAX_LENGTH];
} pn532_reader_card_info_t;

void pn532_reader_init(void);
void pn532_reader_task(void);
void pn532_reader_start_polling(void);
void pn532_reader_stop_polling(void);
bool pn532_reader_copy_card_info(uint32_t now,
                                 pn532_reader_card_info_t *card_info);
void pn532_reader_copy_last_block(
    uint8_t block[PN532_READER_CARD_BLOCK_LENGTH]);

#ifdef __cplusplus
}
#endif

#endif /* PN532_READER_H */
