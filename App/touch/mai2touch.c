#include "mai2touch.h"
#include "cdc_manager.h"
#include "io_mode.h"
#include "main.h"
#include "touch_mode.h"
#include "touch_pipeline.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// ================= Mai2Touch 协议常量 =================
// 命令标识 (packet[3])
#define MAI2TOUCH_CMD_RSET    0x45  // 'E'
#define MAI2TOUCH_CMD_HALT    0x4C  // 'L'
#define MAI2TOUCH_CMD_STAT    0x41  // 'A'
#define MAI2TOUCH_CMD_RATIO   0x72  // 'r'
#define MAI2TOUCH_CMD_SENS    0x6B  // 'k'

#define MAI2TOUCH_FRAME_SIZE  9U     // ( + 7 data + )
#define MAI2TOUCH_TXD_PERIOD_MS 4U   // 约 250Hz (匹配 MPR121 采样率) 的保底节奏

// ================= 触摸状态 =================
// 35 bits 位掩码, 有效 34 bits (touch bit 0~33)
// bit 0 对应映射表第一个触摸区, bit 33 对应最后一个
static uint64_t touch_bits = 0;

// 数据发送控制
static bool sending = false;        // 收到 {STAT} 后开始发送
static uint32_t next_send_tick = 0;
// 上次真正发出的掩码: 掩码一变就立刻补发, 不再等 4 ms 周期
static uint64_t last_sent_bits = 0;
static bool last_sent_valid = false;

// 串口接收缓冲
static uint8_t rx_packet[6];
static uint8_t rx_len = 0;

// ================= 公开 API =================

void mai2touch_init(void) {
    // 触摸位由 touch_pipeline (ButtonDetector) 更新，此处仅初始化为 0
    // 收到 {STAT} 后开始周期性发送
    touch_bits = 0;
    sending = false;
    rx_len = 0;
    next_send_tick = HAL_GetTick();
    last_sent_bits = 0;
    last_sent_valid = false;
}

uint64_t* mai2touch_get_touch_bits(void) {
    return &touch_bits;
}

// ================= 命令处理 =================

static void mai2touch_cmd_halt(void) {
    /* 语义: 立即停止发送触摸数据, 并且保持静默 ——
     * 游戏靠"读超时"进入配置模式 (见 docs/Mai2TouchREADME.md §8.2),
     * 这之后回任何字节都会破坏握手, 所以这里只停发, 不回包。 */
    sending = false;
}

static void mai2touch_cmd_rset(void) {
    /* §8.7: 主机的"重置"就是关串口再打开, 然后从 {RSET} 重来一遍,
     * 设备端必须在收到 {RSET} 时无条件复位自己的发送状态。 */
    touch_bits = 0;
    sending = false;
    rx_len = 0;
    last_sent_bits = 0;
    last_sent_valid = false;
}

static void mai2touch_cmd_stat(void) {
    /* 配置结束, 开始推流。立即对齐发送时刻: 不等下一个周期,
     * 让游戏尽快收到第一帧 (管线未就绪时是空白帧)。 */
    sending = true;
    next_send_tick = HAL_GetTick();
    last_sent_valid = false;
}

static void mai2touch_cmd_echo(const uint8_t *cmd) {
    // Ratio/Sens 回声：原样回复 (cmd)
    uint8_t reply[6];
    reply[0] = 0x28;  // '('
    memcpy(&reply[1], cmd, 4);
    reply[5] = 0x29;  // ')'
    cdc_manager_try_send(true, reply, 6);
}

static void mai2touch_dispatch_command(void) {
    // packet 不含帧头 '{' 和帧尾 '}', 有效命令均为 4 字节
    if (rx_len != 4) return;

    // 配置期靠回显推进 (_countRecv 用回显的 sensor 字符当游标),
    // 因此回显之间绝不能插入任何多余字节: 配置命令必须原样回显, 不夹带其他输出。
    switch (rx_packet[2]) {
        case MAI2TOUCH_CMD_RSET:
            /* {RSET} = 7b 52 53 45 54 7d, 命令标识落在 packet[2] = 'E'(0x45)。
             * 对齐 docs/Mai2TouchREADME.md §3.1: 设备无需回复, 只需复位发送状态。 */
            mai2touch_cmd_rset();
            break;
        case MAI2TOUCH_CMD_HALT:  mai2touch_cmd_halt();  break;
        case MAI2TOUCH_CMD_STAT:  mai2touch_cmd_stat();  break;
        case MAI2TOUCH_CMD_RATIO: mai2touch_cmd_echo(rx_packet); break;
        case MAI2TOUCH_CMD_SENS:  mai2touch_cmd_echo(rx_packet); break;
        default: break;
    }
    rx_len = 0;
}

// ================= 接收处理 =================

