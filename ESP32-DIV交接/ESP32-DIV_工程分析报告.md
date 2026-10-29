# ESP32-DIV 工程全面分析报告

> 分析日期：2026-10-10  
> 分析对象：`ESP32-DIV-main/ESP32-DIV/`（V2 板，ESP32-S3，Arduino 框架，约 52,600 行 C++）  
> 关键 bug 均已逐行核对源码验证，行号以当前文件为准。



---

## 一、工程概览

| 文件                            | 行数                    | 职责                                                                         |
| ----------------------------- | --------------------- | -------------------------------------------------------------------------- |
| wifi.cpp                      | 10,669                | 12 个 WiFi 子功能（PacketMonitor/Beacon Flood/Evil Portal/Deauther/Karma/OTA…）  |
| bluetooth.cpp                 | 9,585                 | 14 个 BLE/2.4G 子功能（Spoofer/SourApple/AirTag/BleJammer/ESB 嗅探与注入/MouseJack…） |
| ESP32-DIV.ino                 | 4,974                 | 主入口 + 菜单/路由 + 所有功能启动器（`handleButtons()` 巨型分发）                              |
| subghz.cpp                    | 4,405                 | SubGHz Replay/配置库/Jammer/SubBrute/干扰检测（CC1101）                             |
| gps.cpp                       | 4,251                 | 卫星扫描 + Wardriver（NMEA 解析、CSV、WiGLE 上传）                                     |
| utils.cpp                     | 4,197                 | 电池/状态栏/息屏/按键/通知/设置 UI/串口监视器/SD 管理器（"第二个主文件"）                               |
| ir.cpp / rfid.cpp / ducky.cpp | 3,167 / 2,896 / 1,476 | 红外 / PN532 / BLE HID DuckyScript                                           |
| shared.h / config.h           | 1,127 / 208           | 板级宏（V1/V2/CYD 三板）+ 上帝头文件                                                   |

**核心架构事实**：所有功能运行在 `handleButtons()` 的内嵌 `while(!feature_exit_requested)` 中（.ino:4680-4838），主 loop 在功能运行期间完全不执行，息屏/提示音只能寄生在输入函数里补偿。 Tools 菜单已抽象出正确的通用启动器 `runToolsFeature()`（.ino:3550-3591），证明重构路径可行，只是没推广。

---

## 二、已验证的确定性 Bug（P0，建议立即修）

### B1. Deauth 帧越界写（wifi.cpp:3094、3102、4316）

```cpp
uint8_t deauth_frame_default[26] = {...};
memcpy(deauth_frame, deauth_frame_default, 26);
deauth_frame[26] = 7;          // ← 数组 26 字节，合法下标 0-25，越界写 1 字节
wsl_bypasser_send_raw_frame(deauth_frame, sizeof(deauth_frame));
```

意图是给 offset 24-25 的 reason code 赋值，正确写法 `deauth_frame[25] = 7;`。静默踩坏链接布局上紧邻的下一个全局变量。  
**修法**：3 处 `[26] = 7` → `[25] = 7`（10 分钟）。

### B2. V2 Jammer 驱动错误引脚（subghz.cpp:2492、2526、2762、2948）

```cpp
digitalWrite(TX_PIN, HIGH);   // shared.h:633 → V2 上 TX_PIN = GPIO3
// 而 CC1101 发射调制脚是 SUBGHZ_TX_PIN = GPIO6（shared.h:721）
```

GPIO3 同时是 `SUBGHZ_RX_PIN`（CC1101 GDO2，被 RCSwitch 中断监听）。CYD/V1 上两宏恰好相等所以历史未暴露；**V2 上 jammer 的载波通断控制作用在错误引脚**，还可能自触发 ISR 风暴。  
**修法**：4 处 `TX_PIN` → `SUBGHZ_TX_PIN`；这两处 digitalWrite 对异步载波模式本就多余，也可直接删除（10 分钟）。

### B3. SourApple 广播 UUID 无限累积 → 堆耗尽（bluetooth.cpp:1175）

```cpp
// sourappleLoop() 每轮（~25Hz）都执行：
Advertising->addServiceUUID(device_uuid);   // NimBLE 内部 vector push_back 无去重
```

长时间运行必然堆耗尽/复位。同文件 AirTagSpoofer 已用 `s_advConfigured` 标志修掉同一 bug（bluetooth.cpp:1527-1536，注释自己写了教训），SourApple 和 BleSpoofer（:771）没跟进。  
**修法**：照抄 AirTagSpoofer 模式，`addServiceUUID` 移到 setup 一次性执行（3 行）。

### B4. Ducky BLE 安全配置从未生效（ducky.cpp:1140-1142）

