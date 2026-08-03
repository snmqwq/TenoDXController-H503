#include "mai2touch.h"
#include "cdc_manager.h"
#include "main.h"
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
#define MAI2TOUCH_TXD_PERIOD_MS 4U   // 约 250Hz (匹配 MPR121 采样率)

// ================= 触摸状态 =================
// 35 bits 位掩码, 有效 34 bits (touch bit 0~33)
// bit 0 对应映射表第一个触摸区, bit 33 对应最后一个
static uint64_t touch_bits = 0;

// 数据发送控制
static bool sending = false;        // 收到 {STAT} 后开始发送
static uint32_t next_send_tick = 0;

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
}

uint64_t* mai2touch_get_touch_bits(void) {
    return &touch_bits;
}

// ================= 命令处理 =================

static void mai2touch_cmd_rset(void) {
    // 复位：清除触摸位，等待 {STAT}
    touch_bits = 0;
    sending = false;
}

static void mai2touch_cmd_halt(void) {
    sending = false;
}

static void mai2touch_cmd_stat(void) {
    sending = true;
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

    // 命令标识在 payload 第 3 字节 (packet[2])
    switch (rx_packet[2]) {
        case MAI2TOUCH_CMD_RSET:  mai2touch_cmd_rset();  break;
        case MAI2TOUCH_CMD_HALT:  mai2touch_cmd_halt();  break;
        case MAI2TOUCH_CMD_STAT:  mai2touch_cmd_stat();  break;
        case MAI2TOUCH_CMD_RATIO: mai2touch_cmd_echo(rx_packet); break;
        case MAI2TOUCH_CMD_SENS:  mai2touch_cmd_echo(rx_packet); break;
        default: break;
    }
    rx_len = 0;
}

// ================= 接收处理 =================

static void mai2touch_process_rx(void) {
    while (cdc_manager_rx_available() > 0) {
        uint8_t byte;
        cdc_manager_rx_read(&byte, 1);

        if (byte == 0x7B) {       // '{'
            rx_len = 0;
        } else if (byte == 0x7D) { // '}'
            mai2touch_dispatch_command();
        } else if (rx_len < 5) {
            rx_packet[rx_len++] = byte;
        }
    }
}

// ================= 触摸数据打包 =================

static void mai2touch_pack_and_send(void) {
    uint8_t frame[MAI2TOUCH_FRAME_SIZE];
    frame[0] = 0x28;  // '('
    frame[8] = 0x29;  // ')'

    // 将 35 bits 打包为 7 字节 (每字节低 5 bits 有效)
    uint64_t bits = touch_bits;
    for (int i = 1; i <= 7; i++) {
        frame[i] = (uint8_t)(bits & 0x1F);
        bits >>= 5;
    }

    cdc_manager_try_send(true, frame, MAI2TOUCH_FRAME_SIZE);
}

// ================= 主循环 =================

void mai2touch_task(void) {
    uint32_t now = HAL_GetTick();

    // 仅在 mai2touch 活跃时处理
    if (!cdc_manager_is_mai2touch_active()) return;

    // 1. 处理接收命令
    mai2touch_process_rx();

    // 2. 定时发送触摸数据
    if (sending && (int32_t)(now - next_send_tick) >= 0) {
        next_send_tick = now + MAI2TOUCH_TXD_PERIOD_MS;
        mai2touch_pack_and_send();
    }
}
