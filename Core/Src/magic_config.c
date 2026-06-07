#include "magic_config.h"

#include "main.h"
#include "usb.h"
#include <stddef.h>
#include <string.h>

#define MAGIC_CONFIG_MODULE_MAX    4U
#define MAGIC_CONFIG_DFU_DELAY_MS       100U
#define MAGIC_CONFIG_NVIC_CLEAR_REGS    8U

#ifndef MAGIC_CONFIG_DFU_BOOTLOADER_ADDRESS
#define MAGIC_CONFIG_DFU_BOOTLOADER_ADDRESS  0x0BF87000UL
#endif

static magic_config_module_t modules[MAGIC_CONFIG_MODULE_MAX];
static bool dfu_pending;
static uint32_t dfu_request_tick;

typedef void (*magic_config_dfu_entry_t)(void);

static void magic_config_jump_to_dfu(void)
{
    uint32_t const boot_address = MAGIC_CONFIG_DFU_BOOTLOADER_ADDRESS;
    uint32_t const boot_stack = *((uint32_t const *)boot_address);
    uint32_t const boot_reset = *((uint32_t const *)(boot_address + 4U));
    if (((boot_stack & 0x2ff00000UL) != 0x20000000UL) ||
        ((boot_reset & 0x00000001UL) == 0U))
    {
        HAL_NVIC_SystemReset();
    }

    __disable_irq();

    (void)HAL_PCD_DeInit(&hpcd_USB_DRD_FS);
    HAL_DeInit();

    SysTick->CTRL = 0U;
    SysTick->LOAD = 0U;
    SysTick->VAL = 0U;

    for (uint32_t i = 0; i < MAGIC_CONFIG_NVIC_CLEAR_REGS; i++)
    {
        NVIC->ICER[i] = 0xffffffffUL;
        NVIC->ICPR[i] = 0xffffffffUL;
    }

    SCB->VTOR = boot_address;
    __DSB();
    __ISB();

    __set_MSP(boot_stack);
    ((magic_config_dfu_entry_t)boot_reset)();

    while (1)
    {
    }
}

void magic_config_init(void)
{
    memset(modules, 0, sizeof(modules));
    dfu_pending = false;
    dfu_request_tick = 0U;
}

void magic_config_task(void)
{
    if (dfu_pending &&
        ((uint32_t)(HAL_GetTick() - dfu_request_tick) >= MAGIC_CONFIG_DFU_DELAY_MS))
    {
        magic_config_jump_to_dfu();
    }
}

bool magic_config_register(magic_config_module_t const *module)
{
    if ((module == NULL) || (module->module == 0U))
    {
        return false;
    }

    for (uint8_t i = 0; i < MAGIC_CONFIG_MODULE_MAX; i++)
    {
        if ((modules[i].module == module->module) || (modules[i].module == 0U))
        {
            modules[i] = *module;
            return true;
        }
    }

    return false;
}

static magic_config_module_t const *find_module(uint8_t module)
{
    for (uint8_t i = 0; i < MAGIC_CONFIG_MODULE_MAX; i++)
    {
        if (modules[i].module == module)
        {
            return &modules[i];
        }
    }

    return NULL;
}

static uint8_t handle_global_read_all(uint8_t *response,
                                      uint8_t response_max,
                                      uint8_t *response_length)
{
    uint8_t offset = 0U;

    if ((response == NULL) || (response_length == NULL))
    {
        return MAGIC_CONFIG_STATUS_CMD_ERROR;
    }

    for (uint8_t i = 0; i < MAGIC_CONFIG_MODULE_MAX; i++)
    {
        uint8_t entry_length = 0U;

        if ((modules[i].module == 0U) || (modules[i].read_all == NULL))
        {
            continue;
        }

        if ((uint8_t)(response_max - offset) < 2U)
        {
            return MAGIC_CONFIG_STATUS_LENGTH_ERROR;
        }

        if (!modules[i].read_all(&response[offset + 2U],
                                 (uint8_t)(response_max - offset - 2U),
                                 &entry_length))
        {
            return MAGIC_CONFIG_STATUS_PARAM_ERROR;
        }

        response[offset] = modules[i].module;
        response[offset + 1U] = entry_length;
        offset = (uint8_t)(offset + 2U + entry_length);
    }

    *response_length = offset;
    return MAGIC_CONFIG_STATUS_OK;
}

static uint8_t handle_global_write_all(uint8_t const *payload, uint8_t payload_length)
{
    uint8_t offset = 0U;

    while (offset < payload_length)
    {
        magic_config_module_t const *handler;
        uint8_t module;
        uint8_t entry_length;

        if ((uint8_t)(payload_length - offset) < 2U)
        {
            return MAGIC_CONFIG_STATUS_LENGTH_ERROR;
        }

        module = payload[offset];
        entry_length = payload[offset + 1U];
        offset = (uint8_t)(offset + 2U);

        if (entry_length > (uint8_t)(payload_length - offset))
        {
            return MAGIC_CONFIG_STATUS_LENGTH_ERROR;
        }

        handler = find_module(module);
        if ((handler == NULL) || (handler->write_all == NULL))
        {
            return MAGIC_CONFIG_STATUS_MODULE_ERROR;
        }

        if (!handler->write_all(&payload[offset], entry_length))
        {
            return MAGIC_CONFIG_STATUS_PARAM_ERROR;
        }

        offset = (uint8_t)(offset + entry_length);
    }

    return MAGIC_CONFIG_STATUS_OK;
}

