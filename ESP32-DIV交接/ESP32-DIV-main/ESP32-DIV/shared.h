#ifndef SHARED_H
#define SHARED_H
#include <stdint.h>
#include "BoardConfig.h"

/* ===== 临时屏幕调试层（SCREEN_DEBUG）=====
 * 串口芯片损坏时的替代查看手段：把原本发往串口的调试日志镜像到屏幕底部。
 *   SCREEN_DEBUG = 1：DBG() 同时写串口（无害，串口坏了）与上屏环形日志。
 *   SCREEN_DEBUG = 0：DBG() 退化为 Serial.printf，完全恢复原始程序行为。
 * 调试完成后撤除：将本宏改 0 即恢复纯串口；若要彻底删除屏幕逻辑，
 * 删掉 utils.cpp 的 screenDbg* 实现 + loop() 里的 screenDbgRedraw() 调用 +
 * 把各处的 DBG()/DBGln() 改回 Serial.printf()/Serial.println() 即可。 */
#ifndef SCREEN_DEBUG
#define SCREEN_DEBUG 0
#endif
void screenDbgPush(const char* s);
void screenDbgPushf(const char* fmt, ...);
void screenDbgRedraw();
#if SCREEN_DEBUG
  #define DBG(fmt, ...)    do { Serial.printf(fmt, ##__VA_ARGS__); screenDbgPushf(fmt, ##__VA_ARGS__); } while (0)
  #define DBGln(s)         do { Serial.println(s); screenDbgPush(s); } while (0)
#else
  #define DBG(fmt, ...)    Serial.printf(fmt, ##__VA_ARGS__)
  #define DBGln(s)         Serial.println(s)
#endif

/*──────────────────── Colors ────────────────────*/
const uint16_t GRAY = 0x8410, BLUE = 0x001F, RED = 0xF800,
               GREEN  = 0xB721, BLACK = 0x0000, WHITE = 0xFFFF,
               LIGHT_GRAY = 0xC618, DARK_GRAY = 0x4208;

uint16_t uiUniversalColor();
#define ORANGE uiUniversalColor()
               
#define TFT_DARKBLUE   0x3166
#define TFT_LIGHTBLUE  0x051F
#define TFTWHITE       0xFFFF
#define TFT_GRAY       0x8410

#ifdef TFT_GREEN
#undef TFT_GREEN
#endif
#define TFT_GREEN GREEN
#ifdef TFT_GREENYELLOW
#undef TFT_GREENYELLOW
#endif
#define TFT_GREENYELLOW GREEN

#define BG_Dark        0x20e4
#define BG_Light       0xf7de
#define FG_Dark        0x3166
#define FG_Light       0xe73c
#define LINE_Dark      0x8410  
#define LINE_Light     0x8410  
#define ICON_Dark      0xFBE4 
#define ICON_Light     0xFBE4
#define TEXT_Dark      0xFFFF
#define TEXT_Light     0x0000   
#define UI_ACCENT      0x3166

#define L_Dark        0x4208
#define L_Light       0xC618

// Default background for feature screens (separate from main menu background).
// Currently black, but you can change this one define to restyle all features.
#ifndef FEATURE_BG
#define FEATURE_BG BLACK
#endif

#define SELECTED_ICON_COLOR UI_ICON

/*──────────────────── UI Defaults ────────────────────*/
#ifndef UI_BG
#define UI_BG UI.bg
#endif
#ifndef UI_FG
#define UI_FG UI.fg
#endif
#ifndef UI_ICON
#define UI_ICON UI.icon
#endif
#ifndef UI_TEXT
#define UI_TEXT UI.text
#endif
#ifndef UI_LINE
#define UI_LINE UI.line
#endif
#ifndef UI_LABLE
#define UI_LABLE UI.lable
#endif
#ifndef UI_ACCENT
#define UI_ACCENT UI.accent
#endif
#ifndef UI_WARN
#define UI_WARN UI.warn
#endif
#ifndef UI_OK
#define UI_OK GREEN
#endif

/*──────────────────── Project Info ────────────────────*/
#ifndef ESP32DIV_NAME
#define ESP32DIV_NAME "ESP32-DIV"
#endif
#ifndef ESP32DIV_VERSION
#define ESP32DIV_VERSION "V2.1"
#endif


static inline constexpr uint8_t k0() { return (uint8_t)('h' - '`'); }

// 关于页信息（2026-10-10 定制：作者 SZ-plaza、网址 www.sz-plaza.com，邮箱/GitHub 已移除）。
// 数组为 XOR(k0()=8) 混淆，tftPrintObf() 解码显示；注释里标明文，改文字时按此重编码。
static const uint8_t OBF_PN[]   = {77, 91, 88, 59, 58, 37, 76, 65, 94};                              // "ESP32-DIV"
static const uint8_t OBF_DN[]   = {91, 82, 37, 120, 100, 105, 114, 105};                             // "SZ-plaza"
static const uint8_t OBF_WB[]   = {96, 124, 124, 120, 50, 39, 39, 127, 127, 127, 38, 123, 114, 37, 120, 100, 105, 114, 105, 38, 107, 103, 101}; // "http://www.sz-plaza.com"
 
/*──────────────────── Board Selection ────────────────────*/
// Default is selected in BoardConfig.h. You can also pass a BOARD_* define
// from your build flags to target a board without editing source.
#if !defined(BOARD_ESP32_DIV_V2) && !defined(BOARD_CYD) && !defined(BOARD_ESP32_DIV_V1)
#define BOARD_ESP32_DIV_V2
#endif

#if (defined(BOARD_ESP32_DIV_V2) + defined(BOARD_CYD) + defined(BOARD_ESP32_DIV_V1)) > 1
#error "Select only one board: BOARD_ESP32_DIV_V2, BOARD_ESP32_DIV_V1, or BOARD_CYD"
#endif

