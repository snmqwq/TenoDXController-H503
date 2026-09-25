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

/*
 * 把一条完整的 6 字节 Mai2Touch 命令 ({ + 4 字节命令体 + }) 直接喂给解析器。
 *
 * 用途: 从 raw 模式自动切换到 Mai2Touch 时, 触发切换的那条命令已被 raw 侧
 * 读走用于语法校验, 切换完成后需要用本函数补喂回去 —— 否则 {STAT} 会让设备
 * 一直不推流, {L/R..} 会让游戏因为收不到回显而卡在配置阶段。
 */
void mai2touch_replay_command(const uint8_t *frame, uint8_t length);

#endif /* __MAI2TOUCH_H__ */
