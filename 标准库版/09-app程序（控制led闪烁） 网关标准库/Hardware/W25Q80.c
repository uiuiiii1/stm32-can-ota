#include "W25Q80.h"
#include "SPI.h"
#include "Tick.h"

/*============================================================
  W25Q64 SPI NOR Flash 驱动（硬件 SPI1 @ 9MHz，模式0，CS=PA4 软件控制）
  - 擦除是写的前置条件（NOR 只能 1→0）；编程按 256B 页进行、不能跨页。
  - 忙等待用 GetTick() 判超时，所以本驱动依赖 Tick 模块已初始化。
  ============================================================*/

#define CMD_JEDEC_ID      0x9F
#define CMD_WRITE_EN       0x06
#define CMD_READ_SR1       0x05
#define CMD_READ_DATA      0x03
#define CMD_PAGE_PROG      0x02
#define CMD_SECTOR_ERASE   0x20
#define CMD_CHIP_ERASE     0xC7

#define SR1_BUSY           0x01

#define TIMEOUT_PROGRAM    10
#define TIMEOUT_SECTOR     1000
#define TIMEOUT_CHIP       300000UL

/*---------------- 底层：CS 与 SPI 收发 ----------------*/

static void W25_CS_LOW(void)  { GPIO_ResetBits(W25Q80_CS_GPIO, W25Q80_CS_PIN); }
static void W25_CS_HIGH(void) { GPIO_SetBits(W25Q80_CS_GPIO, W25Q80_CS_PIN);   }

// 发送一段数据（HAL 版的 HAL_SPI_Transmit），这里逐字节全双工，丢弃返回
static void W25_Tx(const uint8_t *Data, uint16_t Len)
{
    uint16_t i;
    for (i = 0; i < Len; i++)
    {
        (void)SPI1_RW(Data[i]);
    }
}

// 接收一段数据：主机必须发字节才能移出从机数据，故发 0xFF
static void W25_Rx(uint8_t *Data, uint16_t Len)
{
    uint16_t i;
    for (i = 0; i < Len; i++)
    {
        Data[i] = SPI1_RW(0xFF);
    }
}

static uint8_t W25_WaitBusy(uint32_t TimeoutMs)
{
    uint8_t cmd = CMD_READ_SR1;
    uint8_t sr1;
    uint32_t t0 = GetTick();

    do
    {
        W25_CS_LOW();
        W25_Tx(&cmd, 1);
        W25_Rx(&sr1, 1);
        W25_CS_HIGH();

        if ((sr1 & SR1_BUSY) == 0)
        {
            return W25Q80_OK;
        }
    } while ((GetTick() - t0) < TimeoutMs);

    return W25Q80_ERR_TIMEOUT;
}

static uint8_t W25_WriteEnable(void)
{
    uint8_t cmd = CMD_WRITE_EN;
    W25_CS_LOW();
    W25_Tx(&cmd, 1);
    W25_CS_HIGH();
    return W25Q80_OK;
}

// 发送"命令 + 24 位地址"，地址 MSB 先行
static void W25_SendAddrCmd(uint8_t Cmd, uint32_t Addr)
{
    uint8_t buf[4];
    buf[0] = Cmd;
    buf[1] = (uint8_t)(Addr >> 16);
    buf[2] = (uint8_t)(Addr >> 8);
    buf[3] = (uint8_t)(Addr);
    W25_Tx(buf, 4);
}

/*============================================================
  对外接口
  ============================================================*/

uint8_t W25Q80_ReadID(uint32_t *Id)
{
    uint8_t cmd = CMD_JEDEC_ID;
    uint8_t rsp[3] = {0};

    if (Id == 0)
    {
        return W25Q80_ERR_PARAM;
    }

    W25_CS_LOW();
    W25_Tx(&cmd, 1);
    W25_Rx(rsp, 3);
    W25_CS_HIGH();

    *Id = ((uint32_t)rsp[0] << 16) | ((uint32_t)rsp[1] << 8) | rsp[2];
    return W25Q80_OK;
}

