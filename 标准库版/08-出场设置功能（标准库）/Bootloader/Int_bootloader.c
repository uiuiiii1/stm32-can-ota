#include "Int_bootloader.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

//============================================================================
// 串口接收：DMA1_Channel5（USART1_RX）双缓冲 + IDLE 空闲中断
// 等价于 HAL 版的 HAL_UARTEx_ReceiveToIdle_DMA：
//  - IDLE 事件（串口空闲）= 一帧结束，收到的字节数 = 缓冲长度 - CNDTR 剩余值
//  - TC   事件（缓冲收满）= 也算一帧结束（帧长度≥缓冲且无空闲间隙时才会发生）
//  - HT   事件（DMA 收到一半）不使能、不处理：HAL 版对 HT 直接 return，
//         此时 DMA 仍在跑，切缓冲/累加长度都会出错
//
// 双缓冲 ping-pong：ISR 里"先切另一个缓冲重新武装、再把本帧搬进暂存区"，
// flash 擦除/写入全部放在主循环（App 状态机），ISR 只做微秒级的搬数据，
// 中断处理时间恒定，任何波特率/帧间隙都不会丢字节。
//============================================================================
static uint8_t  rx_buf[2][BOOTLOADER_USART_REC_BUFF_LEN];
static volatile uint8_t rx_active = 0;   //当前接收写入哪个缓冲(0/1)

//整帧暂存：ISR 搬完立即返回，主循环再慢慢写 flash（与接收完全解耦）
static uint8_t           frame_buf[BOOTLOADER_USART_REC_BUFF_LEN];
static volatile uint8_t  frame_ready = 0;  //1=有整帧待写（主循环置0）
static volatile uint16_t frame_len   = 0;

uint16_t bootloader_rec_len=0; //串口接收数据长度
uint16_t bootloader_rec_full_len=0; //串口接收完整数据长度（兼作写进度，供 main.c printf）

static uint32_t write_offset = 0;   //已写入 Flash 的字节偏移，恒为偶数（半字对齐）
static uint32_t current_erased_page = 0xFFFFFFFFU; //已处理过的页（已擦除 或 已验证为干净）

uint8_t last_byte=0; //末尾可能出现的单独字节
uint8_t last_byte_flag=0; //是否需要写入最后一个字节（0=不需要，1=需要）

//调试：定位丢字节，由 main 循环打印（ISR 里只赋值，不 printf）
volatile uint16_t dbg_frame_cnt  = 0;   //收到的帧数
volatile uint16_t dbg_last_size  = 0;   //最近一帧的 Size
volatile uint16_t dbg_short_size = 0;   //第一个"非256"帧的长度(0=还没出现)
volatile uint16_t dbg_ore_cnt   = 0;   //ORE 溢出次数

#define BOOT_FLASH_PAGE_SIZE 0x400U   //F103 中容量：1KB/页（等价 HAL 的 FLASH_PAGE_SIZE）

//把 DMA 接收重新武装到指定缓冲（ISR 里调用）
static void rx_arm(uint8_t idx)
{
    DMA_Cmd(DMA1_Channel5, DISABLE);
    while (DMA1_Channel5->CCR & DMA_CCR1_EN) { ; }   //等通道真正关断再改配置
    DMA1_Channel5->CMAR = (uint32_t)rx_buf[idx];      //标准库没有运行时改内存地址的接口，直写寄存器
    DMA_SetCurrDataCounter(DMA1_Channel5, BOOTLOADER_USART_REC_BUFF_LEN);
    DMA_ClearFlag(DMA1_FLAG_GL5);                     //清本通道全部标志（TC/HT/TE）
    DMA_Cmd(DMA1_Channel5, ENABLE);
}

