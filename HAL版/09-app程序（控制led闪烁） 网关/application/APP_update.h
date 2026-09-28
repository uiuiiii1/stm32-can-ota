#ifndef __INT_UPDATE_H__
#define __INT_UPDATE_H__

//==========================================================================
// 固件 CAN 传输协议（网关发送端定义，接收端复制一份保持一致）
//
// 一次完整传输 = 1 帧 BEGIN + N 帧 DATA + 1 帧 END，全部是标准数据帧：
//   BEGIN  ID=FW_CTRL_ID  DLC=8  内容: 'B''E''G''I''N' + 固件总长高/低字节 + 0xFF
//   DATA   ID=FW_DATA_ID  DLC=8  内容: 帧序号高/低字节(0 起，逐帧 +1) + 6 字节固件数据
//                                      （末帧不足 6 字节的部分补 0xFF，接收端按总长截取）
//   END    ID=FW_CTRL_ID  DLC=8  内容: 'E''N''D' + 0xFF + 固件全文字节累加和(4 字节)
//
// 注意事项：
//   1. 所有多字节数值一律大端——与 W25 元数据、07 接收端的全局约定一致；
//   2. 接收端校验两道关：帧序号必须连续无缺口，END 里的累加和必须等于
//      自己收到的全部固件字节的和（和"串口入库"用的是同一套求和约定）；
//   3. 总帧数 = (总长 + 5) / 6，接收端据此判断是否收全。
//==========================================================================
#define FW_CTRL_ID   0x100    //固件传输控制帧（BEGIN/END）
#define FW_DATA_ID   0x101    //固件传输数据帧
#define FW_READY_ID  0x102    //接收端就绪握手帧（擦完 W25Q64 暂存区后回给网关）

typedef enum
{
    APP_UPDATE_WAIT_CMD = 0x00,//等待更新指令
    APP_UPDATE_SEND_APP_CMD ,//发送应用程序指令
} APP_UPDATE_STATE;
// 更新初始化
void APP_update_init(void);
// 等待更新指令
void APP_update_wait(void);
// 发送应用程序
void APP_update_send_app(void);
// main循环程序
void APP_update_work_cmd(void);
#endif /* __INT_UPDATE_H__ */
