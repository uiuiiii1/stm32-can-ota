#include "App_reset.h"
#include "Int_bootloader.h"
#include <stdio.h>
#include <string.h>

//状态枚举
typedef enum
{
    BOOTLOADER_STATE_INIT,        //初始化：等待 start:len 命令
    BOOTLOADER_STATE_RUN,         //运行：接收数据帧并写入 flash
    BOOTLOADER_STATE_CHECK_DATA,  //校验：长度/读回求和/应用向量
    BOOTLOADER_STATE_JUMP_APP,    //跳转：进入 A 程序
}BOOTLOADER_status;

static BOOTLOADER_status app_bootloader_state = BOOTLOADER_STATE_INIT;

static uint32_t expected_len = 0;   //start:len 指定的期望字节数（上限 APP 区 48KB）
static uint32_t received_len = 0;   //已实际写入 flash 的字节数
static uint32_t data_sum     = 0;   //写入字节累加和（读回校验用）
static uint8_t  work_buf[BOOTLOADER_USART_REC_BUFF_LEN]; //帧暂存，隔离 frame_buf 复用
static uint32_t last_frame_tick = 0; //RUN 状态超时计时
static uint32_t last_err_print  = 0; //cmd err 打印节流（同一内容最多 1 秒打一次）

//============================================================================
//  解析升级开始命令 "start:3884"
//
//  说明：
//   1. 不要求命令必须从帧的第 0 字节开始，允许前面有空格/杂字节
//      （工具粘贴、前导字符等干扰也能容忍）。
//   2. 大小写敏感（固定小写 "start:"）。
//   3. 数字解析完后，只认数字前的长度；尾部多余字节一律丢弃、不再写入
//      flash —— 之前"命令和首帧数据同帧直接写"的做法，会把工具追加的
//      \r\n / 杂字节写进镜像开头，导致长度错位、check 失败。
//
//  参数：buf      = 帧数据
//        len      = 帧长度
//        out_len  = 解析出的期望长度
//        end_idx  = 数字结束的下标（若 end_idx < len 说明帧尾还有多余字节）
//  返回：1=成功，0=失败
//============================================================================
static uint8_t parse_start(const uint8_t *buf, uint16_t len, uint32_t *out_len, uint16_t *end_idx)
{
    uint16_t s = 0;
    //在帧内搜索 "start:" 子串，容忍前面有任意杂字节
    while (s + 6 <= len)
    {
        if (memcmp(buf + s, "start:", 6) == 0) break;
        s++;
    }
    if (s + 6 > len) return 0;   //没找到命令

    uint32_t v = 0;
    uint16_t i = s + 6;
    while (i < len && buf[i] >= '0' && buf[i] <= '9')
    {
        v = v * 10U + (uint32_t)(buf[i] - '0');
        if (v > 0xC000UL) return 0;   //上限保护：APP 区 48KB
        i++;
    }
    if (i == s + 6) return 0;   //命令后没有数字
    if (v == 0) return 0;       //长度不能为 0

    *out_len = v;
    *end_idx = i;
    return 1;
}

void App_bootloader_init(void)
{
    // 设置bootloader状态为初始化状态
    app_bootloader_state = BOOTLOADER_STATE_INIT;
    // 预期固件总长度，清零等待start命令赋值
    expected_len = 0;
    // 已经接收的固件字节计数清零
    received_len = 0;
    // 校验和清零（简单累加校验用）
    data_sum     = 0;
    // 记录上一帧接收的系统时间戳，用于超时判断
    last_frame_tick = 0;
    printf("App_reset init\r\n");
    printf("input 'start:len' to begin\r\n");
}


