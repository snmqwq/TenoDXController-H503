#ifndef __TOUCH_CORE_PORT_H__
#define __TOUCH_CORE_PORT_H__

#include <stdbool.h>
#include <stdint.h>

#include "teno_touch_core.h"

/*
 * H503 判定核心适配层：把共享的 teno_touch_core 接到固件的触摸管线上。
 *
 * 本文件只做三件事：
 *   1. 用固件当前的硬件映射初始化核心通道分组
 *   2. 转发逐通道判定调用
 *   3. 暴露诊断量给回放/回放测试
 *
 * 判定逻辑一行都不在这里，全部在 teno_touch_core.c。
 */

void touch_core_port_reset(void);
void touch_core_port_reset_channel(uint8_t channel);

bool touch_core_port_process(uint8_t channel,
                             double raw,
                             double diff,
                             double setup_raw);

/* 核心版本信息，用于日志与三端一致性核对。 */
uint32_t touch_core_port_abi_version(void);
uint32_t touch_core_port_algorithm_version(void);
uint32_t touch_core_port_params_hash(void);

/* 旧版核心的端口能力：0 表示不接受运行期调参。 */
#define TOUCH_CORE_PORT_ERR_UNSUPPORTED (-5)
uint32_t touch_core_port_features(void);

/*
 * 运行期覆盖 B/C/D/E 的四个判定数值 (A 区不可覆盖)。
 * 返回值是核心返回码：0 成功，负数为 TENO_TOUCH_ERR_*。
 * 数值范围与关系约束由核心校验，本层不做二次判定。
 */
int32_t touch_core_port_set_zone_tuning(uint8_t group,
                                        double diff_trig,
                                        double deriv_trig,
                                        double deriv_rel,
                                        double diff_rel);

/* 复位某区所有通道的判定状态 (切换配置 / 重新校准后)。 */
void touch_core_port_reset_zone(uint8_t group);

/* 返回该通道最近一次判定输出；通道非法时返回 NULL。 */
const TenoTouchDecision *touch_core_port_decision(uint8_t channel);

#endif /* __TOUCH_CORE_PORT_H__ */
