#ifndef __TESTS_NATIVE_STUBS_MAIN_H__
#define __TESTS_NATIVE_STUBS_MAIN_H__

/* 本机单元测试用的 main.h stub (原文件依赖 STM32 HAL)。 */

#include <stdbool.h>
#include <stdint.h>

extern uint32_t native_test_tick;
#define HAL_GetTick() (native_test_tick)

#endif /* __TESTS_NATIVE_STUBS_MAIN_H__ */
