# ESP32-DIV 项目长期笔记

## 电池电量（V2，2026-10-09 标定修正）
- **分压比 = 2:1（K=2.01）**：实测 VBAT=3.570V 时 IO2=1.776V。旧记录"6.6:1 / 588mV"为误测，作废。
- `BATTERY_VDIV_MULT 2.01f`（shared.h V2 分支，~959 行）；有效窗口 (2.5,4.6)V 在 utils.cpp readBatteryVoltage()（~923 行，5 次取中位数初检 + 16 次均值）；判定失败回退 IP5306，本板 IP5306 不在线 → 返回 0 → 状态栏 "--"。
- USB 插入检测 = IO39 纯数字（黄插头标），不经过 ADC 换算；蜂鸣器硬件在 IO2 与电池分压撞脚，固件 `BUZZER_PIN -1` 让位给 ADC。
- 验证手段：`DIAG_PRINTS_ENABLED 1` 时串口每 3s 打 `[IP5306] ... Vbat=xxxmV src=A adc=xxxmV`；src=A = ADC 主源生效（当前已临时打开）。

## RFID / PN532（V2 最终方案 = SPI）
- **V2 用 SPI（软件 bitbang，库 4 参数构造），不用 I2C。** 根因：I2C 下 PN532 会把共享总线拖死（clock stretch 拉低 SCL / 拉低 SDA），键盘 PCF8574(0x20) 与 PMIC IP5306(0x75) 同总线，整机变慢至卡死，且主机复位解不开、须断电。实测"拔掉模块就不卡"确认根因。
- 引脚（**交叉接法**，与 SD/CC1101 同名网络相反）：PN532 SCK→GPIO12、MISO→GPIO11、MOSI→GPIO13、CS→**GPIO1**。MISO/MOSI 接反会"找不到设备"（用户曾手动对调两根线解决）。
- RSTPDN → **硬件 10k 上拉到 3V3**，固件不驱动（PN532_I2C_RST=-1）；GPIO1 因此让给 CS（原 WS2812 DIN 随 CS 翻转会随机闪，可接受）。
- 相关宏见 shared.h：RFID_USE_I2C=0、PN532_SCK/MISO/MOSI/SS、PN532_I2C_RST=-1。
- 进 RFID 失败时才跑裸 SPI 诊断（rfidSpiRawStatusPins，测 N=定义 / S=对调两种映射）帮排查接线，正常路径不调用。

## 编译约束（硬）
- FQBN 必须 **PSRAM=disabled**（模组 MCN16R8 八线 PSRAM 占 GPIO33-37，开则屏黑）；V2 分区 **huge_app(3.3 MB)**、FlashSize=8M。
- 本机（Administrator）esp32 core **2.0.10**（`AppData/Local/Arduino15/packages/espressif/hardware/esp32/2.0.10`）；库在 `C:/Users/Administrator/Documents/Arduino/libraries`。
- arduino-cli 1.5.1：`C:/Users/Administrator/AppData/Local/Programs/Arduino IDE/resources/app/lib/backend/resources/arduino-cli.exe`
- esp32s3 FQBN **没有 FlashFreq 选项**（只有 FlashMode）；分区值写作 `huge_app`（带下划线，CLI 校验会拒收 hugeapp）。
- 主固件**不能加 --warnings=all** 编译：platform.txt 自带 -Werror=unused-*，工程原有未用变量会变 error。
- IDE 缓存 core.a 残缺会导致 core 符号 undefined；改用 arduino-cli 干净 --build-path 可规避。全量编译约 4~5 分钟，bash 超时须 >10 分钟（默认 120s 会杀进程且无输出）。
- 会话坑：命令行传输出现过字符级变形（FQBN 键名被改写 → CLI 静默失败 exit 1 无输出）。规避：流程写进纯 ASCII 脚本文件，路径用 find/cygpath 运行时探测，后台跑 + 重定向日志再读。
