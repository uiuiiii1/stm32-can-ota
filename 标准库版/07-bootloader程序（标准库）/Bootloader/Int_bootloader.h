#ifndef __INT_BOOTLOADER_H
#define __INT_BOOTLOADER_H

#include <stdint.h>

// Flash 分区（与 HAL 版一致）
#define PESET_START           0x08004000U   // 默认/出厂程序基址
#define APP_FLASH_BASE_ADDR   0x08008000U   // app 基址（程序写入起始位置）
#define APP_SRAM_START        0x20000000U   // SRAM 起始地址（栈顶高16位应为此值）
#define APP_END_ADDR          0x08010000U   // app 区结束地址

// 跳转到应用程序：校验栈顶+复位向量（按分区精确校验），再清理外设、设 MSP/VTOR、跳转
uint8_t bootloader_jump_to_app(uint32_t app_reset_addr);

#endif /* __INT_BOOTLOADER_H */