#if defined(BOARD_CYD)
#ifndef ESP32DIV_BOARD_NAME
#define ESP32DIV_BOARD_NAME "CYD ESP32-2432S028R"
#endif
#ifndef HAS_PCF8574_BUTTONS
#define HAS_PCF8574_BUTTONS 0
#endif
#ifndef BOARD_HAS_ESP32S3
#define BOARD_HAS_ESP32S3 0
#endif
#elif defined(BOARD_ESP32_DIV_V1)
#ifndef ESP32DIV_BOARD_NAME
#define ESP32DIV_BOARD_NAME "ESP32-DIV V1 ESP32U"
#endif
#ifndef HAS_PCF8574_BUTTONS
#define HAS_PCF8574_BUTTONS 1
#endif
#ifndef BOARD_HAS_ESP32S3
#define BOARD_HAS_ESP32S3 0
#endif
#elif defined(BOARD_ESP32_DIV_V2)
#ifndef ESP32DIV_BOARD_NAME
#define ESP32DIV_BOARD_NAME "A04"
#endif
#ifndef HAS_PCF8574_BUTTONS
#define HAS_PCF8574_BUTTONS 1
#endif
#ifndef BOARD_HAS_ESP32S3
#define BOARD_HAS_ESP32S3 1
#endif
#else
#error "Unknown board: define BOARD_ESP32_DIV_V2, BOARD_ESP32_DIV_V1, or BOARD_CYD"
#endif

/* v1 ESP32 has less internal DRAM for static buffers; v2/S3 keeps full sizes. */
#if BOARD_HAS_ESP32S3
#define ESP32DIV_FFT_SAMPLES       256
#define ESP32DIV_FFT_PALETTE_SIZE  128
#define ESP32DIV_PKT_GRAPH_WIDTH   240
#define ESP32DIV_JD_WAVE_WIDTH     220
#define ESP32DIV_JD_RSSI_SAMPLES   128
#define ESP32DIV_BLE_SCANNER_BARS  128
#define ESP32DIV_BLE_SCANNER_CHANS 128
#define ESP32DIV_ESB_RP_CAPTURES   8
#define ESP32DIV_ESB_RP_SEL_LINES  8
#define ESP32DIV_ESB_RP_LOG_LINES  10
#define ESP32DIV_MAX_WIFI_NETWORKS 50
#define ESP32DIV_PCAP_POOL_SIZE    10
#define ESP32DIV_PCAP_SNAP_LEN     2324
#define ESP32DIV_RFID_SRC_PAGES    256
#else
#define ESP32DIV_FFT_SAMPLES       256
#define ESP32DIV_FFT_PALETTE_SIZE  128
#define ESP32DIV_PKT_GRAPH_WIDTH   240
#define ESP32DIV_JD_WAVE_WIDTH     220
#define ESP32DIV_JD_RSSI_SAMPLES   64
#define ESP32DIV_BLE_SCANNER_BARS  64
#define ESP32DIV_BLE_SCANNER_CHANS 64
#define ESP32DIV_ESB_RP_CAPTURES   4
#define ESP32DIV_ESB_RP_SEL_LINES  4
#define ESP32DIV_ESB_RP_LOG_LINES  6
#define ESP32DIV_MAX_WIFI_NETWORKS 28
#define ESP32DIV_PCAP_POOL_SIZE    3
#define ESP32DIV_PCAP_SNAP_LEN     512
#define ESP32DIV_RFID_SRC_PAGES    128
#endif

/*──────────────────── Touch calibration profiles ────────────────────*/
/* Factory defaults per board (raw XPT2046 range). Override in BoardConfig.h.
 * User-saved calibration in settings.json overrides when board id matches. */
#if defined(BOARD_CYD)
#ifndef TOUCH_PROFILE_ID
#define TOUCH_PROFILE_ID "CYD"
#endif
#ifndef TOUCH_X_MIN
#define TOUCH_X_MIN 200
#endif
#ifndef TOUCH_X_MAX
#define TOUCH_X_MAX 3700
#endif
#ifndef TOUCH_Y_MIN
#define TOUCH_Y_MIN 240
#endif
#ifndef TOUCH_Y_MAX
#define TOUCH_Y_MAX 3800
#endif
#elif defined(BOARD_ESP32_DIV_V1)
#ifndef TOUCH_PROFILE_ID
#define TOUCH_PROFILE_ID "ESP32_DIV_V1"
#endif
#ifndef TOUCH_X_MIN
#define TOUCH_X_MIN 300
#endif
#ifndef TOUCH_X_MAX
#define TOUCH_X_MAX 3800
#endif
#ifndef TOUCH_Y_MIN
#define TOUCH_Y_MIN 300
#endif
#ifndef TOUCH_Y_MAX
#define TOUCH_Y_MAX 3800
#endif
#elif defined(BOARD_ESP32_DIV_V2)
#ifndef TOUCH_PROFILE_ID
#define TOUCH_PROFILE_ID "ESP32_DIV_V2"
#endif
#ifndef TOUCH_X_MIN
#define TOUCH_X_MIN 280
#endif
#ifndef TOUCH_X_MAX
#define TOUCH_X_MAX 3850
#endif
#ifndef TOUCH_Y_MIN
#define TOUCH_Y_MIN 320
#endif
#ifndef TOUCH_Y_MAX
#define TOUCH_Y_MAX 3750
#endif
#endif

#ifndef TOUCH_PROFILE_ID
#define TOUCH_PROFILE_ID "GENERIC"
#endif
#ifndef TOUCH_X_MIN
#define TOUCH_X_MIN 300
#endif
#ifndef TOUCH_X_MAX
#define TOUCH_X_MAX 3800
#endif
#ifndef TOUCH_Y_MIN
#define TOUCH_Y_MIN 300
#endif
#ifndef TOUCH_Y_MAX
#define TOUCH_Y_MAX 3800
#endif

#ifndef TFT_ROTATION
#if defined(BOARD_CYD) || defined(BOARD_ESP32_DIV_V1)
#define TFT_ROTATION 0
#else
#define TFT_ROTATION 2
#endif
#endif

#ifndef TOUCH_SHARES_TFT_SPI
/* CYD uses a dedicated VSPI touch bus (T_CLK/T_DIN/T_OUT on 25/32/39); TFT stays on HSPI. */
#define TOUCH_SHARES_TFT_SPI 0
#endif

#if defined(BOARD_CYD)
#ifndef TOUCH_ROTATION
/* Match TFT_ROTATION (RNT CYD test uses the same rotation for tft and touch). */
#define TOUCH_ROTATION TFT_ROTATION
#endif
#endif

