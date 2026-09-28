#ifndef __SPI_H
#define __SPI_H

#include "stm32f10x.h"

// 硬件 SPI1：PA5=SCK / PA6=MISO / PA7=MOSI，主模式 8bit 模式0，预分频8 → 9MHz
// CS(PA4) 由 W25Q64 驱动软件控制，本文件只管 SPI 外设和它的 3 根通信脚

void     SPI1_Init(void);
uint8_t  SPI1_RW(uint8_t tx);   // 全双工收发一个字节（写时忽略返回，读时发 0xFF）

#endif /* __SPI_H */
