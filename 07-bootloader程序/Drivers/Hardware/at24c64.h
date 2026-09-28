#ifndef __AT24C64_H
#define __AT24C64_H

#include <stdint.h>

// AT24C64 引脚：PB10=SCL，PB11=SDA（开漏输出+内部上拉，由 MX_GPIO_Init() 初始化）
#define AT24C64_SCL_PORT      GPIOB
#define AT24C64_SCL_PIN       GPIO_PIN_10
#define AT24C64_SDA_PORT      GPIOB
#define AT24C64_SDA_PIN       GPIO_PIN_11

// 器件参数
#define AT24C64_DEV_ADDR      0xA0    // 器件地址：A2/A1/A2 全部接地
#define AT24C64_SIZE          8192    // 总容量：8192 字节（64Kbit）
#define AT24C64_PAGE_SIZE     32      // 页大小：32 字节（页内写入地址会回卷，跨页须分批）
#define AT24C64_TWC_MS        5       // 写周期：5ms（写入后需等器件内部编程完成）
#define AT24C64_I2C_FREQ_KHZ  100     // 软件 I2C 目标速率

// 返回值定义
#define AT24C64_OK            0       // 操作成功
#define AT24C64_ERR_NAK       1       // 器件无应答（未接/写周期未结束/地址错误）
#define AT24C64_ERR_PARAM     2       // 地址或长度越界

uint8_t AT24C64_Init(void);                         // 初始化并探测器件是否在线
uint8_t AT24C64_WriteByte(uint16_t Addr, uint8_t Data);                   // 单字节写入
uint8_t AT24C64_ReadByte(uint16_t Addr, uint8_t *Data);                   // 单字节读取
uint8_t AT24C64_Write(uint16_t Addr, const uint8_t *Buf, uint16_t Len);   // 连续写入（自动处理跨页，阻塞至写完成）
uint8_t AT24C64_Read(uint16_t Addr, uint8_t *Buf, uint16_t Len);          // 连续读取（顺序读，无长度限制）
uint8_t AT24C64_WaitReady(void);                    // 应答轮询，等待器件内部写周期结束

#endif /* __AT24C64_H */
