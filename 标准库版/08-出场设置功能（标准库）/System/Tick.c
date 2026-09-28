#include "Tick.h"

// TIM4 接在 APB1，APB1 预分频=2(36MHz)，按 STM32 规则定时器时钟自动×2=72MHz。
// 故 1ms：预分频 72-1、周期 1000-1，与 HAL 版完全一致。

volatile uint32_t g_tick = 0;

void Tick_Init(void)
{
    TIM_TimeBaseInitTypeDef tim = {0};
    NVIC_InitTypeDef        nvic = {0};

    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM4, ENABLE);

    tim.TIM_Prescaler     = 72 - 1;
    tim.TIM_Period        = 1000 - 1;
    tim.TIM_ClockDivision = TIM_CKD_DIV1;
    tim.TIM_CounterMode   = TIM_CounterMode_Up;
    TIM_TimeBaseInit(TIM4, &tim);

    TIM_ClearFlag(TIM4, TIM_FLAG_Update);          // 清掉上电残留标志，避免一启动就误触发一次
    TIM_ITConfig(TIM4, TIM_IT_Update, ENABLE);

    nvic.NVIC_IRQChannel                   = TIM4_IRQn;
    nvic.NVIC_IRQChannelPreemptionPriority = 3;    // 与 HAL 版优先级一致
    nvic.NVIC_IRQChannelSubPriority        = 0;
    nvic.NVIC_IRQChannelCmd                = ENABLE;
    NVIC_Init(&nvic);

    TIM_Cmd(TIM4, ENABLE);
}

uint32_t GetTick(void)
{
    return g_tick;
}

void Tick_DelayMs(uint32_t ms)
{
    uint32_t start = g_tick;
    while ((g_tick - start) < ms) { ; }
}

// 在 TIM4_IRQHandler 里调用，只做计数
void Tick_ISR(void)
{
    if (TIM_GetITStatus(TIM4, TIM_IT_Update) != RESET)
    {
        TIM_ClearITPendingBit(TIM4, TIM_IT_Update);
        g_tick++;
    }
}
