#ifndef __BUTTON_DETECTOR_CONFIG_H__
#define __BUTTON_DETECTOR_CONFIG_H__

#include <stdint.h>

// ================= 物理通道 → 区块映射 =================
// 基于 PhysicalToLogicalMap:
//   E4,D4,B3,A3,C1,E3,D3,B2,A2,E2,D2,B1,A1,E1,D1,B8,A8,
//   E8,D8,B7,A7,C2,E7,D7,B6,A6,E6,D6,B5,A5,E5,D5,B4,A4
// 返回 'A'~'E' 或 0xff 表示无效
char detector_get_block(uint8_t physical_channel);

// ================= A区 累积-导数双鉴算法参数 =================
#define DETECTOR_A_TRIGGER_SENSITIVITY    700
#define DETECTOR_A_LARGE_SIGNAL_GATE      1
#define DETECTOR_A_WINDOW_SIZE            8
#define DETECTOR_A_TRIGGER_RATIO          1.8f
#define DETECTOR_A_TRIGGER_DERIV          28
#define DETECTOR_A_TRIGGER_DIFF_MIN       55
#define DETECTOR_A_CONFIRM_FRAMES         10
#define DETECTOR_A_CONFIRM_DIFF           200
#define DETECTOR_A_RELEASE_FLOOR          35
#define DETECTOR_A_RELEASE_RATIO          0.35f
#define DETECTOR_A_SHARP_RELEASE_DERIV    -40
#define DETECTOR_A_CRASH_WINDOW           7
#define DETECTOR_A_CRASH_DERIV_THRESHOLD  -8
#define DETECTOR_A_CRASH_DIFF_THRESHOLD   280

// ================= C区 判定参数 =================
#define DETECTOR_C_DIFF_THRESHOLD         50
#define DETECTOR_C_DERIV_THRESHOLD        25
#define DETECTOR_C_DERIV_RELEASE          -20
#define DETECTOR_C_DIFF_RELEASE           15

// ================= B/D/E区 判定参数 =================
#define DETECTOR_B_DIFF_THRESHOLD         20
#define DETECTOR_B_DERIV_RELEASE          -20
#define DETECTOR_D_DIFF_THRESHOLD         20
#define DETECTOR_D_DERIV_RELEASE          -20
#define DETECTOR_E_DIFF_THRESHOLD         15
#define DETECTOR_E_DERIV_RELEASE          -16

#endif /* __BUTTON_DETECTOR_CONFIG_H__ */
