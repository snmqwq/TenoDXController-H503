#ifndef __TOUCH_MODE_H__
#define __TOUCH_MODE_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * CDC0 输出模式的自动识别。
 *
 * 背景: 同一个 USB 口要被两方使用 ——
 *   · 游戏客户端: Mai2Touch 协议, 命令是 6 字节 `{` + 4 字节命令体 + `}`
 *   · 上位机调试: raw 协议, 扫描参数帧是 `0xAA 0x01` + 136 字节 + 校验和
 * 手工切换模式很麻烦, 所以这里按"收到的帧特征"自动判断该走哪条流程:
 *   · raw 侧看到一个**完整且合法**的 Mai2Touch 命令 -> 切到 Mai2Touch 流程
 *   · Mai2Touch 侧看到 `0xAA 0x01`                  -> 切回 raw 流程
 *
 * 识别只在 cdc_manager 处于 AUTO 模式时生效; 一旦用 Magic 显式设过模式,
 * 就固定为该模式, 不再自动切换 (便于强制调试)。
 *
 * 两个方向为什么不对称:
 *   · Mai2Touch 的数据字节只用到低 5 位 (<=0x1F), 命令是 ASCII, 流里**永远
 *     不会出现 0xAA**, 所以 `0xAA 0x01` 是可靠标志, 看到就可以切。
 *   · raw 帧的 136 字节 payload 全是小数值 (res/mod/sns/div <= 16), 唯一的
 *     校验和字节却可能等于 `{`(0x7B)。若一看到 `{` 就切, 上位机每秒重发一次
 *     配置文件, 迟早会误切。所以 raw -> Mai2Touch 必须凑齐 6 字节并校验命令
 *     语法才切换。
 */

// 上电调用一次
void touch_mode_init(void);

// 每读到一个 CDC 字节就喂进来 (由当前活跃的消费者调用)
void touch_mode_observe_byte(uint8_t byte);

/*
 * 已识别出"对方是 Mai2Touch"、但切换还没执行。
 *
 * raw 消费者用它决定是否立刻停手: 切换是在 touch_mode_task() 里才执行的,
 * 而识别是读字节当刻完成的 —— 只看 cdc_manager_is_mai2touch_active() 会多读
 * 字节, 把对方命令吃掉, 游戏就收不到完整命令。
 */
bool touch_mode_pending_to_mai2touch(void);

/* 已识别出"对方是 raw"、但切换还没执行 (mai2touch 消费者用) */
bool touch_mode_pending_to_raw(void);

/*
 * 取回"触发切换的那条 Mai2Touch 命令"原文 (6 字节), 取一次就清空。
 *
 * 为什么需要它: raw 侧为了校验命令语法, 已经把整条命令从 CDC 读走了; 而
 * `{STAT}`(开始推流) 与 `{L/R..}`(必须回显, 游戏靠回显推进配置) 都不是
 * "复位"语义, 不能靠 mai2touch_init() 顶替 —— 否则游戏会卡在配置阶段或
 * 收不到触摸数据。所以切换完成后要把这条命令原样补喂给 mai2touch 解析。
 */
bool touch_mode_take_replay(uint8_t *frame_out, uint8_t *length_out);

// 主循环调用: 消化识别结果 —— 切换模式、清参数覆盖、必要时请求重配
void touch_mode_task(void);

#ifdef __cplusplus
}
#endif

#endif /* __TOUCH_MODE_H__ */
