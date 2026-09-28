#ifndef __USART_H
#define __USART_H

#include "stm32f10x.h"
#include <stdio.h>

// USART1：PA9=TX / PA10=RX，9600 8N1
// printf 发送（轮询，重写 fputc），CAN 测试信息从串口打印

void USART1_Init(void);

#endif /* __USART_H */
