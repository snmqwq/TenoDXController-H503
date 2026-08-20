#include "tusb.h"
#include <stdio.h>
//--------------------------------------------------------------------
// Device Descriptor
//--------------------------------------------------------------------

#define USB_VID   0x0483
#define USB_PID   0x5740
#define USB_BCD   0x0100

enum
{
  ITF_NUM_CDC0 = 0,
  ITF_NUM_CDC0_DATA,
  ITF_NUM_CDC1,
  ITF_NUM_CDC1_DATA,
  ITF_NUM_CDC2,
  ITF_NUM_CDC2_DATA,
  ITF_NUM_CDC3,
  ITF_NUM_CDC3_DATA,
  ITF_NUM_HID,
  ITF_NUM_TOTAL
};

#define APP_TUD_CDC_COMPAT_DESC_LEN \
  (8 + 9 + 5 + 5 + 4 + 5 + 9 + 7 + 7)

#define APP_TUD_CDC_COMPAT_DESCRIPTOR( \
    _itfnum, _stridx, _epout, _epin, _epsize) \
  /* Interface Association Descriptor */ \
  8, TUSB_DESC_INTERFACE_ASSOCIATION, \
  _itfnum, 2, \
  TUSB_CLASS_CDC, CDC_COMM_SUBCLASS_ABSTRACT_CONTROL_MODEL, \
  CDC_COMM_PROTOCOL_NONE, 0, \
  /* CDC Control Interface */ \
  9, TUSB_DESC_INTERFACE, \
  _itfnum, 0, 0, \
  TUSB_CLASS_CDC, CDC_COMM_SUBCLASS_ABSTRACT_CONTROL_MODEL, \
  CDC_COMM_PROTOCOL_NONE, _stridx, \
  /* CDC Header Functional Descriptor */ \
  5, TUSB_DESC_CS_INTERFACE, CDC_FUNC_DESC_HEADER, \
  U16_TO_U8S_LE(0x0120), \
  /* Call Management Functional Descriptor */ \
  5, TUSB_DESC_CS_INTERFACE, CDC_FUNC_DESC_CALL_MANAGEMENT, \
  0, (uint8_t)((_itfnum) + 1), \
  /* Accept line coding and control-line-state requests. */ \
  4, TUSB_DESC_CS_INTERFACE, \
  CDC_FUNC_DESC_ABSTRACT_CONTROL_MANAGEMENT, 0x02, \
  /* Union Functional Descriptor */ \
  5, TUSB_DESC_CS_INTERFACE, CDC_FUNC_DESC_UNION, \
  _itfnum, (uint8_t)((_itfnum) + 1), \
  /* CDC Data Interface */ \
  9, TUSB_DESC_INTERFACE, \
  (uint8_t)((_itfnum) + 1), 0, 2, \
  TUSB_CLASS_CDC_DATA, 0, 0, 0, \
  /* Bulk OUT */ \
  7, TUSB_DESC_ENDPOINT, _epout, TUSB_XFER_BULK, \
  U16_TO_U8S_LE(_epsize), 0, \
  /* Bulk IN */ \
  7, TUSB_DESC_ENDPOINT, _epin, TUSB_XFER_BULK, \
  U16_TO_U8S_LE(_epsize), 0

#define CONFIG_TOTAL_LEN  \
  (TUD_CONFIG_DESC_LEN + 4 * APP_TUD_CDC_COMPAT_DESC_LEN + \
   TUD_HID_DESC_LEN)

// Endpoint Address
#define EPNUM_CDC0_OUT     0x01
#define EPNUM_CDC0_IN      0x81

#define EPNUM_CDC1_OUT     0x02
#define EPNUM_CDC1_IN      0x82

#define EPNUM_CDC2_OUT     0x03
#define EPNUM_CDC2_IN      0x83

#define EPNUM_CDC3_OUT     0x04
#define EPNUM_CDC3_IN      0x84

#define EPNUM_HID_IN       0x85

tusb_desc_device_t const desc_device =
{
  .bLength            = sizeof(tusb_desc_device_t),
  .bDescriptorType    = TUSB_DESC_DEVICE,
  .bcdUSB             = 0x0200,

  .bDeviceClass       = TUSB_CLASS_MISC,
  .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
  .bDeviceProtocol    = MISC_PROTOCOL_IAD,

  .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,

  .idVendor           = USB_VID,
  .idProduct          = USB_PID,
  .bcdDevice          = USB_BCD,

  .iManufacturer      = 0x01,
  .iProduct           = 0x02,
  .iSerialNumber      = 0x03,

  .bNumConfigurations = 0x01
};

uint8_t const * tud_descriptor_device_cb(void)
{
  return (uint8_t const *) &desc_device;
}

//--------------------------------------------------------------------
// HID Report Descriptor
//--------------------------------------------------------------------