//一帧接收完成（ISR 里调用）：切缓冲重新武装 + 搬进暂存区 + 置标志
static void rx_frame_done(uint16_t len)
{
    //刚收完的缓冲与长度
    uint8_t  done = rx_active;

    //立刻切到另一个缓冲并重新武装 DMA 接收（微秒级）——
    //ISR 里只搬数据、不碰 flash；擦除/写入全部放到主循环，中断处理时间恒定，
    //任何波特率/帧间隙都不会丢字节。
    rx_active ^= 1;
    rx_arm(rx_active);

    //统计 / 进度
    bootloader_rec_len = len;
    bootloader_rec_full_len += len;
    dbg_frame_cnt++;
    dbg_last_size = len;
    if (len != 256 && dbg_short_size == 0) dbg_short_size = len;  //256=发送包大小

    //把本帧搬进暂存区，置标志；主循环 App 状态机负责写 flash
    memcpy(frame_buf, rx_buf[done], len);
    frame_len  = len;
    frame_ready = 1;
}

//串口接收=>准备接收A程序
void bootloader_init(void)
{
    DMA_InitTypeDef dma = {0};
    NVIC_InitTypeDef nvic = {0};

    //清空掉初始化串口之前的所有残留：读 SR 后读 DR 一并清掉 ORE 溢出/IDLE 空闲标志
    {
        volatile uint32_t tmp = USART1->SR;
        tmp = USART1->DR;
        (void)tmp;
    }

    rx_active = 0;
    bootloader_flash_reset(); // 清零写入状态（偏移/遗留字节/已擦除页标记）

    //DMA1_Channel5 = USART1_RX：外设→内存、字节宽度、内存递增、普通模式（收满自动停）
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);
    DMA_DeInit(DMA1_Channel5);
    dma.DMA_PeripheralBaseAddr = (uint32_t)&USART1->DR;
    dma.DMA_MemoryBaseAddr     = (uint32_t)rx_buf[0];
    dma.DMA_DIR                = DMA_DIR_PeripheralSRC;
    dma.DMA_BufferSize         = BOOTLOADER_USART_REC_BUFF_LEN;
    dma.DMA_PeripheralInc      = DMA_PeripheralInc_Disable;
    dma.DMA_MemoryInc          = DMA_MemoryInc_Enable;
    dma.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
    dma.DMA_MemoryDataSize     = DMA_MemoryDataSize_Byte;
    dma.DMA_Mode               = DMA_Mode_Normal;
    dma.DMA_Priority           = DMA_Priority_Low;   //与 HAL 版一致
    dma.DMA_M2M                = DMA_M2M_Disable;
    DMA_Init(DMA1_Channel5, &dma);
    DMA_ClearFlag(DMA1_FLAG_GL5);
    //只开 TC 中断，不开 HT：DMA 收到一半的中途通知不需要处理（见文件头注释）
    DMA_ITConfig(DMA1_Channel5, DMA_IT_TC, ENABLE);

    //USART1 中断：IDLE=一帧结束；ERR=ORE 溢出等（清标志自愈，计数用于调试）
    USART_ITConfig(USART1, USART_IT_IDLE, ENABLE);
    USART_ITConfig(USART1, USART_IT_ERR, ENABLE);

    //中断优先级与 HAL 版一致（USART1/DMA1_Channel5 抢占2，TIM4 抢占3）
    nvic.NVIC_IRQChannel                   = DMA1_Channel5_IRQn;
    nvic.NVIC_IRQChannelPreemptionPriority = 2;
    nvic.NVIC_IRQChannelSubPriority        = 0;
    nvic.NVIC_IRQChannelCmd                = ENABLE;
    NVIC_Init(&nvic);
    nvic.NVIC_IRQChannel = USART1_IRQn;
    NVIC_Init(&nvic);

    USART_DMACmd(USART1, USART_DMAReq_Rx, ENABLE);  //USART1 收到的字节走 DMA
    DMA_Cmd(DMA1_Channel5, ENABLE);                 //武装第一个缓冲，开始接收
}

//USART1 IDLE 中断入口（stm32f10x_it.c 里清完 IDLE 标志后调用）
//一段数据结束：收到的字节数 = 缓冲长度 - CNDTR 剩余值
void bootloader_uart_isr_idle(void)
{
    uint16_t len = (uint16_t)(BOOTLOADER_USART_REC_BUFF_LEN - DMA_GetCurrDataCounter(DMA1_Channel5));
    if (len == 0) return;   //整缓冲刚被 TC 当帧交付过、还没有新数据：跳过空帧
    rx_frame_done(len);
}

