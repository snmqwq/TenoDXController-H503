#ifndef __MAI2TOUCH_H__
#define __MAI2TOUCH_H__

#include <stdbool.h>
#include <stdint.h>

// 上电后调用一次
void mai2touch_init(void);

// 主循环中反复调用：接收命令 + 发送触摸数据
void mai2touch_task(void);

// 获取 34 通道触摸状态数组的指针 (供外部读取或修改)
// 每通道 1 bit: 0=未触摸, 1=触摸
// 返回指向 35-bit 位掩码 (uint64_t) 的指针
uint64_t* mai2touch_get_touch_bits(void);

#endif /* __MAI2TOUCH_H__ */
