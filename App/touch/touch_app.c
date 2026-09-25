#include "touch_app.h"

#include <string.h>

#include "cdc_manager.h"
#include "mai2touch.h"
#include "tenodata.h"
#include "touch_pipeline.h"

/* Magic configuration is handled synchronously in the main loop. */
static TenodataChannelMapping
    current_mapping_buffer[TENODATA_TOTAL_CHANNELS];

void touch_app_init(void)
{
    cdc_manager_init();
    tenodata_init();
    mai2touch_init();
}

void touch_app_task(void)
{
    tenodata_task();
    mai2touch_task();
}

bool touch_app_is_mai2touch_active(void)
{
    return cdc_manager_is_mai2touch_active();
}

bool touch_app_get_mapping(TenodataChannelMapping *mapping, uint8_t count)
{
    return tenodata_config_get_mapping(mapping, count);
}

bool touch_app_get_default_mapping(TenodataChannelMapping *mapping,
                                   uint8_t count)
{
    return tenodata_config_get_default_mapping(mapping, count);
}

bool touch_app_apply_config(TenodataChannelMapping const *mapping,
                            uint8_t count,
                            bool mai2touch_active,
                            bool force_host_config_resend)
{
    bool mapping_changed;
    bool mode_changed;

    if (!tenodata_config_validate_mapping(mapping, count) ||
        !tenodata_config_get_mapping(current_mapping_buffer,
                                      TENODATA_TOTAL_CHANNELS))
    {
        return false;
    }

    mapping_changed =
        memcmp(current_mapping_buffer,
               mapping,
               sizeof(current_mapping_buffer)) != 0;
    mode_changed =
        cdc_manager_is_mai2touch_active() != mai2touch_active;

    if (mapping_changed &&
        !tenodata_config_set_mapping(mapping, count))
    {
        return false;
    }

    if (force_host_config_resend)
    {
        /* 显式要求上位机重新下发扫描参数: 丢弃上一次的参数覆盖,
         * 由下面统一的 request_reconfigure 走 WAIT_HOST_CONFIG。 */
        tenodata_clear_host_config_override();
    }

    if (mode_changed)
    {
        cdc_manager_set_mai2touch_active(mai2touch_active);
        mai2touch_init();
        touch_pipeline_init();
        /* 切换输出模式时必须丢弃上一模式的扫描参数覆盖, 否则
         * "在 raw 模式调过参 -> 切到 Mai2Touch" 会继续沿用上位机参数,
         * 而不是本地固定档位。清掉后 raw 模式会重新向上位机索要参数,
         * Mai2Touch 模式则直接用本地档位初始化。 */
        tenodata_clear_host_config_override();
        /* 重走初始化流程: 切到 tenodata 模式时若无上位机配置,
         * 进入等待下发状态并上报 0x01 提示上位机; 切到 mai2touch
         * 模式则用本地默认参数直接初始化。 */
        tenodata_request_reconfigure();
    }

    if (mapping_changed)
    {
        /* The state machine drains an active I2C read before it follows the
         * complete boot initialization path with the new mapping table.
         */
        tenodata_request_reconfigure();
    }
    else if (force_host_config_resend && !mode_changed)
    {
        /* 映射与模式都没变, 仍需要重走一次初始化才能真正回到
         * WAIT_HOST_CONFIG 并上报 0x01。 */
        tenodata_request_reconfigure();
    }

    return true;
}

void touch_app_force_host_config_resend(void)
{
    tenodata_force_host_config_resend();
}

bool touch_app_get_cp(uint8_t device_index, uint8_t *cp_out)
{
    return tenodata_get_cp(device_index, cp_out);
}

bool touch_app_get_comp_idac(uint8_t device_index, uint8_t *comp_out)
{
    return tenodata_get_comp_idac(device_index, comp_out);
}

bool touch_app_get_cal_age_ms(uint8_t device_index, uint16_t *age_ms)
{
    return tenodata_get_cal_age_ms(device_index, age_ms);
}

bool touch_app_get_psoc_status(TenodataStatusSnapshot *snapshot)
{
    return tenodata_get_status(snapshot);
}
