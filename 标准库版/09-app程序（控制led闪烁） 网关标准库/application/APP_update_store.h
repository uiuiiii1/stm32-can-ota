#ifndef __APP_UPDATE_STORE_H
#define __APP_UPDATE_STORE_H

//==========================================================================
// 固件入库模块（标准库版）：PC 串口发 start:<字节数> + bin 裸流 → 存入网关 W25Q80
//
// 布局（App_bootloader.h 契约）：扇区 0 = 8 字节元数据（大端 [正文地址][正文长度]），
// 正文从 APP_ADDR_MIN(0x1000) 起。收满读回校验，通过才写元数据。
// 上位机流程：发 `start:<size>` → 等 `ready` → 发 .bin 裸流 → `done sum=... size=...`
//==========================================================================

void APP_update_store_init(void);   //初始化串口接收与状态机，上电调用一次
void APP_update_store_poll(void);   //主循环 10ms 调一次

#endif /* __APP_UPDATE_STORE_H */
