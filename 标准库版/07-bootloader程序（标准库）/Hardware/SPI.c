#include "SPI.h"

void SPI1_Init(void)
{
    GPIO_InitTypeDef  gpio = {0};
    SPI_InitTypeDef   spi  = {0};

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_SPI1 | RCC_APB2Periph_AFIO, ENABLE);

    // PA5(SCK)、PA7(MOSI)：复用推挽；PA6(MISO)：浮空输入
    gpio.GPIO_Pin   = GPIO_Pin_5 | GPIO_Pin_7;
    gpio.GPIO_Mode  = GPIO_Mode_AF_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &gpio);

    gpio.GPIO_Pin   = GPIO_Pin_6;
    gpio.GPIO_Mode  = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOA, &gpio);

    spi.SPI_Direction         = SPI_Direction_2Lines_FullDuplex;
    spi.SPI_Mode              = SPI_Mode_Master;
    spi.SPI_DataSize          = SPI_DataSize_8b;
    spi.SPI_CPOL              = SPI_CPOL_Low;        // 模式0：CPOL=0
    spi.SPI_CPHA              = SPI_CPHA_1Edge;      //       CPHA=0
    spi.SPI_NSS               = SPI_NSS_Soft;
    spi.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_8; // 72MHz/8=9MHz
    spi.SPI_FirstBit          = SPI_FirstBit_MSB;
    spi.SPI_CRCPolynomial     = 7;
    SPI_Init(SPI1, &spi);
    SPI_Cmd(SPI1, ENABLE);

    // 上电后硬件可能残留一个 RXNE 假读，读一次清掉，免得首次收发取错数据
    (void)SPI_I2S_ReceiveData(SPI1);
}

// 全双工收发一个字节。标准库 SPI 主机写一个字节必须同时读一个字节，
// 所以"只写"调它忽略返回，"只读"调它传 0xFF 取返回。
uint8_t SPI1_RW(uint8_t tx)
{
    // 等发送区空，再写
    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_TXE) == RESET) { ; }
    SPI_I2S_SendData(SPI1, tx);
    // 等接收区有数据
    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_RXNE) == RESET) { ; }
    return (uint8_t)SPI_I2S_ReceiveData(SPI1);
}
