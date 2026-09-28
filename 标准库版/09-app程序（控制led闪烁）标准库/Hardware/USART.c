#include "USART.h"

// Microlib 下，重写 fputc 即可让 printf 走 USART1（与 HAL 版 fputc 重定向一致）
int fputc(int ch, FILE *f)
{
    (void)f;
    while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET) { ; }
    USART_SendData(USART1, (uint16_t)ch);
    return ch;
}

void USART1_Init(void)
{
    GPIO_InitTypeDef  gpio = {0};
    USART_InitTypeDef uart = {0};

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_USART1 | RCC_APB2Periph_AFIO, ENABLE);

    // PA9(TX)：复用推挽；PA10(RX)：浮空输入（与 HAL 版 MspInit 一致）
    gpio.GPIO_Pin   = GPIO_Pin_9;
    gpio.GPIO_Mode  = GPIO_Mode_AF_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &gpio);

    gpio.GPIO_Pin   = GPIO_Pin_10;
    gpio.GPIO_Mode  = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOA, &gpio);

    uart.USART_BaudRate            = 9600;   //与 HAL 版一致
    uart.USART_WordLength          = USART_WordLength_8b;
    uart.USART_StopBits            = USART_StopBits_1;
    uart.USART_Parity              = USART_Parity_No;
    uart.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    uart.USART_Mode                = USART_Mode_Tx | USART_Mode_Rx;
    USART_Init(USART1, &uart);
    USART_Cmd(USART1, ENABLE);
}