#define TUD_HID_REPORT_DESC_KEYBOARD_12KRO(...) \
  HID_USAGE_PAGE ( HID_USAGE_PAGE_DESKTOP     )                    ,\
  HID_USAGE      ( HID_USAGE_DESKTOP_KEYBOARD )                    ,\
  HID_COLLECTION ( HID_COLLECTION_APPLICATION )                    ,\
    __VA_ARGS__ \
    HID_USAGE_PAGE ( HID_USAGE_PAGE_KEYBOARD )                     ,\
      HID_USAGE_MIN    ( 224                                    )  ,\
      HID_USAGE_MAX    ( 231                                    )  ,\
      HID_LOGICAL_MIN  ( 0                                      )  ,\
      HID_LOGICAL_MAX  ( 1                                      )  ,\
      HID_REPORT_COUNT ( 8                                      )  ,\
      HID_REPORT_SIZE  ( 1                                      )  ,\
      HID_INPUT        ( HID_DATA | HID_VARIABLE | HID_ABSOLUTE )  ,\
      HID_REPORT_COUNT ( 1                                      )  ,\
      HID_REPORT_SIZE  ( 8                                      )  ,\
      HID_INPUT        ( HID_CONSTANT                           )  ,\
    HID_USAGE_PAGE  ( HID_USAGE_PAGE_LED                   )       ,\
      HID_USAGE_MIN    ( 1                                       ) ,\
      HID_USAGE_MAX    ( 5                                       ) ,\
      HID_REPORT_COUNT ( 5                                       ) ,\
      HID_REPORT_SIZE  ( 1                                       ) ,\
      HID_OUTPUT       ( HID_DATA | HID_VARIABLE | HID_ABSOLUTE  ) ,\
      HID_REPORT_COUNT ( 1                                       ) ,\
      HID_REPORT_SIZE  ( 3                                       ) ,\
      HID_OUTPUT       ( HID_CONSTANT                            ) ,\
    HID_USAGE_PAGE ( HID_USAGE_PAGE_KEYBOARD )                     ,\
      HID_USAGE_MIN    ( 0                                   )     ,\
      HID_USAGE_MAX_N  ( 255, 2                              )     ,\
      HID_LOGICAL_MIN  ( 0                                   )     ,\
      HID_LOGICAL_MAX_N( 255, 2                              )     ,\
      HID_REPORT_COUNT ( 12                                  )     ,\
      HID_REPORT_SIZE  ( 8                                   )     ,\
      HID_INPUT        ( HID_DATA | HID_ARRAY | HID_ABSOLUTE )     ,\
  HID_COLLECTION_END

uint8_t const desc_hid_report[] =
{
  TUD_HID_REPORT_DESC_KEYBOARD_12KRO()
};

uint8_t const * tud_hid_descriptor_report_cb(uint8_t instance)
{
  (void) instance;
  return desc_hid_report;
}

//--------------------------------------------------------------------
// Configuration Descriptor
//--------------------------------------------------------------------

uint8_t const desc_configuration[] =
{
  TUD_CONFIG_DESCRIPTOR(
    1,
    ITF_NUM_TOTAL,
    0,
    CONFIG_TOTAL_LEN,
    TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP,
    100
  ),

  // CDC0
  APP_TUD_CDC_COMPAT_DESCRIPTOR(
    ITF_NUM_CDC0,
    4,
    EPNUM_CDC0_OUT,
    EPNUM_CDC0_IN,
    64
  ),

  // CDC1
  APP_TUD_CDC_COMPAT_DESCRIPTOR(
    ITF_NUM_CDC1,
    5,
    EPNUM_CDC1_OUT,
    EPNUM_CDC1_IN,
    64
  ),

  // CDC2
  APP_TUD_CDC_COMPAT_DESCRIPTOR(
    ITF_NUM_CDC2,
    6,
    EPNUM_CDC2_OUT,
    EPNUM_CDC2_IN,
    64
  ),

  // CDC3 Debug (Magic protocol only)
  APP_TUD_CDC_COMPAT_DESCRIPTOR(
    ITF_NUM_CDC3,
    7,
    EPNUM_CDC3_OUT,
    EPNUM_CDC3_IN,
    64
  ),

  // HID Keyboard
  TUD_HID_DESCRIPTOR(
    ITF_NUM_HID,
    8,
    HID_ITF_PROTOCOL_KEYBOARD,
    sizeof(desc_hid_report),
    EPNUM_HID_IN,
    CFG_TUD_HID_EP_BUFSIZE,
    10
  )
};

_Static_assert(sizeof(desc_configuration) == CONFIG_TOTAL_LEN,
               "USB configuration descriptor length mismatch");

uint8_t const * tud_descriptor_configuration_cb(uint8_t index)
{
  (void) index;
  return desc_configuration;
}

//--------------------------------------------------------------------
// String Descriptors
//--------------------------------------------------------------------

char const * string_desc_arr[] =
{
  (const char[]) { 0x09, 0x04 },
  "Teno_DX",
  "TenoDX Controller",
  "SERIAL_BY_UID",
  "TenoDX Touch Port",
  "TenoDX LED Port",
  "TenoDX Aime Port",
  "TenoDX Debug Port",
  "HID Keyboard"
};

static uint16_t _desc_str[32];

static void get_unique_id_16(char out[17])
{
  uint32_t uid0 = HAL_GetUIDw0();
  uint32_t uid1 = HAL_GetUIDw1();
  uint32_t uid2 = HAL_GetUIDw2();

  // 折叠成 64-bit，再转成 16位HEX字符串
  uint32_t hi = uid0 ^ uid2;
  uint32_t lo = uid1;

  snprintf(out, 17, "%08lX%08lX",
           (unsigned long)hi,
           (unsigned long)lo);
}

uint16_t const * tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
  (void) langid;

  char const *str = NULL;
  char serial[17];
  uint8_t chr_count;

  if (index == 0)
  {
    _desc_str[1] = 0x0409;
    _desc_str[0] = (TUSB_DESC_STRING << 8) | (2 * 1 + 2);
    return _desc_str;
  }

  if (index == 3)
  {
    get_unique_id_16(serial);
    str = serial;
  }
  else
  {
    if (index >= sizeof(string_desc_arr) / sizeof(string_desc_arr[0]))
      return NULL;

    str = string_desc_arr[index];
  }

  chr_count = strlen(str);
  if (chr_count > 31) chr_count = 31;

  for (uint8_t i = 0; i < chr_count; i++)
  {
    _desc_str[1 + i] = str[i];
  }

  _desc_str[0] = (TUSB_DESC_STRING << 8) | (2 * chr_count + 2);

  return _desc_str;
}