/*──────────────────── I/O & Pins ────────────────────*/
// PCF8574 I2C address: auto-detect 0x20-0x27 by default.
// To force a fixed address, add to BoardConfig.h: #define pcf_ADDR 0x21
#ifndef pcf_ADDR
#define PCF8574_AUTO_DETECT 1
#define PCF8574_I2C_ADDR  0x20
#else
#define PCF8574_AUTO_DETECT 0
#define PCF8574_I2C_ADDR  pcf_ADDR
#endif
#define PCF8574_ADDR_MIN 0x20
#define PCF8574_ADDR_MAX 0x27
#if defined(BOARD_ESP32_DIV_V1)
#define BTN_UP       6
#define BTN_DOWN     3
#define BTN_LEFT     4
#define BTN_RIGHT    5
#define BTN_SELECT   7
#else
#define BTN_UP       7
#define BTN_DOWN     5
#define BTN_LEFT     3
#define BTN_RIGHT    4
#define BTN_SELECT   6
#endif

/* Buzzer */
#ifndef BUZZER_PIN
// 2026-10-09：蜂鸣器飞线从 IO2 改到 IO45（模组脚位 26），解除与电池分压 ADC 的撞脚，
// IO2 全权让给 BATTERY_ADC_PIN。IO45 是 S3 strapping 脚（VDD_SPI 电压选择），
// 仅作 LEDC 输出无碍；若上电有短鸣/常鸣需检查蜂鸣器驱动极性。
// SubGHz 检测提示音走 subghz.cpp 的 replayBeep()（LEDC 通道 7）。
#define BUZZER_PIN 45
#endif

/* Backlight / PWM */
#ifndef BACKLIGHT_PIN
#if defined(BOARD_CYD)
#define BACKLIGHT_PIN   21
#elif defined(BOARD_ESP32_DIV_V1)
#define BACKLIGHT_PIN   32
#else
#define BACKLIGHT_PIN   7
#endif
#endif
#define PWM_CHANNEL     0
#define PWM_FREQ        5000
#define PWM_RESOLUTION  8

/* XPT2046 (Touch) SPI */
#ifndef XPT2046_CS
#if defined(BOARD_CYD)
#define XPT2046_CS   33
#elif defined(BOARD_ESP32_DIV_V1)
#define XPT2046_CS   33
#else
#define XPT2046_CS   18
#endif
#endif
#ifndef XPT2046_MOSI
#if defined(BOARD_CYD)
#define XPT2046_MOSI 32
#elif defined(BOARD_ESP32_DIV_V1)
#define XPT2046_MOSI 32
#else
#define XPT2046_MOSI 35
#endif
#endif
#ifndef XPT2046_MISO
#if defined(BOARD_CYD)
#define XPT2046_MISO 39
#elif defined(BOARD_ESP32_DIV_V1)
#define XPT2046_MISO 35
#else
#define XPT2046_MISO 37
#endif
#endif
#ifndef XPT2046_CLK
#if defined(BOARD_CYD)
#define XPT2046_CLK  25
#elif defined(BOARD_ESP32_DIV_V1)
#define XPT2046_CLK  25
#else
#define XPT2046_CLK  36
#endif
#endif
#ifndef XPT2046_IRQ
#if defined(BOARD_CYD)
#define XPT2046_IRQ  36
#elif defined(BOARD_ESP32_DIV_V1)
#define XPT2046_IRQ  34
#else
#define XPT2046_IRQ  255
#endif
#endif

/* SD Card */
#ifndef SD_CS
#if defined(BOARD_CYD)
#define SD_CS    5
#elif defined(BOARD_ESP32_DIV_V1)
#define SD_CS    5
#else
#define SD_CS    10
#endif
#endif
#ifndef SD_MOSI
#if defined(BOARD_CYD)
#define SD_MOSI  23
#elif defined(BOARD_ESP32_DIV_V1)
#define SD_MOSI  23
#else
#define SD_MOSI  11
#endif
#endif
#ifndef SD_MISO
#if defined(BOARD_CYD)
#define SD_MISO  19
#elif defined(BOARD_ESP32_DIV_V1)
#define SD_MISO  19
#else
#define SD_MISO  13
#endif
#endif
#ifndef SD_SCLK
#if defined(BOARD_CYD)
#define SD_SCLK  18
#elif defined(BOARD_ESP32_DIV_V1)
#define SD_SCLK  18
#else
#define SD_SCLK  12
#endif
#endif
#if !defined(BOARD_CYD) && !defined(BOARD_ESP32_DIV_V1) && !defined(SD_CD)
#define SD_CD    38
#endif
#ifndef SD_CS_PIN
#define SD_CS_PIN 5
#endif

/* PN532 RFID/NFC (SPI).
 * CYD has few spare GPIOs; these are suggested external wiring defaults.
 * Override any pin below if your wiring differs. Requires Adafruit PN532 library. */
#ifndef PN532_SCK
#if defined(BOARD_CYD)
#define PN532_SCK  18
#else
#define PN532_SCK  12
#endif
#endif
#ifndef PN532_MISO
#if defined(BOARD_CYD)
#define PN532_MISO 19
#else
#define PN532_MISO 11
#endif
#endif
#ifndef PN532_MOSI
#if defined(BOARD_CYD)
#define PN532_MOSI 23
#else
#define PN532_MOSI 13
#endif
#endif
#ifndef PN532_SS
#if defined(BOARD_CYD)
#define PN532_SS   25
#elif defined(BOARD_ESP32_DIV_V2)
/* V2：CS 接到 **GPIO1**（原 WS2812 焊盘）。
 * 不能用默认的 5 —— 那正是板上 CC1101_CS。GPIO1 是 V2 上唯一的空闲脚，
 * 而它原先被 RSTPDN 占着；改走 SPI 后 RSTPDN 改为**硬件 10k 上拉到 3V3**
 * （固件不再驱动它），GPIO1 就腾出来做 CS。
 * 副作用：WS2812 的 DIN 随 CS 翻转会随机闪 —— 不介意就留着，介意就拆掉。 */
#define PN532_SS   1
#else
#define PN532_SS   5
#endif
#endif

