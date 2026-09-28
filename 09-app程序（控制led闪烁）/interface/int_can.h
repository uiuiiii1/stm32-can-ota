#ifndef __INT_CAN_H__
#define __INT_CAN_H__

#include "main.h"

//==========================================================================
// 固件传输协议帧 ID（必须与网关 application/APP_update.h 的契约保持一致）
// BEGIN/END 控制帧走 FW_CTRL_ID，固件数据帧走 FW_DATA_ID，int_can_init 的
// 过滤器按这两个 ID 精确放行
//==========================================================================
#define FW_CTRL_ID  0x100   //固件传输控制帧（BEGIN/END）
#define FW_DATA_ID  0x101   //固件传输数据帧
#define FW_READY_ID 0x102   //接收端就绪握手帧（擦完暂存区后回给网关）

// CAN接收报文结构体：帧头 + 8字节数据
typedef struct Int_can
{
    CAN_RxHeaderTypeDef RxHeader;  // 帧头：含ID、帧类型、数据长度(DLC)等
    uint8_t Data[8];               // 接收数据，最多8字节，有效长度看RxHeader.DLC
} CAN_Rec_MSG;

// CAN初始化：配置过滤器(接收所有报文) + 启动CAN外设，main里调用一次
void int_can_init(void);

// CAN发送（固定标准帧+数据帧）
// ID：标准帧ID，范围0~0x7FF，越小优先级越高
// pData：待发数据数组指针
// DataLength：数据长度，0~8
void Int_CAN_send(uint16_t ID, uint8_t *pData, uint32_t DataLength);

// CAN接收，从FIFO0读出所有缓存报文
// pMsg：报文结构体数组首地址（输出，调用方提前定义）
// msg_count：读到的报文数量（输出，必须传&地址，0表示无报文）
void Int_CAN_Rec(CAN_Rec_MSG *pMsg, uint8_t *msg_count);

#endif /* __INT_CAN_H__ */
