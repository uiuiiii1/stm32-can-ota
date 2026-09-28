# STM32 CAN 总线 OTA 远程升级

基于 STM32F103C8T6 + CAN 总线的固件远程升级（OTA）完整实现：上位机经串口把固件入库到网关的 W25Q80 → 接收板按键触发 → 网关经 CAN 分帧下发（带握手与校验）→ 接收板收进自己的 W25Q64 暂存区并置升级标志 → 复位后由 bootloader 把固件搬运到内部 flash 并跳转运行。

**全链路每一环都有校验**：串口入库读回比对、CAN 传输帧序号连续性 + 全文累加和、搬运后读回校验。任何一环失败都会显式报错，不会静默写坏固件。

## 系统架构

```
PC(串口助手 9600) ──UART──> ┌────────────────┐              ┌────────────────┐
  start:<size> + .bin       │  网关板          │              │  接收板          │
                            │  STM32F103C8T6  │──CAN 200k───>│  STM32F103C8T6 │
                            │  @0x08000000    │  TJA1050 ×2  │  @0x08008000    │
                            │  W25Q80 固件仓库 │  120Ω×2 终端  │  W25Q64 暂存区   │
                            └────────────────┘              └───────┬────────┘
                                                                    │ EEPROM 0x20 置标志
                                                                    ▼ 复位
                                                            ┌────────────────┐
                                                            │ 07 bootloader   │
                                                            │  W25Q64→内部flash│
                                                            │  校验→清标志→跳转 │
                                                            └────────────────┘
```

## 硬件清单

| 硬件 | 数量 | 说明 |
|---|---|---|
| STM32F103C8T6 最小系统板 | 2 | 72MHz、64KB flash、20KB RAM，LQFP48 |
| CAN 收发器模块（TJA1050） | 2 | **VCC 必须接 5V**；CANH/CANL 接总线 |
| W25Q80（SPI NOR 1MB） | 1 | 网关板固件仓库，SOIC8 裸芯片 + 转接板 |
| W25Q64（SPI NOR 8MB） | 1 | 接收板固件暂存区（兼出厂备份区） |
| AT24C64（EEPROM 8KB） | 1 | 接收板升级标志存放处 |
| 轻触按键 | 1 | 接收板 PB12（单击触发升级 / 长按 2s 进出厂程序区） |
| USB-TTL 串口模块 | 1~2 | 9600 8N1，接网关/接收板的 USART1 |
| ST-Link | 1 | 首次烧录用 |
| 120Ω 电阻 | 2 | CAN 总线两端各一个终端电阻 |

## 接线

### CAN 总线（两板互联）

| 连接点 | 说明 |
|---|---|
| TJA1050 ① CANH ↔ TJA1050 ② CANH | 总线高线 |
| TJA1050 ① CANL ↔ TJA1050 ② CANL | 总线低线 |
| 两板 GND ↔ GND | 必须共地 |
| 每块收发器 CANH–CANL 之间各接 120Ω | 两端终端（并联后总线约 60Ω，可用万用表验证） |

### 网关板

| STM32F103 引脚 | 接到 | 说明 |
|---|---|---|
| PA12 | TJA1050 TXD | CAN_TX（同名相接，不交叉） |
| PA11 | TJA1050 RXD | CAN_RX |
| TJA1050 VCC | **5V** | TJA1050 只支持 5V 供电 |
| PA4 | W25Q80 pin1 (/CS) | 软件 片选 |
| PA5 | W25Q80 pin6 (CLK) | SPI1_SCK |
| PA6 | W25Q80 pin2 (DO) | SPI1_MISO |
| PA7 | W25Q80 pin5 (DI) | SPI1_MOSI |
| 3.3V | W25Q80 pin8 (VCC)、pin3 (/WP)、pin7 (/HOLD) | WP/HOLD 禁止悬空或接地 |
| GND | W25Q80 pin4 (GND) | |
| PA9 | USB-TTL RXD | 串口发送（调试/日志） |
| PA10 | USB-TTL TXD | 串口接收（入库命令） |
| 3.3V / GND | USB-TTL VCC / GND | 串口模块供电与共地 |

### 接收板

