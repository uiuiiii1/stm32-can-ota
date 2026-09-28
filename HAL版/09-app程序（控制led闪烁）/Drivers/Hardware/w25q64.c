#include "w25q64.h"
#include "spi.h"
#include "stm32f1xx_hal.h"

/*============================================================
  W25Q64 SPI NOR Flash 驱动（硬件 SPI1 @ 9MHz，模式0，CS=PA4 软件控制）

  与 AT24C64（EEPROM）的本质区别——NOR Flash 的两个"物理铁律"：
  1. 擦除是写的前置条件：位只能从 1 编程成 0，不能从 0 改回 1。
     往没擦过的区域直接写会得到新旧数据的按位与（静默损坏），
     所以覆盖写之前必须先 W25Q64_EraseSector()。
  2. 编程按页进行：一次 Page Program 不能跨越 256 字节页边界，
     跨页会回卷到页首覆盖本页开头。W25Q64_Write() 已自动按页切分。

  时序要点：CS 拉低期间发命令+地址+数据，操作结束拉高 CS；
  编程/擦除类命令在 CS 拉高后才执行，期间读 SR1 的 BUSY 位等待。
  ============================================================*/

// 常用命令
#define CMD_JEDEC_ID     0x9F
#define CMD_WRITE_EN     0x06    // WREN：编程/擦除前必须先发
#define CMD_READ_SR1     0x05
#define CMD_READ_DATA    0x03
#define CMD_PAGE_PROG    0x02
#define CMD_SECTOR_ERASE 0x20    // 4KB 扇区擦除
#define CMD_CHIP_ERASE   0xC7

#define SR1_BUSY         0x01    // SR1 的 BUSY 位

// 忙等待超时（ms）：数据手册典型值 × 较大裕量
#define TIMEOUT_PROGRAM  10      // 页编程典型 0.4/0.7ms，上限 3ms
#define TIMEOUT_SECTOR   1000    // 扇区擦除典型 45ms，上限 400ms
#define TIMEOUT_CHIP     300000UL // 全片擦除典型 30~150s，上限 200s

/*---------------- 底层：CS 与 SPI 收发 ----------------*/

static void W25_CS_LOW(void)
{
    HAL_GPIO_WritePin(W25Q64_CS_GPIO_Port, W25Q64_CS_Pin, GPIO_PIN_RESET);
}

static void W25_CS_HIGH(void)
{
    HAL_GPIO_WritePin(W25Q64_CS_GPIO_Port, W25Q64_CS_Pin, GPIO_PIN_SET);
}

static uint8_t W25_Tx(uint8_t *Data, uint16_t Len)
{
    return (HAL_SPI_Transmit(&hspi1, Data, Len, 100) == HAL_OK) ? W25Q64_OK : W25Q64_ERR_SPI;
}

static uint8_t W25_Rx(uint8_t *Data, uint16_t Len)
{
    return (HAL_SPI_Receive(&hspi1, Data, Len, 100) == HAL_OK) ? W25Q64_OK : W25Q64_ERR_SPI;
}

// 等待内部操作完成：轮询 SR1.BUSY，超时返回 W25Q64_ERR_TIMEOUT
static uint8_t W25_WaitBusy(uint32_t TimeoutMs)
{
    uint8_t Cmd = CMD_READ_SR1;
    uint8_t Sr1;
    uint32_t T0 = HAL_GetTick();

    do
    {
        W25_CS_LOW();
        if (W25_Tx(&Cmd, 1) != W25Q64_OK)
        {
            W25_CS_HIGH();
            return W25Q64_ERR_SPI;
        }
        if (W25_Rx(&Sr1, 1) != W25Q64_OK)
        {
            W25_CS_HIGH();
            return W25Q64_ERR_SPI;
        }
        W25_CS_HIGH();

        if ((Sr1 & SR1_BUSY) == 0)
        {
            return W25Q64_OK;
        }
    } while (HAL_GetTick() - T0 < TimeoutMs);

    return W25Q64_ERR_TIMEOUT;
}

