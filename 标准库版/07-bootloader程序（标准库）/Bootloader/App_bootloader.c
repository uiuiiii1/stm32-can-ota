#include "App_bootloader.h"
#include "Int_bootloader.h"
#include "W25Q64.h"
#include "AT24C64.h"
#include "Key.h"
#include "Tick.h"
#include "stm32f10x_flash.h"
#include <stdio.h>

#define BOOT_KEY_WAIT_MS  3000U   // 开机按键等待窗口：无按键时最长等 3s

// 按当前状态分发处理（主循环轮询调用）。只处理一次状态变化，避免重复打印刷屏
static uint8_t s_last_status = 0xFF;

uint8_t App_bootloader_update_status = BOOT_NO_UPDATE;   // 默认状态为不更新

// 本次成功烧写的目标区基址（0=没烧写过）。跳转必须以它为准
static uint32_t s_app_run_addr = 0;

// W25→内部Flash 搬运的分块缓冲：256 字节正好是 W25 一页
static uint8_t s_copy_chunk[256];

// 把升级状态连同校验密钥写回 EEPROM（0x20: [状态][0x5A][0x6B]）
static void App_bootloader_save_status(uint8_t status)
{
    uint8_t data[3];
    data[0] = status;
    data[1] = (uint8_t)(CHECK_KEY >> 8);
    data[2] = (uint8_t)(CHECK_KEY & 0xFF);
    AT24C64_Write(APP_UPDATE_ADDR, data, 3);
}

//==========================================================================
//  擦除目标区 [dst_base, dst_base+size)，按 1KB 页（F103C8 FLASH_PAGE_SIZE）逐页擦。
//  擦是写的前置条件。解锁/上锁收在本函数内部，调用方错误路径不用各自记着 Lock。
//  返回 1=全部擦成功，0=某页失败（已重新上锁）。
//==========================================================================
static uint8_t App_flash_erase_region(uint32_t dst_base, uint32_t size)
{
    uint32_t page;

    FLASH_Unlock();
    for (page = dst_base; page < dst_base + size; page += 0x400)   // F103C8 页大小 1KB=0x400
    {
        if (FLASH_ErasePage(page) != FLASH_COMPLETE)
        {
            FLASH_Lock();
            printf("erase fail @0x%08lX\r\n", (unsigned long)page);
            return 0;
        }
    }
    FLASH_Lock();
    return 1;
}

//==========================================================================
//  把 W25 的 size 字节搬到内部 Flash dst_base，同时把源数据字节累加和写进 *sum。
//  每次读 256 字节 = W25 一页；F103 Flash 只支持半字编程，一次写 2 字节。
//  返回 1=整段写完，0=中途失败（已重新上锁，调用方保持 BOOT_UPDATE 等重试）。
//==========================================================================
static uint8_t App_flash_copy_from_w25(uint32_t w25_addr, uint32_t dst_base,
                                       uint32_t size, uint32_t *sum)
{
    uint32_t off = 0;
    FLASH_Unlock();
    while (off < size)
    {
        uint32_t remain = size - off;
        uint16_t n = (remain > sizeof(s_copy_chunk)) ? (uint16_t)sizeof(s_copy_chunk)
                                                     : (uint16_t)remain;
        uint16_t i;
        uint32_t addr;
        if (W25Q64_Read(w25_addr + off, s_copy_chunk, n) != W25Q64_OK)
        {
            FLASH_Lock();
            printf("w25 read fail @%lu\r\n", (unsigned long)off);
            return 0;
        }
        for (i = 0; i < n; i++) *sum += s_copy_chunk[i];

        addr = dst_base + off;
        i = 0;
        for (; i + 1 < n; i += 2)
        {
            uint16_t half = (uint16_t)s_copy_chunk[i] | ((uint16_t)s_copy_chunk[i + 1] << 8);
            if (FLASH_ProgramHalfWord(addr, half) != FLASH_COMPLETE)
            {
                FLASH_Lock();
                printf("program fail @0x%08lX\r\n", (unsigned long)addr);
                return 0;
            }
            addr += 2;
        }
        if (i < n)   // 奇数长度最后一个字节，补 0xFF 凑半字
        {
            uint16_t half = (uint16_t)s_copy_chunk[i] | 0xFF00U;
            if (FLASH_ProgramHalfWord(addr, half) != FLASH_COMPLETE)
            {
                FLASH_Lock();
                printf("program fail @0x%08lX\r\n", (unsigned long)addr);
                return 0;
            }
        }
        off += n;
    }
    FLASH_Lock();
    return 1;
}