| STM32F103 引脚 | 接到 | 说明 |
|---|---|---|
| PA12 / PA11 | TJA1050 TXD / RXD | 同网关板 |
| TJA1050 VCC | **5V** | 同网关板 |
| PA4 | W25Q64 pin1 (/CS) | 软件 片选 |
| PA5 / PA6 / PA7 | W25Q64 pin6 (CLK) / pin2 (DO) / pin5 (DI) | SPI1 |
| 3.3V | W25Q64 pin8 (VCC)、pin3 (/WP)、pin7 (/HOLD) | |
| GND | W25Q64 pin4 (GND) | |
| PB10 | AT24C64 pin6 (SCL) | 软件 I2C 时钟（开漏+上拉） |
| PB11 | AT24C64 pin5 (SDA) | 软件 I2C 数据 |
| 3.3V | AT24C64 pin8 (VCC)、pin7 (WP) | |
| GND | AT24C64 pin1~3 (A0/A1/A2)、pin4 (GND) | 器件地址 0xA0 |
| PB12 | 轻触按键 → GND | 内部上拉，按下为低 |
| PA1 | LED（可选） | 心跳指示，0.5s 翻转 |
| PA9 / PA10 | USB-TTL RXD / TXD | 9600 8N1 |

### 供电

- 两块 STM32 板：USB 或 5V 供电（板载稳压到 3.3V）
- TJA1050：**5V**（两块）
- W25Q80 / W25Q64 / AT24C64：**3.3V**（禁接 5V）

## 两种实现：HAL 库版 / 标准库版

本仓库提供**功能完全一致**的两种实现，目录按开发体系分组，任选其一即可完成整套 OTA：

| 实现 | 目录 | 工程入口 | 外设初始化 | 时基 |
|---|---|---|---|---|
| **HAL 库版**（CubeMX 生成） | `HAL版/` | 各工程 `MDK-ARM/01-LED1.uvprojx` | CubeMX 生成的 MX_*_Init | HAL_GetTick（SysTick） |
| **标准库版**（StdPeriph V3.5，江协式结构） | `标准库版/` | 各工程 `1.uvprojx`（Target 1） | 寄存器/模块化直配 | TIM4 1ms 中断 GetTick |

两者 CAN 协议、W25 镜像布局、EEPROM 标志格式、按键行为**完全一致**，均经双板实测；烧在硬件上不可区分。选择取决于你熟悉的开发体系，也可对照阅读两种写法。

### HAL 库版目录

| 目录 | 内容 | 烧录位置 |
|---|---|---|
| `HAL版/07-bootloader程序/` | 接收板 bootloader | 0x08000000（16KB） |
| `HAL版/09-app程序（控制led闪烁）/` | 接收端固件：按键触发 + CAN 接收 + 置升级标志 | 0x08008000 |
| `HAL版/09-app程序（控制led闪烁） 网关/` | 网关发送端：串口入库 + CAN 分帧下发 | 0x08000000（独占整片） |
| `HAL版/08-出场设置功能/` | 出厂/串口备份升级程序（长按 2s 进入的逃生舱） | 0x08004000 |
| `HAL版/01-LED1/` | 示例升级对象：PA0 LED 每 1s 翻转 | 0x08008000（经 OTA 灌入） |

> 注意：**网关固件**与 **07 bootloader** 都链接在 0x08000000，但烧在不同的物理板子上，互不冲突。

### 标准库版目录

| 目录 | 内容 |
|---|---|
| `标准库版/07-bootloader程序（标准库）/` | 接收板 bootloader（与 HAL 版功能一致） |
| `标准库版/09-app程序（控制led闪烁）标准库/` | 接收端固件：按键触发 + CAN 接收 + 置升级标志 |
| `标准库版/08-出场设置功能（标准库）/` | 出厂/串口备份升级程序（长按 2s 进入的逃生舱） |
| `标准库版/09-app程序（控制led闪烁） 网关标准库/` | 网关发送端：串口入库 + CAN 分帧下发 |

> 结构为江协式模板：`Start/System/Hardware/User` + `interface/application`；时基由 TIM4 1ms 中断提供（GetTick）。

## 编译与烧录

1. 安装 Keil MDK 5（ARMCC V5.06）；
2. HAL 版打开各工程 `MDK-ARM/01-LED1.uvprojx`；标准库版打开各工程 `1.uvprojx`（Target 1）——**Rebuild** 应为 0 Error（模板工程少量 Warning 属正常）；
3. 接 ST-Link，**LOAD** 烧录。首次部署需有线烧三样：
   - 接收板：`07-bootloader程序`（烧在 0x08000000）+ `09-app程序（控制led闪烁）`（工程已配置链接到 0x08008000，直接 LOAD）；
   - 网关板：`09-app程序（控制led闪烁） 网关`；
