#include "stm32f10x.h"                  // Device header
#include "LED.h"
#include "USART.h"
#include "Tick.h"
#include "int_can.h"
#include "W25Q80.h"
#include "APP_update_store.h"
#include "APP_update.h"
#include <stdio.h>

int main(void)
{
	LED_Init();      // PA0/PA1/PA2
	USART1_Init();   // PA9/PA10 9600 8N1：printf 日志 + bin 入库数据通道
	Tick_Init();     // TIM4 1ms 时基：GetTick()/超时判定/帧间隔的时基

	//初始化CAN
	int_can_init();

	//W25Q80 上电自检：固件仓库芯片，接收板的固件从这里分发
	{
		uint32_t w25_id = 0;
		if (W25Q80_Init() == W25Q80_OK)
		{
			W25Q80_ReadID(&w25_id);
			printf("W25Q80 ok, JEDEC ID: 0x%06X\r\n", (unsigned)w25_id);
		}
		else
		{
			printf("W25Q80 init failed! check wiring/CS\r\n");
		}
	}
	APP_update_store_init();
	APP_update_init();
	printf("gateway start\r\n");

	while (1)
	{
		APP_update_store_poll();   //串口入库：PC 发 start:<size> + bin 裸流
		APP_update_poll();         //接收板 update 触发 → CAN 分帧分发（BEGIN→ready→DATA→END）
		Tick_DelayMs(10);          //主循环节拍 10ms
	}
}
