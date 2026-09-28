#ifndef __APP_UPDATE_H
#define __APP_UPDATE_H

//==========================================================================
// 网关发送端协议契约（标准库版；与接收端 int_can.h/APP_firmware_recv.h 保持一致）
//
// CAN 分帧：BEGIN(ID 0x100) → DATA(ID 0x101, 序号2B+6B数据) → END(ID 0x100, 累加和)
// 握手：网关发 BEGIN 后等接收端擦完 W25Q64 回 ready(ID 0x102)，5s 超时放弃
// 触发：接收板按键单击发 ID 0x1 "update"(6B)，网关收到即走一轮完整分发
//==========================================================================

#define FW_CTRL_ID   0x100    //BEGIN/END 控制帧
#define FW_DATA_ID   0x101    //固件数据帧
#define FW_READY_ID  0x102    //接收端就绪握手帧

void APP_update_init(void);   //状态机复位，上电调用一次
void APP_update_poll(void);   //主循环 10ms 调一次

#endif /* __APP_UPDATE_H */
