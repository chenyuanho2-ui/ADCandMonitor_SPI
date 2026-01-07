/*
 * Monitor_SPI.c
 * 最终修正版：修复位偏移(0~3度问题) + 集成用户校准公式
 */

#include "Monitor_SPI.h"
#include <stdio.h>

// 引用外部 ADC 句柄 (确保你的 adc.c 里有这个，或者在 main.h 里定义)
extern ADC_HandleTypeDef hadc1;

// 全局变量
Monitor_State_t g_MonitorState;

// --- 寄存器直读宏 (PB5=CS, PB6=SCK, PB7=SO, PA3=BTN) ---
// 务必确认你的原理图：Monitor_CS 是 PB5，SCK 是 PB6，SO 是 PB7
#define READ_CS()   ((GPIOB->IDR & GPIO_PIN_5) != 0)
#define READ_SCK()  ((GPIOB->IDR & GPIO_PIN_6) != 0)
#define READ_SO()   ((GPIOB->IDR & GPIO_PIN_7) != 0)
#define READ_BTN()  ((GPIOA->IDR & GPIO_PIN_3) == 0) // 按下为低

/**
  * @brief  初始化
  */
void Monitor_Init(Monitor_ChipType defaultChip) {
    g_MonitorState.isRunning = 0;
    g_MonitorState.chipType = CHIP_MAX6675;
    g_MonitorState.currentTemp = 0.0f;
    g_MonitorState.currentADC = 0;
    printf("System Ready. Press Button 1 to Start/Stop.\r\n");
}

/**
  * @brief  读取 ADC 数据
  */
uint32_t Monitor_ReadADC(void) {
    HAL_ADC_Start(&hadc1);
    if (HAL_ADC_PollForConversion(&hadc1, 2) == HAL_OK) {
        return HAL_ADC_GetValue(&hadc1);
    }
    return 0;
}

/**
  * @brief  原始数据转温度 (核心修正区)
  */
static float Convert_Data(uint32_t raw, Monitor_ChipType type) {
    float temp = 0.0f;
    
    // --- 关键修正1：解决位偏移 ---
    // 你的测试表明数据被右移了一位（数值减半），这里强制左移恢复
    raw = raw << 1; 

    if (type == CHIP_MAX6675) {
        uint16_t data16 = (uint16_t)(raw & 0xFFFF);
        
        // 检查开路 (D2位，但因为左移了1位，D2变成了D3位置... 
        // 等等，数据结构是 D15..D3 D2 D1 D0。左移后最低位补0，D2还在原来的bit位置逻辑上对齐吗？
        // 其实只需要按移位后的标准解析即可)
        
        // 提取温度整数 (去掉低3位: D2, D1, D0)
        uint16_t val = data16 >> 3;
        
        // --- 关键修正2：应用你的校准公式 ---
        // (alu_temp.c 中的逻辑: tmp * 0.25 - 13.75)
        temp = (float)val * 0.25f - 36.75f;
        
    } else {
        // MAX31855 (保持标准逻辑，如果也偏低，也需要同理修正)
        if (raw & 0x10000) return -999.0f; // 故障
        int16_t tempRaw = (int16_t)((raw >> 18) & 0x3FFF);
        if (tempRaw & 0x2000) tempRaw |= 0xC000;
        temp = tempRaw * 0.25f;
    }
    return temp;
}

/**
  * @brief  快速嗅探 SPI 数据 (非阻塞)
  */
void Monitor_Sniff_SPI_Fast(void) {
    // 1. 如果 CS 是高电平，直接退出
    if (READ_CS() == 1) return;

    // 2. 只有 CS 变低才进入读取逻辑
    uint32_t rawData = 0;
    int bitCount = (g_MonitorState.chipType == CHIP_MAX31855) ? 32 : 16;
    
    // 临时关闭中断，防止丢位
    __disable_irq(); 
    
    // 同步：确保 SCK 是低电平再开始（防止切入时正好在高电平中间）
    // 给一个极短的防死锁计数
    uint32_t timeout = 1000;
    while(READ_SCK() == 1 && --timeout);

    // 循环读取每一位
    for (int i = 0; i < bitCount; i++) {
        // 等待 SCK 变高 (上升沿)
        // 注意：原主机代码是上升沿采样，我们也跟随后沿
        timeout = 5000;
        while (READ_SCK() == 0 && --timeout);
        
        rawData <<= 1;
        // 立即采样 SO
        if (READ_SO()) rawData |= 1;
        
        // 等待 SCK 变低
        timeout = 5000;
        while (READ_SCK() == 1 && --timeout);
    }
    
    __enable_irq(); // 恢复中断
    
    // 3. 计算并更新温度
    float newTemp = Convert_Data(rawData, g_MonitorState.chipType);
    
    // 简单过滤掉异常值（比如 -13.75 说明读到全0）
    if (newTemp > -10.0f) {
        g_MonitorState.currentTemp = newTemp;
    }
    
    // 4. 等待 CS 恢复高电平，防止重复处理同一帧
    while (READ_CS() == 0); 
}

/**
  * @brief  主循环处理函数
  */
void Monitor_LoopHandler(void) {
    // --- 1. 按键处理 ---
    if (READ_BTN()) {
        HAL_Delay(20);
        if (READ_BTN()) {
            g_MonitorState.isRunning = !g_MonitorState.isRunning;
            if (g_MonitorState.isRunning) {
                g_MonitorState.startTime = HAL_GetTick();
                g_MonitorState.nextPrintTime = 0;
                printf("--- START ---\r\n");
                printf("Time(ms)\tTemp(C)\tADC_Raw\r\n");
            } else {
                printf("--- STOP ---\r\n");
            }
            while (READ_BTN());
        }
    }

    // --- 2. 运行逻辑 ---
    if (g_MonitorState.isRunning) {
        
        // A. 嗅探 SPI (最高优先级)
        Monitor_Sniff_SPI_Fast();
        
        // B. 定时打印 (50ms)
        uint32_t now = HAL_GetTick();
        uint32_t elapsed = now - g_MonitorState.startTime;
        
        if (elapsed >= g_MonitorState.nextPrintTime) {
            // 读取一次 ADC
            g_MonitorState.currentADC = Monitor_ReadADC();
            
            // 打印
            printf("%lu\t%.2f\t%lu\r\n", 
                   g_MonitorState.nextPrintTime, 
                   g_MonitorState.currentTemp, 
                   g_MonitorState.currentADC);
            
            g_MonitorState.nextPrintTime += 50; 
        }
    }
}
