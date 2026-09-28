#ifndef __APP_BOOTLOADER_H
#define __APP_BOOTLOADER_H

#include <stdint.h>

// 检验密钥，用于 Bootloader 校验标志、确认 APP 合法性
#define CHECK_KEY_ADDR 0x21
#define CHECK_KEY       0x5A6B

// 存储 APP 升级状态的 EEPROM 地址
#define APP_UPDATE_ADDR 0x20

// APP 更新状态
#define BOOT_UPDATE     0x01   // 需要执行固件升级
#define BOOT_NO_UPDATE  0x02   // 无需升级，直接运行 APP
#define BOOT_RESET      0x03   // 复位，进入默认程序

// W25 里的元数据（大端存放，由打包/下载端写入）
#define META_APP_ADDR   0x00
#define META_APP_SIZE   8

// 程序存储判断条件（校验对象是 W25 地址空间偏移）
#define APP_SIZE_MAX    0x8000          // 程序最大 32KB = app 区大小
#define APP_SIZE_MIN    500
#define APP_ADDR_MIN    0x001000UL      // 镜像在 W25 里最低从 0x1000 开始
#define W25_FIRMWARE_END 0x0010000UL    // W25 固件区上限 64KB

void App_bootloader_check_update(void);
void App_bootloader_reset(void);
void App_bootloader_update(void);
void App_bootloader_jump_app(void);
void App_bootloader_process(void);
void App_bootloader_wait_key(void);

#endif /* __APP_BOOTLOADER_H */
