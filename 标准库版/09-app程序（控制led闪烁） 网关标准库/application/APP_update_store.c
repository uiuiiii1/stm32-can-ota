#include "APP_update_store.h"
#include "App_bootloader.h"   //镜像布局常量（元数据格式、大小限制）
#include "USART.h"            //串口环形缓冲读取
#include "W25Q80.h"
#include "Tick.h"             //GetTick
#include <stdio.h>
#include <string.h>

//入库状态机：IDLE 等 start 命令 → RECV 边收边按页写 W25Q80 → 收满读回校验 → 写元数据
typedef enum
{
    STORE_IDLE = 0,   //等待 start:<size> 命令
    STORE_RECV,       //正在接收 bin 正文
} STORE_STATE;

static STORE_STATE s_state = STORE_IDLE;

static char     s_cmd[32];         //start 命令行的累积缓冲
static uint8_t  s_cmd_len = 0;

static uint32_t s_size;            //本次要收的镜像总长（start: 参数）
static uint32_t s_sum;             //收到原始数据时的字节累加和（校验基准）
static uint32_t s_off;             //已写入 W25Q80 的字节数（整页部分）
static uint16_t s_fill;            //页缓冲里攒了多少字节
static uint32_t s_next_prog;       //下次打印进度的时间点（每 2KB 一次）
static uint32_t s_last_tick;       //最后一个字节的到达时刻（判超时）
static uint8_t  s_page[W25Q80_PAGE_SIZE];   //页缓冲：攒满 256B 编程一次

static void store_reset(void)
{
    s_state = STORE_IDLE;
    s_cmd_len = 0;
    s_fill = 0;
    s_size = 0;
}

//把页缓冲编程进 W25Q80。APP_ADDR_MIN 页对齐且 s_off 恒为 256 倍数，
//编程起始地址天然页对齐，不会触碰"一次编程不可跨页"的红线
static uint8_t store_program_page(void)
{
    if (W25Q80_PageProgram(APP_ADDR_MIN + s_off, s_page, s_fill) != W25Q80_OK)
    {
        printf("err:w25 write\r\n");
        return 0;
    }
    s_off += s_fill;
    s_fill = 0;
    return 1;
}

//处理一行完整命令，目前只认 start:<镜像字节数>
static void store_handle_cmd(void)
{
    if (strncmp(s_cmd, "start:", 6) != 0)
    {
        printf("err:cmd <%s>\r\n", s_cmd);
        return;
    }
    uint32_t len = 0;
    const char *p = s_cmd + 6;
    if (*p == '\0')
    {
        printf("err:len empty\r\n");
        return;
    }
    while (*p >= '0' && *p <= '9')
    {
        len = len * 10u + (uint32_t)(*p - '0');
        p++;
    }
    if (*p != '\0')
    {
        printf("err:len <%s>\r\n", s_cmd);
        return;
    }
    if (len < APP_SIZE_MIN || len > APP_SIZE_MAX)
    {
        printf("err:len %lu out of range\r\n", (unsigned long)len);
        return;
    }
    printf("store size=%lu, erasing...\r\n", (unsigned long)len);
    uint32_t addr = APP_ADDR_MIN;
    while (addr < APP_ADDR_MIN + len)
    {
        if (W25Q80_EraseSector(addr) != W25Q80_OK)
        {
            printf("err:w25 erase @0x%08lX\r\n", (unsigned long)addr);
            return;
        }
        addr += W25Q80_SECTOR_SIZE;
    }
    s_size = len;
    s_sum = 0;
    s_off = 0;
    s_fill = 0;
    s_next_prog = 2048;
    s_last_tick = GetTick();
    s_state = STORE_RECV;
    printf("ready\r\n");
}

//IDLE 态：从串口攒一行命令（\n 或 \r 结尾，空行跳过）
static void store_poll_idle(uint8_t *buf, uint16_t n)
{
    for (uint16_t i = 0; i < n; i++)
    {
        char c = (char)buf[i];
        if (c == '\n' || c == '\r')
        {
            if (s_cmd_len == 0)
            {
                continue;   //\r\n 连发时第二个结束符不带内容
            }
            s_cmd[s_cmd_len] = '\0';
            store_handle_cmd();
            s_cmd_len = 0;
        }
        else if (s_cmd_len < sizeof(s_cmd) - 1)
        {
            s_cmd[s_cmd_len++] = c;
        }
        else
        {
            printf("err:cmd too long\r\n");
            s_cmd_len = 0;
        }
    }
}

