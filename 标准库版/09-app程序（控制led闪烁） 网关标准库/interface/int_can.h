#ifndef __INT_CAN_H__
#define __INT_CAN_H__

#include "stm32f10x.h"

typedef struct Int_can
{
    CanRxMsg RxHeader;      //报文头：StdId/IDE/RTR/DLC（沿用 HAL 版字段名，main.c 打印 RxHeader.StdId）
    uint8_t Data[8];

}CAN_Rec_MSG;
//固件传输协议帧 ID（与网关侧 APP_update.h 契约一致；过滤器按这三个 ID 精确放行）
#define FW_CTRL_ID   0x100   //BEGIN/END 控制帧
#define FW_DATA_ID   0x101   //固件数据帧
#define FW_READY_ID  0x102   //接收端就绪握手帧（擦完 W25Q64 暂存区后回给网关）
//配置白名单过滤器（精确收 FW_CTRL_ID/FW_DATA_ID），开启CAN（正常模式）
void int_can_init(void);
//一般还需要 1 个参数：报文类型（标准帧 / 扩展帧 + 数据帧 / 远程帧），这里默认标准帧数据格式所以不加
//发送CAN数据
void Int_CAN_send(uint16_t ID, uint8_t *pData, uint32_t DataLength);
//接收CAN数据
void Int_CAN_Rec(CAN_Rec_MSG *pMsg,uint8_t *msg_count);
#endif /* __INT_CAN_H__ */
