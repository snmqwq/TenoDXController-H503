#include "tenodata_config.h"

// ==============================================================
// 34 通道硬件扫描参数硬编码表
// 与 C# 端 HardwareConfig.PhysicalToLogicalMap 顺序一致:
//   E4,D4,B3,A3,C1,E3,D3,B2,A2,E2,D2,B1,A1,E1,D1,B8,A8,E8,D8,B7,A7,C2,E7,D7,B6,A6,E6,D6,B5,A5,E5,D5,B4,A4
// 各区块默认值:
//   A区: Res=12, Mod=15, Sns=2, Div=2
//   B区: Res=10, Mod=25, Sns=4, Div=4
//   C区: Res=12, Mod=30, Sns=4, Div=4
//   D区: Res=8,  Mod=10, Sns=2, Div=2
//   E区: Res=8,  Mod=8,  Sns=2, Div=2
// ==============================================================
static const TenodataChannelConfig hw_config[TENODATA_TOTAL_CHANNELS] = {
    // ch0:  E4
    {8, 8, 2, 2},
    // ch1:  D4
    {8, 10, 2, 2},
    // ch2:  B3
    {10, 25, 4, 4},
    // ch3:  A3
    {12, 15, 2, 2},
    // ch4:  C1
    {12, 30, 4, 4},
    // ch5:  E3
    {8, 8, 2, 2},
    // ch6:  D3
    {8, 10, 2, 2},
    // ch7:  B2
    {10, 25, 4, 4},
    // ch8:  A2
    {12, 15, 2, 2},
    // ch9:  E2
    {8, 8, 2, 2},
    // ch10: D2
    {8, 10, 2, 2},
    // ch11: B1
    {10, 25, 4, 4},
    // ch12: A1
    {12, 15, 2, 2},
    // ch13: E1
    {8, 8, 2, 2},
    // ch14: D1
    {8, 10, 2, 2},
    // ch15: B8
    {10, 25, 4, 4},
    // ch16: A8
    {12, 15, 2, 2},
    // ch17: E8
    {8, 8, 2, 2},
    // ch18: D8
    {8, 10, 2, 2},
    // ch19: B7
    {10, 25, 4, 4},
    // ch20: A7
    {12, 15, 2, 2},
    // ch21: C2
    {12, 30, 4, 4},
    // ch22: E7
    {8, 8, 2, 2},
    // ch23: D7
    {8, 10, 2, 2},
    // ch24: B6
    {10, 25, 4, 4},
    // ch25: A6
    {12, 15, 2, 2},
    // ch26: E6
    {8, 8, 2, 2},
    // ch27: D6
    {8, 10, 2, 2},
    // ch28: B5
    {10, 25, 4, 4},
    // ch29: A5
    {12, 15, 2, 2},
    // ch30: E5
    {8, 8, 2, 2},
    // ch31: D5
    {8, 10, 2, 2},
    // ch32: B4
    {10, 25, 4, 4},
    // ch33: A4
    {12, 15, 2, 2},
};

TenodataChannelConfig tenodata_config_get_channel(uint8_t channel) {
    if (channel >= TENODATA_TOTAL_CHANNELS) {
        TenodataChannelConfig zero = {0, 0, 0, 0};
        return zero;
    }
    return hw_config[channel];
}

void tenodata_config_get_payload(uint8_t *payload) {
    for (int i = 0; i < TENODATA_TOTAL_CHANNELS; i++) {
        payload[0   + i] = hw_config[i].res;
        payload[34  + i] = hw_config[i].mod;
        payload[68  + i] = hw_config[i].sns;
        payload[102 + i] = hw_config[i].div;
    }
}
