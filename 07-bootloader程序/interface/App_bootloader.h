#ifndef __APP_BOOTLOADER_H
#define __APP_BOOTLOADER_H


//添加检验密钥，用于Bootloader校验标志，确认APP合法性
#define CHECK_KEY_ADDR 0x21    //EEPROM存放校验密钥的地址
#define CHECK_KEY 0x5A6B       //校验密钥值

//存储APP升级状态的EEPROM地址
#define APP_UPDATE_ADDR 0x20  

//APP更新状态定义
#define BOOT_UPDATE    0x01  //标记：需要执行固件升级
#define BOOT_NO_UPDATE 0x02  //标记：无需升级，直接运行APP
#define BOOT_RESET     0x03  //新增标记：复位，进入默认程序

//W25 里的元数据（大端存放，由打包/下载端写入）：
//  0x00~0x03 = 镜像正文在 W25 里的起始地址
//  0x04~0x07 = 镜像总字节数
//注意：只有这 8 字节是大端；镜像正文是 .bin 裸数据，它的向量表是 Cortex-M 原生小端
#define META_APP_ADDR 0x00
#define META_APP_SIZE 8

//程序存储的判断条件（校验对象都是 W25 地址空间的偏移，不能和内部 Flash 地址混比）
#define APP_SIZE_MAX 0x8000          //程序最大32KB，正好等于 app 区(0x08008000~0x08010000)大小
#define APP_SIZE_MIN 500             //程序最小大小500字节
#define APP_ADDR_MIN 0x001000UL      //镜像在 W25 里最低从 0x1000 开始，0x000000 那一扇区放元数据
#define W25_FIRMWARE_END 0x0010000UL //W25 固件区上限 64KB：起始地址+长度都不能超过


//检查是否需要更新
void App_bootloader_check_update(void);

//重置指令
void App_bootloader_reset(void);

//执行更新操作
void App_bootloader_update(void);

//执行跳转操作
void App_bootloader_jump_app(void);

//按状态分发处理（主循环轮询调用）
void App_bootloader_process(void);

//开机按键等待窗口：长按2s→进默认程序，无按键/短按→超时后跳app
void App_bootloader_wait_key(void);

#endif
