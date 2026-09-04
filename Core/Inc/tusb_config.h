#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32h5xx_hal.h"

//--------------------------------------------------------------------
// COMMON CONFIGURATION
//--------------------------------------------------------------------

#define CFG_TUSB_MCU                OPT_MCU_STM32H5
#define CFG_TUSB_RHPORT0_MODE       (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS                 OPT_OS_NONE
#endif

#define CFG_TUSB_DEBUG              0

//--------------------------------------------------------------------
// DEVICE CONFIGURATION
//--------------------------------------------------------------------

#define CFG_TUD_ENDPOINT0_SIZE      64

// 4 个无 Notification 端点的 CDC，1 个 HID
#define CFG_TUD_CDC                 4
#define CFG_TUD_HID                 1
#define CFG_TUD_MSC                 0
#define CFG_TUD_MIDI                0
#define CFG_TUD_VENDOR              0

// EP0 + CDC0..3 Bulk + HID，共使用端点编号 0..5 和接口编号 0..8
#define CFG_TUD_CDC_NOTIFY          0
#define CFG_TUD_ENDPPOINT_MAX       6
#define CFG_TUD_INTERFACE_MAX       9

// CDC buffer
#define CFG_TUD_CDC_RX_BUFSIZE      256
#define CFG_TUD_CDC_TX_BUFSIZE      256
#define CFG_TUD_CDC_EP_BUFSIZE      64

// HID buffer
#define CFG_TUD_HID_EP_BUFSIZE      16

#ifdef __cplusplus
}
#endif

#endif