/* ── PN532 传输方式 ────────────────────────────────────────────────
 * 1 = I2C，0 = 软件 SPI（bitbang）。
 *
 * 【2026-09-23：V2 从 I2C 改回 SPI】
 * 曾经为绕开引脚冲突把 V2 改成 I2C（零新增引脚），但实测确认：
 * **PN532 会把这条件共享 I2C 总线拖死**（clock stretch 卡住 / 拉低 SDA），
 * 而键盘(PCF8574)和 PMIC(IP5306) 也在同一条总线上 —— 于是整机变慢直到卡死，
 * 且主机复位解不开、要断电。用户实测：**拔掉 PN532 模块就不卡了**，根因确认。
 * 换成 SPI 后 PN532 走自己的 4 根线，I2C 上只剩板上原装器件，
 * 就算 PN532 再闹脾气，影响面也只局限在 RFID 功能内部。
 *
 * SPI 布线要点（与共享总线**交叉**）：
 *   PN532 SCK → 12、MISO → 11、MOSI → 13、CS → **GPIO1**
 *   （SD/CC1101 的 MISO 是 13、MOSI 是 11，所以是交叉的，别接错）
 *   RSTPDN → **10k 上拉到 3V3**（固件不再驱动；悬空则芯片不应答）
 * SPI 模式用库的 bitbang（4 参数构造），所以**不占硬件 FSPI 外设**；
 * 但与 SD/CC1101 共用 11/12/13，进出功能时要靠 restoreSdAfterSharedSpi()
 * 重挂 SD（本文件下方及 utils.cpp 已处理）。
 *
 * 想切回 I2C：把 V2 分支改成 1，并恢复 PN532_SS / PN532_I2C_RST 的旧值。
 *
 * I2C 侧备忘：地址固定 0x24（库内 0x48>>1），与 PCF8574(0x20) / IP5306(0x75)
 * 不冲突；SDA/SCL 用 ESP32-S3 变体默认的 GPIO8/GPIO9（板上已有 10k 上拉）。
 * IRQ/RST 默认 -1 = 不接。 */
#ifndef RFID_USE_I2C
#if defined(BOARD_ESP32_DIV_V2)
/* 2026-09-23：V2 改回 SPI。I2C 下 PN532 会拖死共享总线（键盘/PMIC 也在上面），
 * 实测"拔掉模块就不卡"确认根因。详见下方传输方式说明。 */
#define RFID_USE_I2C 0
#else
#define RFID_USE_I2C 0
#endif
#endif
#ifndef PN532_I2C_IRQ
#define PN532_I2C_IRQ (-1)
#endif
#ifndef PN532_I2C_RST
#if defined(BOARD_ESP32_DIV_V2)
/* V2：-1 = 固件不驱动 RSTPDN。
 * 改走 SPI 后 GPIO1 让给了 CS，RSTPDN 改为**硬件 10k 上拉到 3V3** ——
 * 芯片上电即自动释放，不再需要固件管（也少一个由软件时序引入的失效点）。
 * 为什么必须拉高：RSTPDN 悬空时 PN532 停在复位态、**连地址都不应答**。 */
#define PN532_I2C_RST (-1)
#else
#define PN532_I2C_RST (-1)
#endif
#endif
/* PN532 探测/会话期间的 I2C 超时。
 * ⚠ 结论先行：**保持与板上常规值一致（50ms），不要放宽。**
 *   实测教训：Adafruit 库的 `waitready(timeout)` 是**按 10ms 步进空转**到超时的，
 *   每一步里还各含一次 I2C 事务，各吃一次本超时。所以
 *     单次 waitready ≈ 10 × (本超时 + 10ms)，
 *     sendCommandCheckAck 内部调两次，begin() 里共 3 个命令，
 *   把本值从 50 放宽到 100 会让最坏阻塞从 ~3s 变成 ~7s，观感就是"卡死"。
 *   ESP32 core 的 `_timeOutMillis` 默认就是 50ms，Adafruit 自己的 PN532 I2C
 *   例程也用它、从未调过 —— 50ms 对 PN532 的 clock stretch 是够的。
 *   只有在拿到"确实是 stretch 导致读失败"的实测证据时才考虑调大。 */
#ifndef PN532_I2C_TIMEOUT_MS
#define PN532_I2C_TIMEOUT_MS 50
#endif
/* 板上 I2C 总线的常规超时（initPcf8574Buttons 里设置，防总线卡死触发 WDT）。 */
#ifndef I2C_BUS_TIMEOUT_MS
#define I2C_BUS_TIMEOUT_MS 50
#endif

/* 上电即把 PN532 的 RSTPDN 驱动为高（= 当前 PN532_I2C_RST 所指的脚）。
 * 现状：V2 已改走 SPI，RSTPDN 交给**硬件 10k 上拉到 3V3**，PN532_I2C_RST = -1，
 * 所以本开关目前是空操作 —— 保留它是为了切回 I2C 时不再踩同一个坑：
 *   那时若 setup 不主动驱动 RSTPDN 脚，从上电到第一次进 RFID 之间它是高阻
 *   （对芯片 = 悬空），PN532 会吊在复位态并扰动 I2C，导致紧随其后的
 *   PCF8574 地址探测失败 ⇒ 五个按键全部按不动，且屏幕上看不出任何异常。 */
#ifndef BOOT_RELEASE_PN532
#define BOOT_RELEASE_PN532 1
#endif

/* 启动时的 I2C 体检（SDA/SCL 静态电平 + 总线扫描 + 读 PCF8574 原始字节）。
 *   0 = 关
 *   1 = **只在异常时显示**（默认）—— 正常路径不画屏、不停留，不给开机加延时
 *   2 = 每次都显示（排查用）
 * 为什么要留着：键盘(PCF8574)探不到时 `isPhysicalButtonPressed()` 会直接
 * 短路成 false，五个键全废而屏幕毫无提示 —— 那时这两行是唯一的线索。 */
#ifndef BOOT_I2C_SELFTEST
#define BOOT_I2C_SELFTEST 1
#endif

/* I2C 总线锁死恢复：SCL 上打 9 个脉冲 + 生成 STOP，逼从机把 SDA 放掉。
 * **为什么必须有**：从机在传输中途被干扰（或半死不活）时会一直拉低 SDA，
 * 而**主机复位不会让从机释放总线** —— 没有这个恢复流程，唯一的出路就是
 * 给整个板子断电。实测症状：RFID 卡死后"按复位键重启也没用、必须关电源"。
 * 引脚用 ESP32-S3 变体默认的 SDA=8 / SCL=9。 */
#ifndef I2C_BUS_RECOVER
#define I2C_BUS_RECOVER 1
#endif
#ifndef I2C_SDA_PIN
#define I2C_SDA_PIN 8
#endif
#ifndef I2C_SCL_PIN
#define I2C_SCL_PIN 9
#endif
/* 板上 I2C 时钟。默认 100kHz 是 I2C 标准速率。
 * 外接模块走杜邦线时，线容 + 上拉偏弱会把上升沿拖缓，表现为"能通但很慢/
 * 偶发丢包"。这种情况把本值降到 50000 往往立刻变稳（代价：21 字节读从
 * 约 2ms 变 4ms，可忽略）。 */
