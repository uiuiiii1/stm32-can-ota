#include "App_bootloader.h"
#include "Int_bootloader.h"
#include "w25q64.h"
#include "at24c64.h"
#include "key.h"
#include <stdio.h>

#define BOOT_KEY_WAIT_MS  3000U   //开机按键等待窗口：无按键时最长等3s

//按当前状态分发处理（主循环轮询调用）。
//只处理一次状态变化，避免每轮循环重复打印刷屏
static uint8_t s_last_status = 0xFF;

uint8_t App_bootloader_update_status = BOOT_NO_UPDATE;   //默认状态为不更新

//本次成功烧写的目标区基址（0=没烧写过）。烧哪个区由镜像自己决定，跳转必须以它为准
static uint32_t s_app_run_addr = 0;

//W25→内部Flash 搬运的分块缓冲：256 字节正好是 W25 的一页；
//用 static 是因为 F103C8 只有 20KB RAM，放栈上会吃掉 1/10 的栈
static uint8_t s_copy_chunk[256];

//把升级状态连同校验密钥一起写回 EEPROM（0x20: [状态][0x5A][0x6B]），下次复位读到的就是它
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
//  擦是写的前置条件：NOR 只能 1→0，不擦直接写会得到新旧数据按位与的静默损坏。
//  dst_base 最低 0x08004000，本身就页对齐，也绝不碰 0x08000000~0x08003FFF 的 bootloader。
//
//  解锁/上锁收在本函数内部：调用方的每条错误路径就不用各自记着 HAL_FLASH_Lock()。
//  返回 1=全部擦成功，0=某页失败（已重新上锁）。
//==========================================================================
static uint8_t App_flash_erase_region(uint32_t dst_base, uint32_t size)
{
  HAL_FLASH_Unlock();

  for (uint32_t page = dst_base; page < dst_base + size; page += FLASH_PAGE_SIZE)
  {
    FLASH_EraseInitTypeDef erase = {0};
    uint32_t page_error = 0;

    erase.TypeErase   = FLASH_TYPEERASE_PAGES;
    erase.Banks       = FLASH_BANK_1;
    erase.PageAddress = page;
    erase.NbPages     = 1;
    if (HAL_FLASHEx_Erase(&erase, &page_error) != HAL_OK)
    {
      HAL_FLASH_Lock();
      printf("erase fail @0x%08lX\r\n", (unsigned long)page);
      return 0;
    }
  }

  HAL_FLASH_Lock();
  return 1;
}

