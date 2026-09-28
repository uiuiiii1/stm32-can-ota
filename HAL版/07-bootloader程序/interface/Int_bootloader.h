#ifndef __BOOTLOADER_H
#define __BOOTLOADER_H
#include "usart.h"


#define PESET_START        0x08004000U
#define APP_FLASH_BASE_ADDR   0x08008000U  //程序写入起始位置，ppa起始地址
#define APP_SRAM_START 0x20000000U          //重置地址
#define APP_END_ADDR   0x08010000U          //程序结束地址



//调试量（定位丢字节，由 main 打印）
extern volatile uint16_t dbg_frame_cnt;
extern volatile uint16_t dbg_last_size;
extern volatile uint16_t dbg_short_size;
extern volatile uint16_t dbg_ore_cnt;   //ORE 溢出次数（HAL 把 ORE 当致命错误关接收，此处已自愈）

//跳转到应用程序
uint8_t bootloader_jump_to_app(uint32_t app_reset_ptr);
#endif
