# ESP32-DIV-main（自研 V2 复刻板）长期记忆

> **2026-09-21 工程已搬迁**：工作区根为 `C:\Users\cxw\Desktop\工程项目\ESP32-DIV交接\`，
> 项目本体在其下的 **`ESP32-DIV-main\`**，`.workbuddy\` 与 `memory\` 也都在 `ESP32-DIV-main\` 内（**不在根**）。
> 同为 `ESP32-DIV交接\` 下的其余目录：`板子原理图\`、`编译环境相关资料\`、`测试代码以及文档\`、`项目交接文档.docx`。
> 编译环境（arduino-cli + `C:\Users\cxw\Documents\Arduino\libraries`）**未受影响，仍然有效**。

## 硬件事实（已实测/查证，改动前必读）

### 模组：ESP32-S3-WROOM-1，丝印第二行 `MCN16R8`
- = **16 MB Flash + 8 MB 八线（Octal/OPI）PSRAM**。（`MC` 为批次前缀，`N16`=16MB Flash，`R8`=8MB 八线 PSRAM）

### ⛔ PSRAM 必须保持 `PSRAM=disabled` —— 这是硬约束，不是疏忽
- 八线 PSRAM 占用 **GPIO33–37**（SPIIO4/5/6/7/DQS）。
- 本板**显示与触摸正好在这三根上**：`TFT_MOSI 35 / TFT_SCLK 36 / TFT_MISO 37`
  （生效配置见下），触摸 XPT2046 同为 35/37/36 + CS 18。
- 屏幕现在能亮，**正是因为 PSRAM 未初始化**（PSRAM 芯片一直未选中/高阻，35–37 才可当普通 IO）。
- **一旦把 IDE 的 PSRAM 打开（尤其 "OPI PSRAM"），八线接口会接管 35/36/37 → 屏幕和触摸失效。**
  恢复：改回 `PSRAM=disabled` 重烧；进不去下载就 BOOT + RESET。
- ⇒ 涉及"内存不够"的问题，**不要建议开 PSRAM**。

### 显示 / 触摸引脚
- 生效的 TFT 配置在 **`C:\Users\cxw\Documents\Arduino\libraries\TFT_eSPI-master\User_Setup.h`**
  （Arduino 实际用的是 `Documents\Arduino\libraries`，**不是**工程里的 `Libraries/` 副本；
  两份 md5 相同，但改库必须两边同步）。用 build 目录的 `.d` 依赖文件可确认。
- `USE_HSPI_PORT` + `ILI9341_DRIVER` + `TFT_MISO 37 / MOSI 35 / SCLK 36 / CS 17 / DC 16 / RST 0`
- XPT2046（`shared.h` V2）：MOSI 35 / MISO 37 / CLK 36 / CS 18，`TOUCH_SHARES_TFT_SPI = 0`
  （独立 SPIClass，物理引脚相同）
- SD 走 `s_sdSpi = SPIClass(FSPI)`，显示走 HSPI ⇒ **不同 SPI 主机**，显示刷新不会干扰 SD。

### GPIO 占用（V2）—— 2026-09-23 全量重新核准
```
0=TFT_RST(+BOOT strapping)  1=PN532 CS(原 WS2812 焊盘)  2=电量 ADC
3=CC1101 GDO2(SUBGHZ_RX)  4=nRF24#1 CSN  5=CC1101_CS  6=CC1101 GDO0(SUBGHZ_TX)
7=背光  8/9=I2C(PCF8574+IP5306)  10=SD_CS  11/12/13=共享 FSPI(含 PN532 SPI)
14=nRF24#3 CE  15=nRF24#1 CE  16=TFT_DC  17=TFT_CS  18=触摸 CS  19/20=USB D±
21=nRF24#3 CSN  35/36/37=TFT HSPI  38=SD_CD  39=USB 插入检测  40=IR RX
41/42=GPS  46=IR TX  47=nRF24#2 CE  48=nRF24#2 CSN
```
**★ 已无空闲脚**（`GPIO1` 已分配给 PN532 的 CS）。
**不可用**：19/20=USB D±（占 USB-JTAG）、22–25 在 S3 上不存在、
43/44=UART0 控制台、45=strapping、**33/34 模组未引出**、26–32=内部 Flash

**⚠️ GPIO1 现在的作用与注意点（2026-09-23 最终定案）**
- **PN532 的 CS**（2026-09-23 从 I2C 改回 SPI 后，RSTPDN 挪到了硬件 10k 上拉到 3V3）
- 上面仍焊着 WS2812 的 DIN ⇒ SPI 事务会让它**随机闪**（电气无害，介意就拆）
- 历史教训（I2C 时代）：该脚曾被用作 RSTPDN，而固件从不 `pinMode(1,…)` ⇒
  高阻≈悬空 ⇒ PN532 吊在复位态拖坏 I2C ⇒ **键盘探测失败 ⇒ 五个按键全废**。
  ⇒ **凡把外设接到"固件原本不碰"的引脚上，必须在 `setup()` 最早期显式初始化它。**

### PN532（RFID/NFC，外部模块）
**现行配置（2026-09-23 起）：走软件 SPI(bitbang)**，`RFID_USE_I2C = 0`。
生效引脚 **SCK 12 / MISO 11 / MOSI 13 / CS 1**，RSTPDN 硬件 10k 上拉到 3V3。
**⚠️ MISO/MOSI 与 SD/CC1101 交叉**（共享总线是 MISO 13 / MOSI 11），接错完全不通信。
✅ **用户实测定案**：按"同名对齐"接 → 完全找不到设备（裸 SPI 读回 0xFF）；
**把 MISO/MOSI 对调后完全正常，且读取速度比 I2C 快得多**。
改 SPI 的原因见下方"I2C 时代"一节：PN532 会拖死共享 I2C，
而键盘(PCF8574)和 PMIC(IP5306)也在上面 ⇒ 整机卡死且要断电。
**用户实测"拔掉 PN532 就不卡"确认了根因**，故改为总线隔离。

**软件 SPI 还是硬件 SPI**：**软件（bitbang）**。
`Adafruit_PN532(clk,miso,mosi,ss)` 4 参数构造 → `Adafruit_SPIDevice` 4 脚构造
**不设 `_spi`** ⇒ `begin()` 走 `else` 走 pinMode/digitalWrite，全靠 GPIO 位操作，
**不占硬件 FSPI 外设**（原设计刻意如此，避免与 SD/CC1101 抢总线）。
速率约 400kHz~1MHz（GPIO 翻转限制），但全双工 + 无 ACK + 无 clock stretch
⇒ 实际远快于 100kHz 的 I2C。想更快可换硬件 SPI，但要接 FSPI + 严格 CS 管理，不建议。

### PN532「I2C 时代」的结论（历史，切回时看）

**这套默认值只对 CYD 成立 → V1/V2 全是坏的（2026-09-23 查证）**
- CYD 分支 18/19/23/**25**：25 在 CYD 上空闲（CYD 的 CC1101_CS=27、SD_CS=5）⇒ 不冲突 ✓
- **V1：`PN532_SS=5` == `SD_CS=5`** ✗
- **V2：`PN532_SS=5` == `CC1101_CS=5`** ✗ ⇒ V2 上**绝不能按默认值接线**

**冲突的电路级后果**：PN532 选中时 CC1101 同时被选中 → CC1101 的 MISO 驱动 GPIO13，
而 GPIO13 正是 PN532 的 MOSI（ESP 在推）⇒ **GPIO13 双向争用**；
CC1101 还会在被选中状态下收到乱时钟 ⇒ **可能误写寄存器**。
另 **MISO/MOSI 与共享总线（SD/CC1101/nRF24 是 MISO 13 / MOSI 11）交叉**。

**库事实**：`Adafruit_PN532` **v1.3.4**（`depends=Adafruit BusIO`，已装 1.17.4）。
4 参数构造 `(clk,miso,mosi,ss)` = 软件 SPI；`Adafruit_SPIDevice` 的 4 脚构造函数
**不设 `_spi`**（保持 NULL），`begin()` 走 `else` 分支做 pinMode/digitalWrite
⇒ **真 bitbang，不碰硬件 FSPI**（rfid.cpp:43 的注释是对的）。
**`PN532_IRQ` 全工程 0 引用 ⇒ IRQ 不用接**；**RSTPDN 必须接**（见下）。

**⚠️ RSTPDN 必须接、且固件必须主动驱动（2026-09-23 实测）**
本模块的 RSTPDN **内部没有上拉** —— 悬空时芯片停在复位态，**连 I2C 地址都不应答**
（现象：总线扫描里 0x24 完全消失）。现已接到 **GPIO1** 并把 `PN532_I2C_RST` 设为 1。
且 **setup 第一件事必须 `bootReleasePn532Reset()` 把它驱动为高** ——
否则从上电到第一次进 RFID 之间它一直是高阻（= 悬空），PN532 会拖坏 I2C 总线，
导致 **PCF8574 探测失败 ⇒ 五个按键全废**（详见上文 GPIO1 的两条硬规则）。

**当前实现：路线 A（I2C），2026-09-23 落地并编译通过**
`shared.h` 新增 **`RFID_USE_I2C`（V2 默认 1）**、`PN532_I2C_IRQ/RST`（默认 -1，不接）、
`PN532_I2C_TIMEOUT_MS`(200)、`I2C_BUS_TIMEOUT_MS`(50)。
`rfid.cpp` 构造按宏分支为 `Adafruit_PN532(PN532_I2C_IRQ, PN532_I2C_RST, &Wire)`；
`rfidAttachBus()`/`rfidRestoreBus()` 做成传输感知（**这两个函数被调用各 40+ 次，
所以只改它们就覆盖了全部 8 个会话**，会话代码零改动）；
`begin()` 用 `rfidI2cWiden/Restore` 包住探测。`utils.cpp` 里 PN532 的 SPI 引脚处理
加 `&& !RFID_USE_I2C`。**符号表已证 `Adafruit_SPIDevice` 不再被引用。**

**按键故障的判据（重要，别再往按键逻辑里查）**
`isPhysicalButtonPressed()`(ESP32-DIV.ino:706) 开头是
`if (getPcf8574Address() != 0) {...} return false;`
⇒ **`s_pcf8574Addr == 0` 时五个键恒返回"未按下"，且屏幕无任何异常。**
所以"按键完全没反应"先查这个；而"按键恒被按下、到处乱跳"才是 I2C 读失败
（PCF8574 库读失败时保持旧缓冲 → `digitalRead()` 返回 LOW → `!LOW` = 按下）。
**"能 ACK" ≠ "能读数据"** —— 启动体检条 `bootI2cSelftest()` 会读**原始字节**
（空闲应为非 0，正常 0xFF；读到 `0x00` = 5 键同按或总线被拖），
用来区分这两件事。开关 `BOOT_I2C_SELFTEST`（异常时停 4s、正常 2.5s）。
**体检条必须带闸门**：先 ping 常驻器件 0x20，它不应答就直接报 `BUS STUCK`
并**跳过全扫** —— 否则总线坏时 117 个地址 × 50ms ≈ 5.9s 纯黑屏，会被当成死机。

### ⚠️ I2C 总线锁死：主机复位解不开，只有从机断电才行（2026-09-23）

**症状指纹**：设备变慢 → 卡死；**按复位键重启也没用，必须整个断电**。
原因：从机（PN532）在传输中途被打断后，会停在"还剩几位没吐完"的状态**继续拉低
SDA**；此后主机发不出 START，每次都吃满超时。而**主机复位不会让从机释放总线**。

**修法**：`i2cBusRecover()`（utils.cpp）—— SCL 当普通 GPIO 打 9 个时钟 +
在 SCL 高时做 SDA 低→高的 STOP。
`setup()` 里在 `Wire.begin()` 之前调用（必须早于外设接管引脚）。
`I2C_BUS_RECOVER` / `I2C_SDA_PIN`(8) / `I2C_SCL_PIN`(9) 可配。

**⚠️ 判据必须两条线都查（2026-09-23 踩过的坑）**：最初写成
`if (digitalRead(SDA) == HIGH) return false;` —— 结果**完全没用**：
PN532 卡住时拉低的往往是 **`SCL`**（clock stretch 不释放），那种情况下 SDA 仍是高，
恢复流程根本没被触发，用户只能断电。
正确判据：`if (sdaHigh && sclHigh) return false;`（只有两条都高才是真空闲）。

### ⚠️ 库的读卡是阻塞式的 —— 会把整个 UI 拖死（2026-09-23）

`readPassiveTargetID()` → `sendCommandCheckAck()` → `waitready()`
**按 10ms 步进空转**到超时；`readdata()` 还**完全没有就绪检查**（无条件读 21 字节）。
按键/触摸都在主循环轮询 ⇒ 每次读卡都把 UI 排在 I2C 后面，
表现就是"扫卡特别慢 + 触屏按键基本卡死"。

**修法**：`rfidPollTag()`（rfid.cpp）拆成「发一次命令」+「每帧只查 RDY 位」：
`startPassiveTargetIDDetection()` → 之后每帧 `rfidI2cReadyBit()`（Wire 直读
1 字节 == 0x01，代价几百 µs）→ 只有 ready 才 `readDetectedPassiveTargetID()`。
`rfidI2cReadyBit()` 自己实现，因为库的 `isready()` 是 private；
用公开宏 `PN532_I2C_ADDRESS`(0x24) / `PN532_I2C_READY`(0x01)。
SPI 模式保持原阻塞调用。
注：`rfidTagStillPresent()`（Dump 中确认卡还在）**保留**阻塞版 —— 它要同步结论。

### ⚠️ 进 RFID 不要再无条件 pulse RSTPDN（2026-09-23）

PN532 正在工作时被突然复位，其 I2C 引脚可能进到不确定状态**把 SDA 拉住**
（即上面那条总线锁死的成因之一）。改成：先 ping 0x24 → 在就别碰；
不在才复位 + `i2cBusRecover()`。另：能 ACK 但拿不到固件版本时复位重试一次。

**会话看门狗**：`rfidListenIso14443a` 里 `s_pollMs > RFID_BUS_STALL_MS`(700ms)
连续 `RFID_BUS_STALL_LIMIT`(3) 次 ⇒ 解总线 + 报 `[!] I2C bus stalled` + 退出会话。
**外接模块走杜邦线时优先试 `I2C_BUS_CLOCK_HZ = 50000`**（默认 100000）。

| 路线 | 接线 | 状态 |
|---|---|---|
| **A. I2C** | SDA→8 / SCL→9（取点在 PCF8574T(U9) 的 15/14 脚） | ✅ **已实现（V2 默认）** |
| B. SPI 改 CS | SCK12/MISO11/MOSI13/**SS1** | 未采用（WS2812 已焊，会闪） |
| C. SPI 转正 | SCK12/MISO**13**/MOSI**11**/SS1 + 硬件 SPI 重载 | 未采用 |

**I2C 相关库事实（已核实，别重查）**
- 4 参构造 = 软件 SPI（`Adafruit_SPIDevice` 4 脚构造不设 `_spi` ⇒ 真 bitbang）。
- `wakeup()` 按传输分支；I2C 下只 `SAMConfig()`，不做 CS 拉低。
- `reset()` 有 `if (_reset != -1)` 保护 ⇒ RST 可传 -1；
  `isready()` I2C 下读 RDY 字节、**不用 IRQ 脚**。
- `TwoWire::begin()` **可重入**（已初始化时 log_w + 返回 true）；
  `TwoWire` 内置 FreeRTOS 互斥锁 ⇒ 与 `statusBarTask` 的 IP5306 轮询自动串行。
- 传输上限 `I2C_BUFFER_LENGTH=128`；最大写是 `setDataTarget` 的 74 字节，**不撞限**。
- 传 -1 会走一次 `pinMode(-1,…)`，被 `GPIO_IS_VALID_GPIO()` 拦掉 +
  `CORE_DEBUG_LEVEL=0` 时 `log_e` 编译掉 ⇒ 无副作用。

**原理图（`板子原理图/_crop_mcu.png`）**
PCF8574T(U9)：A0/A1/A2 经 **R28/R29/R32 = 1k** 下拉 ⇒ 地址 **0x20**；
**14 脚 = SCL、15 脚 = SDA**（T 型 SO16 引脚序，不是 DIP 的 9 脚！）。
SD 卡座：CS→IO10、DI→MOSI、CLK→CLK、DO→MISO、SHIELD→IO38（另有 R1–R4 10k 上拉）。
**GPIO0 既是 TFT_RST 又是 BOOT 键**（经 R21 1k）。

**代码完整度**：`rfid.cpp` 2633 行 / 86 函数，8 会话全部实装，**无 TODO/空壳**（bitbang 交叉线是唯一隐患）。

### GPS（ATGM336H）
- `GPS_UART_RX = 41` / `GPS_UART_TX = 42`（`shared.h` V2 分支）。
- 原默认 5/6 与 CC1101_CS / GDO0 同脚，**必须**用 41/42：GPS 上电即持续吐 NMEA，
  物理常连通，会在 CC1101 片选线上制造跳变。

## 编译 / 验证约定
- 离线编译：`C:/Program Files/Arduino IDE/resources/app/lib/backend/resources/arduino-cli.exe`
- FQBN 抄自 `%LOCALAPPDATA%\arduino\sketches\<hash>\build.options.json`
- 常用 FQBN：`esp32:esp32:esp32s3:UploadSpeed=921600,USBMode=hwcdc,CDCOnBoot=default,MSCOnBoot=default,DFUOnBoot=default,UploadMode=default,CPUFreq=240,FlashMode=qio,FlashSize=4M,PartitionScheme=min_spiffs,DebugLevel=none,PSRAM=disabled,LoopCore=1,EventsCore=1,EraseFlash=all,JTAGAdapter=default`
- `--build-path` 必须用 `C:/...` 盘符形式（Git Bash 的 `/c/...` 会拼成 `C:\c\...`）。
- 诊断打印总开关：`shared.h` 的 `DIAG_PRINTS_ENABLED`（0=静默）。
- 关注 Warning：**必须零告警**才算通过。
- 当前占用：sketch 约 1775 KB / 1966080 (90%)，RAM 135 KB (41%)。
  ⇒ Flash 实际 16 MB 但 `FlashSize=4M` 只用了 1/4，必要时可改 16M 抬高 APP 分区上限。

## 中文化（i18n）约定
- 生成器：`.workbuddy/i18n/gen_all.py`（词表内联在脚本的 `ZH` 字典里），
  一条命令重生成 `i18n_table.h` + `zh_font.h/.cpp`（字库按译文自动推导，不会漏字）。
- 字形/翻译工具：`dump_glyph.py`（把字解回字符画）、`edit_glyph.py`（单字微调）。
- 绘制必须走 `ui*` 镜像 API；**离屏 sprite 走 `uiGfxDrawString(Gfx&, ...)`**
  （`i18n.h` 的泛型内核；状态要显式传参，sprite 没有 getter）。
- 汉字 12px 比 font1 的 8px 行高上溢 2px ⇒ 清屏矩形上边减 3（`UI_CJK_TOP_PAD`）、
  上一行是汉字时下一行要下移 1~3px，否则被底色削掉笔画。

## 息屏（Screen Off / 低亮度电量屏）架构 —— 2026-09-21~22 建立

**设置项**：Settings 第 7 行 `Screen Off` → `AppSettings::screenOffIdx`
（Off/15s/30s/1min/2min/5min，预设表在 `SettingsStore.cpp`），**默认 Off**。

**为什么计时不能只挂主 loop**：每个功能都是 `while(!feature_exit_requested){ X::loop(); }`
阻塞在 `handleButtons()` 里，主 loop 的尾巴在功能运行期间不执行。
⇒ 计时推进挂在**输入读取的咽喉**上：`readTouchXY()` / `readTouchXYDismiss()`（Touchscreen.cpp）
与 `isButtonPressed()` / `isButtonPressedEdge()`（ESP32-DIV.ino），主 loop 只补一次。

**`utils.cpp` 里的四个出入口**：`screenNoteActivity()` 刷时间戳、
`screenIdleTick()` 推进超时、`screenFilterInput()` 输入闸门（熄屏后第一下只点亮，250ms 内吞掉余振）、
`screenIsOff()`。两个注册式回调：`screenSetWakeHook()`（唤醒整体重绘）、
`screenSetAutoOffGate()`（此刻是否允许息屏）。

**四条必须记住的坑**（都是实测踩出来的）：
1. **唤醒分支必须调 `screenNoteActivity()`**。否则唤醒后 `s_screenOff` 已 false，
   下一拍用旧时间戳判定"仍超时" → 亮一下又黑，表现为"点屏幕像卡死"。
2. **`drawStatusBar()` / `updateStatusBar()` 开头必须 `if (screenIsOff()) return;`**。
   `statusBarTask` 是独立 FreeRTOS 任务、每 400ms 因 WiFi/BLE 计数置脏，
   主 loop 就会把状态栏画到电量屏上。
3. **闸门抑制期间要持续刷新时间戳**，否则从功能退出时"一退出就立刻黑屏"。
4. **动态画面功能要禁息屏**（名单 `kDynamicScreenFeatures[]`，在 ESP32-DIV.ino，
   按 `active_submenu_items[current_submenu_index]` 的**名字**匹配）。
   判断用 `autoOffAllowed()`：`!feature_active` 允许、`feature_active && !in_sub_menu`
   （Settings/关于页）允许、其余功能屏只有在名单里才禁。

**电量屏（AOD）**：`screenDrawAod()`，竖版电池 112×148 + 下方黄插头，整组垂直居中，
低亮度 `SCREEN_OFF_DIM_LEVEL=24`。刷新走**区域刷新**：电量变化只动"高度差细条" + 数字框 92×50，
插头变化只动 36×42；`SCREEN_OFF_AOD_RESYNC_MS=10s` 兜底整屏重画。
⚠ 电量条必须用**方角** `fillRect`（圆角没法做精确的差值/交集）。
⚠ 数字用 font 6（27px/位，数字专用字体**没有 `%` 字模**），`%` 必须换 font 2/4；
居中不能用 `setTextDatum`（`uiPrint` 的 ASCII 路径转发 `tft.print`，不认 datum），
要用 `tft.textWidth(s,font)` 实测宽度自己定位。详见 skill `embedded-tft-cjk-i18n`。

**主固件编译验证不要加 `--warnings=all`**（平台 `-Werror=` 会把工程里原有的
unused-variable/function 升级成 error）。

## ⚠ I2C 总线死了会「假按键」把 UI 锁死 —— 写任何模态等待前必读

**`PCF8574::digitalRead()` 在 I2C 读失败时不报错，会返回 LOW。**
库实现里先 `value = (bit(pin) & readModePullUp) ? HIGH : LOW;`，读失败时
`byteBuffered` 不更新，最后走到 `bit(pin) & byteBuffered` 为 0 的分支 → **返回 LOW**。
而 `isPhysicalButtonPressed()` 是 `return !pcf.digitalRead(pin);`
⇒ **总线一死，所有按键都被读成"一直按住"。**

**后果**：任何 `while (isButtonPressed(...))` / `while (!isButtonPressed(...))`
的无超时循环都会**永久空转**，表现为「屏不动、点不了、退不出」。
实测踩点：`otherDismissPlaceholder()`（进 RFID 失败后的提示框收起逻辑）—— 已加上限
（释放等 3s / 等点击 8s）。

**约定**：本工程任何"等用户输入"的模态循环**必须带超时**，不能信任
`isButtonPressed()` 在总线故障时的语义。

**另一个同源坑**：ESP32 `Wire.setTimeOut()` 同时就是"总线异常时每次事务阻塞多久"的上限。
库里的 `waitready(100)` 是**按 10ms 步进**空转到超时的，总线异常时每步还要再吃一次
`Wire` 超时 ⇒ 把 `Wire` 超时放宽 4 倍，进功能的最坏阻塞就从 ~1.3s 变成 ~5s。
⇒ 别为了"容忍 PN532 clock stretch"大幅放宽它（**`PN532_I2C_TIMEOUT_MS` 现为 50，
即 core 默认，不要再放宽** —— 放宽会把故障阻塞按倍数放大）。
更稳的做法是**先用 `i2cPing(addr)` 做纯 ACK 闸门**
（`beginTransmission` + `endTransmission()==0`，健康总线 ~0.1ms），
不应答就别进慢探测 —— 见 `otherRfidPlaceholderAction()`。

**屏幕自检条**：`otherRfidProbeBanner()` 会在进 RFID 前打出
`I2C 0x20:? 0x75:? 0x24:?`。判读：0x20(键盘)/0x75(IP5306) 一起 `--` ⇒
总线被外接模块拖死（硬件）；只有 0x24 `--` ⇒ PN532 本身没应答。
> `0x75:--` **单独**出现是正常的（电池冷启动时 IP5306 的 I2C 本就不 alive），不是故障。

> ⚠ 上面写的 `PN532_I2C_TIMEOUT_MS` **现为 50**（不要再放宽，理由见下）。

## ⚠ PN532「无限重试」会把整个 UI 拖慢 —— `PN532_PASSIVE_RETRIES`

**`setPassiveActivationRetries(0xFF)` 是陷阱，不要用。** 它的含义是 PN532
**一直开着 RF 场反复轮询、直到找到卡才报"就绪"**；没有卡时永远不就绪。连锁反应：

```
readPassiveTargetID → sendCommandCheckAck → waitready(150)   // 10ms 步进 ⇒ 16 次迭代
每一步的 isready() 都是一次 I2C 读，而忙着的 PN532 会把 SCL 拉住（clock stretch）
⇒ 该读要等满 Wire 超时 50ms
⇒ 单次 readPassiveTargetID ≈ 16 × (50+10) ≈ 960ms
⇒ 会话界面约 1 秒才刷一帧，且芯片始终"忙"，后续命令也全都要等超时
```
**实测症状**：会话界面半天不刷新（**只有独立任务 `statusBarTask` 画的那条栏还在动**）、
读卡特别慢。改成 `PN532_PASSIVE_RETRIES = 2` 后芯片迅速空闲；
检测能力不受影响（上层本来就在 20s 窗口里反复调用它）。

**一条反直觉但重要的判据**：
**I2C 从机不存在 = 立即 NACK（毫秒级），不吃 `Wire` 超时**；
真正吃满超时的是**存在但忙 / 正在 clock stretch** 的设备。
⇒ 遇到"慢"要往"设备在但不应答或忙"方向查，别往"设备不在"方向查。

**相关**：I2C 模式下 `rfidAttachBus()` 必须保持**空操作**（模块不占 SPI 总线，
init / SAMConfig / 重试次数在 `begin()` 里配一次就够）。沿用 SPI 模式那套
"每个会话重挂一次总线"，会让每个会话白付几百 ms。
