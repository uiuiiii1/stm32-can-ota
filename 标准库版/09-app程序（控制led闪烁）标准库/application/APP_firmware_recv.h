#ifndef __APP_FIRMWARE_RECV_H
#define __APP_FIRMWARE_RECV_H

#include <stdint.h>

//==========================================================================
// 固件接收模块（网关 CAN 分帧发送的对端）——标准库版
//
// 流程：BEGIN → 擦本板 W25Q64 暂存区（元数据扇区 0 + 镜像区）→ 回 ready(ID 0x102)
//       → 流式收 DATA 写 W25Q64（页缓冲攒满 256B 编程一次，跨页帧尾部搬移）
//       → END 读回校验 → 通过后往扇区 0 写元数据（布局同网关 App_bootloader.h）
//
// 校验约定与网关一致：有效数据字节累加和，END 帧比对，不一致整包作废不写元数据。
// 元数据没写 = 07 视为无有效升级包，不会搬运半成品，天然断电安全。
//==========================================================================

void APP_firmware_recv_init(void);       //状态机复位，上电调用一次
void APP_firmware_recv_poll(void);       //主循环 10ms 调一次
uint8_t APP_firmware_recv_stored(void);  //1=一包固件已收完并置好升级标志（主循环据此停发 update）
uint8_t APP_firmware_recv_busy(void);    //1=正在接收固件（主循环据此暂停发 update，避免总线碰撞丢帧）

#endif /* __APP_FIRMWARE_RECV_H */