//==========================================================================
//  把 W25 的 size 字节搬到内部 Flash dst_base，同时把源数据的字节累加和写进 *sum
//  （供调用方读回校验用）。
//
//  每次读 256 字节 = W25 正好一页。块长是偶数、off 也恒为偶数，
//  所以只有"镜像总长是奇数"时最后一片会剩 1 个字节，补 0xFF 凑成半字再写。
//  返回 1=整段写完，0=中途失败（已重新上锁，调用方保持 BOOT_UPDATE 标志等重试）。
//==========================================================================
static uint8_t App_flash_copy_from_w25(uint32_t w25_addr, uint32_t dst_base,
                                       uint32_t size, uint32_t *sum)
{
  uint32_t off = 0;                 //off：当前偏移量，从0开始
  HAL_FLASH_Unlock();               //解锁内部Flash，允许编程写入
  while (off < size)                //循环，直到全部size字节搬运完成
  {
    uint32_t remain = size - off;   //剩余还有多少字节没拷贝
    //一次最多读s_copy_chunk大小(256字节)，不够就读剩下的
    uint16_t n = (remain > sizeof(s_copy_chunk)) ? (uint16_t)sizeof(s_copy_chunk)
                                                : (uint16_t)remain;
    //从W25的w25_addr+off地址，读出n字节放到s_copy_chunk缓冲区
    if (W25Q64_Read(w25_addr + off, s_copy_chunk, n) != W25Q64_OK)
    {
      HAL_FLASH_Lock();              //读W25失败，立刻上锁Flash，退出
      printf("w25 read fail @%lu\r\n", (unsigned long)off);
      return 0;
    }
    //把读到的每个字节累加进校验和sum，后面用来校验写入是否正确
    for (uint16_t i = 0; i < n; i++) *sum += s_copy_chunk[i];

    uint32_t addr = dst_base + off;  //目标：STM32内部Flash的写入地址
    uint16_t i = 0;
    //=============循环，每次取2个字节，按半字(16bit)写入Flash=============
    //F103 Flash只支持半字编程，一次必须写2字节
    for (; i + 1 < n; i += 2)
    {
      //低字节 = s_copy_chunk[i]，高字节 = s_copy_chunk[i+1]
      uint16_t half = (uint16_t)s_copy_chunk[i] | ((uint16_t)s_copy_chunk[i + 1] << 8);
      //写入16位半字到Flash地址addr
      if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, addr, half) != HAL_OK)
      {
        HAL_FLASH_Lock();
        printf("program fail @0x%08lX\r\n", (unsigned long)addr);
        return 0;
      }
      addr += 2; //地址+2，准备写下一组半字
    }
    //============处理最后剩下单独1个字节（总长度是奇数的时候）============
    if (i < n)
    {
      //只剩1字节，拼成16bit：有效字节放低8位，高8位填0xFF
      uint16_t half = (uint16_t)s_copy_chunk[i] | 0xFF00U;
      if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, addr, half) != HAL_OK)
      {
        HAL_FLASH_Lock();
        printf("program fail @0x%08lX\r\n", (unsigned long)addr);
        return 0;
      }
    }
    off += n; //偏移增加n，处理下一块
  }
  HAL_FLASH_Lock(); //全部写完，锁定Flash保护
  return 1; //拷贝成功返回1
}


