# 09-app程序（控制led闪烁） 网关——发送端固件

网关板（链接基址 0x08000000，独占整片 64KB），OTA 的"仓库 + 快递员"：

1. **固件入库**：PC 经串口发 `start:<字节数>`，网关擦好 W25Q80 后回 `ready`，PC 再把 .bin 裸文件灌入；网关边收边按 256B 页写入 W25Q80，收完读回校验，通过才写元数据（大端 `[4B 正文地址][4B 正文长度]`）；
2. **固件分发**：收到接收板的 `update` 帧后，从 W25Q80 读出固件，按 CAN 协议（BEGIN → ready 握手 → 648 帧 DATA → END）流式下发。

## 串口命令与输出（USART1 9600）

```
store: send start:<size>\r\n to load bin
（发送）start:3884
store size=3884, erasing...
ready                          ← 看到这行再用串口助手"发送文件"灌 bin（勿勾十六进制）
recv 2048/3884
done sum=0005A0DE size=3884    ← 入库成功（sum 为固件校验身份证）
```

收到接收板 `update` 后（触发升级）：

```
send meta: addr=0x00001000 size=3884
send begin: 3884 bytes, 648 frames
send: got ready, streaming     ← 接收端擦完暂存区，开始流式发送
send 2048/3884
send done: 648 frames, sum=0005A0DE
```

失败样例：`send: no ready from receiver`（接收端 5 秒未就绪，直接放弃不灌数据）、`err:timeout`（入库时串口中断）。

## 编译烧录

- Keil 打开 `MDK-ARM/01-LED1.uvprojx` → Rebuild（0 Error）→ LOAD（ST-Link）
- 本工程独占整片 flash（链接基址 0x08000000），烧在**网关板**上（与接收板是两块不同的板子）

## 依赖硬件

CAN（PA11/PA12 + TJA1050）、W25Q80（SPI1 PA4~PA7）、USART1（PA9/PA10，同时是 bin 入库通道）

## 协议

帧格式与握手时序详见仓库根目录 README.md（BEGIN 0x100 / DATA 0x101 / END 0x100 / ready 0x102，数值大端，网关与接收端的常量必须保持一致）。
