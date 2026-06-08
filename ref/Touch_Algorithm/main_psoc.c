#include "project.h"
#include <stdio.h>

/* * 辅助函数：获取指定传感器的最大 Raw 值。
 * PSoC Creator 中通常在配置界面全局或分别设置 Widget 的 Resolution。
 * 例如 12-bit 分辨率的最大值为 (1 << 12) - 1 = 4095。
 * 如果您的所有传感器分辨率一致（如 12位），可直接返回固定值 4095u。
 */
uint16 Get_Max_Raw_For_Sensor(uint8 sensor)
{
    uint16 max_raw = 65535; // 默认最大

    switch (sensor) {
        case 0:
            max_raw = 4096;
            break;
        case 4: case 8: case 9: case 13: // A, C
            max_raw = 4096;
            break;
        case 3: case 7: case 12: case 16: // B
            max_raw = 1023;
            break;
        case 1: case 2: case 5: case 6: case 10: case 11: case 14: case 15: //D, E
            max_raw = 255;
            break;
        default:
            max_raw = 65535;
            break;
    }
    return max_raw;
}

/*
 * SAR 二分逼近校准算法 (极速版)
 * @param target_pins: 需要校准的引脚编号数组指针
 * @param num_pins:    需要校准的引脚总数
 */
void Tune_CapSense_CompIDAC_SAR(const uint8* target_pins, uint8 num_pins)
{
    uint8 p;
    uint8 sensor_id;
    uint16 raw_value;
    uint32 target_raw;
    uint8 bit_val;

    // 存放每个引脚当前确定的最佳 IDAC 基础值，最大支持到 16 号引脚
    uint8 current_idac[17] = {0u};

    if (num_pins == 0u) return;

    CapSense_1_Start();

    // 先全量归零并稳定
    for (p = 0u; p < num_pins; p++)
    {
        CapSense_1_SetCompensationIDAC(target_pins[p], 0u);
    }
    for (uint8 settle = 0u; settle < 10u; settle++)
    {
        CapSense_1_ScanEnabledWidgets();
        while (CapSense_1_IsBusy() != 0u) {}
    }

    // ==========================================
    // 核心：7位二分逼近 (只循环 7 次！)
    // bit_val 依次测试: 64, 32, 16, 8, 4, 2, 1
    // ==========================================
    for (bit_val = 64u; bit_val > 0u; bit_val >>= 1)
    {
        // 1. 为所有目标引脚【试探性】地加上当前位
        for (p = 0u; p < num_pins; p++)
        {
            sensor_id = target_pins[p];
            // 测试值 = 已确认的基础值 + 当前试探位
            CapSense_1_SetCompensationIDAC(sensor_id, current_idac[sensor_id] | bit_val);
        }

        // 2. 执行虚拟扫描，等待大跨度调节后的硬件 RC 延迟稳定
        for (uint8 settle = 0u; settle < 10u; settle++) 
        {
            CapSense_1_ScanEnabledWidgets();
            while (CapSense_1_IsBusy() != 0u) {}
        }

        // 3. 读取真实 Raw 数据，决定是否【保留】该比特位
        for (p = 0u; p < num_pins; p++)
        {
            sensor_id = target_pins[p];
            raw_value = CapSense_1_ReadSensorRaw(sensor_id);
            target_raw = Get_Max_Raw_For_Sensor(sensor_id) / 4u;

            // 如果 raw 依然 >= 目标值，说明补偿力度还不够，【必须保留】这个位
            // (即使完美值是127，这里也会完美地把 64,32...1 全都保留下来)
            if (raw_value >= target_raw)
            {
                current_idac[sensor_id] |= bit_val;
            }
            // 否则说明试探加的这个位导致补偿过头了(raw降得太低)，抛弃该位，不作处理
        }
    }

    // 4. 将仅循环7次就确定的完美值，正式写入所有目标引脚
    for (p = 0u; p < num_pins; p++)
    {
        sensor_id = target_pins[p];
        CapSense_1_SetCompensationIDAC(sensor_id, current_idac[sensor_id]);
    }

    // 最后稳定一次并重置基线
    for (uint8 settle = 0u; settle < 10u; settle++)
    {
        CapSense_1_ScanEnabledWidgets();
        while (CapSense_1_IsBusy() != 0u) {}
    }
    CapSense_1_InitializeAllBaselines();
}


// 1. 定义 35 字节的 EZI2C 通信缓存
// Buffer[0]: 状态位
// Buffer[1]~Buffer[34]: 17个通道的Raw数据，高位在前
volatile uint8_t ezi2cBuffer[35] = {0};

int main(void)
{
    char txBuffer[32]; 
    CyGlobalIntEnable; 

    UART_2_Start();
    CapSense_1_Start();
    CapSense_1_InitializeAllBaselines();
    
    
    // === 定义需要单独校准的引脚集合 ===
    // 假设您现在只想校准 1号, 3号, 5号, 和 16号 传感器
    //uint8 my_target_pins[] = {1u, 2u, 5u, 3u, 8u, 12u, 14u, 16u};
    uint8 my_target_pins[] = {
        0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u,
        8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u, 16u
    };
    // 自动计算数组中的引脚数量
    uint8 num_targets = sizeof(my_target_pins) / sizeof(my_target_pins[0]);
    // 启动针对指定引脚的步进校准
    Tune_CapSense_CompIDAC_SAR(my_target_pins, num_targets);
    
    // 2. 启动 EZI2C 组件并设置缓冲区
    // 注意：请将 "EZI2C_1" 替换为你原理图(TopDesign)中实际使用的 EZI2C 组件名称
    EZI2C_Touch_1_Start();
    EZI2C_Touch_1_EzI2CSetBuffer1(sizeof(ezi2cBuffer), sizeof(ezi2cBuffer), (volatile uint8_t *)ezi2cBuffer);
    
    for(;;)
    {

        if(0 == CapSense_1_IsBusy())
        {
            // 1. 准备 I2C/UART 共享的 Buffer
            ezi2cBuffer[0] = 0x01; // 状态位 (也是上位机的同步头)

            EZI2C_Touch_1_DisableInt();
            for(uint8_t i = 0; i < 17; i++)
            {
                uint16_t rawValue = CapSense_1_SensorRaw[i];
                uint8_t highIndex = 1 + (i * 2);
                uint8_t lowIndex  = highIndex + 1;

                ezi2cBuffer[highIndex] = (uint8_t)(rawValue >> 8);
                ezi2cBuffer[lowIndex]  = (uint8_t)(rawValue & 0xFF);
            }
            EZI2C_Touch_1_EnableInt();
        
            // --------------------------------------------------
            // 注意：下面这两行一定要删掉或者注释掉！
            // --------------------------------------------------
            // sprintf(txBuffer, "%u\r\n", rawValue);
            // UART_2_UartPutString(txBuffer);

            // --------------------------------------------------
            // 替换为下面这行！直接把 35 字节的 ezi2cBuffer 发出去
            // --------------------------------------------------
            // CapSense_1_GetCompensationIDAC(0)
            //uint32_t Cp = CapSense_1_GetSensorCp(CapSense_1_SENSOR_BUTTON_A2__BTN);
            //sprintf(txBuffer, "%u\r\n", Cp);
            
            //UART_2_UartPutString(txBuffer); 
            UART_2_SpiUartPutArray(ezi2cBuffer, 35); 

            CapSense_1_ScanEnabledWidgets();
        }
    }
}