#include "at24c64.h"
#include "stm32f1xx_hal.h"

/*============================================================
  AT24C64 软件 I2C 驱动（PB10=SCL，PB11=SDA，开漏+上拉）

  时序说明：
  - 引脚为开漏输出：写 1 = 释放总线（由上拉电阻拉高），写 0 = 拉低。
    因此"读 SDA"前必须先释放 SDA，这也是主机接收数据的基础。
  - AT24C64 最高支持 400kHz，这里用 DWT 周期计数器做 5us 半位延时，
    稳定跑 100kHz，裕量充足。
  - 写操作结束后器件进入内部写周期（最大 5ms），期间不响应任何命令，
    用"应答轮询"（Acknowledge Polling）等待其就绪。
  ============================================================*/

// 半位延时（微秒），基于 DWT CYCCNT，比空循环精确且不受编译优化影响
#define IIC_HALF_BIT_US   (1000U / AT24C64_I2C_FREQ_KHZ / 2U)

static void IIC_Delay(void)
{
    uint32_t Start = DWT->CYCCNT;
    uint32_t Ticks = IIC_HALF_BIT_US * (SystemCoreClock / 1000000U);
    while ((DWT->CYCCNT - Start) < Ticks);
}

// 引脚操作：写 1 即释放总线（开漏特性），读线前必须先释放
static void IIC_SCL(uint8_t BitValue)
{
    HAL_GPIO_WritePin(AT24C64_SCL_PORT, AT24C64_SCL_PIN, (GPIO_PinState)BitValue);
}

static void IIC_W_SDA(uint8_t BitValue)
{
    HAL_GPIO_WritePin(AT24C64_SDA_PORT, AT24C64_SDA_PIN, (GPIO_PinState)BitValue);
}

static uint8_t IIC_R_SDA(void)
{
    return (uint8_t)HAL_GPIO_ReadPin(AT24C64_SDA_PORT, AT24C64_SDA_PIN);
}

// 起始信号：SCL 高电平期间 SDA 由高变低
static void IIC_Start(void)
{
    IIC_W_SDA(1);
    IIC_SCL(1);
    IIC_Delay();
    IIC_W_SDA(0);
    IIC_Delay();
    IIC_SCL(0);
    IIC_Delay();
}

// 停止信号：SCL 高电平期间 SDA 由低变高
static void IIC_Stop(void)
{
    IIC_W_SDA(0);
    IIC_Delay();
    IIC_SCL(1);
    IIC_Delay();
    IIC_W_SDA(1);
    IIC_Delay();
}

// 发送一个字节（MSB 先行），返回器件应答位：0=ACK，1=NAK
static uint8_t IIC_SendByte(uint8_t Byte)
{
    uint8_t i, Ack;

    for (i = 0; i < 8; i++)
    {
        IIC_SCL(0);
        IIC_W_SDA((Byte & 0x80) ? 1 : 0);
        IIC_Delay();
        IIC_SCL(1);
        IIC_Delay();
        Byte <<= 1;
    }

    // 第 9 个时钟：释放 SDA，读取从机应答
    IIC_SCL(0);
    IIC_W_SDA(1);
    IIC_Delay();
    IIC_SCL(1);
    IIC_Delay();
    Ack = IIC_R_SDA();
    IIC_SCL(0);

    return Ack;
}

// 接收一个字节（MSB 先行），参数 Ack：1=主机应答（继续读），0=非应答（结束读取）
static uint8_t IIC_ReadByte(uint8_t Ack)
{
    uint8_t i, Byte = 0;

    IIC_W_SDA(1);   // 释放 SDA，交由从机驱动数据线
    for (i = 0; i < 8; i++)
    {
        IIC_SCL(0);
        IIC_Delay();
        IIC_SCL(1);
        IIC_Delay();
        Byte <<= 1;
        if (IIC_R_SDA())
        {
            Byte |= 0x01;
        }
    }

    // 第 9 个时钟：给出应答/非应答
    IIC_SCL(0);
    IIC_W_SDA((Ack) ? 0 : 1);
    IIC_Delay();
    IIC_SCL(1);
    IIC_Delay();
    IIC_SCL(0);
    IIC_W_SDA(1);   // 释放 SDA，恢复总线空闲

    return Byte;
}

// 写指针（哑写）：发送器件地址+写方向和 16 位存储地址，此后可接数据（写）或重启（读）
// 返回：AT24C64_OK=器件应答，AT24C64_ERR_NAK=无应答
static uint8_t AT24C64_SetAddr(uint16_t Addr)
{
    IIC_Start();
    if (IIC_SendByte(AT24C64_DEV_ADDR | 0x00))  // 写方向，无应答则放弃
    {
        IIC_Stop();
        return AT24C64_ERR_NAK;
    }
    if (IIC_SendByte((uint8_t)(Addr >> 8)))     // 存储地址高 8 位
    {
        IIC_Stop();
        return AT24C64_ERR_NAK;
    }
    if (IIC_SendByte((uint8_t)Addr))            // 存储地址低 8 位
    {
        IIC_Stop();
        return AT24C64_ERR_NAK;
    }
    return AT24C64_OK;
}

