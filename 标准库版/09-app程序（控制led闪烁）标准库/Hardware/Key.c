#include "Key.h"
#include "Tick.h"

// 按键状态定义
#define KEY_PRESSED           1
#define KEY_UNPRESSED         0

// 时间参数（ms，基于 GetTick 的 1ms 时基）
#define KEY_TIME_DEBOUNCE     20
#define KEY_TIME_DOUBLE        200
#define KEY_TIME_LONG          2000
#define KEY_TIME_REPEAT        100

// 状态机状态
#define KEY_ST_WAIT            0
#define KEY_ST_WAIT_LONG       1
#define KEY_ST_WAIT_DBL        2
#define KEY_ST_DBL_DONE        3
#define KEY_ST_LONG_REPEAT     4

uint8_t Key_Flag;

// 长按"强制结束"请求标志：TIM4 中断里置 1，主循环消费后清零
volatile uint8_t Key_Event_Flag;

static uint8_t Key_GetState(void)
{
    return (GPIO_ReadInputDataBit(KEY_PORT, KEY_PIN) == Bit_RESET) ? KEY_PRESSED : KEY_UNPRESSED;
}

void Key_Init(void)
{
    GPIO_InitTypeDef gpio = {0};
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);

    gpio.GPIO_Pin   = KEY_PIN;
    gpio.GPIO_Mode  = GPIO_Mode_IPU;     // 内部上拉输入
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(KEY_PORT, &gpio);

    Key_Flag = 0;
}

uint8_t Key_Check(uint8_t Flag)
{
    if (Key_Flag & Flag)
    {
        if (Key_Flag != KEY_HOLD)
        {
            Key_Flag &= ~Flag;
        }
        return 1;
    }
    return 0;
}

void Key_Clear(void)
{
    Key_Flag = 0;
}

void Key_Tick(void)
{
    static uint8_t  currState = KEY_UNPRESSED;
    static uint8_t  prevState = KEY_UNPRESSED;
    static uint8_t  s = KEY_ST_WAIT;
    static uint32_t stable = 0;       // 当前电平已稳定持续的起点
    static uint32_t waitStart = 0;    // 当前事件窗口起点

    uint32_t now = GetTick();
    uint8_t  raw = Key_GetState();

    // 消抖
    if (raw == currState)
    {
        stable = now;
    }
    else if ((now - stable) >= KEY_TIME_DEBOUNCE)
    {
        prevState = currState;
        currState = raw;
        stable = now;
    }

    if (currState == KEY_PRESSED)   Key_Flag |=  KEY_HOLD;
    else                            Key_Flag &= ~KEY_HOLD;

    if (currState == KEY_PRESSED && prevState == KEY_UNPRESSED) Key_Flag = KEY_DOWN;
    if (currState == KEY_UNPRESSED && prevState == KEY_PRESSED) Key_Flag = KEY_UP;

    switch (s)
    {
    case KEY_ST_WAIT:
        if (currState == KEY_PRESSED) { s = KEY_ST_WAIT_LONG; waitStart = now; }
        break;

    case KEY_ST_WAIT_LONG:
        if (currState == KEY_UNPRESSED)
        {
            s = KEY_ST_WAIT_DBL; waitStart = now;
        }
        else if ((now - waitStart) >= KEY_TIME_LONG)
        {
            Key_Flag |= KEY_LONG; s = KEY_ST_LONG_REPEAT; waitStart = now;
        }
        break;

    case KEY_ST_WAIT_DBL:
        if (currState == KEY_PRESSED)
        {
            Key_Flag |= KEY_DOUBLE; s = KEY_ST_DBL_DONE;
        }
        else if ((now - waitStart) >= KEY_TIME_DOUBLE)
        {
            Key_Flag |= KEY_SINGLE; s = KEY_ST_WAIT;
        }
        break;

    case KEY_ST_DBL_DONE:
        if (currState == KEY_UNPRESSED) s = KEY_ST_WAIT;
        break;

    case KEY_ST_LONG_REPEAT:
        if (currState == KEY_UNPRESSED)
        {
            s = KEY_ST_WAIT;
        }
        else if ((now - waitStart) >= KEY_TIME_REPEAT)
        {
            Key_Flag |= KEY_REPEAT; waitStart = now;
        }
        break;

    default:
        s = KEY_ST_WAIT;
        break;
    }
}
