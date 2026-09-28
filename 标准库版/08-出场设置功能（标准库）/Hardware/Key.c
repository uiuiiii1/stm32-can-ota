#include "stm32f10x.h"                  // Device header
#include "Delay.h"
void Key_Init()
{
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB,ENABLE);
	GPIO_InitTypeDef a;
	a.GPIO_Mode=GPIO_Mode_IPU;
	a.GPIO_Pin=GPIO_Pin_11|GPIO_Pin_1;
	a.GPIO_Speed=GPIO_Speed_50MHz;
	GPIO_Init(GPIOB,&a);
}
uint8_t Get_Num()
{
	uint8_t KeyNum=0;
	if(GPIO_ReadInputDataBit(GPIOB,GPIO_Pin_1)==0)
	{
		Delay_ms(15);
		while(GPIO_ReadInputDataBit(GPIOB,GPIO_Pin_1)==0){}
		Delay_ms(15);
        KeyNum=1;
	}
	if(GPIO_ReadInputDataBit(GPIOB,GPIO_Pin_11)==0)
	{
		Delay_ms(15);
		while(GPIO_ReadInputDataBit(GPIOB,GPIO_Pin_11)==0){}
		Delay_ms(15);
        KeyNum=2;
	}
	return KeyNum;
}