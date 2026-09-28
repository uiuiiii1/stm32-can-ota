#include "App_update_store.h"
#include "App_bootloader.h"   //镜像布局常量（元数据地址、大小限制）
#include "int_uart.h"
#include "w25q80.h"
#include "main.h"             //HAL_GetTick
#include <stdio.h>
#include <string.h>

//入库流程：IDLE 等 start 命令 → RECV 边收边按页写 W25 → 收完自动校验+写元数据
typedef enum
{
    STORE_IDLE = 0,   //等待 start:<size> 命令
    STORE_RECV,       //正在收 bin 正文
} STORE_STATE;

static STORE_STATE s_state = STORE_IDLE;

static char     s_cmd[32];         //start 命令行的累积缓冲
static uint8_t  s_cmd_len = 0;  // 命令缓存s_cmd当前有效字符个数

static uint32_t s_img_size = 0;    //本次要收的镜像总长（start: 参数）
static uint32_t s_img_sum  = 0;    //收到原始数据时的字节累加和（校验基准）
static uint32_t s_img_off  = 0;    //已写入 W25 的字节数（整页部分）
static uint16_t s_page_fill = 0;   //页缓冲里攒了多少字节（上限 256——不能用 uint8_t，255 装不下会永远差 1）
static uint32_t s_next_progress = 0;  //下次打印进度的时间点（每 2KB 一次）
static uint32_t s_last_rx_tick = 0;   //最后一个字节的到达时刻（判超时用）
static uint8_t  s_page[W25Q80_PAGE_SIZE];  //页缓冲：攒满 256B 编程一次

//重置入库模块，回到空闲状态
static void store_reset(void)
{
    s_state = STORE_IDLE;
    s_cmd_len = 0;
    s_page_fill = 0;
    s_img_size = 0;
}

//把页缓冲编程进 W25。APP_ADDR_MIN 页对齐且 s_img_off 恒为 256 的倍数，
//所以每次编程的起始地址天然页对齐，不会触碰"一次编程不可跨页"的红线
static uint8_t store_program_page(void)
{
    //调用W25Q80页编程函数，把s_page里s_page_fill个字节写到APP_ADDR_MIN+s_img_off地址
    if (W25Q80_PageProgram(APP_ADDR_MIN + s_img_off, s_page, s_page_fill) != W25Q80_OK)
    {
        printf("err:w25 write\r\n");  //写入失败打印错误
        return 0;                     //返回0表示失败
    }
    s_img_off += s_page_fill;        //更新已写入偏移，推进到下一页起始位置
    s_page_fill = 0;                 //清空页缓冲计数，准备攒下一页数据
    return 1;                        //返回1表示成功
}

//处理一行完整命令，目前只识别 start:<镜像字节数> 格式
static void store_handle_cmd(void)
{
    //1. 校验命令前缀：前6个字符必须是 "start:"，不是则报错返回
    if (strncmp(s_cmd, "start:", 6) != 0)
    {
        printf("err:cmd <%s>\r\n", s_cmd);
        return;
    }
    //2. 手动把冒号后面的十进制字符串转成uint32数字（固件总字节数）
    uint32_t len = 0;
    const char *p = s_cmd + 6;   //指针跳到冒号后面第一个字符
    //冒号后面是空的，没有数字，报错
    if (*p == '\0')
    {
        printf("err:len empty\r\n");
        return;
    }
    //逐字符解析数字：'0'~'9' 字符减 '0' 得到对应数字，累乘10累加
    while (*p >= '0' && *p <= '9')
    {
        len = len * 10u + (uint32_t)(*p - '0');
        p++;
    }
    //解析完数字后，当前字符不是字符串结束符'\0'，说明后面有非法字符（字母/符号），报错
    if (*p != '\0')
    {
        printf("err:len <%s>\r\n", s_cmd);
        return;
    }
    //3. 校验固件长度是否在允许范围内（APP_SIZE_MIN ~ APP_SIZE_MAX）
    if (len < APP_SIZE_MIN || len > APP_SIZE_MAX)
    {
        printf("err:len %lu out of range\r\n", (unsigned long)len);
        return;
    }
    printf("store size=%lu, erasing...\r\n", (unsigned long)len);
    //4. 擦除固件将要占用的所有W25Q80扇区（每个扇区4KB，从APP_ADDR_MIN开始按扇区推进）
    //   Flash写入前必须先擦除，擦除后全是0xFF，才能写入新数据
    uint32_t addr = APP_ADDR_MIN;
    while (addr < APP_ADDR_MIN + len)
    {
        if (W25Q80_EraseSector(addr) != W25Q80_OK)
        {
            printf("err:w25 erase @0x%08lX\r\n", (unsigned long)addr);
            return;   //擦除失败，停留在IDLE状态，等待用户重新发start指令
        }
        addr += W25Q80_SECTOR_SIZE;  //推进到下一个扇区起始地址
    }
    //5. 擦除成功，初始化本次接收的所有参数
    s_img_size = len;              //记录本次要接收的固件总长度
    s_img_sum = 0;                 //清零接收数据累加和（后续校验用）
    s_img_off = 0;                 //清零已写入W25的字节偏移
    s_page_fill = 0;               //清零页缓冲计数
    s_next_progress = 2048;        //设置第一次打印进度的阈值（每收2KB打印一次）
    s_last_rx_tick = HAL_GetTick();//记录当前时间，用于后续接收超时判断
    s_state = STORE_RECV;          //切换状态机到接收状态，开始接收bin正文
    printf("ready\r\n");           //回复上位机"准备就绪"，PC端看到这行后再开始发送bin文件
}


