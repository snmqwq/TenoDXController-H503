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
                            bool mai2touch_active)
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

    if (mode_changed)
    {
        cdc_manager_set_mai2touch_active(mai2touch_active);
        mai2touch_init();
        touch_pipeline_init();
    }

    if (mapping_changed)
    {
        /* The state machine drains an active I2C read before it follows the
         * complete boot initialization path with the new mapping table.
         */
        tenodata_request_reconfigure();
    }

    return true;
}

bool touch_app_get_psoc_status(TenodataStatusSnapshot *snapshot)
{
    return tenodata_get_status(snapshot);
}