#ifndef I2C_BUS_CLOCK_HZ
#define I2C_BUS_CLOCK_HZ 100000
#endif

/* 单次被动读卡耗时超过这个值，就说明这条命令在吃满 I2C 超时（而不是正常轮询）。
 * 连续 RFID_BUS_STALL_LIMIT 次即判定总线异常，主动退出会话把界面还回去 ——
 * 否则 UI 会一直排在 I2C 后面，观感就是"触屏和按键全卡死"。 */
#ifndef RFID_BUS_STALL_MS
#define RFID_BUS_STALL_MS 700
#endif
#ifndef RFID_BUS_STALL_LIMIT
#define RFID_BUS_STALL_LIMIT 3
#endif

/* 一次 InListPassiveTarget 命令最多等多久的回音（非阻塞轮询用）。
 * 正常无卡时应 50~150ms 内返回（MxRtyPassiveActivation 个轮询周期）；
 * 超过就从"等"改成"重发"，避免一个卡住的命令把倒计时一直挂着。 */
#ifndef RFID_TAG_CMD_RETRY_MS
#define RFID_TAG_CMD_RETRY_MS 1000
#endif

/* 进 RFID 时在屏幕上打"探测分步 + 每帧耗时 P=/F="（画在自检条下方）。
 * 排查"卡在哪一步/被 I2C 阻塞多久"时打开；正常使用置 0（默认）。
 * 注意：置 0 只是不显示，s_pollMs / s_frameMs 仍会统计 —— 会话看门狗要用。 */
#ifndef RFID_PROBE_TRACE
#define RFID_PROBE_TRACE 0
#endif

/* PN532 被动激活的最大重试次数（RFConfiguration item 5 的 MxRtyPassiveActivation）。
 *
 * ⚠ **绝对不要用 0xFF（= 无限重试）**。0xFF 的含义是：PN532 会一直开着 RF 场
 *   反复轮询、**直到找到卡才报"就绪"**。没有卡时它永远不就绪 ⇒
 *     每次 readPassiveTargetID 只能靠库里的 waitready() 空转到超时，
 *     而 waitready 按 10ms 步进迭代（16 步）、每步还是一次 I2C 读；
 *     芯片一直"忙"着，这次超时又让后续每条命令的 isready() 都得等超时。
 *   实机表现（实测）：会话界面半天不刷新 —— 只有独立任务画的那条栏还在动；
 *     好不容易刷新之后又"读卡特别慢"。
 * 给小值让每条命令**迅速返回"无卡"**，芯片随即空闲，轮询就轻快了；
 * 检测能力不受影响：上层本来就在一个 20s 的窗口里反复调用它。
 * 若贴卡检测变迟钝可加大到 4~6；若仍然慢，先量 isready() 的单次耗时。 */
#ifndef PN532_PASSIVE_RETRIES
#define PN532_PASSIVE_RETRIES 2
#endif

/* 单次 readPassiveTargetID 的库内轮询超时（ms）。它同时是"一次调用最多阻塞多久"
 * 的兜底；取小值让 UI 有更多机会刷新。 */
#ifndef PN532_READ_POLL_TIMEOUT_MS
#define PN532_READ_POLL_TIMEOUT_MS 100
#endif

/* UART (if you use hardware serial on external pins) */
#ifndef RX_PIN
#if defined(BOARD_CYD)
#define RX_PIN 35
#elif defined(BOARD_ESP32_DIV_V1)
#define RX_PIN 16
#else
#define RX_PIN 6
#endif
#endif
#ifndef TX_PIN
#if defined(BOARD_CYD)
#define TX_PIN 22
#elif defined(BOARD_ESP32_DIV_V1)
#define TX_PIN 26
#else
#define TX_PIN 3
#endif
#endif

/* Neo-6M GPS — GPS module TX → ESP RX, GPS RX → ESP TX (optional). Uses UART2 by default. */
#ifndef GPS_UART_RX
#if defined(BOARD_CYD)
#define GPS_UART_RX 35
#elif defined(BOARD_ESP32_DIV_V1)
#define GPS_UART_RX 3
#else
// V2: 原为 GPIO5 —— 与 CC1101_CS 同脚。GPS 模块只要上电就持续输出 NMEA，
// 该网络物理上常连通，模块 TX 会不停在 CC1101 片选线上制造跳变，
// 造成随机片选 / 误写寄存器，故必须挪到空闲脚。
// GPIO41 = S3 的 JTAG MTDI：非 strapping 脚；使用内置 USB Serial/JTAG 时
// 可作普通 IO，仅改用外置 JTAG 调试器时才会被征用。
#define GPS_UART_RX 41
#endif
#endif
#ifndef GPS_UART_TX
#if defined(BOARD_CYD)
#define GPS_UART_TX 22
#elif defined(BOARD_ESP32_DIV_V1)
#define GPS_UART_TX 1
#else
// V2: 原为 GPIO6 —— 与 SUBGHZ_TX_PIN / CC1101_GDO0 同脚（Replay/Brute 时
// ESP 高速翻转的发射调制脚），同理挪到空闲脚。
// GPIO42 = S3 的 JTAG MTMS，非 strapping。此脚为可选：
// 仅当需要向模块下发 $PCAS 配置帧（改波特率 / 关语句）时才必须接。
#define GPS_UART_TX 42
#endif
#endif

/* CC1101 (Sub-GHz) */
#ifndef CC1101_SCK
#if defined(BOARD_CYD)
#define CC1101_SCK  18
#elif defined(BOARD_ESP32_DIV_V1)
#define CC1101_SCK  18
#else
#define CC1101_SCK  12
#endif
#endif
#ifndef CC1101_MISO
#if defined(BOARD_CYD)
#define CC1101_MISO 19
#elif defined(BOARD_ESP32_DIV_V1)
#define CC1101_MISO 19
#else
#define CC1101_MISO 13
#endif
#endif
#ifndef CC1101_MOSI
#if defined(BOARD_CYD)
#define CC1101_MOSI 23
#elif defined(BOARD_ESP32_DIV_V1)
#define CC1101_MOSI 23
#else
#define CC1101_MOSI 11
#endif
#endif
#ifndef CC1101_CS
#if defined(BOARD_CYD)
#define CC1101_CS   27
#elif defined(BOARD_ESP32_DIV_V1)
#define CC1101_CS   27
#else
#define CC1101_CS   5
#endif
#endif