//IDLE空闲态：从串口缓冲区逐字节累积命令行，遇到换行符'\n'或回车符'\r'代表一条命令结束，触发解析
//参数：
//  buf - 本次从串口环形缓冲读到的数据
//  n   - 本次数据字节数
static void store_poll_idle(uint8_t *buf, uint16_t n)
{
    //逐个字节处理本次收到的数据
    for (uint16_t i = 0; i < n; i++)
    {
        char c = (char)buf[i];
        //遇到 '\n' 或 '\r' 都算一条命令结束。只认 '\n' 会踩坑：
        //有的串口助手"发送新行"只补 \r，板子会永远等不到行尾、一声不吭
        if (c == '\n' || c == '\r')
        {
            //空行直接跳过（\r\n 连发时第二个结束符不带内容）
            if (s_cmd_len == 0)
            {
                continue;
            }
            //在命令末尾补字符串结束符'\0'，变成标准C字符串，方便后续strncmp等函数处理
            s_cmd[s_cmd_len] = '\0';
            //调用命令解析函数，处理这条完整命令（目前只认 start:<长度>）
            store_handle_cmd();
            //命令处理完，清零命令长度，准备接收下一条命令
            s_cmd_len = 0;
        }
        //不是换行符，且命令缓冲还有空余位置（留1字节给'\0'），就把字符存入命令缓冲
        else if (s_cmd_len < sizeof(s_cmd) - 1)
        {
            s_cmd[s_cmd_len++] = c;
        }
        //命令缓冲已满还没遇到换行，说明命令太长，报错并丢弃当前累积的命令
        else
        {
            printf("err:cmd too long\r\n");
            s_cmd_len = 0;
        }
    }
}

//收尾函数：全部bin数据接收完成后调用，执行三步：
//  1. 把页缓冲里剩余不足256字节的尾巴写入W25
//  2. 从W25读回全部固件重新求和，与接收时累加和比对校验
//  3. 校验通过才写入元数据（固件起始地址+长度），供bootloader读取使用
static void store_finish(void)
{
    //1. 如果页缓冲还有没写完的残余字节（不足256），先补写进W25
    if (s_page_fill > 0)
    {
        if (!store_program_page())
        {
            store_reset();   //写入失败，重置状态，本次升级作废
            return;
        }
    }
    //2. 读回校验：把刚写入W25的整段固件读出来，逐字节重新累加求和
    //   与接收时实时累加的s_img_sum对比，能抓到丢字节、写错位、页没擦干净等静默损坏
    uint32_t sum_flash = 0;       //从Flash读回数据的累加和
    uint8_t buf[64];              //临时读缓冲，每次读64字节
    //按64字节分块读取整个固件区域
    for (uint32_t k = 0; k < s_img_size; k += sizeof(buf))
    {
        uint32_t chunk = s_img_size - k;  //本次要读的字节数
      if (chunk > sizeof(buf))          // 如果剩下的数据 >64
        {
            chunk = sizeof(buf);          // 本次就读64字节（缓冲区最大只能装64）
        }
        //从W25读取chunk字节到buf
        if (W25Q80_Read(APP_ADDR_MIN + k, buf, chunk) != W25Q80_OK)
        {
            printf("err:w25 read back\r\n");
            store_reset();
            return;
        }
        //逐字节累加求和
        for (uint32_t i = 0; i < chunk; i++)
        {
            sum_flash += buf[i];
        }
    }
    //校验和不一致，说明固件写入/接收过程出错，升级失败
    if (sum_flash != s_img_sum)
    {
        printf("err:verify flash=%08lX rx=%08lX\r\n",
               (unsigned long)sum_flash, (unsigned long)s_img_sum);
        store_reset();
        return;
    }
    //3. 校验通过，擦除元数据所在扇区，准备写入新元数据
    //   元数据必须校验通过后才写，否则bootloader会读到损坏的固件信息
    if (W25Q80_EraseSector(META_APP_ADDR) != W25Q80_OK)
    {
        printf("err:w25 meta erase\r\n");
        store_reset();
        return;
    }
    //构造元数据：大端格式，前4字节=固件正文起始地址，后4字节=固件长度
    uint8_t meta[META_APP_SIZE];
    meta[0] = (uint8_t)(APP_ADDR_MIN >> 24);  //地址最高字节
    meta[1] = (uint8_t)(APP_ADDR_MIN >> 16);
    meta[2] = (uint8_t)(APP_ADDR_MIN >> 8);
    meta[3] = (uint8_t)(APP_ADDR_MIN);        //地址最低字节
    meta[4] = (uint8_t)(s_img_size >> 24);    //长度最高字节
    meta[5] = (uint8_t)(s_img_size >> 16);
    meta[6] = (uint8_t)(s_img_size >> 8);
    meta[7] = (uint8_t)(s_img_size);          //长度最低字节
    //把元数据写入W25的META_APP_ADDR地址
    if (W25Q80_PageProgram(META_APP_ADDR, meta, META_APP_SIZE) != W25Q80_OK)
    {
        printf("err:w25 meta write\r\n");
        store_reset();
        return;
    }
    //全部完成，打印校验和与固件大小，重置状态回到IDLE，等待下一次升级
    printf("done sum=%08lX size=%lu\r\n",
           (unsigned long)s_img_sum, (unsigned long)s_img_size);
    store_reset();
}