/* 单字节解析: 与"字节从哪来"无关 (CDC 直读 / 模式识别后补喂都用它) */
static void mai2touch_handle_byte(uint8_t byte) {
    if (byte == 0x7B) {        // '{'
        rx_len = 0;
    } else if (byte == 0x7D) { // '}'
        mai2touch_dispatch_command();
    } else if (rx_len < 5) {
        rx_packet[rx_len++] = byte;
    }
}

/* 非 static: 便于 tests/native_mode_test.c 直接驱动 (本机单元测试) */
void mai2touch_process_rx(void) {
    /* 已经识别出对方切回 raw (切换在 touch_mode_task() 里才执行) 时
     * 一个字节都不能再读, 否则会把上位机的扫描参数帧啃掉几个字节。 */
    if (!cdc_manager_is_mai2touch_active() || touch_mode_pending_to_raw()) {
        return;
    }

    while (cdc_manager_rx_available() > 0) {
        uint8_t byte;
        cdc_manager_rx_read(&byte, 1);

        if (io_mode_observe_rx_byte(byte)) {
            rx_len = 0;
            return;
        }

        /* 喂给模式识别器: 若收到 raw 帧头 (0xAA 0x01) 会自动切回 raw 流程 */
        touch_mode_observe_byte(byte);

        /* 识别出对方切到 raw 就立刻停手, 剩下的字节留给 tenodata 解析。
         * 用 pending 判断: 切换在 touch_mode_task() 里才执行。 */
        if (touch_mode_pending_to_raw() ||
            !cdc_manager_is_mai2touch_active()) {
            return;
        }

        mai2touch_handle_byte(byte);
    }
}

/*
 * raw -> Mai2Touch 自动切换时, 那条触发切换的命令 ({STAT} / {LAr2} ...) 的字节
 * 已经被 raw 消费者读走并用于语法校验了, 这里原样补喂回来。
 *
 * 为什么必须补: {STAT} 是"开始推流", {L/R..} 要求逐条回显 (游戏靠回显推进配置),
 * 都不是"复位"语义 —— 光靠 mai2touch_init() 顶替会让游戏卡在配置阶段或
 * 在 Drive 阶段收不到触摸数据。
 */
void mai2touch_replay_command(const uint8_t *frame, uint8_t length) {
    if (!cdc_manager_is_mai2touch_active()) {
        return;
    }

    for (uint8_t i = 0; i < length; i++) {
        mai2touch_handle_byte(frame[i]);
    }
}

// ================= 触摸数据打包 =================

static void mai2touch_send_bits(uint64_t bits) {
    uint8_t frame[MAI2TOUCH_FRAME_SIZE];
    frame[0] = 0x28;  // '('
    frame[8] = 0x29;  // ')'

    // 将 35 bits 打包为 7 字节 (每字节低 5 bits 有效)
    for (int i = 1; i <= 7; i++) {
        frame[i] = (uint8_t)(bits & 0x1F);
        bits >>= 5;
    }

    cdc_manager_try_send(true, frame, MAI2TOUCH_FRAME_SIZE);
}

/*
 * 管线还没就绪 (PSoC 正在校准 / 软件基线还在预热) 时对外是"空白帧"。
 *
 * 为什么必须先发空白而不是等数据: 游戏收到 {STAT} 后立刻进入 Drive 状态,
 * 只收不发; 如果这边一直不吐字节, 游戏的读循环拿不到任何触摸结果就会报错。
 * 空白帧 = 全部 34 位为 0 (无人触摸), 是协议内的合法帧。
 */
static uint64_t mai2touch_emit_bits(void) {
    return touch_pipeline_is_ready() ? touch_bits : 0U;
}

// ================= 主循环 =================

void mai2touch_task(void) {
    uint32_t now = HAL_GetTick();

    // 仅在 mai2touch 活跃时处理
    if (!cdc_manager_is_mai2touch_active()) return;

    // 1. 处理接收命令
    mai2touch_process_rx();

    if (!sending || !cdc_manager_is_mai2touch_active()) {
        last_sent_valid = false;
        return;
    }

    // 2. 发送触摸数据: 掩码一变立刻发 (docs/Mai2TouchREADME.md §6 允许随时发,
    //    主机同一读周期内的多帧按位或合并), 其余时间按 4 ms 保底续发。
    {
        uint64_t bits = mai2touch_emit_bits();
        bool due = (int32_t)(now - next_send_tick) >= 0;
        bool changed = !last_sent_valid || (bits != last_sent_bits);

        if (due || changed) {
            if (due) {
                next_send_tick = now + MAI2TOUCH_TXD_PERIOD_MS;
            }
            mai2touch_send_bits(bits);
            last_sent_bits = bits;
            last_sent_valid = true;
        }
    }
}
