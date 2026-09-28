#ifndef __USART_H
#define __USART_H

#include "stm32f10x.h"
#include <stdio.h>

// USART1：PA9=TX / PA10=RX，9600 8N1
// printf 发送（轮询，重写 fputc），CAN 测试信息从串口打印

void USART1_Init(void);
void USART1_rec_init(void);                        //使能 RXNE 中断与 NVIC，开始接收
uint16_t USART1_rec_read(uint8_t *buf, uint16_t max_len);  //从环形缓冲取走数据
void USART1_RX_ISR(void);                          //it.c 的 USART1_IRQHandler 调用

#endif /* __USART_H */
