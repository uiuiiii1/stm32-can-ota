#ifndef __W25Q80_H
#define __W25Q80_H

#include <stdint.h>

// W25Q80 接线：CS=PA4（软件片选，PA4 初始化在 main.c 的 USER CODE 2 里补配），
// SCK=PA5 / MISO=PA6 / MOSI=PA7（硬件 SPI1，9MHz，模式0）
// SPI1 由 MX_SPI1_Init() 初始化，本驱动只控制片选和收发
// 从 07-bootloader 的 W25Q64 驱动移植：同家族指令集一致，仅容量与 JEDEC ID 不同

// 器件参数
#define W25Q80_PAGE_SIZE      256         // 页：256 字节（编程单位，一次页编程不能跨页）
#define W25Q80_SECTOR_SIZE    4096        // 扇区：4KB（最小擦除单位）
#define W25Q80_BLOCK_SIZE     65536       // 块：64KB
#define W25Q80_TOTAL_SIZE     0x100000UL  // 总容量：1MB（8Mbit），注意与 W25Q64 的 8MB 区分

#define W25Q80_JEDEC_ID       0xEF4014UL  // 正确器件应答的 JEDEC ID（EF=Winbond，4014=W25Q80）
                                            // W25Q64 是 0xEF4017，别混用，Init 会校验

// 片选引脚：若以后在 CubeMX 里给 PA4 配了用户标签会生成同名宏，这里做了防重复定义
#ifndef W25Q80_CS_Pin
#define W25Q80_CS_Pin         GPIO_PIN_4
#define W25Q80_CS_GPIO_Port   GPIOA
#endif

// 返回值定义
#define W25Q80_OK             0           // 操作成功
#define W25Q80_ERR_SPI        1           // SPI 收发异常
#define W25Q80_ERR_ID         2           // JEDEC ID 不匹配（未接/接错/非 W25Q80）
#define W25Q80_ERR_PARAM      3           // 地址或长度越界
#define W25Q80_ERR_TIMEOUT    4           // 等待器件忙状态超时

uint8_t W25Q80_Init(void);                                  // 初始化并校验器件 ID
uint8_t W25Q80_ReadID(uint32_t *Id);                        // 读 JEDEC ID
uint8_t W25Q80_Read(uint32_t Addr, uint8_t *Buf, uint32_t Len);          // 连续读取（无长度限制）
uint8_t W25Q80_PageProgram(uint32_t Addr, const uint8_t *Buf, uint16_t Len); // 单页编程（≤256B，不可跨页）
uint8_t W25Q80_Write(uint32_t Addr, const uint8_t *Buf, uint32_t Len);   // 连续编程（自动按页切分）
uint8_t W25Q80_EraseSector(uint32_t Addr);                  // 擦除 Addr 所在 4KB 扇区
uint8_t W25Q80_ChipErase(void);                             // 全片擦除（很慢，约几十秒到 200 秒）

#endif /* __W25Q80_H */