4. 串口监视：USB-TTL 接各板 USART1，**9600 8N1**。

## CAN 传输协议

全部为标准数据帧，多字节数值大端：

| 帧 | ID | DLC | 内容 |
|---|---|---|---|
| BEGIN | 0x100 | 8 | `'BEGIN'` + 固件总长(2B) + 0xFF |
| ready | 0x102 | 8 | `'RDY'` + 0xFF×5（接收端擦完暂存区后回给网关） |
| DATA | 0x101 | 8 | 帧序号(2B, 从 0 递增) + 6 字节固件数据（末帧补 0xFF） |
| END | 0x100 | 8 | `'END'` + 0xFF + 全文字节累加和(4B) |

时序：网关发 BEGIN → 接收端擦 W25Q64 暂存区（45~400ms/扇区，期间不能收数据）→ 回 ready → 网关以 5ms/帧 流式发 DATA → END。接收端双重校验：帧序号严格连续 + END 累加和一致，任一不满足整包作废（不写元数据，天然无半成品）。

固件在 W25 flash 的布局（元数据大端）：扇区 0 = `[4B 正文起始地址][4B 正文长度]`，正文从 `0x1000` 起。与 07 bootloader 搬运逻辑的约定一致。

## 完整使用流程

### 1. 固件入库（PC → 网关，串口 9600）

```
发送:  start:3884          ← 固件字节数（带换行）
等待:  ready               ← 网关擦完 W25Q80 扇区
发送:  <.bin 裸文件>        ← 串口助手"发送文件"，勿勾十六进制
结果:  done sum=0005A0DE size=3884   ← 入库成功（sum 为固件校验身份证）
```

### 2. 触发升级（接收板上电运行后）

**单击 PB12 按键一次** → 接收板发 `update` → 网关自动完成握手与传输 → 接收板串口显示：

```
recv: stored sum=0005A0DE size=3884, flag set, reset to update!
```

并停发 update（网关随之安静）。升级包就绪。

### 3. 搬运跳转（复位接收板）

复位后 07 bootloader 自动执行：

```
ee:01 5A 6B        ← 读到升级标志
update
meta: w25_addr=0x00001000 size=3884
copy ok -> 0x08008000   ← 搬运+读回校验通过，自动清标志
no update
jump app 0x08008000     ← 跳转新固件
```

新固件（01-LED1）开始运行：PA0 LED 每 1s 翻转。

### 按键行为一览（接收板 PB12）

| 操作 | 时机 | 效果 |
|---|---|---|
| **单击** | app 运行中 | 发送一帧 update，触发网关推送固件 |
| **长按 ≥2s** | 复位后 3 秒窗口内（bootloader 阶段） | 跳转 0x08004000 出厂程序区（**08-出场设置功能**：串口 start:长度 收 bin 写 app 区并跳转，是 CAN 之外的备用串口升级通道，源码见 `08-出场设置功能/` 与 `08-出场设置功能（标准库）/`） |

## 各工程串口输出速查

| 板 | 串口 | 典型输出 |
|---|---|---|
| 网关 | USART1 9600 | `store: send start:<size>` / `ready` / `done sum=...` / `send begin: N bytes, M frames` / `send: got ready, streaming` / `send done: M frames, sum=...` |
| 接收板 | USART1 9600 | `recv: size=3884, erasing...` / `recv: ready` / `recv: stored sum=... size=..., flag set, reset to update!` |
| 07 bootloader | USART1 9600 | `bootloader start` / `ee:01 5A 6B` / `update` / `copy ok -> 0x08008000` / `jump app 0x08008000` |

## 已知边界

- CAN 帧间隔 5ms + 接收端 FIFO(3) 的消化设计实测无丢帧；若自行提高 CAN 波特率，需同步核对接收端消化能力；
- 网关 CAN 自动重发已关闭（AutoRetransmission=DISABLE），传输期间接收端发其他帧可能因仲裁致网关帧丢失——接收端在接收过程中已停止发送其他帧；
- 07 bootloader 的 16KB 分区已满（16384/16384），修改 07 代码需同步瘦身。