/* SubGHz (RCSwitch/Replay) data pins (wired to CC1101 GDO pins) */
// Override these in your board config if your wiring differs.
#ifndef SUBGHZ_RX_PIN
#if defined(BOARD_CYD)
#define SUBGHZ_RX_PIN 35
#elif defined(BOARD_ESP32_DIV_V1)
#define SUBGHZ_RX_PIN 16
#else
#define SUBGHZ_RX_PIN 3
#endif
#endif
#ifndef SUBGHZ_TX_PIN
#if defined(BOARD_CYD)
#define SUBGHZ_TX_PIN 22
#elif defined(BOARD_ESP32_DIV_V1)
#define SUBGHZ_TX_PIN 26
#else
#define SUBGHZ_TX_PIN 6
#endif
#endif

// CC1101 GDO mapping (used by ELECHOUSE_cc1101.setGDO).
// Default follows the legacy wiring used in the SubGHz code: setGDO(TX, RX).
#ifndef CC1101_GDO0
#define CC1101_GDO0 SUBGHZ_TX_PIN
#endif
#ifndef CC1101_GDO2
#define CC1101_GDO2 SUBGHZ_RX_PIN
#endif

/* SubGHz debug (0=off, 1=on) */
#ifndef SUBGHZ_DEBUG
#define SUBGHZ_DEBUG 0
#endif

/* NRF24 */
#ifndef CE_PIN_1
#if defined(BOARD_CYD)
#define CE_PIN_1  16
#elif defined(BOARD_ESP32_DIV_V1)
#define CE_PIN_1  4
#else
#define CE_PIN_1  15
#endif
#endif
#ifndef CSN_PIN_1
#if defined(BOARD_CYD)
#define CSN_PIN_1 17
#elif defined(BOARD_ESP32_DIV_V1)
#define CSN_PIN_1 5
#else
#define CSN_PIN_1 4
#endif
#endif
#ifndef CE_PIN_2
#if defined(BOARD_CYD)
#define CE_PIN_2  22
#elif defined(BOARD_ESP32_DIV_V1)
#define CE_PIN_2  26
#else
#define CE_PIN_2  47
#endif
#endif
#ifndef CSN_PIN_2
#if defined(BOARD_CYD)
#define CSN_PIN_2 27
#elif defined(BOARD_ESP32_DIV_V1)
#define CSN_PIN_2 27
#else
#define CSN_PIN_2 48
#endif
#endif
#ifndef CE_PIN_3
#if defined(BOARD_CYD)
#define CE_PIN_3  4
#elif defined(BOARD_ESP32_DIV_V1)
#define CE_PIN_3  16
#else
#define CE_PIN_3  14
#endif
#endif
#ifndef CSN_PIN_3
#if defined(BOARD_CYD)
#define CSN_PIN_3 25
#elif defined(BOARD_ESP32_DIV_V1)
#define CSN_PIN_3 17
#else
#define CSN_PIN_3 21
#endif
#endif

/* nRF24 bit-bang CE/CSN pair used by Scanner / ESB / MouseJack helpers. */
#ifndef NRF24_SCAN_CE
#define NRF24_SCAN_CE  CE_PIN_3
#endif
#ifndef NRF24_SCAN_CSN
#define NRF24_SCAN_CSN CSN_PIN_3
#endif
/* Shared SPI pinout for those nRF features (must match board SD/CC1101 bus on v1). */
#if BOARD_HAS_ESP32S3
#ifndef NRF24_SPI_SCK
#define NRF24_SPI_SCK  12
#endif
#ifndef NRF24_SPI_MISO
#define NRF24_SPI_MISO 13
#endif
#ifndef NRF24_SPI_MOSI
#define NRF24_SPI_MOSI 11
#endif
#ifndef NRF24_SPI_SS
#define NRF24_SPI_SS   4
#endif
#else
#ifndef NRF24_SPI_SCK
#define NRF24_SPI_SCK  SD_SCLK
#endif
#ifndef NRF24_SPI_MISO
#define NRF24_SPI_MISO SD_MISO
#endif
#ifndef NRF24_SPI_MOSI
#define NRF24_SPI_MOSI SD_MOSI
#endif
#ifndef NRF24_SPI_SS
#define NRF24_SPI_SS   CSN_PIN_1
#endif
#endif

/* IR Remote (Record/Replay)
 * V2: IR_TX = GPIO46, IR_RX = GPIO40 —— 已与 nRF24 #3 的默认接线
 *     (CE_PIN_3 = GPIO14 / CSN_PIN_3 = GPIO21) 完全解耦，红外与 2.4G 可各自独立使用。
 * CYD / V1 分支仍可能与 NRF24 #3 默认接线重叠，如使用需在板卡配置中覆盖。
 */
#ifndef IR_RX_PIN
#if defined(BOARD_CYD)
#define IR_RX_PIN 35
#elif defined(BOARD_ESP32_DIV_V1)
#define IR_RX_PIN 4
#else
// V2: 原为 GPIO21 —— 与 nRF24 #3 的 CSN_PIN_3 同脚（红外接收在 GPIO21 上挂
// CHANGE 中断，而该脚同时是 SPI 片选）。已挪到 GPIO40，把 GPIO21 让回 CSN，
// 红外与 2.4G 彻底解耦。
// GPIO40 = S3 的 JTAG MTDO：eFuse 默认时 JTAG 走内置 USB Serial/JTAG，不占用
// GPIO39-42，故可作普通 IO；仅当改用外部 JTAG 调试器时才会被征用。
#define IR_RX_PIN 40
#endif
#endif
#ifndef IR_TX_PIN
#if defined(BOARD_CYD)
#define IR_TX_PIN 4
#elif defined(BOARD_ESP32_DIV_V1)
#define IR_TX_PIN 5
#else
// V2: GPIO14 与 nRF24 #3 的 CE_PIN_3 共用 —— 红外发射时 IRsend 会把该脚当
// 输出翻转，把 nRF24 #3 的 CE 拉低，干扰 2.4G 功能。故移到 GPIO46，
// 让 GPIO14 归 nRF24 #3 的 CE 独占。
//
// ⚠ GPIO46 是 ESP32-S3 的 strapping 脚（兼 ROM 启动日志开关）：
//   1) 硬件上【严禁加外部上拉/上拉到 VCC 的 LED 驱动】（如 VCC→LED→电阻→GPIO 的
//      灌电流接法）。上电瞬间若被拉到 1，会禁用 ROM 启动日志、并可能与
//      GPIO0=0(BOOT 键) 组成无效组合而进不了下载模式。
//      应使用低侧驱动（GPIO→电阻→三极管基极/LED→GND）。
//   2) 软件侧安全：IRsend 仅在进入红外功能时 pinMode(OUTPUT)，上电期间该脚
//      保持高阻/内部弱下拉，复位采样值为 0。
#define IR_TX_PIN 46
#endif
#endif
#ifndef IR_DEFAULT_KHZ
#define IR_DEFAULT_KHZ 38
#endif