//DMA1_Channel5 传输完成中断入口（stm32f10x_it.c 里清完 TC 标志后调用）
//缓冲收满也算"一段数据结束"（与 HAL 版 TC 事件一致）
void bootloader_dma_isr_tc(void)
{
    uint16_t len = (uint16_t)(BOOTLOADER_USART_REC_BUFF_LEN - DMA_GetCurrDataCounter(DMA1_Channel5));
    if (len != 0) rx_frame_done(len);
}

//USART1 错误中断入口：ORE 溢出等标志由 stm32f10x_it.c 读 SR+DR 清掉（接收不会"死"），这里只计数
void bootloader_uart_isr_error(void)
{
    dbg_ore_cnt++;
}

//主循环轮询：有整帧待写则写入 flash。返回1=处理了一帧，0=无帧。
uint8_t bootloader_process_frame(void)
{
    if (!frame_ready) return 0;
    frame_ready = 0;
    bootloader_flash_write_frame(frame_buf, frame_len);
    return 1;
}

//帧访问接口（App_bootloader 状态机用）：只读/消费，不写 flash

// 查询是否有收到待处理的完整帧，1=有帧等待，0=无
uint8_t  bootloader_frame_pending(void)     { return frame_ready; }
// 获取待处理帧的数据缓冲区指针（只读，不能修改缓冲区内容）
const uint8_t *bootloader_frame_data(void)  { return frame_buf; }
// 获取待处理帧的有效字节长度
uint16_t bootloader_frame_size(void)        { return frame_len; }
// 标记当前帧已经处理完毕，清空帧就绪标志，准备接收下一帧
void     bootloader_frame_consume(void)     { frame_ready = 0; }

//按页检查/擦除并登记为已处理页（供 write_frame 和 flush 复用）
static void flash_ensure_page_erased(uint32_t addr)
{
    uint32_t page = addr & ~(BOOT_FLASH_PAGE_SIZE - 1U);

    if (page != current_erased_page)
    {
        uint8_t need_erase = 0;
        for (uint32_t a = page; a < page + BOOT_FLASH_PAGE_SIZE; a++)
            if (*(volatile uint8_t *)a != 0xFFU) { need_erase = 1; break; }
        if (need_erase)
        {
            FLASH_ErasePage(page);   //整页全 0xFF 的干净页跳过擦除，省时间
        }
        current_erased_page = page;
    }
}

//传输完成后调用：若遗留了最后一个字节（总长奇数），把它补 0xFF 写入 flash，
//否则该字节永远不会写进去（write_frame 只写偶数个字节）。
void bootloader_flash_flush(void)
{
    if (!last_byte_flag) return;

    FLASH_Unlock();
    uint32_t addr = APP_FLASH_BASE_ADDR + write_offset;
    flash_ensure_page_erased(addr);
    //低字节=遗留字节，高字节=0xFF 补齐（不覆盖任何有效数据）
    FLASH_ProgramHalfWord(addr, (uint16_t)((uint16_t)last_byte | 0xFF00U));
    write_offset += 2;
    last_byte      = 0;
    last_byte_flag = 0;
    FLASH_Lock();
}

//接收前清零写入状态（偏移/遗留字节/已擦除页标记）
void bootloader_flash_reset(void)
{
    write_offset = 0;
    current_erased_page = 0xFFFFFFFFU;
    last_byte = 0;
    last_byte_flag = 0;
}

