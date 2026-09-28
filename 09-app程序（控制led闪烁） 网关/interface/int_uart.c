#include "int_uart.h"
#include "usart.h"    // huart1

//环形缓冲大小：必须是2的幂（用位与掩码代替取模，效率更高）
//1KB裕量：擦4KB扇区最长约400ms，9600波特率期间涌进约384字节，主循环10ms排水一轮，1KB足够
#define UART_RB_SIZE 1024U

static uint8_t s_rb[UART_RB_SIZE];           //环形接收缓冲区
static volatile uint16_t s_rb_head = 0;      //写入位置（中断里推进，volatile防止编译器优化）
static volatile uint16_t s_rb_tail = 0;      //读走位置（主循环推进）
static uint8_t s_byte;                       //HAL单字节中断接收的临时落点

//HAL收完1字节触发回调：把字节塞进环形缓冲，再挂下一个字节的接收
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART1)
  {
    //计算下一个写入位置，用掩码实现环形回绕
    uint16_t next = (uint16_t)((s_rb_head + 1) & (UART_RB_SIZE - 1));
    if (next != s_rb_tail)            //缓冲未满才写入，满则丢弃新字节
    {
      s_rb[s_rb_head] = s_byte;       //存入环形缓冲
      s_rb_head = next;               //推进写指针
    }
    //重新开启下一字节中断接收，形成连续接收
    HAL_UART_Receive_IT(&huart1, &s_byte, 1);
  }
}

//接收出错回调（如ORE溢出）：HAL出错后会自动停掉接收，这里重新挂上，
//否则一次错误之后串口就永久停止接收
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART1)
  {
    HAL_UART_Receive_IT(&huart1, &s_byte, 1);
  }
}

//串口接收初始化：清空环形缓冲指针，开启第一个字节的中断接收
void Int_UART_rec_init(void)
{
  s_rb_head = 0;
  s_rb_tail = 0;
  HAL_UART_Receive_IT(&huart1, &s_byte, 1);
}

//从环形缓冲读取数据到buf，最多读max_len字节，返回实际读到的字节数
uint16_t Int_UART_rec_read(uint8_t *buf, uint16_t max_len)
{
  uint16_t n = 0;
  while (n < max_len)
  {
    uint16_t head = s_rb_head;       //volatile变量先快照一次，避免中断中途修改导致判断异常
    if (s_rb_tail == head)
    {
      break;                         //读写指针相等，缓冲空了，退出
    }
    buf[n++] = s_rb[s_rb_tail];      //读出一个字节
    //推进读指针，掩码实现环形回绕
    s_rb_tail = (uint16_t)((s_rb_tail + 1) & (UART_RB_SIZE - 1));
  }
  return n;
}
