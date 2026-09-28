#ifndef __TICK_H
#define __TICK_H

#include "stm32f10x.h"

// TIM4 1ms 中断时基：替代 HAL 版的 HAL_GetTick / HAL_Delay。
// 按键扫描 Key_Tick() 也在同一中断里跑（见 stm32f10x_it.c 的 TIM4_IRQHandler）。

void     Tick_Init(void);                   // 初始化 TIM4 为 1ms 中断并启动
uint32_t GetTick(void);                     // 取当前毫秒计数值（替代 HAL_GetTick）
void     Tick_DelayMs(uint32_t ms);         // 毫秒阻塞延时（替代 HAL_Delay）
void     Tick_ISR(void);                    // TIM4 中断里调用：tick++ （按键扫描由 it.c 单独调）

#endif /* __TICK_H */
