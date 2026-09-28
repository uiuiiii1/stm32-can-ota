#include "APP_firmware_recv.h"
#include "App_bootloader.h"   //镜像布局常量（元数据格式、大小限制）
#include "int_can.h"
#include "W25Q64.h"
#include "AT24C64.h"
#include "Tick.h"             //GetTick
#include <stdio.h>

//升级标志约定（与 07 接收端 check_update 的读取格式严格一致）：
//EEPROM 0x20=状态字节(0x01=BOOT_UPDATE)，0x21~0x22=校验密钥 0x5A 0x6B（大端）
//复位后 07 读到它就把 W25Q64 暂存区的镜像搬进内部 flash 并跳转
#define BOOT_FLAG_ADDR    0x20
#define BOOT_FLAG_UPDATE  0x01
#define BOOT_FLAG_KEY_HI  0x5A
#define BOOT_FLAG_KEY_LO  0x6B

//接收状态机：IDLE 等 BEGIN → RECV 流式收帧写 W25Q64 → END 收尾
typedef enum
{
    FRV_IDLE = 0,   //等待 BEGIN 帧
    FRV_RECV,       //正在接收固件数据
} FRV_STATE;

static FRV_STATE s_state = FRV_IDLE;

static uint32_t s_size;        //BEGIN 声明的固件总长
static uint32_t s_got;         //已编程进 W25Q64 的字节数（整页部分）
static uint16_t s_next_seq;    //期望的 DATA 帧序号（严格连续，跳变即失败）
static uint16_t s_page_fill;   //页缓冲里攒了多少字节
static uint32_t s_sum;         //有效数据字节的累加和（END 比对用）
static uint32_t s_last_tick;   //最后一帧到达时刻（判超时）
static uint8_t  s_page[W25Q64_PAGE_SIZE];   //页缓冲：攒满 256B 编程一次
static uint8_t  s_stored = 0;   //一包固件完整接收+校验+置标志后置 1（主循环据此停发 update）

static void frv_reset(void)
{
    s_state = FRV_IDLE;
    s_page_fill = 0;
    s_size = 0;
    //注意：s_stored 不在这里清——收完一包后要一直保持，主循环靠它停发 update
}

//把页缓冲编程进 W25Q64。APP_ADDR_MIN 页对齐且 s_got 恒为 256 倍数，
//编程起始地址天然页对齐，不会触碰"一次编程不可跨页"的红线
static uint8_t frv_program_page(void)
{
    if (W25Q64_PageProgram(APP_ADDR_MIN + s_got, s_page, s_page_fill) != W25Q64_OK)
    {
        printf("recv: w25 write fail\r\n");
        return 0;
    }
    s_got += s_page_fill;
    s_page_fill = 0;
    return 1;
}