//==========================================================================
//  把 W25 里的固件镜像搬进 STM32 内部 Flash
//  元数据（W25 第 0 页头 8 字节，大端）：[0..3]=镜像起始地址 [4..7]=镜像总字节数
//  镜像正文是 .bin 裸数据，向量表按 Cortex-M 小端。
//  烧到哪个区由镜像自己的复位入口决定（分区校验与目标路由同一步）。
//  擦除/编程里不加 __disable_irq：W25_WaitBusy 用 GetTick 判超时，关全局中断后
//  tick 不再走 → 超时永远不成立 → 卡死。
//  返回 1=写入并读回校验通过；0=任一步失败（失败时不清 EEPROM 标志，下次复位重做）。
//==========================================================================
static uint8_t App_bootloader_write_app_flash(void)
{
    uint8_t  buff[8];
    uint32_t w25_addr = 0;
    uint32_t app_size = 0;
    uint32_t msp, reset, vec_addr;
    uint32_t dst_base, dst_end;
    uint32_t sum_src, sum_dst, k;

    // 1，读元数据（前4字节=镜像地址，后4字节=长度，大端）
    if (W25Q64_Read(META_APP_ADDR, buff, META_APP_SIZE) != W25Q64_OK)
    {
        printf("read meta app addr error\r\n");
        return 0;
    }
    w25_addr = ((uint32_t)buff[0] << 24) | ((uint32_t)buff[1] << 16) |
               ((uint32_t)buff[2] <<  8) | (uint32_t)buff[3];
    app_size = ((uint32_t)buff[4] << 24) | ((uint32_t)buff[5] << 16) |
               ((uint32_t)buff[6] <<  8) | (uint32_t)buff[7];
    printf("meta: w25_addr=0x%08lX size=%lu\r\n",
           (unsigned long)w25_addr, (unsigned long)app_size);

    // 2，大小范围校验
    if (app_size < APP_SIZE_MIN || app_size > APP_SIZE_MAX)
    {
        printf("app size out of range\r\n");
        return 0;
    }
    if (w25_addr < APP_ADDR_MIN || w25_addr + app_size > W25_FIRMWARE_END)
    {
        printf("w25 addr out of range\r\n");
        return 0;
    }

    // 3，读镜像向量表头 8 字节（小端：前4字节栈顶，后4字节复位入口）
    if (W25Q64_Read(w25_addr, buff, 8) != W25Q64_OK)
    {
        printf("read app vector table fail\r\n");
        return 0;
    }
    msp   = (uint32_t)buff[0]         | ((uint32_t)buff[1] <<  8) |
            ((uint32_t)buff[2] << 16) | ((uint32_t)buff[3] << 24);
    reset = (uint32_t)buff[4]         | ((uint32_t)buff[5] <<  8) |
            ((uint32_t)buff[6] << 16) | ((uint32_t)buff[7] << 24);

    if ((msp & 0xFFFF0000U) != APP_SRAM_START)
    {
        printf("stack addr error\r\n");
        return 0;
    }

    // 4，用复位入口挑目标分区
    vec_addr = reset & 0xFFFFFFFEU;
    if (vec_addr >= PESET_START && vec_addr < APP_FLASH_BASE_ADDR)
    {
        dst_base = PESET_START;  dst_end = APP_FLASH_BASE_ADDR;
    }
    else if (vec_addr >= APP_FLASH_BASE_ADDR && vec_addr < APP_END_ADDR)
    {
        dst_base = APP_FLASH_BASE_ADDR;  dst_end = APP_END_ADDR;
    }
    else
    {
        printf("reset address error 0x%08lX\r\n", (unsigned long)vec_addr);
        return 0;
    }
    if (app_size > (dst_end - dst_base))
    {
        printf("app size > target region\r\n");
        return 0;
    }

    // 5，擦除目标区
    if (App_flash_erase_region(dst_base, app_size) == 0) return 0;

    // 6，从 W25 搬运镜像进内部 Flash，算源数据字节和
    sum_src = 0;
    if (App_flash_copy_from_w25(w25_addr, dst_base, app_size, &sum_src) == 0) return 0;

    // 7，读回校验：目标区整段求和与源数据比对
    sum_dst = 0;
    for (k = 0; k < app_size; k++)
    {
        sum_dst += *(volatile uint8_t *)(dst_base + k);
    }
    if (sum_dst != sum_src)
    {
        printf("verify sum err %lu/%lu\r\n", (unsigned long)sum_dst, (unsigned long)sum_src);
        return 0;
    }

    s_app_run_addr = dst_base;
    printf("copy ok -> 0x%08lX\r\n", (unsigned long)dst_base);
    return 1;
}