//把一帧数据写入 flash：解锁 → 按页检查/擦除 → 半字编程 → 遗留字节 → 推进偏移 → 上锁。
//可被 App_bootloader 复用（传入它自己的缓冲与长度即可）。
void bootloader_flash_write_frame(const uint8_t *buf, uint16_t len)
{
    //1，解锁flash
    FLASH_Unlock();

    //2,本帧写入起始地址（write_offset 恒偶数，半字对齐）
    uint32_t write_addr = APP_FLASH_BASE_ADDR + write_offset;

    //3,把 [上帧遗留字节(若有)] + [本帧buf] 视为一条逻辑字节流。
    //   F1 只能半字编程，按2字节配对写入；若总长为奇数，末字节留给下一帧。
    uint8_t  lb_flag = last_byte_flag;                 //快照上帧遗留标志
    uint8_t  lb_val  = last_byte;                      //快照上帧遗留字节
    uint16_t total   = (uint16_t)lb_flag + len;
    uint16_t to_write = total & (uint16_t)~1U;          //本次实际写入字节数(恒偶数)
    uint8_t  new_last = 0, new_last_flag = 0;           //留给下一帧的遗留

    if (total & 1U) //总长为奇数：末字节成为新遗留
    {
        uint16_t idx = total - 1;                      //末字节在逻辑流中的下标
        new_last = (lb_flag && idx == 0)
                   ? lb_val
                   : buf[idx - lb_flag];
        new_last_flag = 1;
    }

    //取逻辑流第 p 字节：p==0 且有遗留 => 取遗留字节；否则取 buf[p-lb_flag]
    #define STREAM_BYTE(p) ((lb_flag && ((p)==0)) ? lb_val \
                        : buf[(p) - lb_flag])

    uint16_t p = 0;
    while (p < to_write)
    {
        flash_ensure_page_erased(write_addr);

        uint16_t halfword = (uint16_t)STREAM_BYTE(p)
                          | ((uint16_t)STREAM_BYTE(p + 1) << 8);
        FLASH_ProgramHalfWord(write_addr, halfword);

        write_addr += 2;
        p += 2;
    }
    #undef STREAM_BYTE

    //更新遗留字节，供下一帧使用
    last_byte      = new_last;
    last_byte_flag = new_last_flag;

    //4,推进 write_offset（恒偶数）
    write_offset = write_addr - APP_FLASH_BASE_ADDR;

    //5,上锁
    FLASH_Lock();
}

void bootloader_jump_to_app(void)
{
    typedef void (*pFunc)(void);

    //1，检验
    uint32_t app_start_ptr = *(volatile uint32_t *)APP_FLASH_BASE_ADDR;
    uint32_t app_reset_ptr = *(volatile uint32_t *)(APP_FLASH_BASE_ADDR + 4);

    if((app_start_ptr & 0xffff0000)!=APP_RESET_ADDR)
    {
        printf("栈顶地址错误\n");
        return;
    }

    if(app_reset_ptr < APP_FLASH_BASE_ADDR||app_reset_ptr >= APP_END_ADDR)
    {
        printf("重置地址错误\n");
        return;
    }

    //2，注销本程序运行环境
    //关中断
    __disable_irq();

    //停掉本程序用到的外设（等价 HAL 版 HAL_RCC_DeInit/HAL_DeInit 的收尾）：
    //尤其要停掉还处于武装状态的 DMA——否则跳进 app 后 DMA 仍可能把新收到的
    //字节写进已被 app 占用的 RAM
    DMA_Cmd(DMA1_Channel5, DISABLE);
    USART_DMACmd(USART1, USART_DMAReq_Rx, DISABLE);
    USART_ITConfig(USART1, USART_IT_IDLE, DISABLE);
    USART_ITConfig(USART1, USART_IT_ERR, DISABLE);
    USART_Cmd(USART1, DISABLE);
    TIM_Cmd(TIM4, DISABLE);

    /* 关闭并清除所有外部中断，防止残留 pending 位把 app 带进 Default_Handler */
    for (uint32_t i = 0; i < 8; i++)
    {
        NVIC->ICER[i] = 0xFFFFFFFFU;   /* disable */
        NVIC->ICPR[i] = 0xFFFFFFFFU;   /* clear pending */
    }
    SysTick->CTRL = 0;                 /* 顺手停掉 SysTick */
    SysTick->VAL = 0;                  /* 重置 SysTick 值 */
    SysTick->LOAD = 0;                 /* 重置 SysTick 装载值 */

    //修改主栈指针
    __set_MSP(app_start_ptr);
    //重定向中断向量表
    SCB->VTOR = APP_FLASH_BASE_ADDR;
    //3，跳转A程序复位中断
    pFunc jump_to_app = (pFunc)app_reset_ptr;
    jump_to_app();
}