```cpp
auto* sec = new NimBLESecurity();
sec->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_BOND);
sec->setCapability(BLE_HS_IO_NO_INPUT_OUTPUT);
// ← 缺 NimBLEDevice::setSecurity(sec); 配置根本没注册进协议栈
```

这正是 iOS/Android 上偶发"配对失败"的典型根源。顺带：三个 `new` 的回调对象从不 delete（一次性小泄漏）。  
**修法**：补一行 `NimBLEDevice::setSecurity(sec);`（1 行，直接影响配对成功率）。

### B5. CaptivePortal EEPROM 布局重叠（wifi.cpp:2807-2812、2915-2918）

`CRED_ADDR=32, CRED_SIZE=64, MAX_CREDS=20` → 第 20 条凭据地址 = 32+19×64 = **1248，恰好覆盖 COUNT_ADDR 计数器**。保存第 20 条会写坏计数器（`clearAllCredentials` 已在为此打补丁）。  
**修法**：`MAX_CREDS` 20→19，或 COUNT_ADDR 移到 1312 之后。

### B6. NRF 菜单越界检查用错常量（.ino:2658-2671）

`handleNRFSubmenuButtons()` 用 `NUM_SUBMENU_ITEMS`（=12，WiFi 菜单条目数）给 7 项 NRF 菜单做边界检查——`%` 在前所以当前无害，但这是复制粘贴未改净的实锤，改写法即越界。  
**修法**：改用 NRF 自己的条目数常量（1 分钟）。

### B7. Scanner 噪声校准数学错误（bluetooth.cpp:4271-4307）

外层只采样 2 轮，却 `backgroundNoise[i] /= 5`；且校准结果此后从未参与任何判决——整个校准是无效功。  
**修法**：`/5` → `/2`，或整段删除。

---

## 三、高风险结构性缺陷

### R1. 61 处无超时忙等可致整机卡死

`while (isButtonPressed(BTN_SELECT)) {}`（.ino 共 61 处）。`isButtonPressed()` 走 PCF8574 I2C 读，总线故障时引脚读成 LOW ⇒ "永远按住" ⇒ 死循环断电才能解。**代码自己在 .ino:3662-3697 已写出带超时的修复版**（`otherDismissPlaceholder()`），但只用在 Other 菜单一处。  
**修法**：抽成 `waitSelectReleaseWithTimeout()` 放 utils，全局替换。半天工作量，消除最高频现场故障。

### R2. BleSniffer 在 NimBLE 回调任务里画屏（bluetooth.cpp:9460-9483 → 9102）

`onResult` 回调直接调 `tft.fillRect/uiPrint`，与主 loop 的绘制并发 → TFT SPI 总线竞争，花屏/死锁。同文件 AirTagSniffer/BleSkimmer 的正确范式（回调只更新数据 + portMUX，主循环刷屏）已存在。  
**修法**：回调只置脏标志，绘制移到主循环（~40 行）。

### R3. BLE 前后台扫描数据竞争（bluetooth.cpp:3447-3448、3546-3575）

后台任务写 `bleResults`（普通全局，非 volatile）时，前台正在遍历读取 → 半更新状态。`fgBleScanInProgress` 只挡"叠加扫描"，不挡"读时写"。  
**修法**：portMUX 或进入前台前先停后台扫描（~15 行）。

### R4. EsbSniffer 每包两次 SPI 总线重配（bluetooth.cpp:5913-5936）

每收一个包：`esbSwitchToSdSpi`（重映射）→ 写 SD → `esbInitRadioSpi + 完整重配 nRF24`。高流量时丢包严重、SD 吞吐极差。  
**修法**：4KB RAM 环形缓冲批量落盘（~50 行）。

### R5. GPS NMEA 无校验和验证（gps.cpp:632-656）

UART 噪声坏帧直接驱动 `navLat/navLon`，坏坐标会写进 Wardriver CSV。行缓冲边界（128B）倒是防住了。  
**修法**：入口加 10 行 XOR 校验和验证（投入极小、收益直接）。

### R6. GPS 导航量跨任务撕裂读

`navLat/navLon`（double，64 位）由 core0 后台任务写、core1 主循环读，非 volatile 无锁。同文件 `wardLog` 的 `s_wardLogMux`（gps.cpp:1872）是现成的正确范式。  
**修法**：照抄 portMUX 小临界区（2 小时）。

### R7. NVS/EEPROM 写入磨损

- PacketMonitor 每次换信道 `preferences.putUInt`（wifi.cpp:607-609）；
- SubGHz 每次 RSSI 锁定/解码 commit（subghz.cpp:765-766、1662-1676）。  
  **修法**：脏标志 + 退出功能或 5s 防抖提交。