void App_bootloader_work(void)
{
    switch (app_bootloader_state)
    {
    case BOOTLOADER_STATE_INIT:   //初始化：等待 start:len 命令
    {
        if (!bootloader_frame_pending()) break;
        uint16_t n = bootloader_frame_size();
        memcpy(work_buf, bootloader_frame_data(), n);
        bootloader_frame_consume();

        uint32_t l = 0; uint16_t end = 0;
        if (parse_start(work_buf, n, &l, &end))
        {
            expected_len = l;
            received_len = 0;
            data_sum     = 0;
            last_frame_tick = HAL_GetTick();
            bootloader_flash_reset();   //清写入偏移/遗留字节/已擦除页标记
            app_bootloader_state = BOOTLOADER_STATE_RUN;
            printf("start recv, len=%lu\r\n", (unsigned long)l);

            //============================================================
            //  命令已解析成功。帧里数字之后若还有多余字节（工具追加的
            //  \r\n 或杂字节），一律丢弃、绝不写入 flash。
            //  命令必须单独一帧发送；首帧数据请在收到 start recv 后另发。
            //============================================================
            if (end < n)
            {
                printf("warn: ignore %u trailing bytes\r\n", (unsigned)(n - end));
            }
        }
        else
        {
            //============================================================
            //  命令解析失败！把收到的帧内容按十六进制打出来，一眼就能
            //  看出板子到底收到的是不是 "start:3884"（其 hex 为：
            //   73 74 61 72 74 3A 33 38 38 34）。
            //  同一内容 1 秒内最多打一次，避免每帧刷屏。
            //============================================================
            if (HAL_GetTick() - last_err_print >= 1000UL)
            {
                last_err_print = HAL_GetTick();
                printf("cmd err len=%u:", n);
                for (uint16_t i = 0; i < n && i < 16; i++) printf(" %02X", work_buf[i]);
                printf("\r\n");
            }
        }
        break;
    }
    case BOOTLOADER_STATE_RUN:     //运行：接收数据帧并写入 flash
    {
        if (bootloader_frame_pending())
        {
            uint16_t n = bootloader_frame_size();
            memcpy(work_buf, bootloader_frame_data(), n);
            bootloader_frame_consume();

            //============================================================
            //  RUN 里收到以 "start:" 开头的帧 → 视为重新开始：
            //  重置写入状态、换新长度、仍停在 RUN 等新数据。
            //  发错长度或想重传时，直接再发一次 start:NNNN 即可，不用等超时。
            //  （memcmp 限制命令必须在帧开头，避免文件数据误触发）
            //============================================================
            uint32_t l = 0; uint16_t end = 0;
            if (memcmp(work_buf, "start:", 6) == 0 && parse_start(work_buf, n, &l, &end))
            {
                printf("restart, len=%lu\r\n", (unsigned long)l);
                expected_len = l;
                received_len = 0;
                data_sum     = 0;
                last_frame_tick = HAL_GetTick();
                bootloader_flash_reset();
                break;   // 重新开始，停在 RUN 等新数据
            }

            bootloader_flash_write_frame(work_buf, n);
            received_len += n;
            for (uint16_t i = 0; i < n; i++) data_sum += work_buf[i];
            last_frame_tick = HAL_GetTick();

            if (received_len >= expected_len)
            {
                printf("recv done len=%lu\r\n", (unsigned long)received_len);
                bootloader_flash_flush();   //奇数长度收尾：写入遗留的最后字节
                app_bootloader_state = BOOTLOADER_STATE_CHECK_DATA;
            }
        }
        else if (HAL_GetTick() - last_frame_tick > 60000UL)   //60s 无新帧：超时回 INIT（覆盖命令到首帧的人为间隔）
        {
            printf("recv timeout\r\n");
            App_bootloader_init();
        }
        break;
    }
 
    case BOOTLOADER_STATE_CHECK_DATA:   //校验：长度/读回求和/应用向量
        if (App_bootloader_check())
        {
            app_bootloader_state = BOOTLOADER_STATE_JUMP_APP;
        }
        else
        {
            printf("check fail, back to init\r\n");
            App_bootloader_init();
        }
        break;
    case BOOTLOADER_STATE_JUMP_APP:     //跳转：进入 A 程序
        App_bootloader_jump_to_app();
        App_bootloader_init();   //跳转失败（校验不过）则回到等待命令
        break;
    default:
        break;
    }
}

uint8_t App_bootloader_check(void)
{
    //============================================================
    //  长度校验：必须收满 start:len 声明的字节数才算完成。
    //  求和校验按 expected_len 读回 flash 求和。
    //============================================================
    //1) 长度校验
    if (received_len != expected_len)
    {
        printf("len err %lu/%lu\r\n", (unsigned long)received_len, (unsigned long)expected_len);
        return 0;
    }

    //2) 读回求和校验：能抓到 ORE 丢字节/写错位导致的数据不符
    uint32_t sum = 0;
    for (uint32_t i = 0; i < expected_len; i++)
        sum += *(volatile uint8_t *)(APP_FLASH_BASE_ADDR + i);
    if (sum != data_sum)
    {
        printf("sum err %lu/%lu\r\n", (unsigned long)sum, (unsigned long)data_sum);
        return 0;
    }

    //3) 应用入口向量校验
    uint32_t sp = *(volatile uint32_t *)APP_FLASH_BASE_ADDR;
    uint32_t pc = *(volatile uint32_t *)(APP_FLASH_BASE_ADDR + 4);
    if ((sp & 0xFFFF0000UL) != APP_RESET_ADDR)
    {
        printf("stack ptr err\r\n");
        return 0;
    }
    if (pc < APP_FLASH_BASE_ADDR || pc >= APP_END_ADDR)
    {
        printf("reset ptr err\r\n");
        return 0;
    }

    printf("check ok\r\n");
    return 1;
}

uint32_t App_bootloader_recv_len(void)
{
    return received_len;
}

//start:len 声明的期望长度，主循环画 OLED 进度条用（"rx:已收/期望"）
uint32_t App_bootloader_expected_len(void)
{
    return expected_len;
}

void App_bootloader_jump_to_app(void)
{
    bootloader_jump_to_app();
}
