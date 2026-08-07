#ifndef __TENODATA_H__
#define __TENODATA_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* These values are part of the Magic status protocol. Keep them stable. */
#define TENODATA_STATUS_DEVICE_COUNT              2U
#define TENODATA_STATUS_FLAG_REINIT_REQUESTED     0x01U
#define TENODATA_STATUS_FLAG_READ_INFLIGHT        0x02U

#define TENODATA_DEVICE_FLAG_CONNECTED            0x01U
#define TENODATA_DEVICE_FLAG_OPERATIONAL          0x02U
#define TENODATA_DEVICE_FLAG_UNAVAILABLE          0x04U
#define TENODATA_DEVICE_FLAG_STATUS_VALID         0x08U
#define TENODATA_DEVICE_FLAG_SOFT_RESET_SUPPORTED 0x10U
#define TENODATA_DEVICE_FLAG_LEGACY_FIRMWARE      0x20U
#define TENODATA_DEVICE_FLAG_POWER_CYCLE_REQUIRED 0x40U

typedef struct
{
    uint8_t address;
    uint8_t status;
    uint8_t flags;
    uint8_t consecutive_failures;
    uint16_t status_age_ms;
} TenodataPsocStatus;

typedef struct
{
    uint8_t state;
    uint8_t flags;
    uint8_t device_count;
    TenodataPsocStatus devices[TENODATA_STATUS_DEVICE_COUNT];
} TenodataStatusSnapshot;

void tenodata_init(void);
void tenodata_request_reconfigure(void);
void tenodata_task(void);
bool tenodata_get_status(TenodataStatusSnapshot *snapshot);

#ifdef __cplusplus
}
#endif

#endif /* __TENODATA_H__ */
