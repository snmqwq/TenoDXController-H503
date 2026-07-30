#ifndef __MAGIC_CONFIG_H__
#define __MAGIC_CONFIG_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#define MAGIC_CONFIG_MODULE_GLOBAL     0x00U
#define MAGIC_CONFIG_MODULE_TOUCH      0x10U
#define MAGIC_CONFIG_MODULE_LIGHT      0x20U
#define MAGIC_CONFIG_MODULE_READER     0x30U
#define MAGIC_CONFIG_MODULE_KEYBOARD   0x40U

#define MAGIC_CONFIG_CMD_READ          0x01U
#define MAGIC_CONFIG_CMD_WRITE         0x02U
#define MAGIC_CONFIG_CMD_SAVE          0x03U
#define MAGIC_CONFIG_CMD_LOAD_DEFAULT  0x04U
#define MAGIC_CONFIG_CMD_GET_INFO      0x05U

#define MAGIC_CONFIG_CMD_READ_ALL      0x81U
#define MAGIC_CONFIG_CMD_WRITE_ALL     0x82U
#define MAGIC_CONFIG_CMD_SAVE_ALL      0x83U
#define MAGIC_CONFIG_CMD_ENTER_DFU     0x84U

#define MAGIC_CONFIG_PARAM_ALL         0x00U
#define MAGIC_CONFIG_DFU_CONFIRM       0xA5U

#define MAGIC_CONFIG_STATUS_OK             0x00U
#define MAGIC_CONFIG_STATUS_SUM_ERROR      0x01U
#define MAGIC_CONFIG_STATUS_MODULE_ERROR   0x02U
#define MAGIC_CONFIG_STATUS_CMD_ERROR      0x03U
#define MAGIC_CONFIG_STATUS_PARAM_ERROR    0x04U
#define MAGIC_CONFIG_STATUS_LENGTH_ERROR   0x05U
#define MAGIC_CONFIG_STATUS_IO_ERROR       0x06U

#define MAGIC_CONFIG_MAX_PAYLOAD       192U

typedef bool (*magic_config_read_cb_t)(uint8_t param,
                                       uint8_t *data,
                                       uint8_t max_length,
                                       uint8_t *out_length);
typedef bool (*magic_config_write_cb_t)(uint8_t param,
                                        uint8_t const *data,
                                        uint8_t length);
typedef bool (*magic_config_action_cb_t)(uint8_t param);
typedef bool (*magic_config_read_all_cb_t)(uint8_t *data,
                                           uint8_t max_length,
                                           uint8_t *out_length);
typedef bool (*magic_config_write_all_cb_t)(uint8_t const *data,
                                            uint8_t length);

typedef struct
{
    uint8_t module;
    magic_config_read_cb_t read;
    magic_config_write_cb_t write;
    magic_config_action_cb_t save;
    magic_config_action_cb_t load_default;
    magic_config_read_cb_t get_info;
    magic_config_read_all_cb_t read_all;
    magic_config_write_all_cb_t write_all;
} magic_config_module_t;

void magic_config_init(void);
void magic_config_task(void);
bool magic_config_register(magic_config_module_t const *module);
uint8_t magic_config_handle(uint8_t module,
                            uint8_t cmd,
                            uint8_t param,
                            uint8_t const *payload,
                            uint8_t payload_length,
                            uint8_t *response,
                            uint8_t response_max,
                            uint8_t *response_length);

#ifdef __cplusplus
}
#endif

#endif /* __MAGIC_CONFIG_H__ */