// 写使能：编程/擦除前必须置位 WEL，操作开始后 WEL 自动清零
static uint8_t W25_WriteEnable(void)
{
    uint8_t Cmd = CMD_WRITE_EN;

    W25_CS_LOW();
    if (W25_Tx(&Cmd, 1) != W25Q64_OK)
    {
        W25_CS_HIGH();
        return W25Q64_ERR_SPI;
    }
    W25_CS_HIGH();

    return W25Q64_OK;
}

// 发送"命令 + 24 位地址"，地址 MSB 先行
static uint8_t W25_SendAddrCmd(uint8_t Cmd, uint32_t Addr)
{
    uint8_t Buf[4];

    Buf[0] = Cmd;
    Buf[1] = (uint8_t)(Addr >> 16);
    Buf[2] = (uint8_t)(Addr >> 8);
    Buf[3] = (uint8_t)Addr;

    return W25_Tx(Buf, 4);
}

/*============================================================
  对外接口
  ============================================================*/

// 读 JEDEC ID：应答 0xEF4017 表示 W25Q64 在线
uint8_t W25Q64_ReadID(uint32_t *Id)
{
    uint8_t Cmd = CMD_JEDEC_ID;
    uint8_t Rsp[3] = {0};

    if (Id == 0)
    {
        return W25Q64_ERR_PARAM;
    }

    W25_CS_LOW();
    if (W25_Tx(&Cmd, 1) != W25Q64_OK ||
        W25_Rx(Rsp, 3) != W25Q64_OK)
    {
        W25_CS_HIGH();
        return W25Q64_ERR_SPI;
    }
    W25_CS_HIGH();

    *Id = ((uint32_t)Rsp[0] << 16) | ((uint32_t)Rsp[1] << 8) | Rsp[2];
    return W25Q64_OK;
}

// 初始化：释放可能的掉电状态，读 ID 校验器件
uint8_t W25Q64_Init(void)
{
    uint32_t Id;
    uint8_t Cmd = 0xAB;     // Release Power-down / Device ID，确保器件处于标准待机态
    uint8_t Dummy[3];

    W25_CS_LOW();
    if (W25_Tx(&Cmd, 1) != W25Q64_OK)
    {
        W25_CS_HIGH();
        return W25Q64_ERR_SPI;
    }
    if (W25_Rx(Dummy, 3) != W25Q64_OK)  // 3 个无效字节后器件 ID 才出现，这里忽略
    {
        W25_CS_HIGH();
        return W25Q64_ERR_SPI;
    }
    W25_CS_HIGH();

    if (W25Q64_ReadID(&Id) != W25Q64_OK)
    {
        return W25Q64_ERR_SPI;
    }
    if (Id != W25Q64_JEDEC_ID)
    {
        return W25Q64_ERR_ID;
    }
    return W25Q64_OK;
}

// 连续读取：读操作没有页限制，地址自动跨页/跨扇区递增
uint8_t W25Q64_Read(uint32_t Addr, uint8_t *Buf, uint32_t Len)
{
    uint32_t Remain, Chunk;
    uint8_t Ret;

    if (Addr >= W25Q64_TOTAL_SIZE || Len > W25Q64_TOTAL_SIZE - Addr || Buf == 0)
    {
        return W25Q64_ERR_PARAM;
    }
    if (Len == 0)
    {
        return W25Q64_OK;
    }

    W25_CS_LOW();
    Ret = W25_SendAddrCmd(CMD_READ_DATA, Addr);
    if (Ret != W25Q64_OK)
    {
        W25_CS_HIGH();
        return Ret;
    }

    // HAL 的收发长度是 16 位，大块数据分片收
    Remain = Len;
    while (Remain > 0)
    {
        Chunk = (Remain > 0xFFFFU) ? 0xFFFFU : Remain;
        Ret = W25_Rx(Buf, (uint16_t)Chunk);
        if (Ret != W25Q64_OK)
        {
            W25_CS_HIGH();
            return Ret;
        }
        Buf += Chunk;
        Remain -= Chunk;
    }
    W25_CS_HIGH();

    return W25Q64_OK;
}