uint8_t W25Q80_Init(void)
{
    uint32_t id;
    uint8_t  cmd = 0xAB;       // Release Power-down / Device ID
    uint8_t  dummy[3];

    // 初始化 CS 引脚：PA4 推挽输出，默认拉高（不选中）
    GPIO_InitTypeDef gpio = {0};
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
    gpio.GPIO_Pin   = W25Q80_CS_PIN;
    gpio.GPIO_Mode  = GPIO_Mode_Out_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(W25Q80_CS_GPIO, &gpio);
    W25_CS_HIGH();

    SPI1_Init();

    W25_CS_LOW();
    W25_Tx(&cmd, 1);
    W25_Rx(dummy, 3);
    W25_CS_HIGH();

    if (W25Q80_ReadID(&id) != W25Q80_OK)
    {
        return W25Q80_ERR_SPI;
    }
    if (id != W25Q80_JEDEC_ID)
    {
        return W25Q80_ERR_ID;
    }
    return W25Q80_OK;
}

uint8_t W25Q80_Read(uint32_t Addr, uint8_t *Buf, uint32_t Len)
{
    uint32_t remain, chunk;

    if (Addr >= W25Q80_TOTAL_SIZE || Len > W25Q80_TOTAL_SIZE - Addr || Buf == 0)
    {
        return W25Q80_ERR_PARAM;
    }
    if (Len == 0)
    {
        return W25Q80_OK;
    }

    W25_CS_LOW();
    W25_SendAddrCmd(CMD_READ_DATA, Addr);

    // 标准库按字节收，没有 16 位长度限制，直接整段收
    W25_Rx(Buf, (uint16_t)Len);
    W25_CS_HIGH();

    (void)remain; (void)chunk;   // 兼容原 HAL 版变量，标准库按字节收不再需要分片
    return W25Q80_OK;
}

uint8_t W25Q80_PageProgram(uint32_t Addr, const uint8_t *Buf, uint16_t Len)
{
    if (Addr >= W25Q80_TOTAL_SIZE || Buf == 0)
    {
        return W25Q80_ERR_PARAM;
    }
    if (Len == 0)
    {
        return W25Q80_OK;
    }
    if (Len > W25Q80_PAGE_SIZE ||
        (Addr % W25Q80_PAGE_SIZE) + Len > W25Q80_PAGE_SIZE ||
        Addr + Len > W25Q80_TOTAL_SIZE)
    {
        return W25Q80_ERR_PARAM;
    }

    if (W25_WriteEnable() != W25Q80_OK)
    {
        return W25Q80_ERR_SPI;
    }

    W25_CS_LOW();
    W25_SendAddrCmd(CMD_PAGE_PROG, Addr);
    W25_Tx(Buf, Len);
    W25_CS_HIGH();

    return W25_WaitBusy(TIMEOUT_PROGRAM);
}

uint8_t W25Q80_Write(uint32_t Addr, const uint8_t *Buf, uint32_t Len)
{
    uint32_t remain, pageRemain, chunk;

    if (Addr >= W25Q80_TOTAL_SIZE || Len > W25Q80_TOTAL_SIZE - Addr || Buf == 0)
    {
        return W25Q80_ERR_PARAM;
    }

    remain = Len;
    while (remain > 0)
    {
        pageRemain = W25Q80_PAGE_SIZE - (uint16_t)(Addr % W25Q80_PAGE_SIZE);
        chunk = (remain < pageRemain) ? remain : pageRemain;

        if (W25Q80_PageProgram(Addr, Buf, (uint16_t)chunk) != W25Q80_OK)
        {
            return W25Q80_ERR_SPI;
        }

        Addr  += chunk;
        Buf   += chunk;
        remain -= chunk;
    }
    return W25Q80_OK;
}

uint8_t W25Q80_EraseSector(uint32_t Addr)
{
    if (Addr >= W25Q80_TOTAL_SIZE)
    {
        return W25Q80_ERR_PARAM;
    }

    if (W25_WriteEnable() != W25Q80_OK)
    {
        return W25Q80_ERR_SPI;
    }

    W25_CS_LOW();
    W25_SendAddrCmd(CMD_SECTOR_ERASE, Addr);
    W25_CS_HIGH();

    return W25_WaitBusy(TIMEOUT_SECTOR);
}

uint8_t W25Q80_ChipErase(void)
{
    uint8_t cmd = CMD_CHIP_ERASE;

    if (W25_WriteEnable() != W25Q80_OK)
    {
        return W25Q80_ERR_SPI;
    }

    W25_CS_LOW();
    W25_Tx(&cmd, 1);
    W25_CS_HIGH();

    return W25_WaitBusy(TIMEOUT_CHIP);
}