//RECV接收态：从串口环形缓冲读取bin固件字节，攒进页缓冲，满256字节写入W25，全部收完进入收尾校验
static void store_poll_recv(void)
{
    //超时检测：距离上次收到字节超过5秒，判定上位机传输中断，放弃本次升级
    if (HAL_GetTick() - s_last_rx_tick > 5000u)
    {
        printf("err:timeout recv %lu/%lu\r\n",
               (unsigned long)(s_img_off + s_page_fill), (unsigned long)s_img_size);
        store_reset();   //重置回IDLE状态
        return;
    }
    //计算本次最多读取多少字节，受两个上限约束：
    //  1. 页缓冲剩余空间（最多还能放多少字节才满256）
    //  2. 距离目标固件总长度还差多少字节（防止上位机多发，写越界）
    uint32_t total = s_img_off + s_page_fill;   //当前已接收总字节（已写入W25 + 页缓冲暂存）
    uint32_t remain = s_img_size - total;        //距离目标长度还差多少字节
    uint16_t room = (uint16_t)(W25Q80_PAGE_SIZE - s_page_fill);  //页缓冲剩余可写入空间
    //取两者较小值，作为本次读取上限
    uint16_t want = (room < remain) ? room : (uint16_t)remain;
    //从串口环形缓冲读取want个字节，直接存入页缓冲s_page的当前位置
    uint16_t n = Int_UART_rec_read(&s_page[s_page_fill], want);
    if (n == 0)
    {
        return;   //本次没读到数据，直接返回，等下一轮轮询
    }
    s_last_rx_tick = HAL_GetTick();  //更新最后接收时间，重置超时计时
    //把本次收到的字节累加到校验和s_img_sum（接收时实时求和，后续和Flash读回值对比）
    for (uint16_t i = 0; i < n; i++)
    {
        s_img_sum += s_page[s_page_fill + i];
    }
    //推进页缓冲计数
    s_page_fill = (uint16_t)(s_page_fill + n);
    //页缓冲攒满256字节，调用写页函数写入W25（用 >= 防御：want 已限制最多恰好 256，正常不会超过）
    if (s_page_fill >= W25Q80_PAGE_SIZE)
    {
        if (!store_program_page())
        {
            store_reset();   //写入失败，重置状态
            return;
        }
    }
    //每接收满2KB打印一次进度
    total = s_img_off + s_page_fill;
    if (total >= s_next_progress)
    {
        printf("recv %lu/%lu\r\n", (unsigned long)total, (unsigned long)s_img_size);
        s_next_progress += 2048;  //下一次进度阈值+2KB
    }
    //已接收总字节达到目标固件长度，全部收完，进入收尾（写残余页+校验+写元数据）
    if (total >= s_img_size)
    {
        store_finish();
    }
}


//固件入库模块初始化：初始化串口接收、重置状态机、打印操作提示
void App_update_store_init(void)
{
    Int_UART_rec_init();          //初始化串口环形缓冲，开启串口中断接收
    store_reset();                //重置入库状态机，回到IDLE空闲状态，清空所有计数
    printf("store: send start:<size>\\r\\n to load bin\r\n");  //提示上位机发送 start:<固件长度> 指令开始升级
}


//固件入库主轮询函数：放在主循环持续调用，根据当前状态机分支处理不同任务
void App_update_store_poll(void)
{
    if (s_state == STORE_IDLE)
    {
        //IDLE空闲态：等待上位机发送 start:<长度> 命令
        uint8_t buf[32];                              //临时缓冲，每次最多读32字节命令
        uint16_t n = Int_UART_rec_read(buf, sizeof(buf));  //从串口环形缓冲读取数据
        if (n > 0)
        {
            store_poll_idle(buf, n);   //有数据，交给IDLE态处理函数累积命令行
        }
    }
    else
    {
        //STORE_RECV接收态：已经收到start命令，正在接收bin固件正文
        store_poll_recv();   //接收固件数据，攒页写W25，收完自动收尾校验
    }
}
