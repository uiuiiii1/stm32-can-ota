#include "stm32f10x.h"                  // Device header

void LED_Init()
{
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA,ENABLE);
	GPIO_InitTypeDef a;
	a.GPIO_Mode=GPIO_Mode_Out_PP;
	a.GPIO_Pin=GPIO_Pin_0|GPIO_Pin_1|GPIO_Pin_2;   //PA0/PA1/PA2：HAL 版流水灯用的三个引脚
	a.GPIO_Speed=GPIO_Speed_50MHz;
	GPIO_Init(GPIOA,&a);
	GPIO_SetBits(GPIOA,GPIO_Pin_0|GPIO_Pin_1|GPIO_Pin_2);
}
void LED1_ON()
{
	GPIO_ResetBits(GPIOA,GPIO_Pin_1);
}
void LED1_OFF()
{
	GPIO_SetBits(GPIOA,GPIO_Pin_1);
}
void LED1_Turn()
{
	if(GPIO_ReadInputDataBit(GPIOA,GPIO_Pin_1)==0)
		GPIO_SetBits(GPIOA,GPIO_Pin_1);
	else GPIO_ResetBits(GPIOA,GPIO_Pin_1);
}
void LED2_ON()
{
	GPIO_ResetBits(GPIOA,GPIO_Pin_2);
}
void LED2_OFF()
{
	GPIO_SetBits(GPIOA,GPIO_Pin_2);
}
void LED2_Turn()
{
	if(GPIO_ReadInputDataBit(GPIOA,GPIO_Pin_2)==0)
		GPIO_SetBits(GPIOA,GPIO_Pin_2);
	else GPIO_ResetBits(GPIOA,GPIO_Pin_2);
}