#ifndef PN532_READER_H
#define PN532_READER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#define PN532_READER_CARD_BLOCK_LENGTH  16U

void pn532_reader_init(void);
void pn532_reader_task(void);
bool pn532_reader_card_is_present(uint32_t now);
void pn532_reader_copy_last_block(
    uint8_t block[PN532_READER_CARD_BLOCK_LENGTH]);

#ifdef __cplusplus
}
#endif

#endif /* PN532_READER_H */