// 检查是否需要更新
void App_bootloader_check_update(void)
{
    uint8_t  data[3];
    uint16_t check_key;

    printf("bootloader start\n");
    printf("check update\n");
    AT24C64_Read(APP_UPDATE_ADDR, data, 3);
    check_key = ((uint16_t)data[1] << 8) | data[2];
    printf("ee:%02X %02X %02X\n", data[0], data[1], data[2]);   // 状态字节上电可见，便于排查
    if (check_key != CHECK_KEY)
    {
        // 密钥错误，不更新，重置密钥
        App_bootloader_save_status(BOOT_NO_UPDATE);
    }
    else if (data[0] == BOOT_UPDATE || data[0] == BOOT_NO_UPDATE || data[0] == BOOT_RESET)
    {
        // 密钥正确，读取当前是否需要更新
        App_bootloader_update_status = data[0];
    }
    else
    {
        // 密钥对但状态字节是未知值（历史版本只写过密钥/写入被破坏）：
        // process() 三分支都不命中会静默空转不跳转，这里兜底按"不更新"处理并写回修复
        App_bootloader_save_status(BOOT_NO_UPDATE);
    }
}

void App_bootloader_reset(void)
{
    App_bootloader_update_status = BOOT_RESET;   // 重置指令：长按按钮 → 置复位标志
}

// 开机按键等待窗口（方案A：等按键松手再判断）。
// 长按满 2s → BOOT_RESET（进默认程序0x4000）；无按键/短按 → 超时正常返回（跳 app0x8000）。
// 窗口超时但按键仍按住 → 继续等，给晚按的按键成为长按的机会；最长等待 ≈ 窗口 + 长按 2s。
void App_bootloader_wait_key(void)
{
    uint32_t start = GetTick();
    printf("wait key\r\n");

    while (1)
    {
        if (Key_Event_Flag || Key_Check(KEY_LONG))
        {
            Key_Event_Flag = 0;
            App_bootloader_reset();
            printf("key long -> default\r\n");
            return;
        }
        if ((GetTick() - start) >= BOOT_KEY_WAIT_MS && !(Key_Flag & KEY_HOLD))
        {
            printf("no key -> app\r\n");
            return;
        }
        Tick_DelayMs(1);
    }
}

void App_bootloader_process(void)
{
    if (App_bootloader_update_status == s_last_status)
    {
        return;
    }
    s_last_status = App_bootloader_update_status;

    if (s_last_status == BOOT_UPDATE)
    {
        // 需要更新：把 W25 里的镜像搬进内部 Flash
        printf("update\n");
        if (App_bootloader_write_app_flash())
        {
            // 只有"写完 + 读回校验通过"才清标志；失败保持 BOOT_UPDATE 下次重做
            App_bootloader_save_status(BOOT_NO_UPDATE);
            App_bootloader_update_status = BOOT_NO_UPDATE;
        }
        else
        {
            printf("update fail, flag kept\r\n");
        }
    }
    else if (s_last_status == BOOT_NO_UPDATE)
    {
        // 不需要更新：开机窗口无按键 → 跳转 app（0x8000）
        printf("no update\n");
        App_bootloader_jump_app();
    }
    else if (s_last_status == BOOT_RESET)
    {
        // 重置指令：打印 reset 并跳转默认程序
        printf("reset\n");
        App_bootloader_jump_app();
    }
}

void App_bootloader_jump_app(void)
{
    uint32_t target;

    if (s_app_run_addr != 0)
    {
        // 刚烧写过：跳进本次真正写入的那个区
        target = s_app_run_addr;
    }
    else if (App_bootloader_update_status == BOOT_RESET)
    {
        // 重置指令：跳转到默认程序
        target = PESET_START;
    }
    else
    {
        // 默认/更新后运行：跳转到 app 区
        target = APP_FLASH_BASE_ADDR;
    }

    printf("jump app 0x%08lX\n", (unsigned long)target);
    bootloader_jump_to_app(target);
}
