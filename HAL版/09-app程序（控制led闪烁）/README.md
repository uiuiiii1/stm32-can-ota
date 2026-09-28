# 09-app程序（控制led闪烁）——接收端固件

接收板的 app（链接基址 0x08008000），由 07 bootloader 在复位后拉起。角色是 OTA 的"收货方"：

1. **单击 PB12** 发送一帧 `update`，请求网关推送固件；
2. 接收网关的 CAN 分帧（BEGIN → ready 握手 → 流式 DATA → END）；
3. 数据写入本板 **W25Q64 暂存区**，END 后读回校验，通过则写元数据并向 **AT24C64 置升级标志**；
4. 标志置位后自动停发 update（打断网关重发循环），**复位即由 07 搬运跳转新固件**。

平时运行：PA1 LED 每 0.5s 翻转（心跳）。

## 串口输出（USART1 9600）

```
app start
W25Q64 ok, JEDEC ID: 0xEF4017
recv: size=3884, erasing...     ← 收到 BEGIN，正在擦暂存区
recv: ready                     ← 擦完，已通知网关开始发送
recv: stored sum=0005A0DE size=3884, flag set, reset to update!
                                 ← 收完+校验+置标志，复位即升级
```

失败样例：`recv: seq 2048 != 203`（丢帧/数据错乱，整包作废）、`recv: timeout`（网关中断）、`recv: erase fail`（W25Q64 异常）。

## 编译烧录

- Keil 打开 `MDK-ARM/01-LED1.uvprojx` → Rebuild（0 Error）→ LOAD
- 工程链接基址已配置为 **0x08008000**（`LR_IROM1` 起始 + `SCB->VTOR`），直接 LOAD 到已装有 07 bootloader 的板子即可

## 依赖硬件

CAN（PA11/PA12 + TJA1050）、W25Q64（SPI1 PA4~PA7）、AT24C64（PB10/PB11）、按键 PB12、USART1（PA9/PA10）、LED PA1
