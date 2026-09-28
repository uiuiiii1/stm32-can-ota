#include "stm32f10x.h"                  // Device header
#include "USART.h"
#include "Tick.h"
#include "LED.h"
#include "App_reset.h"
#include "Int_bootloader.h"
#include <stdio.h>

int main(void)
{
    //本程序链接在 0x08004000（出厂设置程序区）：先重定向中断向量表再开中断，
    //否则复位后 VTOR 仍指向 0x08000000（bootloader 区），中断会跑进 bootloader 的向量表
    SCB->VTOR = PESET_START;

    //bootloader(07) 跳转前 __disable_irq() 关了全局中断，这里必须重新打开，
    //否则 TIM4 时基/USART1 中断全失效，LED 不闪、升级也收不到帧
    __enable_irq();

    USART1_Init();   // PA9/PA10 9600 8N1，printf 发送
    printf("\r\n=== boot ===\r\n");   //开机提示：每次复位（上电/点击复位）串口都会显示

    Tick_Init();     // TIM4 1ms 中断时基：GetTick() 替代 HAL 版的 HAL_GetTick
    LED_Init();      // LED（PA2，对应 HAL 版 LED_Pin）

    bootloader_init();          //底层：启动串口 DMA 双缓冲接收（必须先调用，否则收不到任何数据）
    App_bootloader_init();      //上层：升级状态机复位（等待 start:len 命令）

    uint32_t last_blink_tick = GetTick();   //LED 连续闪烁计时基准，观察程序运行用

    while (1)
    {
        //升级状态机轮询：解析 start:len → 收帧写 flash → 校验 → 跳转 app（必须在主循环里持续调用）
        App_bootloader_work();

        //LED 连续闪烁，观察程序运行用；不影响其他功能
        if (GetTick() - last_blink_tick >= 2000)
        {
            last_blink_tick = GetTick();
            LED2_Turn();   //翻转 PA2，对应 HAL 版 HAL_GPIO_TogglePin(LED_GPIO_Port, LED_Pin)
        }
    }
}
