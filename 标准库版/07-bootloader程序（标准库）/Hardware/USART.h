#ifndef __USART_H
#define __USART_H

#include "stm32f10x.h"
#include <stdio.h>

// USART1：PA9=TX / PA10=RX，9600 8N1，仅做 printf 发送（polled）
// 串口 DMA 接收下载路径本版不实现（与 HAL 版一致）

void USART1_Init(void);

#endif /* __USART_H */