//BEGIN：校验总长 → 擦元数据扇区 + 镜像区 → 回 ready → 进 RECV
//擦除期间不收任何数据（FIFO 只有 3 帧），所以擦完才回 ready，由网关决定何时开灌
static void frv_on_begin(const CAN_Rec_MSG *msg)
{
    uint32_t size = ((uint32_t)msg->Data[5] << 8) | msg->Data[6];
    if (size < APP_SIZE_MIN || size > APP_SIZE_MAX)
    {
        printf("recv: bad size %lu\r\n", (unsigned long)size);
        return;   //留在 IDLE，网关超时后重试
    }
    printf("recv: size=%lu, erasing...\r\n", (unsigned long)size);
    if (W25Q64_EraseSector(META_APP_ADDR) != W25Q64_OK)
    {
        printf("recv: erase meta fail\r\n");
        return;
    }
    uint32_t addr = APP_ADDR_MIN;
    while (addr < APP_ADDR_MIN + size)
    {
        if (W25Q64_EraseSector(addr) != W25Q64_OK)
        {
            printf("recv: erase fail @0x%08lX\r\n", (unsigned long)addr);
            return;
        }
        addr += W25Q64_SECTOR_SIZE;
    }
    s_size = size;
    s_got = 0;
    s_next_seq = 0;
    s_page_fill = 0;
    s_sum = 0;
    s_last_tick = GetTick();
    //回 ready 帧：网关收到才开始灌数据
    uint8_t rdy[8] = {'R','D','Y', 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    Int_CAN_send(FW_READY_ID, rdy, 8);
    s_state = FRV_RECV;
}

//END：补写尾页 → 读回校验 → 通过才写元数据（扇区 0 在 BEGIN 时已擦好）
static void frv_on_end(void)
{
    if (s_page_fill > 0)
    {
        if (!frv_program_page())
        {
            frv_reset();
            return;
        }
    }
    //读回校验：整段从 W25Q64 读出重算字节和，与接收时累加和比对
    uint32_t sum_flash = 0;
    uint8_t buf[64];
    for (uint32_t k = 0; k < s_size; k += sizeof(buf))
    {
        uint32_t chunk = s_size - k;
        if (chunk > sizeof(buf))
        {
            chunk = sizeof(buf);
        }
        if (W25Q64_Read(APP_ADDR_MIN + k, buf, chunk) != W25Q64_OK)
        {
            printf("recv: verify read fail\r\n");
            frv_reset();
            return;
        }
        for (uint32_t i = 0; i < chunk; i++)
        {
            sum_flash += buf[i];
        }
    }
    if (sum_flash != s_sum)
    {
        printf("recv: verify err flash=%08lX rx=%08lX\r\n",
               (unsigned long)sum_flash, (unsigned long)s_sum);
        frv_reset();
        return;
    }
    //校验通过才写元数据：大端 [4B 正文起始地址][4B 正文长度]，07 按它定位搬运
    uint8_t meta[META_APP_SIZE];
    meta[0] = (uint8_t)(APP_ADDR_MIN >> 24);
    meta[1] = (uint8_t)(APP_ADDR_MIN >> 16);
    meta[2] = (uint8_t)(APP_ADDR_MIN >> 8);
    meta[3] = (uint8_t)(APP_ADDR_MIN);
    meta[4] = (uint8_t)(s_size >> 24);
    meta[5] = (uint8_t)(s_size >> 16);
    meta[6] = (uint8_t)(s_size >> 8);
    meta[7] = (uint8_t)(s_size);
    if (W25Q64_PageProgram(META_APP_ADDR, meta, META_APP_SIZE) != W25Q64_OK)
    {
        printf("recv: meta write fail\r\n");
        frv_reset();
        return;
    }
    //第④步：往 AT24C64 写升级标志，复位后 07 读到它就开始搬运跳转
    uint8_t flag[3] = {BOOT_FLAG_UPDATE, BOOT_FLAG_KEY_HI, BOOT_FLAG_KEY_LO};
    if (AT24C64_Write(BOOT_FLAG_ADDR, flag, 3) != AT24C64_OK)
    {
        printf("recv: eeprom flag fail\r\n");   //标志没写成功就不停发 update，网关下轮会重发
        frv_reset();
        return;
    }
    s_stored = 1;   //主循环据此停发 update，打断网关的重发循环
    printf("recv: stored sum=%08lX size=%lu, flag set, reset to update!\r\n",
           (unsigned long)s_sum, (unsigned long)s_size);
    frv_reset();
}

void APP_firmware_recv_init(void)
{
    frv_reset();
    //软件 I2C 引脚（PB10/PB11 开漏）与 DWT 延时时基都在 AT24C64_Init 里初始化——
    //不调它直接 Write，引脚未配置，芯片不应答，置标志必然失败
    if (AT24C64_Init() != AT24C64_OK)
    {
        printf("recv: AT24C64 init fail! (flag write will not work)\r\n");
    }
}

void APP_firmware_recv_poll(void)
{
    CAN_Rec_MSG m[3];
    uint8_t n = 0;
    Int_CAN_Rec(m, &n);
    for (uint8_t i = 0; i < n; i++)
    {
        if (m[i].RxHeader.StdId == FW_CTRL_ID)               //控制帧
        {
            if (m[i].Data[0]=='B' && m[i].Data[1]=='E' &&
                m[i].Data[2]=='G' && m[i].Data[3]=='I' && m[i].Data[4]=='N')
            {
                if (s_state == FRV_IDLE)
                {
                    frv_on_begin(&m[i]);   //RECV 中再收 BEGIN 视为异常，忽略
                }
            }
            else if (s_state == FRV_RECV &&
                     m[i].Data[0]=='E' && m[i].Data[1]=='N' && m[i].Data[2]=='D')
            {
                frv_on_end();
            }
        }
        else if (m[i].RxHeader.StdId == FW_DATA_ID && s_state == FRV_RECV)
        {   //数据帧：序号必须严格等于期望值，跳变 = 丢帧，整包作废
            uint16_t seq = ((uint16_t)m[i].Data[0] << 8) | m[i].Data[1];
            if (seq != s_next_seq)
            {
                printf("recv: seq %u != %u\r\n", (unsigned)seq, (unsigned)s_next_seq);
                frv_reset();
                return;
            }
            //6 字节进页缓冲（末帧的 0xFF 填充一并写入，读回校验只累加有效字节）
            for (uint8_t j = 0; j < 6; j++)
            {
                s_page[s_page_fill + j] = m[i].Data[2 + j];
            }
            uint32_t start = (uint32_t)seq * 6;
            for (uint8_t j = 0; j < 6 && start + j < s_size; j++)
            {
                s_sum += m[i].Data[2 + j];
            }
            s_page_fill = (uint16_t)(s_page_fill + 6);
            //满一页就编程。必须用 >= 且循环处理：数据帧每帧 6 字节而页是 256 字节，
            //256 不是 6 的倍数——用 == 永远等不到，帧还会跨页边界，
            //所以编程整页后要把多出的尾巴搬到缓冲开头，留给下一页
            while (s_page_fill >= W25Q64_PAGE_SIZE)
            {
                if (W25Q64_PageProgram(APP_ADDR_MIN + s_got, s_page, W25Q64_PAGE_SIZE) != W25Q64_OK)
                {
                    printf("recv: w25 write fail\r\n");
                    frv_reset();
                    return;
                }
                s_got += W25Q64_PAGE_SIZE;
                uint16_t extra = (uint16_t)(s_page_fill - W25Q64_PAGE_SIZE);
                s_page_fill = extra;
                for (uint16_t t = 0; t < extra; t++)
                {
                    s_page[t] = s_page[W25Q64_PAGE_SIZE + t];   //尾巴搬到缓冲开头
                }
            }
            s_next_seq++;
            s_last_tick = GetTick();
        }
    }
    //RECV 态 5s 无帧 = 网关中断，放弃本次（元数据未写，暂存内容自动作废）
    if (s_state == FRV_RECV && GetTick() - s_last_tick > 5000u)
    {
        printf("recv: timeout\r\n");
        frv_reset();
    }
}

//主循环查询：一包固件是否已完整接收并置好升级标志（1=是，停发 update）
uint8_t APP_firmware_recv_stored(void)
{
    return s_stored;
}

//主循环查询：是否正在接收固件（1=是，暂停发 update——
//接收端的 update 帧会和网关的数据帧抢总线，网关关了自动重发，输仲裁的帧直接丢）
uint8_t APP_firmware_recv_busy(void)
{
    return s_state == FRV_RECV;
}
