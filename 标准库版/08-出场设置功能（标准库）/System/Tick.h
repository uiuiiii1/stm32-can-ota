#ifndef __TICK_H
#define __TICK_H

#include "stm32f10x.h"

// TIM4 1ms 中断时基：替代 HAL 版的 HAL_GetTick。
// 升级过程中的 flash 擦写会阻塞主循环，毫秒计时放中断里不受影响。

void     Tick_Init(void);                   // 初始化 TIM4 为 1ms 中断并启动
uint32_t GetTick(void);                     // 取当前毫秒计数值（替代 HAL_GetTick）
void     Tick_DelayMs(uint32_t ms);         // 毫秒阻塞延时（替代 HAL_Delay）
void     Tick_ISR(void);                    // TIM4 中断里调用：tick++

#endif /* __TICK_H */
