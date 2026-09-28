#include "stm32f10x.h"                  // Device header
#include "Delay.h"
#include "LED.h"
#include "USART.h"
#include "Key.h"
#include "Tick.h"
#include "int_can.h"
#include "W25Q64.h"
#include "APP_firmware_recv.h"
#include <stdio.h>

#define APP_FLASH_BASE_ADDR   0x08008000U  //程序起始位置

int main(void)
{
	//重定向中断向量表（本程序链接在 0x08008000 APP 区，由 07 bootloader 跳转进来）
	SCB->VTOR = APP_FLASH_BASE_ADDR;
	//开启中断（07 跳转前全局中断被关，这里必须重新打开，否则 TIM4 等中断全失效）
	__enable_irq();

	LED_Init();      // PA0/PA1/PA2
	USART1_Init();   // PA9/PA10 9600 8N1，printf 发送
	Tick_Init();     // TIM4 1ms 时基：GetTick()/超时判定的时基
	Key_Init();      // PB12 按键（07 标准库驱动，内部自配 GPIO）

	//初始化CAN
	int_can_init();

	//W25Q64 上电自检：暂存芯片，07 将从这里搬运新固件到内部 flash
	{
		uint32_t w25_id = 0;
		if (W25Q64_Init() == W25Q64_OK)
		{
			W25Q64_ReadID(&w25_id);
			printf("W25Q64 ok, JEDEC ID: 0x%06X\r\n", (unsigned)w25_id);
		}
		else
		{
			printf("W25Q64 init failed! check wiring/CS\r\n");
		}
	}
	APP_firmware_recv_init();
	printf("app start\r\n");

	//主循环节拍 10ms：按键扫描 + 接收状态机 + 心跳
	uint32_t heartbeat_tick = GetTick();
	while (1)
	{
		Key_Tick();   //按键扫描（内部用 GetTick 计时，单击=按下+松开+200ms 无二击）
		if (Key_Check(KEY_SINGLE))   //单击事件：一次点击发一帧 update
		{
			if (APP_firmware_recv_busy())
			{
				//正在收包，忽略本次点击（此处也不打印——打印 20ms 会挤爆 FIFO 丢帧）
			}
			else if (APP_firmware_recv_stored())
			{
				printf("key: already updated, reset to run it\r\n");
			}
			else
			{
				Int_CAN_send(0x1,(uint8_t*)"update",6);
				printf("key: update sent\r\n");
			}
		}

		//固件接收状态机：BEGIN→擦 W25Q64 暂存区→回 ready→流式收帧写 W25Q64→END 校验
		APP_firmware_recv_poll();

		if (GetTick() - heartbeat_tick >= 500)
		{
			heartbeat_tick = GetTick();
			//PA1 心跳翻转（板载 LED 低电平点亮）
			GPIO_WriteBit(GPIOA, GPIO_Pin_1,
				(GPIO_ReadOutputDataBit(GPIOA, GPIO_Pin_1) == Bit_RESET) ? Bit_SET : Bit_RESET);
		}

		Tick_DelayMs(10);
	}
}
