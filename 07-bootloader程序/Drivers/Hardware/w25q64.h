#ifndef __W25Q64_H
#define __W25Q64_H

#include <stdint.h>

// W25Q64 接线：CS=PA4（本驱动软件控制），SCK=PA5 / MISO=PA6 / MOSI=PA7（硬件 SPI1，9MHz，模式0）
// SPI1 由 MX_SPI1_Init() 初始化，本驱动只控制片选和收发

// 器件参数
#define W25Q64_PAGE_SIZE      256         // 页：256 字节（编程单位，一次页编程不能跨页）
#define W25Q64_SECTOR_SIZE    4096        // 扇区：4KB（最小擦除单位）
#define W25Q64_BLOCK_SIZE     65536       // 块：64KB
#define W25Q64_TOTAL_SIZE     0x800000UL  // 总容量：8MB（64Mbit）

#define W25Q64_JEDEC_ID       0xEF4017UL  // 正确器件应答的 JEDEC ID（EF=Winbond，4017=W25Q64）

// 返回值定义
#define W25Q64_OK             0           // 操作成功
#define W25Q64_ERR_SPI        1           // SPI 收发异常
#define W25Q64_ERR_ID         2           // JEDEC ID 不匹配（未接/接错/非 W25Q64）
#define W25Q64_ERR_PARAM      3           // 地址或长度越界
#define W25Q64_ERR_TIMEOUT    4           // 等待器件忙状态超时

uint8_t W25Q64_Init(void);                                  // 初始化并校验器件 ID
uint8_t W25Q64_ReadID(uint32_t *Id);                        // 读 JEDEC ID
uint8_t W25Q64_Read(uint32_t Addr, uint8_t *Buf, uint32_t Len);          // 连续读取（无长度限制）
uint8_t W25Q64_PageProgram(uint32_t Addr, const uint8_t *Buf, uint16_t Len); // 单页编程（≤256B，不可跨页）
uint8_t W25Q64_Write(uint32_t Addr, const uint8_t *Buf, uint32_t Len);   // 连续编程（自动按页切分）
uint8_t W25Q64_EraseSector(uint32_t Addr);                  // 擦除 Addr 所在 4KB 扇区
uint8_t W25Q64_ChipErase(void);                             // 全片擦除（很慢，约几十秒到 200 秒）

#endif /* __W25Q64_H */
