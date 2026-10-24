#ifndef __TESTS_NATIVE_BUTTON_STUBS_MAIN_H__
#define __TESTS_NATIVE_BUTTON_STUBS_MAIN_H__

#include <stdint.h>

typedef struct
{
    uint8_t port_id;
} GPIO_TypeDef;

typedef enum
{
    GPIO_PIN_RESET = 0U,
    GPIO_PIN_SET = 1U
} GPIO_PinState;

extern GPIO_TypeDef native_gpio_a;
extern GPIO_TypeDef native_gpio_b;
extern uint32_t native_test_tick;

#define GPIOA (&native_gpio_a)
#define GPIOB (&native_gpio_b)

#define BTN0_Pin  0x0001U
#define BTN1_Pin  0x0002U
#define BTN2_Pin  0x0004U
#define BTN3_Pin  0x0008U
#define BTN4_Pin  0x0010U
#define BTN5_Pin  0x0020U
#define BTN6_Pin  0x0040U
#define BTN7_Pin  0x0080U
#define BTN8_Pin  0x0001U
#define BTN9_Pin  0x0002U
#define BTN10_Pin 0x0004U
#define BTN11_Pin 0x0400U

#define BTN0_GPIO_Port  GPIOA
#define BTN1_GPIO_Port  GPIOA
#define BTN2_GPIO_Port  GPIOA
#define BTN3_GPIO_Port  GPIOA
#define BTN4_GPIO_Port  GPIOA
#define BTN5_GPIO_Port  GPIOA
#define BTN6_GPIO_Port  GPIOA
#define BTN7_GPIO_Port  GPIOA
#define BTN8_GPIO_Port  GPIOB
#define BTN9_GPIO_Port  GPIOB
#define BTN10_GPIO_Port GPIOB
#define BTN11_GPIO_Port GPIOB

uint32_t HAL_GetTick(void);
GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef *port, uint16_t pin);

#endif /* __TESTS_NATIVE_BUTTON_STUBS_MAIN_H__ */