static uint8_t handle_global_save_all(void)
{
    for (uint8_t i = 0; i < MAGIC_CONFIG_MODULE_MAX; i++)
    {
        if ((modules[i].module != 0U) &&
            (modules[i].save != NULL) &&
            !modules[i].save(MAGIC_CONFIG_PARAM_ALL))
        {
            return MAGIC_CONFIG_STATUS_IO_ERROR;
        }
    }

    return MAGIC_CONFIG_STATUS_OK;
}

static uint8_t handle_global_command(uint8_t cmd,
                                     uint8_t param,
                                     uint8_t const *payload,
                                     uint8_t payload_length,
                                     uint8_t *response,
                                     uint8_t response_max,
                                     uint8_t *response_length)
{
    if (response_length != NULL)
    {
        *response_length = 0U;
    }

    switch (cmd)
    {
        case MAGIC_CONFIG_CMD_READ:
        case MAGIC_CONFIG_CMD_READ_ALL:
            if ((cmd == MAGIC_CONFIG_CMD_READ) && (param != MAGIC_CONFIG_PARAM_ALL))
            {
                return MAGIC_CONFIG_STATUS_PARAM_ERROR;
            }

            return handle_global_read_all(response, response_max, response_length);

        case MAGIC_CONFIG_CMD_WRITE:
        case MAGIC_CONFIG_CMD_WRITE_ALL:
            if ((cmd == MAGIC_CONFIG_CMD_WRITE) && (param != MAGIC_CONFIG_PARAM_ALL))
            {
                return MAGIC_CONFIG_STATUS_PARAM_ERROR;
            }

            return handle_global_write_all(payload, payload_length);

        case MAGIC_CONFIG_CMD_SAVE:
        case MAGIC_CONFIG_CMD_SAVE_ALL:
            if (((cmd == MAGIC_CONFIG_CMD_SAVE) && (param != MAGIC_CONFIG_PARAM_ALL)) ||
                (payload_length != 0U))
            {
                return MAGIC_CONFIG_STATUS_PARAM_ERROR;
            }

            return handle_global_save_all();

        case MAGIC_CONFIG_CMD_ENTER_DFU:
            if ((payload == NULL) ||
                (payload_length != 1U) ||
                (payload[0] != MAGIC_CONFIG_DFU_CONFIRM))
            {
                return MAGIC_CONFIG_STATUS_PARAM_ERROR;
            }

            dfu_pending = true;
            dfu_request_tick = HAL_GetTick();
            return MAGIC_CONFIG_STATUS_OK;

        default:
            return MAGIC_CONFIG_STATUS_CMD_ERROR;
    }
}

uint8_t magic_config_handle(uint8_t module,
                            uint8_t cmd,
                            uint8_t param,
                            uint8_t const *payload,
                            uint8_t payload_length,
                            uint8_t *response,
                            uint8_t response_max,
                            uint8_t *response_length)
{
    magic_config_module_t const *handler;

    if (response_length != NULL)
    {
        *response_length = 0;
    }

    if (module == MAGIC_CONFIG_MODULE_GLOBAL)
    {
        return handle_global_command(cmd,
                                     param,
                                     payload,
                                     payload_length,
                                     response,
                                     response_max,
                                     response_length);
    }

    handler = find_module(module);

    if (handler == NULL)
    {
        return MAGIC_CONFIG_STATUS_MODULE_ERROR;
    }

    switch (cmd)
    {
        case MAGIC_CONFIG_CMD_READ:
            if ((handler->read == NULL) || (response == NULL) || (response_length == NULL))
            {
                return MAGIC_CONFIG_STATUS_CMD_ERROR;
            }

            return handler->read(param, response, response_max, response_length) ?
                   MAGIC_CONFIG_STATUS_OK : MAGIC_CONFIG_STATUS_PARAM_ERROR;

        case MAGIC_CONFIG_CMD_WRITE:
            if (handler->write == NULL)
            {
                return MAGIC_CONFIG_STATUS_CMD_ERROR;
            }

            return handler->write(param, payload, payload_length) ?
                   MAGIC_CONFIG_STATUS_OK : MAGIC_CONFIG_STATUS_PARAM_ERROR;

        case MAGIC_CONFIG_CMD_SAVE:
            if (handler->save == NULL)
            {
                return MAGIC_CONFIG_STATUS_CMD_ERROR;
            }

            return handler->save(param) ? MAGIC_CONFIG_STATUS_OK : MAGIC_CONFIG_STATUS_IO_ERROR;

        case MAGIC_CONFIG_CMD_LOAD_DEFAULT:
            if (handler->load_default == NULL)
            {
                return MAGIC_CONFIG_STATUS_CMD_ERROR;
            }

            return handler->load_default(param) ? MAGIC_CONFIG_STATUS_OK : MAGIC_CONFIG_STATUS_PARAM_ERROR;

        case MAGIC_CONFIG_CMD_GET_INFO:
            if ((handler->get_info == NULL) || (response == NULL) || (response_length == NULL))
            {
                return MAGIC_CONFIG_STATUS_CMD_ERROR;
            }

            return handler->get_info(param, response, response_max, response_length) ?
                   MAGIC_CONFIG_STATUS_OK : MAGIC_CONFIG_STATUS_PARAM_ERROR;

        default:
            return MAGIC_CONFIG_STATUS_CMD_ERROR;
    }
}