//==========================================================================
//  把 W25 里的固件镜像搬进 STM32 内部 Flash
//
//  数据来源：W25 第 0 页头 8 字节元数据（大端，由下载/打包端写入）
//      [0..3] = 镜像正文在 W25 里的起始地址
//      [4..7] = 镜像总字节数
//  镜像正文是 .bin 裸数据，头两个字是向量表（栈顶、复位入口），按 Cortex-M 原生
//  【小端】存放 —— 别拿大端去拼，否则真实镜像一律会被判成 "stack addr error"。
//
//  烧到内部 Flash 的哪里：由镜像自己的复位入口决定（复位向量落在哪个区就烧哪个区），
//  这样"分区校验"和"目标路由"是同一步：链接基址编错的镜像直接被拒，
//  不会出现"A 区固件被烧进 B 区"。
//
//  擦除/编程分别交给 App_flash_erase_region() 与 App_flash_copy_from_w25()，
//  本函数只做"校验 → 擦 → 搬 → 读回比对"，任一步失败即返回 0。
//
//  返回 1 = 已写入且读回校验通过；0 = 任一步失败。
//  失败时调用方【不能】清 EEPROM 的 BOOT_UPDATE 标志（保持原样 → 下次复位重做），
//  否则会留下"标志已清但固件是半成品"的死局。
//
//  另外：这两段擦写里刻意不加 __disable_irq()。W25 驱动的 W25_WaitBusy() 用
//  HAL_GetTick() 判超时，关掉全局中断后 SysTick 不再走，超时条件永远不成立 → 卡死。
//==========================================================================
static uint8_t App_bootloader_write_app_flash(void)
{
  uint8_t  buff[8];
  uint32_t w25_addr = 0;    //镜像在 W25 里的起始地址（W25 地址空间，不是内部 Flash！）
  uint32_t app_size = 0;    //镜像总字节数
  uint32_t msp, reset, vec_addr;
  uint32_t dst_base, dst_end;

  //1，读元数据（前4字节=镜像地址，后4字节=镜像长度，大端）
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

  //2，W25 地址空间边界校验：镜像不能压在第 0 扇区（那里放元数据），
  //   且"起始+长度"不能越过固件区上限（app_size 已先做非零校验，不会下溢）
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

  //3，去 W25 里固件起始位置读向量表头 8 字节（小端：前4字节栈顶，后4字节复位入口）
  if (W25Q64_Read(w25_addr, buff, 8) != W25Q64_OK)
  {
    printf("read app vector table fail\r\n");
    return 0;
  }
  msp   = (uint32_t)buff[0]         | ((uint32_t)buff[1] <<  8) |
          ((uint32_t)buff[2] << 16) | ((uint32_t)buff[3] << 24);
  reset = (uint32_t)buff[4]         | ((uint32_t)buff[5] <<  8) |
          ((uint32_t)buff[6] << 16) | ((uint32_t)buff[7] << 24);

  //栈顶高 16 位必须是 SRAM 起始 0x2000，保证栈在 SRAM 区
  if ((msp & 0xFFFF0000U) != APP_SRAM_START)
  {
    printf("stack addr error\r\n");
    return 0;
  }

  //4，用复位入口挑目标分区（先去掉 bit0 的 Thumb 标记得到真实地址）
  vec_addr = reset & 0xFFFFFFFEU;
  if (vec_addr >= PESET_START && vec_addr < APP_FLASH_BASE_ADDR)
  {
    dst_base = PESET_START;          //默认/出厂程序区 0x08004000~0x08008000（16KB）
    dst_end  = APP_FLASH_BASE_ADDR;
  }
  else if (vec_addr >= APP_FLASH_BASE_ADDR && vec_addr < APP_END_ADDR)
  {
    dst_base = APP_FLASH_BASE_ADDR;   //app 区 0x08008000~0x08010000（32KB）
    dst_end  = APP_END_ADDR;
  }
  else
  {
    //基址不是这两个区（比如误编到 0x08000000，会撞上 bootloader 自己）：直接拒
    printf("reset address error 0x%08lX\r\n", (unsigned long)vec_addr);
    return 0;
  }
  if (app_size > (dst_end - dst_base))
  {
    printf("app size > target region\r\n");   //16KB 区放不下 32KB 镜像
    return 0;
  }
  //（bootloader 只剩几十字节容量，这里不再单独打印搬运信息，
  //  源地址/长度见前面的 "meta:"，结果见后面的 "copy ok"）

  //5，擦除目标区（页对齐、解锁/上锁都在函数内部处理）
  if (App_flash_erase_region(dst_base, app_size) == 0)
  {
    return 0;
  }

  //6，从 W25 搬运镜像进内部 Flash，同时算出源数据的字节和
  uint32_t sum_src = 0;
  if (App_flash_copy_from_w25(w25_addr, dst_base, app_size, &sum_src) == 0)
  {
    return 0;
  }

  //7，读回校验：把目标区整段求和与源数据比对。
  //   能抓到 W25 读错、写错位、页没擦干净（NOR 只能 1→0，未擦就写是按位与的静默损坏）
  uint32_t sum_dst = 0;
  for (uint32_t k = 0; k < app_size; k++)
  {
    sum_dst += *(volatile uint8_t *)(dst_base + k);
  }
  if (sum_dst != sum_src)
  {
    printf("verify sum err %lu/%lu\r\n",
           (unsigned long)sum_dst, (unsigned long)sum_src);
    return 0;
  }

  s_app_run_addr = dst_base;   //记下真正写入的区，跳转时以它为准
  printf("copy ok -> 0x%08lX\r\n", (unsigned long)dst_base);
  return 1;
}