// 单页编程：Len ≤ 256 且不允许跨页（越界部分会被芯片写回页首，必须拦截）
uint8_t W25Q64_PageProgram(uint32_t Addr, const uint8_t *Buf, uint16_t Len)
{
    uint8_t Ret;

    if (Addr >= W25Q64_TOTAL_SIZE || Buf == 0)
    {
        return W25Q64_ERR_PARAM;
    }
    if (Len == 0)
    {
        return W25Q64_OK;
    }
    if (Len > W25Q64_PAGE_SIZE ||
        (Addr % W25Q64_PAGE_SIZE) + Len > W25Q64_PAGE_SIZE ||
        Addr + Len > W25Q64_TOTAL_SIZE)
    {
        return W25Q64_ERR_PARAM;
    }

    Ret = W25_WriteEnable();
    if (Ret != W25Q64_OK)
    {
        return Ret;
    }

    W25_CS_LOW();
    Ret = W25_SendAddrCmd(CMD_PAGE_PROG, Addr);
    if (Ret == W25Q64_OK)
    {
        Ret = W25_Tx((uint8_t *)Buf, Len);
    }
    W25_CS_HIGH();      // CS 拉高后芯片才开始内部编程

    if (Ret != W25Q64_OK)
    {
        return Ret;
    }
    return W25_WaitBusy(TIMEOUT_PROGRAM);
}

// 连续编程：按页边界自动切分，逐页编程并等待完成
// 注意：目标区域必须事先擦除过（见文件头说明），本函数不做擦除
uint8_t W25Q64_Write(uint32_t Addr, const uint8_t *Buf, uint32_t Len)
{
    uint32_t Remain, PageRemain, Chunk;
    uint8_t Ret;

    if (Addr >= W25Q64_TOTAL_SIZE || Len > W25Q64_TOTAL_SIZE - Addr || Buf == 0)
    {
        return W25Q64_ERR_PARAM;
    }

    Remain = Len;
    while (Remain > 0)
    {
        PageRemain = W25Q64_PAGE_SIZE - (uint16_t)(Addr % W25Q64_PAGE_SIZE);
        Chunk = (Remain < PageRemain) ? Remain : PageRemain;

        Ret = W25Q64_PageProgram(Addr, Buf, (uint16_t)Chunk);
        if (Ret != W25Q64_OK)
        {
            return Ret;
        }

        Addr += Chunk;
        Buf += Chunk;
        Remain -= Chunk;
    }
    return W25Q64_OK;
}

// 擦除 Addr 所在的 4KB 扇区（地址会自动对齐到扇区边界）
uint8_t W25Q64_EraseSector(uint32_t Addr)
{
    uint8_t Ret;

    if (Addr >= W25Q64_TOTAL_SIZE)
    {
        return W25Q64_ERR_PARAM;
    }

    Ret = W25_WriteEnable();
    if (Ret != W25Q64_OK)
    {
        return Ret;
    }

    W25_CS_LOW();
    Ret = W25_SendAddrCmd(CMD_SECTOR_ERASE, Addr);
    W25_CS_HIGH();

    if (Ret != W25Q64_OK)
    {
        return Ret;
    }
    return W25_WaitBusy(TIMEOUT_SECTOR);
}

// 全片擦除：整个 8MB 恢复为 0xFF。非常慢（几十秒级），仅初始化场合使用
uint8_t W25Q64_ChipErase(void)
{
    uint8_t Cmd = CMD_CHIP_ERASE;
    uint8_t Ret;

    Ret = W25_WriteEnable();
    if (Ret != W25Q64_OK)
    {
        return Ret;
    }

    W25_CS_LOW();
    Ret = W25_Tx(&Cmd, 1);
    W25_CS_HIGH();

    if (Ret != W25Q64_OK)
    {
        return Ret;
    }
    return W25_WaitBusy(TIMEOUT_CHIP);
}
