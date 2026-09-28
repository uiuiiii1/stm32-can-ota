#ifndef __INT_UART_H__
#define __INT_UART_H__

#include "main.h"

//串口中断接收（USART1，9600 8N1）
//中断里把字节塞进 1KB 环形缓冲，主循环随时来取——
//必须用中断收的原因：主循环一轮 10ms，9600 波特率约 10ms 来 1 字节，
//靠轮询收必然丢字节（USART 只有 1 字节缓冲，没有 FIFO）

//初始化环形缓冲并挂上第一个字节的中断接收，上电调用一次
void Int_UART_rec_init(void);

//从环形缓冲取走最多 max_len 字节，返回实际取到的字节数（0=暂无数据）
uint16_t Int_UART_rec_read(uint8_t *buf, uint16_t max_len);

#endif /* __INT_UART_H__ */