### R8. Deauther/ProbeRequestFlood 无 teardown（wifi.cpp:4751-4856、5439-5543）

退出后 SoftAP（"ESP32-DIV"/"deauth123"）残留、`ap_list` malloc 不释放、promiscuous 不关。同文件 HiddenSsidReveal 的 `teardown()`（6571-6579）是现成模板。BeaconSpammer 洪水循环还有重复 `esp_wifi_init()`（1194）且无 `feature_exit_requested` 检查。

---

## 四、代码质量量化

| 指标                            | 数值                                                                               | 说明                                                                                 |
| ----------------------------- | -------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------- |
| 功能启动样板块                       | 30+ 份                                                                            | `feature_exit_requested = false` 出现 226 次；Tools 菜单已有正确抽象未推广                        |
| 触摸分支复制                        | 整块复制                                                                             | .ino:1637-2007 把按键分支 11 个功能块逐字复制一遍                                                 |
| `while(isButtonPressed){}` 忙等 | 61 处                                                                             | 无超时，见 R1                                                                           |
| `delay(200)`                  | 仅 .ino 就 172 处                                                                   | 菜单连按响应 ≥200ms/步                                                                    |
| wifi.cpp 内重复                  | ~35%                                                                             | Deauther 与 ProbeRequestFlood 整段 ~700 行逐字重复；runUI 模板 12 份；RSSI 比较器 5 处；26 字节帧模板 3 份 |
| bluetooth.cpp 内重复             | runUI ×11、日志缓冲 ×3、nRF24 寄存器助手 ×4、`configureRadio` ×2                             | 估 600+ 行可收敛                                                                        |
| RF 模块重复                       | 图标工具栏 runUI ×7、按键去抖 ×5、频点表 ×3、换行打印 ×3                                            | 估 ~1500 行可收敛                                                                       |
| 超长函数                          | .ino 820/645/475 行 handler；`GpsWardriver::session` 460 行；`performSDUpdate` 244 行 |                                                                                    |
| String 使用                     | 全工程 274 处                                                                        | 终端日志滚动逐行堆分配；HTML 字符串 ~6-8KB 常驻 RAM 未用 PROGMEM                                      |
| 头文件里 `using namespace std;`   | config.h:42、wificonfig.h:35                                                      | 污染                                                                                 |
| 函数体内 `#define SCREEN_WIDTH`   | wifi.cpp 内 8+ 处                                                                  | 宏从定义点泄漏到文件尾                                                                        |

---

## 五、架构问题

1. **无分层**：主文件 + 9 个平级功能大文件 + utils"杂物间"，靠全局对象互相穿透。`tft`/`pcf` 被 12 个文件重复 extern；`feature_active`（UI 状态）竟定义在触摸驱动 Touchscreen.cpp:20；`isButtonPressed()` 声明在 utils.h 实现在 .ino——主文件反向实现库接口。
2. **config.h 是上帝头文件**：一口气 include 30+ 库 + 声明全部 feature namespace，改任何库 include 全量重编。
3. **僵尸头文件**：wificonfig.h/bleconfig.h/subconfig.h 与 config.h 内容重复（config.h:2 注释自认"merged from"），各自重复 extern + `using namespace std;`。
4. **双任务并发无同步**：statusBarTask（core0）写电池状态，主 loop（core1）读，均非 volatile，靠 32 位对齐读写侥幸正确。
5. **i18n 以英文原文为 key**：改一个菜单词就静默丢翻译；`T()` 线性扫描 646 行表且每次 UI 绘制都扫两遍；`kDynamicScreenFeatures[]`（.ino:4609-4630）再拿菜单字符串做息屏豁免身份——同一字符串"翻译/显示/豁免"三处共用。
6. **V1/V2 宏集中但语义混杂**：编译期差异（引脚/尺寸）与运行时规避（"same crash as step 3"，step 3 已无从考证）混在同一套宏里。

---

## 六、修改建议路线图（按投入产出比）

### 第一步：P0 修复（约 1 天，全是小改动）

