#ifndef __KEY_H
#define __KEY_H

#include "stm32f10x.h"

// 按键引脚：PB12 内部上拉，外部按键接地
#define KEY_PORT              GPIOB
#define KEY_PIN               GPIO_Pin_12

// 事件标志位
#define KEY_HOLD              0x01
#define KEY_DOWN              0x02
#define KEY_UP                0x04
#define KEY_SINGLE            0x08
#define KEY_DOUBLE            0x10
#define KEY_LONG              0x20
#define KEY_REPEAT            0x40

extern uint8_t Key_Flag;
extern volatile uint8_t Key_Event_Flag;

void     Key_Init(void);
uint8_t  Key_Check(uint8_t Flag);
void     Key_Clear(void);
void     Key_Tick(void);

#endif /* __KEY_H */
