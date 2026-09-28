#include "int_can.h"
#include "string.h"
#include <stdio.h>

//CAN1 挂 APB1(36MHz)：分频 36 → 1us/tq，位时间 1+2+2=5tq → 200kbit/s（与 HAL 版配置一致）
void int_can_init(void)
{
    GPIO_InitTypeDef      gpio   = {0};
    CAN_InitTypeDef       can    = {0};
    CAN_FilterInitTypeDef filter = {0};

    RCC_APB1PeriphClockCmd(RCC_APB1Periph_CAN1, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_AFIO, ENABLE);

    //PA11=CAN_RX 浮空输入；PA12=CAN_TX 复用推挽（与 HAL 版 MspInit 一致）
    gpio.GPIO_Pin   = GPIO_Pin_11;
    gpio.GPIO_Mode  = GPIO_Mode_IN_FLOATING;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &gpio);

    gpio.GPIO_Pin   = GPIO_Pin_12;
    gpio.GPIO_Mode  = GPIO_Mode_AF_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &gpio);

    //正常模式：双板真实总线（原模板为静默环回，与 HAL 版 09-26 的修改对齐）
    //（标准库 NART=ENABLE 等价 HAL 的 AutoRetransmission=DISABLE）
    can.CAN_TTCM = DISABLE;                 //TimeTriggeredMode
    can.CAN_ABOM = ENABLE;                  //AutoBusOff
    can.CAN_AWUM = ENABLE;                  //AutoWakeUp
    can.CAN_NART = ENABLE;                  //禁止自动重传
    can.CAN_RFLM = DISABLE;                 //ReceiveFifoLocked
    can.CAN_TXFP = DISABLE;                 //TransmitFifoPriority
    can.CAN_Mode = CAN_Mode_Normal;   //双板真实总线（此前只改了注释漏改了这行，已修正）
    can.CAN_SJW  = CAN_SJW_1tq;             //SyncJumpWidth
    can.CAN_BS1  = CAN_BS1_2tq;             //TimeSeg1
    can.CAN_BS2  = CAN_BS2_2tq;             //TimeSeg2
    can.CAN_Prescaler = 36;
    CAN_Init(CAN1, &can);                   //CAN_Init 内部退出初始化模式，等价 HAL_CAN_Start

    //配置白名单过滤器：只放行固件协议的两个 ID（FW_CTRL_ID=0x100 控制帧 / FW_DATA_ID=0x101 数据帧）
    //标准帧 11 位 ID 映射在 FilterIdHigh 的 [15:5]（即 ID<<5）：精确匹配 = IdHigh 写 ID<<5、MaskHigh 写 0xFFE0
    filter.CAN_FilterNumber         = 0;    //Bank0：只收 ID 0x100（控制帧 BEGIN/END）
    filter.CAN_FilterMode           = CAN_FilterMode_IdMask;      //掩码模式
    filter.CAN_FilterScale          = CAN_FilterScale_32bit;      //32位过滤器模式
    filter.CAN_FilterIdHigh         = FW_CTRL_ID << 5;   //0x100<<5 = 0x2000，ID 对齐到 [15:5]
    filter.CAN_FilterIdLow          = 0x0000;     //IDE=0(标准帧)、RTR=0(数据帧)
    filter.CAN_FilterMaskIdHigh     = 0xFFE0;     //11 位 ID 逐位比对
    filter.CAN_FilterMaskIdLow      = 0x0006;     //IDE、RTR 位也比对（拒远程帧/扩展帧）
    filter.CAN_FilterFIFOAssignment = CAN_Filter_FIFO0;  //收到报文存入FIFO0
    filter.CAN_FilterActivation     = ENABLE;     //启用这个过滤器
    CAN_FilterInit(&filter);

    filter.CAN_FilterNumber         = 1;    //Bank1：只收 ID 0x101（固件数据帧），其余配置沿用上面
    filter.CAN_FilterIdHigh         = FW_DATA_ID << 5;   //0x101<<5 = 0x2020
    CAN_FilterInit(&filter);
}


void Int_CAN_send(uint16_t ID, uint8_t *pData, uint32_t DataLength)
{
    CanTxMsg tx;

    //等待邮箱空闲：TSR 的 TME0/1/2 任一置位表示有空邮箱（等价 HAL_CAN_GetTxMailboxesFreeLevel）
    while ((CAN1->TSR & (CAN_TSR_TME0 | CAN_TSR_TME1 | CAN_TSR_TME2)) == 0) { ; }

    //填发送报文头
    tx.StdId = ID;                 //标准帧ID号(11bit)
    tx.ExtId = 0;
    tx.IDE   = CAN_ID_STD;         //帧类型：标准帧（11位ID）
    tx.RTR   = CAN_RTR_DATA;       //报文类型：数据帧，携带有效数据
    tx.DLC   = DataLength;         //数据长度，范围0~8
    memcpy(tx.Data, pData, DataLength);

    //往CAN发送邮箱填入报文，硬件自动发送；返回分配到的发送邮箱号(0/1/2)
    if (CAN_Transmit(CAN1, &tx) == CAN_TxStatus_NoMailBox)
    {
        printf("Error: Failed to add message message to CAN mailbox\n");
    }
}

void Int_CAN_Rec(CAN_Rec_MSG *pMsg,uint8_t *msg_count)
{
    //FIFO0 里待读的报文数（等价 HAL_CAN_GetRxFifoFillLevel）
    *msg_count = (uint8_t)CAN_MessagePending(CAN1, CAN_FIFO0);
    for(uint8_t i=0;i<*msg_count;i++)
    {
        CanRxMsg rx;
        //清空对应消息缓存
        memset(&pMsg[i],0,sizeof(CAN_Rec_MSG));
        //从FIFO0取出一个报文
        CAN_Receive(CAN1, CAN_FIFO0, &rx);
        //报文头/数据搬进调用方的缓存
        pMsg[i].RxHeader.StdId = rx.StdId;
        pMsg[i].RxHeader.ExtId = rx.ExtId;
        pMsg[i].RxHeader.IDE   = rx.IDE;
        pMsg[i].RxHeader.RTR   = rx.RTR;
        pMsg[i].RxHeader.DLC   = rx.DLC;
        pMsg[i].RxHeader.FMI   = rx.FMI;
        memcpy(pMsg[i].Data, rx.Data, 8);
    }
}