| #  | 内容                                                                                                          | 位置                     | 工作量    |
| -- | ----------------------------------------------------------------------------------------------------------- | ---------------------- | ------ |
| 1  | B1 越界写 3 处 `[26]`→`[25]`                                                                                    | wifi.cpp               | 10 分钟  |
| 2  | B2 jammer 引脚 4 处 `TX_PIN`→`SUBGHZ_TX_PIN`                                                                   | subghz.cpp             | 10 分钟  |
| 3  | B3 SourApple/Spoofer 的 addServiceUUID 移出循环                                                                  | bluetooth.cpp:1175、771 | 10 分钟  |
| 4  | B4 ducky 补 `setSecurity` 注册                                                                                 | ducky.cpp:1142 后       | 1 行    |
| 5  | B5 EEPROM 布局 MAX_CREDS 20→19                                                                                | wifi.cpp:2807          | 10 分钟  |
| 6  | B6 NRF 边界常量改正                                                                                               | .ino:2658              | 1 分钟   |
| 7  | R1 忙等加超时（全局替换为带超时版）                                                                                         | .ino 61 处              | 半天     |
| 8  | R5 NMEA 校验和                                                                                                 | gps.cpp:632            | 20 行   |
| 9  | `DIAG_PRINTS_ENABLED` 改回 0（shared.h:925，注释自认"临时打开"）                                                         |                        | 1 分钟   |
| 10 | 死代码清理：`submenu_items[12]`/`wifi_submenu_icons[12]` 僵尸数组、2 个 .bak、3 个 .bin 移出源码树、config.h 重复声明、`icon_colors` |                        | 1-2 小时 |

### 第二步：结构性重构（约 1-2 周，收益最大）

1. **推广 `runToolsFeature` 模式到全部菜单**：定义 `FeatureDef { name, setup, loop, exit, dynamicScreen, touchNav }`，把 ~30 个复制块收敛为一张表 + 一个通用 `runFeature()`。同时把息屏豁免的字符串匹配改为表字段。**预计 .ino 从 4974 行降到 ~2000 行，消除 226 处标志位散写。全工程 ROI 最高。**
2. **抽取公共 UI 组件**：`FeatureUI::runIconToolbar()`（收敛 7+11+12 份 runUI，约 -3000 行）、日志环形缓冲组件（wifi ×3、BT ×3、IR/RFID ×2）、`drawListCommon()`（ArpScanner 已有正确版本，wifi.cpp:7403）。
3. **拆分巨型文件**：wifi.cpp → 每个 namespace 一个 `feature/xxx.cpp`（声明保留在 wificonfig.h）；删除 Deauther/Probe 的 ~700 行重复。
4. **收口公共 API**：`isButtonPressed()` 挪进 utils.cpp，`feature_active` 定义挪出 Touchscreen.cpp。

### 第三步：性能与健壮性（按需排期）

- HTML 字符串加 `PROGMEM` + `send_P`（省 ~6KB RAM，30 分钟）；
- String→栈缓冲优先做 V1 路径（bluetooth 80 处、ir 55 处）；
- WiGLE 上传移入后台任务，UI 轮询完成标志（当前上传期间界面冻结 30-70s）；
- EsbSniffer SD 批量写（R4）；BLE 扫描互斥（R3）；GPS 量加锁（R6）；
- EEPROM/NVS 提交节流（R7）；Deauther/Beacon 补 teardown（R8）；
- ducky 解释器：执行可中断（当前 `REPEAT 999999` 假死）、行长上限 512、修正 `REPEAT`/`DELETE`/`STRING` 前导空格语义、支持连字符组合键；
- `T()` 加首字符分桶（646 条线性扫 → ~25 条比较）；
- 板级引脚表化：shared.h 500 行 `#if` 块 → 每板一个 `BoardPins` 结构体；
- 电池状态变量加 volatile（止血）；中期把 IP5306 读移回主 loop 低频轮询。

### 值得保留的优点（重构时不要破坏）

- **V1/V2 宏分支总体集中可读**（shared.h 约 500 行三路分支）；
- **i18n 模板内核**（i18n.h:36-109）与 SettingsStore、ip5306 边界清楚，是全工程质量最高的部分，可作重构锚点；
- **硬件正确性防线是踩坑补出来的、质量不错**：PN532 总线挂接/归还（rfid.cpp:127-161）、ELECHOUSE 库 MISO 死等的 `s_replayRadioReady` 门禁（subghz.cpp:919-922）、GDO2 上拉防 ISR 风暴（1462-1467）、Karma 的 portMUX、GPS sprite 内存 ladder、AirTagSpoofer 的广播修复范式——这些"带注释的踩坑记录"是工程最宝贵的资产。

---

## 七、遗留文档问题

- **rfid.h:15-16 仍写"I2C 模式（V2 默认）"**，与 shared.h:492 的 `RFID_USE_I2C=0`（2026-09-23 起 V2 用 SPI bitbang）矛盾——交接文档级错误，应更正并把 I2C 分支代码收进文件底部 legacy 段；
- rfid.cpp `sessionCardReader` 精心拼装 640 字节 body 后 `(void)body;` 从未显示（:2445-2483）；`rfidRefreshInfoPanel` 是空函数却被每块调用 8 处——建议接入显示或删除；
- V2 板 RFID SPI 分支的总线看门狗（连续 N 次满超时才触发）几乎不可触发，实际失效，建议改为重跑 `rfidAttachBus()` 自愈。
