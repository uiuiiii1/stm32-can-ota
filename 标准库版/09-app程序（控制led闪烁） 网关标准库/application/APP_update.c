#include "APP_update.h"
#include "App_bootloader.h"   //镜像布局常量（元数据格式、大小限制）
#include "int_can.h"
#include "W25Q80.h"
#include "Tick.h"             //GetTick / Tick_DelayMs
#include <stdio.h>
#include <string.h>

//每个数据帧携带的固件字节数（8 字节帧 - 2 字节序号）
#define FW_DATA_BYTES  6
//帧间隔。接收端 FIFO 只有 3 帧、10ms 轮询一次，消化能力 3 帧/10ms，
//要求帧间隔 >= 3.4ms，取 5ms 留余量（否则接收端轮询不过来会丢帧）
#define FW_FRAME_GAP_MS  5

//发送状态机：WAIT 等 update 指令 → SEND 读 W25Q80 流式分发（BEGIN→ready 握手→DATA→END）
typedef enum
{
    UPD_WAIT = 0,   //等待接收板发来的 update 触发帧（ID 0x1）
    UPD_SEND,       //正在流式发送固件
} UPD_STATE;

static UPD_STATE s_state = UPD_WAIT;

//update 触发帧：接收板单击按键发来（ID 0x1，"update" 6 字节）
static void upd_on_update(void)
{
    //1. 读元数据：W25Q80 扇区 0 前 8 字节，[4B 正文地址][4B 正文长度]，大端
    uint8_t meta[META_APP_SIZE];
    if (W25Q80_Read(META_APP_ADDR, meta, META_APP_SIZE) != W25Q80_OK)
    {
        printf("send: read meta fail\r\n");
        return;
    }
    uint32_t addr = ((uint32_t)meta[0] << 24) | ((uint32_t)meta[1] << 16) |
                    ((uint32_t)meta[2] <<  8) |  (uint32_t)meta[3];
    uint32_t size = ((uint32_t)meta[4] << 24) | ((uint32_t)meta[5] << 16) |
                    ((uint32_t)meta[6] <<  8) |  (uint32_t)meta[7];
    printf("send meta: addr=0x%08lX size=%lu\r\n",
           (unsigned long)addr, (unsigned long)size);

    //2. 元数据合法性校验（全 0xFF = 还没入库过的空片，也挡在这里）
    if (addr != APP_ADDR_MIN || size < APP_SIZE_MIN || size > APP_SIZE_MAX ||
        addr + size > W25_FIRMWARE_END)
    {
        printf("send: no valid image, store first!\r\n");
        return;
    }

    //3. BEGIN 帧：告诉接收端固件总长（总帧数接收端自己算：(size+5)/6）
    uint8_t begin[8] = {'B','E','G','I','N',
                        (uint8_t)(size >> 8), (uint8_t)size, 0xFF};
    Int_CAN_send(FW_CTRL_ID, begin, 8);
    printf("send begin: %lu bytes, %u frames\r\n",
           (unsigned long)size, (unsigned)((size + FW_DATA_BYTES - 1) / FW_DATA_BYTES));

    //4. 等 ready 握手：接收端要先擦 W25Q64 暂存区（45~400ms/扇区），擦完才回 ready，
    //期间灌数据必丢帧（FIFO 只有 3 帧）。5s 收不到 ready 判接收端不在线，放弃本轮
    uint32_t t0 = GetTick();
    uint8_t got_ready = 0;
    while (GetTick() - t0 < 5000u)
    {
        CAN_Rec_MSG m[3];
        uint8_t n = 0;
        Int_CAN_Rec(m, &n);
        for (uint8_t i = 0; i < n; i++)
        {
            if (m[i].RxHeader.StdId == FW_READY_ID &&
                m[i].Data[0]=='R' && m[i].Data[1]=='D' && m[i].Data[2]=='Y')
            {
                got_ready = 1;
                break;
            }
        }
        if (got_ready)
        {
            break;
        }
        Tick_DelayMs(5);
    }
    if (!got_ready)
    {
        printf("send: no ready from receiver\r\n");
        return;
    }
    printf("send: got ready, streaming\r\n");

    //5. 流式发送：每帧带 2 字节序号 + 6 字节数据，帧间留间隔让接收端消化
    uint8_t  frame[8];
    uint16_t seq = 0;              //帧序号，0 起
    uint32_t sent = 0;             //已发送的固件字节数
    uint32_t sum = 0;              //发送数据的字节累加和（放进 END 帧）
    uint32_t next_prog = 2048;     //每 2KB 打一次进度
    while (sent < size)
    {
        uint32_t chunk = (size - sent > FW_DATA_BYTES) ? FW_DATA_BYTES : (size - sent);
        //从 W25Q80 正文区当前偏移读 chunk 字节进帧的数据位
        if (W25Q80_Read(addr + sent, &frame[2], chunk) != W25Q80_OK)
        {
            printf("send: w25 read fail @%lu\r\n", (unsigned long)sent);
            return;
        }
        //末帧不足 6 字节的部分补 0xFF（接收端按总长截取，不会把填充当数据）
        for (uint32_t i = chunk; i < FW_DATA_BYTES; i++)
        {
            frame[2 + i] = 0xFF;
        }
        //累加校验和（只算真实数据，填充的 0xFF 不算）
        for (uint32_t i = 0; i < chunk; i++)
        {
            sum += frame[2 + i];
        }
        frame[0] = (uint8_t)(seq >> 8);   //序号高字节
        frame[1] = (uint8_t)seq;          //序号低字节
        Int_CAN_send(FW_DATA_ID, frame, 8);
        sent += chunk;
        seq++;
        if (sent >= next_prog)
        {
            printf("send %lu/%lu\r\n", (unsigned long)sent, (unsigned long)size);
            next_prog += 2048;
        }
        Tick_DelayMs(FW_FRAME_GAP_MS);
    }

    //6. END 帧：全文 32 位字节累加和。接收端算出的和与它一致，才算传输完整
    uint8_t end[8] = {'E','N','D', 0xFF,
                      (uint8_t)(sum >> 24), (uint8_t)(sum >> 16),
                      (uint8_t)(sum >> 8),  (uint8_t)sum};
    Int_CAN_send(FW_CTRL_ID, end, 8);
    printf("send done: %u frames, sum=%08lX\r\n", (unsigned)seq, (unsigned long)sum);
}

void APP_update_init(void)
{
    s_state = UPD_WAIT;
}

void APP_update_poll(void)
{
    if (s_state != UPD_WAIT)
    {
        return;   //发送是阻塞式全程完成的，正常不会停在中间态（防御保留）
    }
    CAN_Rec_MSG m[3];
    uint8_t n = 0;
    Int_CAN_Rec(m, &n);
    for (uint8_t i = 0; i < n; i++)
    {
        if (m[i].RxHeader.StdId == 0x1 && m[i].RxHeader.DLC >= 6 &&
            strncmp((char *)m[i].Data, "update", 6) == 0)
        {
            printf("Received update command, switch to send app state\r\n");
            upd_on_update();   //全程阻塞式发送，完成后回到 WAIT 等下一次触发
            break;
        }
    }
}
