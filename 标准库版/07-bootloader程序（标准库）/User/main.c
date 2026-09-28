#include "stm32f10x.h"
#include "USART.h"
#include "Tick.h"
#include "Key.h"
#include "AT24C64.h"
#include "W25Q64.h"
#include "App_bootloader.h"
#include "Int_bootloader.h"
#include <stdio.h>

int main(void)
{
    // SystemInit 已由启动文件调用，时钟配好 72MHz。此处只做外设初始化
    USART1_Init();
    printf("\r\n=== boot ===\r\n");

    Key_Init();                 // 按键（PB12）
    Tick_Init();                // TIM4 1ms 中断：tick 计数 + 按键扫描

    AT24C64_Init();            // 必须调用：使能 DWT，否则软 I2C 的 IIC_Delay 死循环卡死
    if (W25Q64_Init() != W25Q64_OK)
    {
        printf("w25 init error\r\n");
    }

    App_bootloader_check_update();
    App_bootloader_wait_key(); // 开机按键窗口：长按2s→默认程序(0x4000)，无按键/短按→超时跳app(0x8000)

    while (1)
    {
        // 按键长按"强制结束"请求的消费点：ISR 置 Key_Event_Flag，主循环清零后再调 app
        if (Key_Event_Flag)
        {
            Key_Event_Flag = 0;
            App_bootloader_reset();   // 长按按钮 → 置 BOOT_RESET
        }

        App_bootloader_process();     // 按状态分发：BOOT_RESET→跳默认程序；NO_UPDATE→跳app；UPDATE→搬运
    }
}