//收尾：补写尾页 → 读回校验 → 通过才写元数据
static void store_finish(void)
{
    if (s_fill > 0)
    {
        if (!store_program_page())
        {
            store_reset();
            return;
        }
    }
    //读回校验：整段从 W25Q80 读出重算字节和，与接收时累加和比对
    uint32_t sum_flash = 0;
    uint8_t buf[64];
    for (uint32_t k = 0; k < s_size; k += sizeof(buf))
    {
        uint32_t chunk = s_size - k;
        if (chunk > sizeof(buf))
        {
            chunk = sizeof(buf);
        }
        if (W25Q80_Read(APP_ADDR_MIN + k, buf, chunk) != W25Q80_OK)
        {
            printf("err:verify read fail\r\n");
            store_reset();
            return;
        }
        for (uint32_t i = 0; i < chunk; i++)
        {
            sum_flash += buf[i];
        }
    }
    if (sum_flash != s_sum)
    {
        printf("err:verify flash=%08lX rx=%08lX\r\n",
               (unsigned long)sum_flash, (unsigned long)s_sum);
        store_reset();
        return;
    }
    //校验通过才写元数据（大端 [4B 正文起始地址][4B 正文长度]）
    if (W25Q80_EraseSector(META_APP_ADDR) != W25Q80_OK)
    {
        printf("err:w25 meta erase\r\n");
        store_reset();
        return;
    }
    uint8_t meta[META_APP_SIZE];
    meta[0] = (uint8_t)(APP_ADDR_MIN >> 24);
    meta[1] = (uint8_t)(APP_ADDR_MIN >> 16);
    meta[2] = (uint8_t)(APP_ADDR_MIN >> 8);
    meta[3] = (uint8_t)(APP_ADDR_MIN);
    meta[4] = (uint8_t)(s_size >> 24);
    meta[5] = (uint8_t)(s_size >> 16);
    meta[6] = (uint8_t)(s_size >> 8);
    meta[7] = (uint8_t)(s_size);
    if (W25Q80_PageProgram(META_APP_ADDR, meta, META_APP_SIZE) != W25Q80_OK)
    {
        printf("err:w25 meta write\r\n");
        store_reset();
        return;
    }
    printf("done sum=%08lX size=%lu\r\n",
           (unsigned long)s_sum, (unsigned long)s_size);
    store_reset();
}

//RECV 态：串口字节攒进页缓冲，满 256B 编程进 W25Q80，收满后进入收尾
static void store_poll_recv(void)
{
    if (GetTick() - s_last_tick > 5000u)
    {
        printf("err:timeout recv %lu/%lu\r\n",
               (unsigned long)(s_off + s_fill), (unsigned long)s_size);
        store_reset();
        return;
    }
    //读入量受两个上限约束：页缓冲剩余空间、距离目标长度还差的字节数
    uint32_t total = s_off + s_fill;
    uint32_t remain = s_size - total;
    uint16_t room = (uint16_t)(W25Q80_PAGE_SIZE - s_fill);
    uint16_t want = (room < remain) ? room : (uint16_t)remain;
    uint16_t n = USART1_rec_read(&s_page[s_fill], want);
    if (n == 0)
    {
        return;
    }
    s_last_tick = GetTick();
    for (uint16_t i = 0; i < n; i++)
    {
        s_sum += s_page[s_fill + i];
    }
    s_fill = (uint16_t)(s_fill + n);
    if (s_fill == W25Q80_PAGE_SIZE)
    {
        if (!store_program_page())
        {
            store_reset();
            return;
        }
    }
    total = s_off + s_fill;
    if (total >= s_next_prog)
    {
        printf("recv %lu/%lu\r\n", (unsigned long)total, (unsigned long)s_size);
        s_next_prog += 2048;
    }
    if (total >= s_size)
    {
        store_finish();
    }
}

void APP_update_store_init(void)
{
    USART1_rec_init();
    store_reset();
    printf("store: send start:<size> to load bin\r\n");
}

void APP_update_store_poll(void)
{
    if (s_state == STORE_IDLE)
    {
        uint8_t buf[32];
        uint16_t n = USART1_rec_read(buf, sizeof(buf));
        if (n > 0)
        {
            store_poll_idle(buf, n);
        }
    }
    else
    {
        store_poll_recv();
    }
}
