#include "Int_bootloader.h"
#include "stdlib.h"
#include "string.h"
#include "stdio.h"


uint8_t bootloader_jump_to_app(uint32_t app_reset_addr)
{
    typedef void (*pFunc)(void);

    //1，检验
    uint32_t app_start_ptr = *(volatile uint32_t *)app_reset_addr;
    uint32_t app_reset_ptr = *(volatile uint32_t *)(app_reset_addr + 4);

    if((app_start_ptr & 0xFFFF0000) != APP_SRAM_START)
    {
        //校验APP栈顶地址：栈顶高16位必须等于SRAM起始地址0x20000000，保证栈在SRAM区域
        printf("Stack pointer error\r\n");
        return 1;
    }
    //校验APP复位入口地址：按本次要跳的目标程序(0x4000默认程序或0x8000 app)划分各自的Flash分区，
    //复位向量(去掉Thumb位)必须落在该分区内，防止向量被写错分区、跳到别的程序里去执行
    uint32_t vector_addr = app_reset_ptr & 0xFFFFFFFEU;   //去掉Thumb位(bit0=1表示Thumb指令)，得到真实入口地址
    uint32_t region_end = (app_reset_addr == PESET_START) ? APP_FLASH_BASE_ADDR : APP_END_ADDR;
    if(app_reset_addr != PESET_START && app_reset_addr != APP_FLASH_BASE_ADDR)
    {
        //基地址本身不合法：只允许 0x4000(默认程序) 或 0x8000(app) 两个合法起点
        printf("App base address error\r\n");
        return 1;
    }
    if(vector_addr < app_reset_addr || vector_addr >= region_end)
    {
        //复位入口必须落在本次目标程序自己的Flash分区内
        printf("Reset address error\r\n");
        return 1;
    }

    


    //2，注销bootloader程序
    //关中断
    __disable_irq();
    /* 顺序敏感：HAL_RCC_DeInit() 内部会调用 HAL_InitTick()，若 HAL 时基用的是某个定时器，
     * 该函数会重启该定时器并重新使能其中断。因此下面的 NVIC 清理必须放在 HAL_RCC_DeInit() 之后执行；
     * 一旦调换顺序，被重启的中断会在跳进 app 后 __enable_irq() 把积压的中断派发给 app 向量表里的
     * 弱符号 Default_Handler（B . 死循环），app 当场卡死。本工程 HAL 时基用 SysTick，
     * 下面统一清掉所有 NVIC + 停 SysTick，对 SysTick/定时器两种时基都安全。 */

    HAL_RCC_DeInit();

    /* 关闭并清除所有外部中断，防止残留 pending 位把 app 带进 Default_Handler */
    for (uint32_t i = 0; i < 8; i++)
    {
        NVIC->ICER[i] = 0xFFFFFFFFU;   /* disable */
        NVIC->ICPR[i] = 0xFFFFFFFFU;   /* clear pending */
    }
    SysTick->CTRL = 0;                 /* 顺手停掉 SysTick */
    SysTick->VAL = 0;                  /* 重置 SysTick 值 */
    SysTick->LOAD = 0;                 /* 重置 SysTick 装载值 */
     //注销HAL库
     HAL_DeInit();
    //修改主栈指针
    __set_MSP(app_start_ptr);
    //重定向中断向量表
    SCB->VTOR = app_reset_addr;
    //3，跳转A程序复位中断
    pFunc jump_to_app = (pFunc)app_reset_ptr;
    jump_to_app();

    return 0;
}