/*──────────────────── Display & Touch ────────────────────*/
#ifndef TFT_WIDTH
#define TFT_WIDTH 240
#endif
#ifndef TFT_HEIGHT
#define TFT_HEIGHT 320
#endif
#ifndef STATUS_BAR_Y_OFFSET
#define STATUS_BAR_Y_OFFSET 0
#endif

/*──────────────────── Timing ────────────────────*/
#ifndef DEBOUNCE_MS
#define DEBOUNCE_MS 40
#endif
#ifndef LONG_PRESS_MS
#define LONG_PRESS_MS 800
#endif
#ifndef REPEAT_SCROLL_MS
#define REPEAT_SCROLL_MS 120
#endif

/*──────────────────── Backlight Levels ────────────────────*/
#ifndef BKL_LEVEL_MIN
#define BKL_LEVEL_MIN 10
#endif
#ifndef BKL_LEVEL_MED
#define BKL_LEVEL_MED 128
#endif
#ifndef BKL_LEVEL_MAX
#define BKL_LEVEL_MAX 255
#endif

/*──────────────────── Filesystem ────────────────────*/
#ifndef FS_MOUNT_OK_MSG
#define FS_MOUNT_OK_MSG "SD OK"
#endif
#ifndef FS_MOUNT_FAIL_MSG
#define FS_MOUNT_FAIL_MSG "SD Fail"
#endif
#ifndef LOG_DIR
#define LOG_DIR "/logs"
#endif
#ifndef CAPTURE_DIR
#define CAPTURE_DIR "/captures"
#endif

/*──────────────────── Battery ────────────────────*/
// 诊断打印总开关：控制 [IP5306] 电池诊断、[INP] 输入探针、[RAS] 初始化里程碑。
// 0 = 静默（生产默认）；1 = 全部打开（排查用）。
#ifndef DIAG_PRINTS_ENABLED
#define DIAG_PRINTS_ENABLED 1   // 临时打开：串口看 [IP5306] 行验证 ADC 电池源（标定完成后可改回 0）
#endif
#ifndef BATTERY_ADC_PIN
#if defined(BOARD_ESP32_DIV_V1)
#define BATTERY_ADC_PIN 36
#elif defined(BOARD_CYD)
#define BATTERY_ADC_PIN -1
#else
// User V2-class board (schematic, power block R11/R16):
// VBAT -> 100k -> GPIO2 <- 100k <- GND. GPIO2 = ADC1_CH1 on the S3 and is not
// a strapping pin, so the divider can be sampled directly (VBAT/2, <= ~2.1 V).
#define BATTERY_ADC_PIN 2
#endif
#endif
#ifndef BATTERY_VDIV_R1
#define BATTERY_VDIV_R1 200000.0f
#endif
#ifndef BATTERY_VDIV_R2
#define BATTERY_VDIV_R2 100000.0f
#endif
#ifndef BATTERY_LOW_VOLT
#define BATTERY_LOW_VOLT 3.40f
#endif

// The IP5306 stays on I2C (SDA=GPIO8, SCL=GPIO9, addr 0x75) and provides the
// charging flag; its 4-step battery level is only used as a fallback when the
// GPIO2 divider reads implausible (e.g. battery absent).
// 2026-10-10：V2 板实际未焊 IP5306，电池电量全走 GPIO2 分压 ADC，USB 插入检测走 IO39。
// 注释掉 BATTERY_USE_IP5306：不再对 0x75 做 I2C 总线扫描/寄存器轮询/四档回退。
// #if defined(BOARD_ESP32_DIV_V2)
// #define BATTERY_USE_IP5306
// #endif

// Battery LEVEL source selector (boards that have both the IP5306 and the
// VBAT divider on BATTERY_ADC_PIN):
//   1 = continuous ADC voltage via the divider (GPIO2)
//   0 = IP5306 4-step level over I2C (0x78), ADC unused
// NOTE: the CHARGING flag always comes from the IP5306 (reg 0x70 bit3) in
// BOTH modes - an ADC on the battery divider cannot detect USB insertion.
// 1 = ADC enabled, with guards: starts after BATTERY_START_DELAY_MS, first
// sample validated (2.5..4.6 V pack range), 5 s backoff + IP5306 fallback on
// any implausible read (WiFi holds the ADC1 lock during init/scan windows).
#ifndef BATTERY_LEVEL_USE_ADC
#define BATTERY_LEVEL_USE_ADC 1
#endif

// Battery measurement (ADC reads + IP5306 I2C polling + bus scan) starts only
// after the power rails have settled. Keeps the first seconds after power-on
// free of extra load and I2C traffic. The status bar shows "--" until then.
// 2026-10-10：5000→2000。此前怀疑 2s 导致冷启动色块不亮，后查明真凶是电池耗尽(3.40V<3.45V 曲线最低点→0%)，
// 2s 属于被冤枉，按用户要求恢复 2s。
#ifndef BATTERY_START_DELAY_MS
#define BATTERY_START_DELAY_MS 2000
#endif

// GPIO2 divider multiplier: VBAT(V) = adc_mV * BATTERY_VDIV_MULT / 1000.
// 实测标定（2026-10-09 万用表）：VBAT=3.570V 时 IO2=1.776V → K=3.570/1.776=2.01。
// 2026-10-10 临时把分压电阻改成两个 10k（10k/10k 对半分压）→ K=2.0。
// 如后续有偏差，按 K = VBAT_实测(V) / IO2_实测(V) 修改此值。
// 注意：K≈2 时 IO2 满电最高约 2.1V，ADC_11db 满量程内，安全。
#ifndef BATTERY_VDIV_MULT
#if defined(BOARD_ESP32_DIV_V2)
#define BATTERY_VDIV_MULT 2.0f
#else
#define BATTERY_VDIV_MULT 2.0f
#endif
#endif

