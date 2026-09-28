#include "int_can.h"
#include "can.h"
#include "string.h"
#include <stdio.h>
void int_can_init(void)
{
    //配置白名单过滤器
CAN_FilterTypeDef sFilterConfig = {0};  // 过滤器配置结构体，全部成员初始化为0
sFilterConfig.FilterBank = 0;           // 使用第0号过滤器组
sFilterConfig.FilterMode = CAN_FILTERMODE_IDMASK; // 掩码模式，支持批量匹配ID
sFilterConfig.FilterScale = CAN_FILTERSCALE_32BIT;// 32位过滤器模式
//填写寄存器 =>接收A程序使用的ID是1
sFilterConfig.FilterIdHigh = 0x0020;    // 过滤ID高16位
sFilterConfig.FilterIdLow = 0x0000;     // 过滤ID低16位
sFilterConfig.FilterMaskIdHigh = 0xffe0;
sFilterConfig.FilterMaskIdLow = 0x0000; 
sFilterConfig.FilterFIFOAssignment = CAN_FILTER_FIFO0; // 收到报文存入FIFO0
sFilterConfig.FilterActivation = ENABLE;// 启用这个过滤器
    //应用过滤器配置
    HAL_CAN_ConfigFilter(&hcan, &sFilterConfig);
    //Bank1：只收 ID 0x102（接收端的 ready 握手帧），掩码沿用 Bank0
    //（不加这个 bank 的话，ready 帧会被硬件过滤器拒收，握手永远等不到）
    //0x2040 = FW_READY_ID(0x102)<<5，ID 定义见 application/APP_update.h 的协议契约
    sFilterConfig.FilterBank = 1;           // 使用第1号过滤器组
    sFilterConfig.FilterIdHigh = 0x102 << 5;  // 0x2040，ID 对齐到 [15:5]
    sFilterConfig.FilterMaskIdHigh = 0xFFE0;// 11 位 ID 逐位比对
    sFilterConfig.FilterMaskIdLow = 0x0006; // IDE、RTR 位也比对（拒远程帧/扩展帧）
    HAL_CAN_ConfigFilter(&hcan, &sFilterConfig);
    //开启CAN接收
    HAL_CAN_Start(&hcan);
}


void Int_CAN_send(uint16_t ID, uint8_t *pData, uint32_t DataLength)
{
    //等待邮箱空闲
    while(HAL_CAN_GetTxMailboxesFreeLevel(&hcan)==0);
    
    //将发送的消息添加到发送邮箱
   CAN_TxHeaderTypeDef TxHeader = {0};
    TxHeader.IDE = CAN_ID_STD;       // 帧类型：标准帧（11位ID）
    TxHeader.RTR = CAN_RTR_DATA;     // 报文类型：数据帧，携带有效数据
    TxHeader.DLC = DataLength;       // 数据长度，范围0~8
    TxHeader.StdId = ID;             // 标准帧ID号(11bit)
    uint32_t mailbox = 0;
    // 往CAN发送邮箱填入报文，硬件自动发送
    // hcan：CAN外设句柄
    // &TxHeader：报文头部信息（ID、帧类型、DLC）
    // pData：待发送数据缓存指针
    // &mailbox：输出参数，保存分配到的发送邮箱号(0/1/2)
   HAL_StatusTypeDef ret = HAL_CAN_AddTxMessage(&hcan, &TxHeader, pData, &mailbox);
   if(ret!=HAL_OK)
   {
       printf("Error: Failed to add message message to CAN mailbox\n");
   }
   
}

void Int_CAN_Rec(CAN_Rec_MSG *pMsg,uint8_t *msg_count)
{
    *msg_count=HAL_CAN_GetRxFifoFillLevel(&hcan,CAN_RX_FIFO0);
    for(uint8_t i=0;i<*msg_count;i++)
    {   //指向当前使用的缓存
        CAN_Rec_MSG *msg = &pMsg[i];
        //清空对应消息缓存
        memset(&pMsg[i],0,sizeof(CAN_Rec_MSG));
        HAL_CAN_GetRxMessage(&hcan,CAN_RX_FIFO0,&msg->RxHeader,msg->Data);
    }
}

