#include "button_detector_config.h"

// 基于 PhysicalToLogicalMap 的每通道所属区块
static const char block_map[34] = {
    'E', 'D', 'B', 'A', 'C', 'E', 'D', 'B', 'A', 'E',
    'D', 'B', 'A', 'E', 'D', 'B', 'A', 'E', 'D', 'B',
    'A', 'C', 'E', 'D', 'B', 'A', 'E', 'D', 'B', 'A',
    'E', 'D', 'B', 'A'
};

char detector_get_block(uint8_t physical_channel) {
    if (physical_channel >= 34) return 0xff;
    return block_map[physical_channel];
}