/*============================================================
  对外接口
  ============================================================*/

// 初始化：使能 DWT 计数器，应答轮询确认器件在线
uint8_t AT24C64_Init(void)
{
    // 使能 DWT 周期计数器（Cortex-M3 调试组件），供 IIC_Delay 使用
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    // 总线置空闲态
    IIC_SCL(1);
    IIC_W_SDA(1);

    return AT24C64_WaitReady();
}

// 应答轮询：写周期期间器件不应答，轮询到 ACK 即表示内部编程完成
uint8_t AT24C64_WaitReady(void)
{
    uint32_t Retry;

    for (Retry = 0; Retry < 1000; Retry++)  // 远大于 5ms 写周期的保险次数
    {
        IIC_Start();
        if (IIC_SendByte(AT24C64_DEV_ADDR) == 0)    // 收到 ACK，器件就绪
        {
            IIC_Stop();
            return AT24C64_OK;
        }
        IIC_Stop();
        IIC_Delay();
    }
    return AT24C64_ERR_NAK;
}

// 单字节写入
uint8_t AT24C64_WriteByte(uint16_t Addr, uint8_t Data)
{
    if (Addr >= AT24C64_SIZE)
    {
        return AT24C64_ERR_PARAM;
    }

    if (AT24C64_SetAddr(Addr) != AT24C64_OK)
    {
        return AT24C64_ERR_NAK;
    }

    if (IIC_SendByte(Data))
    {
        IIC_Stop();
        return AT24C64_ERR_NAK;
    }
    IIC_Stop();     // 停止后器件才开始内部编程

    return AT24C64_WaitReady();
}

// 单字节读取
uint8_t AT24C64_ReadByte(uint16_t Addr, uint8_t *Data)
{
    if (Addr >= AT24C64_SIZE || Data == 0)
    {
        return AT24C64_ERR_PARAM;
    }

    if (AT24C64_SetAddr(Addr) != AT24C64_OK)    // 哑写设定地址
    {
        return AT24C64_ERR_NAK;
    }

    IIC_Start();                                // 重复起始，转为读方向
    if (IIC_SendByte(AT24C64_DEV_ADDR | 0x01))
    {
        IIC_Stop();
        return AT24C64_ERR_NAK;
    }
    *Data = IIC_ReadByte(0);                    // 只读一个字节，非应答结束
    IIC_Stop();

    return AT24C64_OK;
}

// 连续写入：每次页写不能跨页（地址在页内回卷），此处自动按页边界切分
uint8_t AT24C64_Write(uint16_t Addr, const uint8_t *Buf, uint16_t Len)
{
    uint16_t Remain, PageRemain, Chunk;

    if (Addr >= AT24C64_SIZE || Len > AT24C64_SIZE - Addr)
    {
        return AT24C64_ERR_PARAM;
    }

    Remain = Len;
    while (Remain > 0)
    {
        // 本段最多写到当前页末尾
        PageRemain = AT24C64_PAGE_SIZE - (Addr % AT24C64_PAGE_SIZE);
        Chunk = (Remain < PageRemain) ? Remain : PageRemain;

        if (AT24C64_SetAddr(Addr) != AT24C64_OK)
        {
            return AT24C64_ERR_NAK;
        }
        for (PageRemain = Chunk; PageRemain > 0; PageRemain--)  // 注意：不能用 while(Chunk--)，
        {                                                       // 后置自减退出时会把 Chunk 下溢成 0xFFFF
            if (IIC_SendByte(*Buf++))
            {
                IIC_Stop();
                return AT24C64_ERR_NAK;
            }
        }
        IIC_Stop();     // 一页写完，停止并等待内部编程

        Addr += Chunk;
        Remain -= Chunk;

        if (AT24C64_WaitReady() != AT24C64_OK)
        {
            return AT24C64_ERR_NAK;
        }
    }
    return AT24C64_OK;
}

// 连续读取：顺序读没有页限制，地址自动跨越整页/整块回卷
uint8_t AT24C64_Read(uint16_t Addr, uint8_t *Buf, uint16_t Len)
{
    uint16_t i;

    if (Addr >= AT24C64_SIZE || Len > AT24C64_SIZE - Addr)
    {
        return AT24C64_ERR_PARAM;
    }
    if (Len == 0)
    {
        return AT24C64_OK;
    }

    if (AT24C64_SetAddr(Addr) != AT24C64_OK)
    {
        return AT24C64_ERR_NAK;
    }

    IIC_Start();
    if (IIC_SendByte(AT24C64_DEV_ADDR | 0x01))
    {
        IIC_Stop();
        return AT24C64_ERR_NAK;
    }

    for (i = 0; i < Len; i++)
    {
        // 前面字节应答继续读，最后一个字节非应答结束
        Buf[i] = IIC_ReadByte((i < Len - 1) ? 1 : 0);
    }
    IIC_Stop();

    return AT24C64_OK;
}