/*──────────────────── 外部电源(USB)插入检测 ────────────────────*/
// 新增硬件：外部电源(VBUS)检测信号接到 IO39，插入 USB 时 IO39 读到高电平。
// 状态栏电池框右侧据此点亮黄色"插头"标（见 utils.cpp / drawStatusBar）。
// 它是纯数字量、不碰 ADC 与 I2C，比 IP5306 的充电位更直接，也不受其在线与否影响。
#ifndef USB_DETECT_PIN
#if defined(BOARD_ESP32_DIV_V1) || defined(BOARD_CYD)
#define USB_DETECT_PIN -1        // V1 / CYD 没有这颗检测（CYD 的 IO39 还是触摸 MISO）
#else
#define USB_DETECT_PIN 39
#endif
#endif

// 极性：1 = 插入时读到高电平（本板接法）；0 = 插入时读到低电平。
// 若现场发现插拔方向反了，只改这一个宏即可，不用动逻辑。
#ifndef USB_DETECT_ACTIVE_HIGH
#define USB_DETECT_ACTIVE_HIGH 1
#endif

// 0 = 纯 INPUT（默认，电平交给板上检测电路自己决定）
// 1 = INPUT_PULLUP  2 = INPUT_PULLDOWN
// ⚠ 一般不要动：内部上/下拉（约 45k）会与板上分压网络并联，改变分压比。
//   高分压比(如 100k/100k)时足以把高电平拉到逻辑阈值以下，导致恒定判为"未插入"。
#ifndef USB_DETECT_PULL_MODE
#define USB_DETECT_PULL_MODE 0
#endif

/*──────────────────── 息屏后的低亮度电量屏(AOD) ────────────────────*/
// 超时熄屏后不黑屏，而是用"最低亮度"显示一个大电池图标 + 百分比
// （有外部电源时另加黄色插头）。实现见 utils.cpp 的 screenDrawAod()。
// 背光占空比 0..255；越小越省电，但别小到看不见。
#ifndef SCREEN_OFF_DIM_LEVEL
#define SCREEN_OFF_DIM_LEVEL 24
#endif
// 睡眠期间多久校对一次电量/插头状态（只在数值真的变化时才重画）
#ifndef SCREEN_OFF_AOD_REFRESH_MS
#define SCREEN_OFF_AOD_REFRESH_MS 2000
#endif
// 每这么久强制整屏重画一次电量屏：兜底自愈用 —— 万一有没加闸门的地方
// （少数"每圈都重绘自己界面"的功能屏）在电量屏上画了东西，最多 10s 内被擦掉。
#ifndef SCREEN_OFF_AOD_RESYNC_MS
#define SCREEN_OFF_AOD_RESYNC_MS 10000
#endif

/*──────────────────── Wi-Fi ────────────────────*/
#ifndef WIFI_SCAN_ACTIVE_MS
#define WIFI_SCAN_ACTIVE_MS  500
#endif
#ifndef WIFI_SCAN_PASSIVE_MS
#define WIFI_SCAN_PASSIVE_MS 0
#endif
#ifndef WIFI_SPECTRUM_FFT_N
#define WIFI_SPECTRUM_FFT_N  256
#endif

/*──────────────────── BLE ────────────────────*/
#ifndef BLE_ADV_INTERVAL_MS
#define BLE_ADV_INTERVAL_MS 100
#endif
#ifndef BLE_JAMMER_SWEEP_MS
#define BLE_JAMMER_SWEEP_MS 80
#endif

/*──────────────────── Sub-GHz / RF ────────────────────*/
#ifndef SUBGHZ_SAMPLE_RATE
#define SUBGHZ_SAMPLE_RATE 38000
#endif
#ifndef SUBGHZ_DEFAULT_FREQ
#define SUBGHZ_DEFAULT_FREQ 433920000UL
#endif
#ifndef NRF24_DATA_RATE
#define NRF24_DATA_RATE    2
#endif
#ifndef NRF24_PA_LEVEL
#define NRF24_PA_LEVEL     3
#endif
#ifndef CC1101_DEFAULT_MOD
#define CC1101_DEFAULT_MOD 2
#endif

/*──────────────────── Feature Flags ────────────────────*/
#ifndef FEATURE_WIFI_TOOLS
#define FEATURE_WIFI_TOOLS   1
#endif
#ifndef FEATURE_BLE_TOOLS
#define FEATURE_BLE_TOOLS    1
#endif
#ifndef FEATURE_BLE_DUCKY
#define FEATURE_BLE_DUCKY    BOARD_HAS_ESP32S3
#endif
#ifndef FEATURE_SUBGHZ_TOOLS
#define FEATURE_SUBGHZ_TOOLS 1
#endif

/*──────────────────── Utilities ────────────────────*/
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
#endif
#ifndef UNUSED
#define UNUSED(x) (void)(x)
#endif

/*──────────────────── Settings / Themes ────────────────────*/
enum class Theme : uint8_t { Dark = 0, Light = 1 };

#ifndef NEOPIXEL_BRIGHT_MAX
#define NEOPIXEL_BRIGHT_MAX 64
#endif
#ifndef SETTINGS_PATH
#define SETTINGS_PATH "/config/settings.json"
#endif

void displaySubmenu();

/*──────────────────── Runtime UI Palette ────────────────────*/
struct UiPalette { uint16_t bg, fg, icon, text, accent, line, lable, warn, ok; };
extern UiPalette UI;                   
void applyThemeToPalette(Theme t);
uint16_t uiUniversalColor();

/*──────────────────── UI Text Helpers ────────────────────*/
// Dim label text color (used for *text* only).
// Keep UI_LABLE unchanged because it's also used for fills (e.g., status bar bg).
static inline uint16_t uiDimTextColor() {
  // In Dark theme, UI_LABLE (L_Dark) is intentionally very dark; brighten text a bit.
  return (UI.bg == BG_Dark) ? GRAY : UI_LABLE;
}
#ifndef UI_DIM_TEXT
#define UI_DIM_TEXT uiDimTextColor()
#endif

/*──────────────────── Global State Flags ────────────────────*/
extern bool in_sub_menu;
extern bool feature_active;
extern bool submenu_initialized;
extern bool is_main_menu;
extern bool feature_exit_requested;

#endif
