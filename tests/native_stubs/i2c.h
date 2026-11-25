#ifndef NATIVE_IO_I2C_H
#define NATIVE_IO_I2C_H

#include <stdint.h>

typedef struct { unsigned Instance; } I2C_HandleTypeDef;
enum { I2C1 = 1, I2C_MEMADD_SIZE_8BIT = 1, HAL_I2C_STATE_READY = 1 };
typedef enum { HAL_OK, HAL_ERROR } HAL_StatusTypeDef;
extern I2C_HandleTypeDef hi2c1;

HAL_StatusTypeDef HAL_I2C_IsDeviceReady(I2C_HandleTypeDef *, uint16_t, uint32_t, uint32_t);
HAL_StatusTypeDef HAL_I2C_Mem_Write(I2C_HandleTypeDef *, uint16_t, uint16_t, uint16_t,
                                   uint8_t *, uint16_t, uint32_t);
HAL_StatusTypeDef HAL_I2C_Mem_Read(I2C_HandleTypeDef *, uint16_t, uint16_t, uint16_t,
                                  uint8_t *, uint16_t, uint32_t);
HAL_StatusTypeDef HAL_I2C_Mem_Read_IT(I2C_HandleTypeDef *, uint16_t, uint16_t, uint16_t,
                                     uint8_t *, uint16_t);
void HAL_I2C_MemRxCpltCallback(I2C_HandleTypeDef *);
unsigned HAL_I2C_GetState(I2C_HandleTypeDef *);
HAL_StatusTypeDef HAL_I2C_DeInit(I2C_HandleTypeDef *);
void MX_I2C1_Init(void);
void HAL_Delay(uint32_t);

#endif
