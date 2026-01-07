#ifndef __MONITOR_SPI_H
#define __MONITOR_SPI_H

#include "main.h"

// 引入 ADC 定义 (确保 main.h 或 adc.h 中有 ADC_HandleTypeDef 的定义)
#include "adc.h" 

// 定义支持的芯片类型
typedef enum {
    CHIP_MAX6675,
    CHIP_MAX31855
} Monitor_ChipType;

// 系统状态结构体
typedef struct {
    uint8_t  isRunning;       // 1: 运行中, 0: 停止
    float    currentTemp;     // 存储最新的温度值 (从 SPI 截获)
    uint32_t currentADC;      // 存储最新的 ADC 值
    
    uint32_t startTime;       // 按下开始键的时间戳
    uint32_t nextPrintTime;   // 下一次打印的目标时间
    
    Monitor_ChipType chipType;// 芯片类型
} Monitor_State_t;

// 函数声明
void Monitor_Init(Monitor_ChipType defaultChip);
void Monitor_LoopHandler(void);

#endif /* __MONITOR_SPI_H */
