#include "button_detector_config.h"
#include "tenodata_config.h"

char detector_get_block(uint8_t physical_channel)
{
    return tenodata_config_get_block(physical_channel);
}
