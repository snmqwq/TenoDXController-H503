#ifndef __TESTS_NATIVE_STUBS_TUSB_H__
#define __TESTS_NATIVE_STUBS_TUSB_H__

/* 本机单元测试用的 TinyUSB stub：把 CDC0 的收发接到内存缓冲，不碰硬件。 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP 0x20u

extern uint8_t *tusb_stub_rx_buffer(void);
extern int *tusb_stub_rx_len(void);
extern uint8_t *tusb_stub_tx_buffer(void);
extern int *tusb_stub_tx_len(void);

static inline bool tud_cdc_n_ready(uint8_t itf) { (void)itf; return true; }

static inline uint32_t tud_cdc_n_write_available(uint8_t itf) {
    (void)itf;
    return 4096u;
}

static inline uint32_t tud_cdc_n_available(uint8_t itf) {
    (void)itf;
    return (uint32_t)(*tusb_stub_rx_len());
}

static inline bool tud_cdc_n_peek(uint8_t itf, uint8_t *out) {
    (void)itf;
    if (*tusb_stub_rx_len() <= 0) return false;
    *out = tusb_stub_rx_buffer()[0];
    return true;
}

static inline uint32_t tud_cdc_n_read(uint8_t itf, void *buf, uint32_t len) {
    (void)itf;
    uint32_t n = (uint32_t)(*tusb_stub_rx_len());
    if (n > len) n = len;
    if (n == 0) return 0;
    memcpy(buf, tusb_stub_rx_buffer(), n);
    memmove(tusb_stub_rx_buffer(),
            tusb_stub_rx_buffer() + n,
            (size_t)(*tusb_stub_rx_len() - (int)n));
    *tusb_stub_rx_len() -= (int)n;
    return n;
}

static inline uint32_t tud_cdc_n_write(uint8_t itf, const void *buf, uint32_t len) {
    (void)itf;
    int *len_out = tusb_stub_tx_len();
    if (*len_out + (int)len > 4096) return 0;
    memcpy(tusb_stub_tx_buffer() + *len_out, buf, len);
    *len_out += (int)len;
    return len;
}

static inline void tud_cdc_n_write_flush(uint8_t itf) { (void)itf; }

#endif /* __TESTS_NATIVE_STUBS_TUSB_H__ */
