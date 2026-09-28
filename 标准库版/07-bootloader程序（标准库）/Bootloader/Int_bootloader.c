#include "Int_bootloader.h"
#include "stm32f10x.h"
#include <stdio.h>

uint8_t bootloader_jump_to_app(uint32_t app_reset_addr)
{
    typedef void (*pFunc)(void);

    // 1，校验
    uint32_t app_start_ptr   = *(volatile uint32_t *)app_reset_addr;        // 栈顶
    uint32_t app_reset_vector = *(volatile uint32_t *)(app_reset_addr + 4); // 复位向量

    if ((app_start_ptr & 0xFFFF0000) != APP_SRAM_START)
    {
        //校验APP栈顶地址：栈顶高16位必须等于SRAM起始0x20000000，保证栈在SRAM区域
        printf("Stack pointer error\r\n");
        return 1;
    }

    //校验APP复位入口地址：按本次要跳的目标程序(0x4000默认程序或0x8000 app)划分各自的Flash分区，
    //复位向量(去掉Thumb位)必须落在该分区内，防止向量被写错分区、跳到别的程序里去执行
    {
        uint32_t vector_addr = app_reset_vector & 0xFFFFFFFEU;   // 去掉 Thumb 位
        uint32_t region_end  = (app_reset_addr == PESET_START) ? APP_FLASH_BASE_ADDR : APP_END_ADDR;

        if (app_reset_addr != PESET_START && app_reset_addr != APP_FLASH_BASE_ADDR)
        {
            //基地址本身不合法：只允许 0x4000(默认程序) 或 0x8000(app) 两个合法起点
            printf("App base address error\r\n");
            return 1;
        }
        if (vector_addr < app_reset_addr || vector_addr >= region_end)
        {
            //复位入口必须落在本次目标程序自己的Flash分区内
            printf("Reset address error\r\n");
            return 1;
        }
    }

    // 2，注销 bootloader 运行环境
    __disable_irq();
    // 清掉所有 NVIC 使能与 pending 位，防止残留中断把 app 带进 Default_Handler
    {
        uint32_t i;
        for (i = 0; i < 8; i++)
        {
            NVIC->ICER[i] = 0xFFFFFFFFU;   // disable
            NVIC->ICPR[i] = 0xFFFFFFFFU;   // clear pending
        }
    }
    SysTick->CTRL = 0;
    SysTick->VAL  = 0;
    SysTick->LOAD = 0;

    // 3，设主栈指针 + 重定向中断向量表 + 跳复位中断
    __set_MSP(app_start_ptr);
    SCB->VTOR = app_reset_addr;

    pFunc jump_to_app = (pFunc)app_reset_vector;
    jump_to_app();

    return 0;
}
