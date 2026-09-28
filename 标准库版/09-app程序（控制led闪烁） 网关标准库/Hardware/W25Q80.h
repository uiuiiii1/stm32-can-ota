#ifndef __W25Q80_H
#define __W25Q80_H

#include "stm32f10x.h"

// W25Q80 接线：CS=PA4（本驱动软件控制 + 初始化），SCK=PA5 / MISO=PA6 / MOSI=PA7（硬件 SPI1）
// 器件参数
#define W25Q80_PAGE_SIZE      256
#define W25Q80_SECTOR_SIZE     4096
#define W25Q80_BLOCK_SIZE     65536
#define W25Q80_TOTAL_SIZE     0x100000UL  // 1MB（8Mbit）
#define W25Q80_JEDEC_ID        0xEF4014UL

// 返回值
#define W25Q80_OK              0
#define W25Q80_ERR_SPI         1
#define W25Q80_ERR_ID          2
#define W25Q80_ERR_PARAM       3
#define W25Q80_ERR_TIMEOUT     4

// CS 片选引脚：PA4
#define W25Q80_CS_GPIO         GPIOA
#define W25Q80_CS_PIN          GPIO_Pin_4

uint8_t W25Q80_Init(void);
uint8_t W25Q80_ReadID(uint32_t *Id);
uint8_t W25Q80_Read(uint32_t Addr, uint8_t *Buf, uint32_t Len);
uint8_t W25Q80_PageProgram(uint32_t Addr, const uint8_t *Buf, uint16_t Len);
uint8_t W25Q80_Write(uint32_t Addr, const uint8_t *Buf, uint32_t Len);
uint8_t W25Q80_EraseSector(uint32_t Addr);
uint8_t W25Q80_ChipErase(void);

#endif /* __W25Q80_H */