//检查是否需要更新
void App_bootloader_check_update(void)
{
  printf("bootloader start\ncheck update\n");
  uint8_t data[3];
  AT24C64_Read(APP_UPDATE_ADDR, data, 3);
  uint16_t check_key = ((uint16_t)data[1] << 8) | data[2];
  printf("ee:%02X %02X %02X\n", data[0], data[1], data[2]);   //状态字节上电可见，便于排查
  if(check_key != CHECK_KEY)
  {
    //密钥错误，不更新 重置密钥
    App_bootloader_save_status(BOOT_NO_UPDATE);
  }
  else if (data[0] == BOOT_UPDATE || data[0] == BOOT_NO_UPDATE || data[0] == BOOT_RESET)
  {
    //密钥正确，读取当前是否需要更新
    App_bootloader_update_status = data[0];
  }
  else
  {
    //密钥对但状态字节是未知值（历史版本只写过密钥/写入被破坏）：
    //process() 三分支都不命中会静默空转不跳转，这里兜底按"不更新"处理并写回修复
    App_bootloader_save_status(BOOT_NO_UPDATE);
  }

}

void App_bootloader_reset(void)
{
  App_bootloader_update_status = BOOT_RESET;   //重置指令：长按按钮 → 置复位标志
}

//开机按键等待窗口（方案A：等按键松手再判断）。
//按键扫描在 TIM4 中断里跑（Key_Tick 更新 Key_Flag），这里只轮询结果。
//长按满2s → BOOT_RESET（进默认程序0x4000）；无按键/短按 → 超时正常返回（跳app0x8000）。
//窗口超时但按键仍按住 → 继续等，给晚按的按键成为长按的机会；最长等待 ≈ 窗口 + 长按2s。
void App_bootloader_wait_key(void)
{
  uint32_t start = HAL_GetTick();
  printf("wait key\r\n");

  while (1)
  {
    if (Key_Event_Flag || Key_Check(KEY_LONG))
    {
      Key_Event_Flag = 0;              //已消费，避免主循环重复处理
      App_bootloader_reset();          //长按2s确认 → 立即跳默认程序
      printf("key->default\n");
      return;
    }
    if ((HAL_GetTick() - start) >= BOOT_KEY_WAIT_MS && !(Key_Flag & KEY_HOLD))
    {
      printf("no key -> app\r\n");
      return;
    }
    HAL_Delay(1);
  }
}


void App_bootloader_process(void)
{
  if (App_bootloader_update_status == s_last_status)
  {
    return;   //状态没变，不重复处理
  }
  s_last_status = App_bootloader_update_status;

  if (s_last_status == BOOT_UPDATE)
  {
    //需要更新：把 W25 里的镜像搬进内部 Flash
    printf("update\n");
    if (App_bootloader_write_app_flash())
    {
      //只有"写完 + 读回校验通过"才允许清标志；
      //失败时保持 BOOT_UPDATE，下次复位自动重做，不会跳进半成品镜像
      App_bootloader_save_status(BOOT_NO_UPDATE);
      App_bootloader_update_status = BOOT_NO_UPDATE;  //下一轮 process 走 NO_UPDATE 分支去跳转
    }
    else
    {
      printf("update fail, flag kept\r\n");   //停在 bootloader 里等复位/等重传
    }
  }
  else if (s_last_status == BOOT_NO_UPDATE)
  {
    //不需要更新：开机窗口无按键 → 跳转 app（0x8000）
    printf("no update\n");
    App_bootloader_jump_app();
  }
  else if (s_last_status == BOOT_RESET)
  {
    //重置指令：打印 reset 并跳转默认程序
    printf("reset\n");
    App_bootloader_jump_app();
  }
}

void App_bootloader_jump_app(void)
{
  uint32_t target;

  if (s_app_run_addr != 0)
  {
    //刚烧写过：跳进本次真正写入的那个区（可能是 0x4000 也可能是 0x8000）
    target = s_app_run_addr;
  }
  else if (App_bootloader_update_status == BOOT_RESET)
  {
    //重置指令：跳转到默认程序
    target = PESET_START;
  }
  else
  {
    //默认/更新后运行：跳转到 app 区
    target = APP_FLASH_BASE_ADDR;
  }

  printf("jump app 0x%08lX\n", (unsigned long)target);
  bootloader_jump_to_app(target);
}
