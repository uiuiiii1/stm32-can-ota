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

//==== 串口接收：1KB 环形缓冲 + RXNE 中断（bin 入库的数据通道）====
//入库时 PC 以 960B/s 灌数据而主循环 10ms 才轮询一次，靠中断把字节先收进缓冲
#define USART_RB_SIZE 1024u   //必须是 2 的幂（用掩码代替取模）

static uint8_t  s_rb[USART_RB_SIZE];
static volatile uint16_t s_head = 0;   //写入位置（中断里推进）
static volatile uint16_t s_tail = 0;   //读走位置（主循环推进）

//使能 RXNE 中断与 NVIC，开始接收（上电调用一次）
void USART1_rec_init(void)
{
    USART_ITConfig(USART1, USART_IT_RXNE, ENABLE);

    NVIC_InitTypeDef nvic = {0};
    nvic.NVIC_IRQChannel                   = USART1_IRQn;
    nvic.NVIC_IRQChannelPreemptionPriority = 3;
    nvic.NVIC_IRQChannelSubPriority        = 0;
    nvic.NVIC_IRQChannelCmd                = ENABLE;
    NVIC_Init(&nvic);
}

//it.c 的 USART1_IRQHandler 调用：收到的字节塞进环形缓冲
void USART1_RX_ISR(void)
{
    if (USART_GetITStatus(USART1, USART_IT_RXNE) != RESET)
    {
        uint8_t b = (uint8_t)USART_ReceiveData(USART1);   //读 DR 清 RXNE
        uint16_t next = (uint16_t)((s_head + 1) & (USART_RB_SIZE - 1));
        if (next != s_tail)          //满则丢弃新字节（10ms 排水一轮到不了满）
        {
            s_rb[s_head] = b;
            s_head = next;
        }
    }
    if (USART_GetFlagStatus(USART1, USART_FLAG_ORE) != RESET)
    {   //溢出：读 DR 清标志，避免中断反复触发
        (void)USART_ReceiveData(USART1);
    }
}

//从环形缓冲取走最多 max_len 字节，返回实际取到的字节数（0=暂无数据）
uint16_t USART1_rec_read(uint8_t *buf, uint16_t max_len)
{
    uint16_t n = 0;
    while (n < max_len)
    {
        uint16_t head = s_head;   //volatile 变量先读一次再比
        if (s_tail == head)
        {
            break;
        }
        buf[n++] = s_rb[s_tail];
        s_tail = (uint16_t)((s_tail + 1) & (USART_RB_SIZE - 1));
    }
    return n;
}
