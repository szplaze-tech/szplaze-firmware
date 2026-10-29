/* =====================================================================
 * ESP32-DIV V2 — 硬件自检固件 (Hardware Self-Test)
 * ---------------------------------------------------------------------
 * 独立 sketch，不动主固件任何一行代码。上电自动跑完所有外设检测，
 * 结果一屏输出，同时从 Serial 115200 打印同样的结论（方便无屏排障）。
 *
 * 引脚全部按 ESP32-DIV V2 复刻板（ESP32-S3-WROOM-1）的实测映射，
 * 与主工程 shared.h 的 V2 分支保持一致：
 *
 *   显示/触摸  HSPI : SCLK 36 / MISO 37 / MOSI 35  TFT_CS 17 DC 16 RST 0
 *                     触摸 XPT2046 CS 18（挂在 TFT 同一条 HSPI 上，读法与业务代码一致）
 *   共享总线   FSPI : SCLK 12 / MISO 13 / MOSI 11
 *                     SD CS10、CC1101 CS5、nRF24 CSN 4/48/21
 *   I2C              : SDA 8 / SCL 9（core 默认，无需指定）
 *                     PCF8574 按键扩展（IP5306 的专门检测已按要求去掉）
 *   其它             : 电池 ADC GPIO2、USB 插入检测 GPIO39（分压电路）、背光 GPIO7
 *                     SD 卡检测 GPIO38、GPS UART 41/42（需外接模块）
 *
 * ⚠ 编译约束：PSRAM 必须保持 disabled（模组 MCN16R8 的八线 PSRAM 占
 *   GPIO33–37，而屏幕正在 35/36/37 上，开 PSRAM 屏幕必挂）。
 *
 * 判据口径：
 *   OK   = 器件有应答且读数符合预期
 *   FAIL = 器件无应答 / 读数全 0 或全 FF（多半是虚焊、错件、CS 接错）
 *   N/A  = 本固件不具备检测条件，或该项依赖外接模块 / 未启用
 *
 * 交互（可观察自检）：
 *   上电 → 自动逐项自检，每两项之间停顿 STEP_PAUSE_MS（默认 1.5 s）。屏幕最底行同时
 *          给出"上一步耗时 + 倒计时 + 按键字节"，所以哪一步慢、慢多少是看得见的；
 *   分组标题行（[FSPI ...] 那种）画完不停顿 —— 它只是小标题，和组内第一项一起看即可，
 *          这样每个组不会白等一整拍；
 *   自动过程中按任意方向键 / SELECT → 立刻停止自动流程，转入手动单步模式；
 *   手动模式下每按一次键 → 只跑下一项，按键一次对应自检一个外设，依次推进；
 *   整轮跑完后按任意键 → 清屏重来一轮（沿用当前模式）。
 *
 *   触摸那一步是**人工交互**的：整屏画提示与 4 个角靶标，测试人员逐点按住（每点 15 s 超时），
 *   4 点完成或超时才继续下一项；期间按 PCF8574 上任意键可放弃该步。交互结束后屏幕上的
 *   报告会按行历史原样重画回来（见 logReplay）。
 *   显示屏本身不做 ID 回读（默认关，见 ENABLE_LCD_ID_READ）—— 屏能显示就是它在位的证明。
 *
 *   屏幕最底行尾部还会显示 PCF8574 的**实时端口字节** `k=XX`：上电已把 8 个脚整字节
 *   写成 0xFF（全输入），所以松开时按键位应为 1（V2 正常空闲值 = 0xF8：bit0-2 未用），
 *   按下某个键时能看到对应位变 0 —— 这一栏就是"按键到底有没有被读到"的直接证据。
 *   末尾带 `!` = 只有部分键位可用（串口会列出 usableKeys）；显示 `NO KEY` = 扩展芯片
 *   没找到或没有任何可用键位，按键功能停用。
 *
 *   注：SD 挂载 / WiFi 扫描 / BLE 这类「库内部阻塞」（最长 1–3 s）期间收不到按键，
 *       按键若恰好落在这些窗口内，要等该项跑完才停下来；步间停顿做得比较长，
 *       实际观察时绝大多数按键都落在停顿窗口里，响应是即时的。
 * ===================================================================== */

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>

#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <RF24.h>
#include <WiFi.h>

/* 仅用于 CC1101 的**兜底**检测：业务固件读 CC1101 走的就是这个库（setSpiPin + setGDO + Init），
 * 所以当裸 SPI 读不到时，用它跑一遍相同的初始化序列，能区分"芯片/接线问题"与"我的时序问题"。
 * 注意库目录里的重复 .cpp 已经被 .disabled 掉，不会出现 multiple definition。 */
#include <ELECHOUSE_CC1101_SRC_DRV.h>

/* 注：业务固件用 PCF8574 库读按键，但本自检**刻意不用它** —— 那个库把"探测成功"绑在
 * "刚才有没有写端口"上（掩码为 0 时 begin() 恒返回 false），还自带读缓存，容易把硬件
 * 状态判错。这里统一走裸 I2C（pcfProbe / pcfReadPort），结论与屏幕 k= 栏一致。 */

#include <esp_attr.h>     // RTC_DATA_ATTR（复位溯源用）
#include <esp_system.h>
#include <esp_bt.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp_heap_caps.h>
#include <driver/gpio.h>  // gpio_reset_pin：共享总线易主时把引脚从外设手里收回来

/* ---------------- 硬件引脚（V2） ---------------- */
// 显示／触摸：TFT 的 SCLK/MISO/MOSI/CS/DC/RST 由 TFT_eSPI 的 User_Setup.h 决定
// （36 / 37 / 35 / 17 / 16 / 0）。注意：这里不能把变量取名 TOUCH_CS ——
// 那是 TFT_eSPI 的 User_Setup.h 已经占掉的宏名，会直接报编译错误。
// 触摸 XPT2046 同在 HSPI 上，只是片选不同。
static const uint8_t TS_CS = 18;

// 共享 SPI 总线（FSPI）—— SD / CC1101 / nRF24 全在这条总线上
static const uint8_t BUS_SCK  = 12;
static const uint8_t BUS_MISO = 13;
static const uint8_t BUS_MOSI = 11;

static const uint8_t SD_CS    = 10;
static const uint8_t SD_CD    = 38;   // 卡检测（机械开关，多数接 GND）
static const uint8_t CC_CS    = 5;    // CC1101（Sub-GHz）
static const uint8_t CC_GDO0  = 6;    // = SUBGHZ_TX_PIN
static const uint8_t CC_GDO2  = 3;    // = SUBGHZ_RX_PIN

struct NrfPair { uint8_t ce; uint8_t csn; };
static const NrfPair kNrf[3] = {
  { 15, 4  },   // nRF24 #1
  { 47, 48 },   // nRF24 #2
  { 14, 21 },   // nRF24 #3
};

static const uint8_t I2C_SDA   = 8;
static const uint8_t I2C_SCL   = 9;

// 外部电源(USB/充电)插入检测：分压电路输出到 IO39，插入时读到高电平。
// 定义与业务代码 shared.h 的 USB_DETECT_* 一致（V2 = 39，ACTIVE_HIGH = 1，纯 INPUT 不加内部上下拉）。
static const uint8_t USB_DETECT = 39;

// PCF8574 端口位（V2 分支）：UP 7 / DOWN 5 / LEFT 3 / RIGHT 4 / SELECT 6
static const uint8_t BTN_UP = 7, BTN_DOWN = 5, BTN_LEFT = 3, BTN_RIGHT = 4, BTN_SELECT = 6;

static const uint8_t BAT_ADC   = 2;    // 电池分压 ADC1_CH1
static const float   BAT_MULT  = 6.6f; // 实测分压比：VBAT—100k—IO2—18k—GND
static const uint8_t BACKLIGHT = 7;
static const uint8_t GPS_RX    = 41;
static const uint8_t GPS_TX    = 42;
static const uint8_t IR_RX     = 40;   // 本次不做功能测试，仅列出引脚
static const uint8_t IR_TX     = 46;

/* ---------------- 屏幕 ---------------- */
TFT_eSPI tft;

static SPIClass  fspi(FSPI);   // SD / CC1101 / nRF24
// ⚠ 不要再给触摸建第二个 SPIClass(HSPI)：屏幕就挂在 HSPI 上，而 Arduino 的 spiStartBus()
//   在 S3 上会 periph_module_reset(SPI3) 复位该外设、并不检查是否已被占用 ——
//   等于把 TFT_eSPI 的总线从底下拆掉，屏幕立刻卡死。触摸改用 tft.getTouchRaw()（见 testTouch）。

enum { ST_NA = 0, ST_OK = 1, ST_FAIL = 2 };

static const uint16_t C_OK   = 0x07E0;
static const uint16_t C_FAIL = 0xF800;
static const uint16_t C_NA   = 0x7BEF;
static const uint16_t C_INFO = 0xC618;
static const uint16_t C_GRP  = 0xFD20;
static const uint16_t C_TTL  = 0x07FF;

static int g_row = 0;
static const int ROW_H = 10;   // 27 行 × 10 = 270px，240 宽屏在竖屏下正好容得下
static int  g_ok = 0, g_fail = 0, g_na = 0;

/* ---------------- 复位溯源（RTC 保留区） ----------------
 * 这两个变量放在 RTC 慢速内存里：软复位 / PANIC / 看门狗 / BROWNOUT 都不会丢，
 * 只有彻底断电才归零。所以只要屏上 boot# 在涨，就说明**确实在反复重启**；
 * 再看 rst: 是 BROWNOUT 还是 PANIC，就能分清"电源塌了"还是"软件崩了"；
 * 若是 PANIC，last: 直接指出死在哪个测试项上。
 */
RTC_DATA_ATTR static uint32_t g_bootCount = 0;
RTC_DATA_ATTR static char     g_lastStage[16] = "";

static char     g_bootLine[22] = "";      // 复位溯源行内容（最多 21 字符，占满信息列）
static uint16_t g_bootColor   = C_INFO;

static const char* resetReasonStr(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_EXT:       return "EXT_PIN";
    case ESP_RST_SW:        return "SW_RESTART";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "UNKNOWN";
  }
}

/** 每个测试项开跑前调一次：把项名写进 RTC，并在屏幕最底行显示当前进度。
 *  这样即使下一秒板子崩了/掉电，下一次开机也能报出"上次死在 XXX"。 */
static void stageNow(const char* name) {
  strncpy(g_lastStage, name, sizeof(g_lastStage) - 1);
  g_lastStage[sizeof(g_lastStage) - 1] = '\0';
  Serial.printf("[>] %s\n", name);
  tft.setTextFont(1);
  tft.setTextColor(C_INFO, TFT_BLACK);
  tft.setCursor(2, tft.height() - 10);
  tft.printf("> %-18s", name);          // 定宽，防残留字符
}

static void setStatusChar(int st, char* out) {
  strcpy(out, (st == ST_OK) ? "OK" : (st == ST_FAIL) ? "NG" : "--");
}

static void rowDrawOnly(const char* label, int st, const char* info, uint16_t color) {
  char stTxt[3] = "--";
  setStatusChar(st, stTxt);
  const int y = 2 + g_row * ROW_H;
  g_row++;

  tft.setTextFont(1);
  tft.setTextColor(color, TFT_BLACK);
  tft.setCursor(2, y);
  tft.print(label);
  if (info && info[0]) {
    tft.setCursor(112, y);
    tft.setTextColor(C_INFO, TFT_BLACK);
    tft.print(info);
  }
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor((st == ST_OK) ? C_OK : (st == ST_FAIL) ? C_FAIL : C_NA, TFT_BLACK);
  tft.drawString(stTxt, 238, y);
  tft.setTextDatum(TL_DATUM);
}

static void groupOnly(const char* title) {
  const int y = 2 + g_row * ROW_H;
  g_row++;
  tft.setTextFont(1);
  tft.setTextColor(C_GRP, TFT_BLACK);
  tft.setCursor(2, y);
  tft.print(title);
}

/* ---------------- 行历史（供交互步骤之后重画） ----------------
 * 触摸那一步要整屏画文字提示与靶标，会把已经画好的测试行盖掉。这里把每行画过的内容
 * 记下来，交互结束后按原样重画，后面的步骤接着往下堆 ——
 * 屏幕上已有的报告不会因为"这一步要人工点一下"而丢掉。 */
static const int LOG_ROWS = 48;                 // 22 步最多画 30 行左右，留足余量
static char     g_logLabel[LOG_ROWS][22];
static char     g_logInfo[LOG_ROWS][24];
static int8_t   g_logSt[LOG_ROWS];
static uint16_t g_logColor[LOG_ROWS];
static int      g_logN = 0;

static void logClear() {
  g_logN = 0;
}

/** 记一行；st < 0 表示这是分组标题行（没有状态列）。 */
static void logPush(const char* label, int st, const char* info, uint16_t color) {
  if (g_logN >= LOG_ROWS) {
    return;
  }
  strncpy(g_logLabel[g_logN], label, sizeof(g_logLabel[0]) - 1);
  g_logLabel[g_logN][sizeof(g_logLabel[0]) - 1] = '\0';
  if (info && info[0]) {
    strncpy(g_logInfo[g_logN], info, sizeof(g_logInfo[0]) - 1);
    g_logInfo[g_logN][sizeof(g_logInfo[0]) - 1] = '\0';
  } else {
    g_logInfo[g_logN][0] = '\0';
  }
  g_logSt[g_logN] = (int8_t)st;
  g_logColor[g_logN] = color;
  g_logN++;
}

/** 画固定框架：标题 + 复位溯源行，并把行指针定位到表格第一行。 */
static void drawFrame() {
  tft.setTextFont(1);
  tft.setTextColor(C_TTL, TFT_BLACK);
  tft.setCursor(2, 2);
  tft.print("ESP32-DIV V2 HW SELF-TEST");

  // 复位溯源行（独立一行，不参与 OK/FAIL/N-A 计数）：
  // 一眼就能看出"是不是在反复重启、为什么、上次死在哪"。
  tft.setTextColor(C_INFO, TFT_BLACK);
  tft.setCursor(2, 2 + ROW_H);
  tft.print("Boot / Reset");
  tft.setCursor(112, 2 + ROW_H);
  tft.setTextColor(g_bootColor, TFT_BLACK);
  tft.print(g_bootLine);
  g_row = 2;
}

/** 按行历史重画整个报告（交互页之后调用）。 */
static void logReplay() {
  tft.fillScreen(TFT_BLACK);
  drawFrame();
  for (int i = 0; i < g_logN; i++) {
    if (g_logSt[i] < 0) {
      groupOnly(g_logLabel[i]);
    } else {
      rowDrawOnly(g_logLabel[i], g_logSt[i], g_logInfo[i], g_logColor[i]);
    }
  }
}

static void rowDraw(const char* label, int st, const char* info, uint16_t color) {
  rowDrawOnly(label, st, info, color);
  logPush(label, st, info, color);

  const char* stWord = (st == ST_OK) ? "OK  " : (st == ST_FAIL) ? "FAIL" : "N/A ";
  Serial.printf("[%s] %-16s %s\n", stWord, label, info ? info : "");

  if (st == ST_OK) g_ok++;
  else if (st == ST_FAIL) g_fail++;
  else g_na++;
}

static void reportExt(const char* label, int st, const char* info) {
  rowDraw(label, st, info, TFT_WHITE);
}

static void report(const char* label, int st, const char* fmt, ...) {
  char buf[20];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  reportExt(label, st, buf);
}

static void group(const char* title) {
  groupOnly(title);
  logPush(title, -1, nullptr, C_GRP);
  Serial.printf("----- %s -----\n", title);
}

/* ---------------- 共享 SPI 总线管理 ---------------- */
/** 把该总线上所有从机的 CS 拉高，避免多器件同时驱动 MISO。 */
static void busReleaseAllCs() {
  const int pins[] = { SD_CS, CC_CS, kNrf[0].csn, kNrf[1].csn, kNrf[2].csn };
  for (uint8_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
    pinMode((uint8_t)pins[i], OUTPUT);
    digitalWrite((uint8_t)pins[i], HIGH);
  }
  // nRF24 的 CE 保持低电平，避免任何一颗意外进入 TX。
  for (uint8_t i = 0; i < 3; i++) {
    pinMode(kNrf[i].ce, OUTPUT);
    digitalWrite(kNrf[i].ce, LOW);
  }
}

/* ---------------- 按键（PCF8574）与交互调度基础 ----------------
 * 自检默认「自动跑 + 每步之间停顿」；自动过程中一旦检测到按键，立刻停止自动流程，
 * 转入手动单步：之后每按一次键，才执行下一项外设自检。
 * 「一个外设」的粒度 = 原 runAllTests() 里的一个测试函数（分组标题单独算一步）。
 *
 * 按键必须在第一项自检之前就绪，否则自动流程跑到一半时按不动 —— 所以这里在
 * setup() 里就先把 I2C + PCF8574 初始化好，而不是等 testPcf8574() 那一步。
 */
static const uint32_t STEP_PAUSE_MS = 1500;   // 自动模式下两步之间的停顿（毫秒），嫌快就调大
static const uint32_t KEY_POLL_MS   = 20;     // 停顿 / 等按键期间的轮询间隔

static uint8_t g_keyAddr  = 0;
static bool    g_keyReady = false;
static int     g_lastKeyByte = -1;   // keyRawRead() 最近读到的端口字节，供屏幕显示（省一次 I2C）
static uint8_t g_keyGoodMask = 0;    // 空闲时能读到高的键位 —— 只有这些键参与判定

static int  s_step   = 0;      // 下一个待执行的步骤下标；>= STEP_COUNT 表示本轮已跑完
static bool s_manual = false;  // true = 手动单步模式（自动流程已被按键中止）

/* 刚跑完那一步的名字/耗时/类型 —— 用来在停顿期间把"上一步花了多久"打在屏幕上，
 * 哪一步慢、慢多少就不用猜了。类型用于决定要不要停顿（分组标题行不需要停）。 */
static char     g_lastStepName[16] = "";
static uint32_t g_lastStepMs       = 0;
static uint8_t  g_lastKind         = 0;   // 0 = STEP_DEVICE（默认按正常节奏停顿）

/** 在 0x20–0x27 里找出应答的 PCF8574。返回地址，0 = 没找到。
 *
 *  ⚠ 用标准 I2C 探测（只看地址有没有 ACK），**不要用库的 begin() 来探测**：
 *    PCF8574 库把"探测成功"和"刚才有没有写端口"绑死了 ——
 *    `begin()` 结尾 `return isLastTransmissionSuccess()`，而那是 `transmissionStatus==0`；
 *    transmissionStatus 只在 `if (writeMode>0 || readMode>0)` 分支里被赋值，
 *    否则一直停在函数开头设的哨兵值 4。于是**掩码为 0 时 begin() 恒返回 false**，
 *    看起来就是"芯片不存在"。（之前的 testPcf8574() 一直误报 FAIL not found 就是这个原因。） */
static uint8_t pcfProbe() {
  for (uint8_t a = 0x20; a <= 0x27; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {          // 只发地址，收到 ACK 即命中
      return a;
    }
    yield();
  }
  return 0;
}

/* 5 个方向键在 PCF8574 端口上的位掩码（V2：UP7 / DOWN5 / LEFT3 / RIGHT4 / SELECT6）。
 *  松开时这 5 位应全为 1，按下时对应位被按键拉到 0。 */
static const uint8_t KEY_MASK = (1 << BTN_UP) | (1 << BTN_DOWN) | (1 << BTN_LEFT) |
                                (1 << BTN_RIGHT) | (1 << BTN_SELECT);

/** 裸 I2C 读一个 PCF8574 的端口字节。返回 0..255，-1 = I2C 没应答。
 *  不用库的 digitalRead() 有三个原因：
 *    1) 它按对象缓存字节（latency 窗口内不重读），停顿期间每 20 ms 轮询会被缓存吃掉；
 *    2) 冷启动第一次读会因内部掩码未初始化而误报"全部按下"；
 *    3) 它的"已置位"分支依赖 begin() 设过的内部缓冲，没走 begin() 时会全部报 LOW。 */
static int pcfReadPort(uint8_t addr) {
  Wire.beginTransmission(addr);
  if (Wire.endTransmission(false) != 0) {        // 只发地址 + 重复起始位
    return -1;
  }
  if (Wire.requestFrom((uint8_t)addr, (uint8_t)1) != 1) {
    return -1;
  }
  return (int)Wire.read();
}

/** 调度器读按键端口；字节一旦变化就打一行串口 ——
 *  排查"按键到底有没有被读到"时这是最直接的证据。 */
static int keyRawRead() {
  if (!g_keyReady) {
    return -1;
  }
  const int v = pcfReadPort(g_keyAddr);
  if (v < 0) {
    return -1;
  }
  if (v != g_lastKeyByte) {
    Serial.printf("[key] raw port = 0x%02X\n", v);
  }
  g_lastKeyByte = v;
  return v;
}

/** 有任意一个**可用键位**被拉到低 = 有键按下。
 *  只看 g_keyGoodMask 里的位：空闲就为低的那几个脚不参与，避免一个坏脚把所有按键拖死。 */
static bool keyRawDown() {
  if (g_keyGoodMask == 0) {
    return false;
  }
  const int v = keyRawRead();
  return v >= 0 && (v & g_keyGoodMask) != g_keyGoodMask;
}

/** 上电时把按键扩展配成"可读"状态，并实测出**哪些键位真的可用**。
 *
 *  为什么不用库的 pinMode()+begin() / digitalWriteAll() 来做这件事：
 *    1) 那几条路径实际下发的是 ((参数 & writeMode) | (resetInitial & readMode))，
 *       会被对象内部掩码再过滤一遍 —— 掩码不对时"写了等于没写"，端口电平不可预测；
 *    2) 库的 begin() 还把"探测成功"绑在"刚才是否写过端口"上（见 pcfProbe 注释），
 *       掩码为 0 时恒返回 false，会把"按键芯片在不在"判错。
 *  所以这里：**地址用标准 I2C 探测，端口用裸 I2C 整字节写**，全程不碰库的掩码逻辑。
 *
 *  写 0xFF = 8 个脚全部弱拉高，也就是 PCF8574 的"全输入"状态：
 *    · 对 5 个按键脚，效果与写 0xF8 相同；
 *    · 比库的 0xF8 更安全 —— 0xF8 会把 bit0-2 拉低（那是库掩码运算的副产物，并非有意设计）；
 *    · 还能顺带发现"按键接在别的位"的情况：任何一位被按下都能在屏幕 k= 上看到变化。
 *
 *  之后把**空闲时能读到高的键位**记进 g_keyGoodMask，只让这些键参与判定：个别脚被外部
 *  拉低（上拉不足 / 下拉电阻 / 虚焊 / 接线与 shared.h 不一致）时，其余按键仍可用，
 *  不会因为一个坏脚把整块按键拖死。
 *
 *  注意"写前 before"和"写后 after"两个读数都要记下来：它们的差别正是区分
 *  「软件锁存值不对」和「引脚被外部拉低」的关键证据。 */
static bool keyInit() {
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setTimeOut(50);

  g_keyAddr = pcfProbe();
  if (!g_keyAddr) {
    Serial.println("[key] PCF8574 not found (扫了 0x20-0x27) -> 按键功能停用");
    return false;
  }
  g_keyReady = true;

  // 1) 写之前先读一次：这是"上电时端口本来就是什么状态"的对照
  const int before = keyRawRead();

  // 2) 裸写整字节 0xFF，绕开库的掩码过滤
  Wire.beginTransmission(g_keyAddr);
  Wire.write((uint8_t)0xFF);
  const uint8_t wr = Wire.endTransmission();
  delay(10);                                  // 给端口锁存稳定留一点时间

  // 3) 写后**多次读并取"或"**：某一位只要能读到高，就说明它能被弱上拉驱动、该键可用。
  //    只看一次就下结论太脆 —— 单次读可能撞上总线时序还没稳定，会把整块按键误判为不可用。
  uint8_t seen = 0;
  int after = -1;
  for (uint8_t i = 0; i < 4; i++) {
    const int v = pcfReadPort(g_keyAddr);
    if (v >= 0) {
      seen |= (uint8_t)v;
      after = v;
    }
    delay(5);
  }
  g_keyGoodMask = (uint8_t)(seen & KEY_MASK);

  Serial.printf("[key] PCF8574 @0x%02X  write0xFF=%s  usableKeys=0x%02X\n",
                g_keyAddr, wr == 0 ? "ok" : "NACK", g_keyGoodMask);
  Serial.printf("[key]   port before=%d after=%d  (0-255 为字节值，-1 为读失败)\n",
                before, after);

  if (g_keyGoodMask == 0) {
    Serial.println("[key] ! 没有任何键位能读到高 -> 按键无法判定（屏幕仍会实时显示端口字节）");
  } else if (g_keyGoodMask != KEY_MASK) {
    Serial.println("[key] ! 只有部分键位可用（见 usableKeys），其余脚被外部拉低或接线不符");
    Serial.println("[key]   可用键位照常工作；屏幕 k= 后面会带 ! 标记");
  }
  return true;
}

/** 底部状态行的按键后缀：实时端口字节 / 不可用提示。 */
static void keySuffix(char* dst, size_t n) {
  if (!g_keyReady) {
    snprintf(dst, n, "NO KEY");
    return;
  }
  if (g_lastKeyByte < 0) {
    snprintf(dst, n, "k=ERR");
    return;
  }
  // 只要芯片在就**始终显示实时字节**（哪怕一位都不可用）—— 这一栏是判断"按键有没有被
  // 读到"的唯一直接证据，绝不能用 NO KEY 之类不变化的文字把它盖掉。
  // 先掩到 0..255 再格式化：否则 %X 收到 int 时最坏会输出 8 位十六进制。
  // 末尾的 ! = 只有部分键位可用（具体见串口的 usableKeys）。
  snprintf(dst, n, "k=%02X%s", (unsigned)(g_lastKeyByte & 0xFF),
           (g_keyGoodMask == KEY_MASK) ? "" : "!");
}

/** 检测一次「按下沿」：必须曾经全部松开过，才认下一次按下 —— 按住不放不会重复触发。
 *  自动模式与手动模式共用同一份 armed 状态，所以「按下停止自动」那一次不会被
 *  当成手动模式的第一步，必须松手后再按才推进。 */
static bool keyEdge() {
  static bool armed = false;
  if (!keyRawDown()) {
    armed = true;
    return false;
  }
  if (!armed) {
    return false;
  }
  delay(40);                        // 简易消抖
  if (!keyRawDown()) {
    return false;                   // 抖动，不算
  }
  armed = false;
  return true;
}

/** 屏幕最底行的状态提示（与 stageNow 同一行，定宽到 34 字符，防止残留字符）。
 *  34 + 2 = 36 字符 × 6 px = 216 px，240 宽的屏放得下，也给"步骤名+耗时+倒计时"留足了位置。 */
static void statusLine(const char* txt) {
  tft.setTextFont(1);
  tft.setTextColor(C_INFO, TFT_BLACK);
  tft.setCursor(2, tft.height() - 10);
  tft.printf("> %-34s", txt);
}

/** 可中断停顿：期间持续轮询按键并刷新倒计时。
 *  返回 true = 用户在停顿期间按了键（调用方据此立刻中止自动流程）。 */
static bool pauseInterruptible(uint32_t ms) {
  const uint32_t t0 = millis();
  uint32_t shown = 0xFFFFFFFF;
  while (millis() - t0 < ms) {
    if (keyEdge()) {
      return true;
    }
    const uint32_t left = (ms - (millis() - t0) + 99) / 100;   // 剩余 0.1s 为单位，向上取整
    if (left != shown) {
      shown = left;
      char ks[16];
      keySuffix(ks, sizeof(ks));
      // 顺手把**刚跑完那一步的耗时**打在屏幕上：哪一步慢、慢多久，直接看得见。
      // 这比"感觉卡住"可靠 —— 慢到底是硬件等待还是固定停顿，一眼可分。
      const unsigned took = (unsigned)(g_lastStepMs > 9999 ? 9999 : g_lastStepMs);
      char buf[64];
      snprintf(buf, sizeof(buf), "%s %ums  next %u.%us  %s",
               g_lastStepName, took, (unsigned)(left / 10), (unsigned)(left % 10), ks);
      statusLine(buf);
    }
    delay(KEY_POLL_MS);
  }
  return false;
}

/* ---------------- 各外设检测 ---------------- */

/* 系统信息原本由 testSystem() 一次画 3 行 —— 拆成 3 步后，
 * 手动模式下「按键一次看一条」，自动模式下每一项也各有停顿。顺序不变。 */
static void testChip() {
  esp_chip_info_t ci;
  esp_chip_info(&ci);
  const uint32_t cpuMHz = 240;
  report("Chip", ST_NA, "rev%d %dcore %uMHz", ci.revision, ci.cores, cpuMHz);
}

static void testFlashPsram() {
  uint32_t flashBytes = 0;
  if (esp_flash_get_size(NULL, &flashBytes) != ESP_OK) {
    flashBytes = 0;
  }
  const size_t psramBytes = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  // 这里刻意显示 PSRAM：读到 0KB 才是正确的 —— 本板 PSRAM 必须 disabled。
  report("Flash / PSRAM", ST_NA, "%uMB / %uKB", (unsigned)(flashBytes / (1024 * 1024)),
         (unsigned)(psramBytes / 1024));
}

static void testHeap() {
  const uint32_t heap = ESP.getHeapSize();
  const uint32_t freeHeap = ESP.getFreeHeap();
  report("Heap", ST_NA, "%uKB / %uKB free", (unsigned)(heap / 1024), (unsigned)(freeHeap / 1024));
}

/** SD 卡：挂载 + 读类型/容量。必须在 CC1101/nRF24 之前不受打扰地单独占用总线。 */
static void testSdCard() {
  busReleaseAllCs();
  yield();
  // SD.begin 在无卡时要重试若干次，可能阻塞 1–3 秒，先把喂狗做足。
  const bool mounted = SD.begin(SD_CS, fspi, 4000000);
  yield();
  if (!mounted) {
    SD.end();
    busReleaseAllCs();
    reportExt("SD card  CS10", ST_FAIL, "mount failed");
    return;
  }
  const uint8_t type = SD.cardType();
  const uint64_t mb = SD.cardSize() / (1024ULL * 1024ULL);
  const char* tname = (type == CARD_SDHC) ? "SDHC" : (type == CARD_SD) ? "SDSC"
                    : (type == CARD_MMC) ? "MMC" : (type == CARD_NONE) ? "NONE" : "?";
  SD.end();
  busReleaseAllCs();
  report("SD card  CS10", ST_OK, "%s %lluMB", tname, (unsigned long long)mb);
}

/**
 * CC1101：直接用 SPI 读 PARTNUM(0x30) / VERSION(0x31)。
 * CC1101 的 PARTNUM 应为 0x00、VERSION 为 0x14 —— 这是最强的“器件在位”判据，
 * 比调库的 Init() 可靠（Init 即便无器件也常常返回成功）。
 *
 * 但"直接读"有两个前提，缺一个就会读到全 0 或全 FF（看起来像器件坏了）：
 *   1) 读之前要把共享总线从上一位主人（SD 卡）手里收回来 —— 业务代码就是靠
 *      reclaimSharedSpiBus() 做这件事的；
 *   2) 先给芯片一次硬复位（SRES），让状态确定 —— ELECHOUSE 库的 Init() 也是这么开的头。
 * 本函数按顺序补齐这两步，并在首次失败时重试一次 + 打印诊断量。
 */

/** 最近一轮测试里 SO(MISO) 是否**曾经**变低 —— 它是"芯片有电、晶振在跑、SPI 从端活着"的
 *  直接证据。只置位不清零：任一次等待成功就说明芯片当时是就绪的。 */
static bool g_ccSoReady = false;

/** 等 CC1101 的 SO(MISO) 变低（= 就绪）。须在 CS 已拉低时调用。
 *  MISO 在 SPI.begin() 里已被 pinMode(miso, INPUT)（esp32-hal-spi.c:232），
 *  所以这里 digitalRead 读到的是真实电平，不会反向驱动芯片。
 *  超时放宽到 20 ms：芯片刚从复位/休眠醒来时晶振起振要时间，太短会误判"没应答"。 */
static void ccWaitReady(uint32_t msTimeout = 20) {
  const uint32_t t0 = millis();
  while (digitalRead(BUS_MISO)) {
    if (millis() - t0 > msTimeout) {
      return;                        // 超时：SO 仍为高，g_ccSoReady 保持原值
    }
  }
  g_ccSoReady = true;
}

/** 把共享 SPI 总线从上一位主人手里收回来，并重新绑到 CC1101 的引脚上。
 *  等价于业务代码 utils.cpp 的 reclaimSharedSpiBus()：SPI.end() → gpio_reset_pin()
 *  → 重新 begin()。工程注释里有实测记录："SD.begin raises SPI clock and parks the
 *  card on the same MISO line CC1101 needs" —— 所以这一步不能省。 */
static void ccBusReclaim() {
  busReleaseAllCs();
  fspi.end();
  gpio_reset_pin((gpio_num_t)BUS_SCK);
  gpio_reset_pin((gpio_num_t)BUS_MISO);
  gpio_reset_pin((gpio_num_t)BUS_MOSI);
  gpio_reset_pin((gpio_num_t)CC_CS);
  gpio_reset_pin((gpio_num_t)SD_CS);
  pinMode(BUS_MISO, INPUT);                 // 与 ELECHOUSE SpiStart() 一致
  pinMode(SD_CS, OUTPUT);  digitalWrite(SD_CS, HIGH);
  pinMode(CC_CS, OUTPUT);  digitalWrite(CC_CS, HIGH);
  fspi.begin(BUS_SCK, BUS_MISO, BUS_MOSI, CC_CS);
  delay(2);
}

/** 发一个命令选通字节（如 SIDLE 0x36、SRES 0x30）。 */
static void ccStrobe(uint8_t cmd) {
  fspi.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
  digitalWrite(CC_CS, LOW);
  ccWaitReady();
  fspi.transfer(cmd);
  digitalWrite(CC_CS, HIGH);
  fspi.endTransaction();
  delayMicroseconds(200);
}

/** 按 ELECHOUSE 库 Reset() 的做法给 CC1101 一次硬复位：CS 复位脉冲 + SRES。
 *  ⚠ SRES 的字节值 0x30 与 PARTNUM 的寄存器地址相同 —— 两者靠读标志位区分：
 *    读寄存器发 0x30|0x80 = 0xB0，而 SRES 是纯命令 0x30。 */ 
static void ccReset() {
  fspi.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
  digitalWrite(CC_CS, LOW);
  delay(1);
  digitalWrite(CC_CS, HIGH);
  delay(1);
  digitalWrite(CC_CS, LOW);
  ccWaitReady();
  fspi.transfer(0x30);              // SRES
  ccWaitReady();
  digitalWrite(CC_CS, HIGH);
  fspi.endTransaction();
  delay(1);
}

/** 单字节读**状态寄存器**（PARTNUM=0x30 / VERSION=0x31 都属于状态寄存器）。
 *
 *  ⚠ 关键点：这类寄存器要用 **burst 位** 访问 —— header = addr | 0xC0
 *    （0x80 = 读标志位，0x40 = burst）。用"单读"形式 addr | 0x80 读它们读不到东西。
 *    ELECHOUSE 库的 SpiReadStatus() 正是这么写的：`temp = addr | READ_BURST;`
 *    （见 SmartRC-CC1101-Driver-Lib 的 ELECHOUSE_CC1101_SRC_DRV.cpp），
 *    而业务固件读模块走的就是这条函数。之前一直"检测不到"，这里就是原因之一。 */
static uint8_t ccReadStatusReg(uint8_t addr) {
  fspi.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
  digitalWrite(CC_CS, LOW);
  ccWaitReady();
  fspi.transfer((uint8_t)(addr | 0xC0));     // 读 + burst
  const uint8_t v = fspi.transfer(0x00);
  digitalWrite(CC_CS, HIGH);
  fspi.endTransaction();
  return v;
}

/** 全 0 或全 FF = MISO 上没人应答（虚焊 / 缺件 / CS 错 / 总线被占）。 */
static bool ccDead(uint8_t part, uint8_t ver) {
  return (part == 0x00 && ver == 0x00) || (part == 0xFF && ver == 0xFF);
}

/** 空闲期间采样 CC1101 的 CS 脚，统计跳变次数（先把该脚设成纯输入，避免与外部驱动打架）。
 *
 *  为什么必须查这个：V2 的 GPIO5 原本是 GPS 模块 TX 的落点，shared.h 里明确记着 ——
 *  "该网络物理上常连通，模块 TX 会不停在 CC1101 片选线上制造跳变，造成随机片选 / 误写寄存器"。
 *  如果这条网络被别的器件驱动，那么无论固件怎么写，CC1101 的片选都是乱的，读什么都读不到
 *  （而数据线 SCK/MISO/MOSI 是好的，所以 nRF24 照样能测通）。 */
static uint8_t ccCsIdleEdges(uint32_t ms) {
  pinMode(CC_CS, INPUT_PULLUP);          // 45k 弱上拉，对 3.3V 数字网络无影响
  delayMicroseconds(200);
  uint8_t edges = 0;
  int last = digitalRead(CC_CS);
  const uint32_t t0 = millis();
  while (millis() - t0 < ms) {
    const int v = digitalRead(CC_CS);
    if (v != last) {
      edges++;
      last = v;
    }
  }
  pinMode(CC_CS, OUTPUT);
  digitalWrite(CC_CS, HIGH);
  return edges;
}

/** 兜底路径：完全按业务代码的顺序走 ELECHOUSE 库 —— 它的在位检测就是
 *  getCC1101()（内部即 `SpiReadStatus(0x31) > 0`，用 burst 位读 VERSION）。
 *  返回 true = 读到了有效 VERSION。
 *
 *  ⚠ 调库之前**必须先确认 SO 能变低**：库的 SpiReadStatus()/Reset() 里有
 *    `while(digitalRead(MISO_PIN));` 这种**无界等待**，芯片不在或没就绪时
 *    会把整个自检永久挂死。调用方用 g_ccSoReady 做门禁，这里只负责调用。 */
static bool ccLibRead(uint8_t& part, uint8_t& ver) {
  ELECHOUSE_cc1101.setSpiPin(BUS_SCK, BUS_MISO, BUS_MOSI, CC_CS);
  ELECHOUSE_cc1101.setGDO(CC_GDO0, CC_GDO2);
  const bool    libOk = ELECHOUSE_cc1101.getCC1101();
  const uint8_t libV  = ELECHOUSE_cc1101.SpiReadStatus(0x31);
  const uint8_t libP  = ELECHOUSE_cc1101.SpiReadStatus(0x30);
  Serial.printf("[cc1101] A/lib getCC1101=%d  P%02X V%02X\n", libOk ? 1 : 0, libP, libV);
  // 库的 SpiEnd() 把 SPI 四个脚设成了 INPUT（交还总线），要接着用自己的 fspi 必须先重新接管
  ccBusReclaim();
  part = libP;
  ver  = libV;
  return (libOk && libV > 0);
}

static void testCc1101() {
  ccBusReclaim();

  // 诊断 1：CS 脚空闲期间有没有被别的器件驱动（跳变）—— 见 ccCsIdleEdges 注释
  const uint8_t csEdges = ccCsIdleEdges(60);

  // 诊断 2：先甩一个 SIDLE 把状态机按回 IDLE，同时用**有超时**的方式测 SO 会不会变低
  //（SO 能变低 = 芯片有电、晶振在跑、SPI 从端活着；也是调库前的安全门禁）
  g_ccSoReady = false;
  ccStrobe(0x36);
  const bool soReady = g_ccSoReady;

  uint8_t part = 0;
  uint8_t ver  = 0;
  int         hit = 0;            // 0 = 三条路都没读到
  const char* how = "";

  // ---- 方法 A：库自己的在位检测（业务固件能操作模块，用的就是这条路）----
  if (soReady) {
    if (ccLibRead(part, ver)) {
      hit = 1;
      how = "lib";
    }
  }

  // ---- 方法 B：裸读状态寄存器（header 用 burst 位 0xC0|addr，而不是单读的 0x80|addr）----
  if (!hit) {
    part = ccReadStatusReg(0x30);
    ver  = ccReadStatusReg(0x31);
    if (!ccDead(part, ver)) {
      hit = 2;
      how = "raw";
    }
  }

  // ---- 方法 C：先硬复位（CS 脉冲 + SRES）再裸读 ----
  if (!hit) {
    ccReset();
    part = ccReadStatusReg(0x30);
    ver  = ccReadStatusReg(0x31);
    if (!ccDead(part, ver)) {
      hit = 3;
      how = "raw2";
    }
  }

  const int soAfter = digitalRead(BUS_MISO);
  busReleaseAllCs();

  Serial.printf("[cc1101] csEdges=%u soReady=%d soAfter=%d  -> %s P%02X V%02X\n",
                (unsigned)csEdges, soReady ? 1 : 0, soAfter,
                hit ? how : "FAIL(all)", part, ver);

  char info[32];
  if (!hit) {
    // 失败也给可分辨的签名，一眼看出卡在哪一环
    if (csEdges > 2) {
      snprintf(info, sizeof(info), "CS5 busy(%u)!", (unsigned)(csEdges & 0x0F));
    } else if (!soReady) {
      snprintf(info, sizeof(info), "SO stuck hi");
    } else {
      snprintf(info, sizeof(info), "%02X %02X", part, ver);
    }
    reportExt("CC1101   CS5", ST_FAIL, info);
    return;
  }
  // PARTNUM 必须是 0x00（CC110X 系列），VERSION 常见 0x14。
  const bool plausible = (part == 0x00) && (ver != 0x00) && (ver != 0xFF);
  snprintf(info, sizeof(info), "P%02X V%02X %s", part, ver, how);
  reportExt("CC1101   CS5", plausible ? ST_OK : ST_FAIL, info);
}

/* 三颗 nRF24 原本挤在一个 testNrf24() 里一次画 3 行 —— 拆成独立三步后，
 * 手动模式下「按键一次测一颗」，能看清具体是哪颗没应答。顺序 #1 #2 #3 不变。 */
static void testNrf24Slot(uint8_t i) {
  busReleaseAllCs();
  RF24 radio(kNrf[i].ce, kNrf[i].csn, 16000000);
  // RF24 的 begin(_SPI*) 会用我们这条已经配好引脚的 FSPI 总线。
  const bool started = radio.begin(&fspi);
  // isChipConnected() 读 SETUP_AW(0x03)，有效值应在 1..3 —— 器件在位判据。
  const bool present = started && radio.isChipConnected();
  char label[20];
  snprintf(label, sizeof(label), "NRF24 #%d %d/%d", i + 1, kNrf[i].ce, kNrf[i].csn);
  reportExt(label, present ? ST_OK : ST_FAIL, present ? "chip found" : "no response");
  busReleaseAllCs();
}

static void testNrf24_1() { testNrf24Slot(0); }
static void testNrf24_2() { testNrf24Slot(1); }
static void testNrf24_3() { testNrf24Slot(2); }

/* 是否用 tft.readcommand8() 回读屏的 ID —— **默认关闭**。
 *
 * 为什么默认关：这是整个自检里唯一一处「业务固件从没做过」的屏操作。
 * 业务固件（这块板上已验证可用）读屏只用触摸通道（tft.getTouchRawZ / getTouchRaw），
 * 从不发 readcommand/readPixel。而 readcommand8() 会把整条 HSPI 从写模式切成读模式 ——
 * ESP32-S3 上 SET_BUS_READ_MODE 会置 SPI_DOUTDIN（MOSI 变双向），
 * 实测后果就是**白屏**（背光亮、面板无内容）。
 * 要用它排查「能显示但 MISO(GPIO37) 回读线断/虚焊」的板子时，把这个开关置 1 再复测。 */
#define ENABLE_LCD_ID_READ 0

/** 显示屏：验证面板可用。
 *
 *  默认路径**不碰总线读模式**：屏在不在，屏幕自己能证明 —— 上面那些测试行全是它画出来的。
 *  这比"读一个 ID 回来"更直接，也避开了上面说的白屏风险。
 *  打开 ENABLE_LCD_ID_READ 时才做 ID 回读（正常 ID 字节是 0x93）。 */
static void testDisplayId() {
#if ENABLE_LCD_ID_READ
  // index 0 是 dummy 字节，真正的 ID 从 index 1 开始
  const uint8_t d1 = tft.readcommand8(0x04, 1);
  const bool dead = (d1 == 0x00) || (d1 == 0xFF);
  report("LCD ID   CS17", dead ? ST_FAIL : ST_OK, "%02X", d1);
#else
  reportExt("LCD ID   CS17", ST_NA, "skip: display=proof");
#endif
}

/**
 * XPT2046 触摸：**触摸校准式人工检测**（4 角靶标，须测试人员逐点按住）。
 *
 * 整屏画出文字提示 + 4 个角上的十字靶标，测试人员按顺序按住每个靶心：点到的靶标变绿，
 * 4 个点全部完成（或某点超时）才继续下一项。这样验的是**触摸面板整面可用**，
 * 比"读一次原始值"强得多，同时顺带得到 X/Y 原始范围。
 *
 * ⚠ 两条必须遵守的约束（都是踩过的坑）：
 *   1) 绝不能自己 new 一个 SPIClass(HSPI) 再 begin()：ESP32-S3 上 TFT_eSPI 用自己文件里的
 *      `SPIClass spi(HSPI)` + IDF 级 spi_bus_initialize(SPI3_HOST)（TFT_eSPI_ESP32_S3.c:15,874），
 *      而 Arduino 的 spiStartBus() 会无条件 periph_module_reset(PERIPH_SPI3_MODULE)
 *      （esp32-hal-spi.c:735）且不检查占用 —— 会把 TFT 的总线整个复位掉，屏幕当场死。
 *      业务代码在 V2 上也不建第二个 SPI 实例，只把 TOUCH_CS 置高。
 *   2) 不能用 TFT_eSPI 的 tft.getTouch()/calibrateTouch()：它们内部的 validTouch() 有
 *      `while (z1 > z2)` 这类**无界等待**，触摸 IC 一坏就会把整个自检永久挂死。
 *      这里只用非阻塞的 getTouchRawZ()/getTouchRaw() 自己实现，**每一点都有超时**。
 */
static const uint16_t TOUCH_Z_MIN = 300;   // 压力阈值：业务代码用 120 / 200 / 500，取中间值

/** 读一次触摸（非阻塞）。压力够就返回 true 并给出原始 X/Y。
 *  读取顺序照搬业务代码 Touchscreen.cpp:readSharedTouchSample()：
 *  先 tft.endWrite() 收尾上一次写事务，再读 Z，再读 X/Y。 */
static bool touchSampleRaw(uint16_t& rx, uint16_t& ry) {
  uint16_t x = 0;
  uint16_t y = 0;
  tft.endWrite();
  const uint16_t z = tft.getTouchRawZ();
  tft.getTouchRaw(&x, &y);
  // 读完后做一次极小的写操作，强制把总线切回写模式（SET_BUS_WRITE_MODE）。
  // TFT_eSPI 的 end_touch_read_write() 里那句 SET_BUS_WRITE_MODE 是被注释掉的，
  // 正常靠下一次 begin_tft_write() 补上；这里补一次保险，代价是一个像素。
  tft.drawPixel(0, 0, TFT_BLACK);
  if (z < TOUCH_Z_MIN) {
    return false;
  }
  rx = x;
  ry = y;
  return true;
}

static void drawTouchTarget(int cx, int cy, uint16_t color) {
  tft.drawCircle(cx, cy, 7, color);
  tft.drawCircle(cx, cy, 3, color);
  tft.drawFastHLine(cx - 11, cy, 23, color);
  tft.drawFastVLine(cx, cy - 11, 23, color);
}

/** 等一次按压，超时返回 false。期间若按了 PCF8574 上的键 = 测试人员放弃本步（aborted=true），
 *  给操作者一个逃出 4×15 s 等待的口子。 */
static bool waitTouchDown(uint32_t ms, uint16_t& rx, uint16_t& ry, bool& aborted) {
  const uint32_t t0 = millis();
  while (millis() - t0 < ms) {
    if (touchSampleRaw(rx, ry)) {
      return true;
    }
    if (keyRawDown()) {
      aborted = true;
      return false;
    }
    delay(15);
  }
  return false;
}

/** 等松手，超时也放行（避免一次按住被当成连续点中多个靶标）。 */
static void waitTouchUp(uint32_t ms) {
  const uint32_t t0 = millis();
  uint16_t x = 0;
  uint16_t y = 0;
  while (millis() - t0 < ms) {
    if (!touchSampleRaw(x, y)) {
      return;
    }
    delay(15);
  }
}

static void testTouch() {
  const int W = (int)tft.width();
  const int H = (int)tft.height();

  // ---------- 全屏交互页：文字提示 + 4 个角靶标 ----------
  tft.fillScreen(TFT_BLACK);
  tft.setTextFont(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(4, 8);
  tft.print("TOUCH TEST");

  tft.setTextFont(1);
  tft.setTextColor(C_INFO, TFT_BLACK);
  tft.setCursor(4, 30);
  tft.print("Press & hold each target");
  tft.setCursor(4, 42);
  tft.print("until it turns green.");
  tft.setCursor(4, 54);
  tft.print("4 targets, 15 s each.");

  tft.setTextColor(C_NA, TFT_BLACK);
  tft.setCursor(4, 70);
  tft.print("Any key = skip this test");

  const int tx[4] = { 22, W - 23, 22, W - 23 };
  const int ty[4] = { 104, 104, H - 42, H - 42 };

  const uint32_t tAll0 = millis();
  uint8_t  okPts = 0;
  bool     aborted = false;
  uint16_t minX = 4095, maxX = 0, minY = 4095, maxY = 0;

  for (uint8_t i = 0; i < 4 && !aborted; i++) {
    drawTouchTarget(tx[i], ty[i], C_INFO);
    tft.setTextFont(1);
    tft.setTextColor(C_TTL, TFT_BLACK);
    tft.setCursor(4, H - 16);
    tft.printf("Point %u/4  waiting...      ", (unsigned)(i + 1));

    uint16_t rx = 0;
    uint16_t ry = 0;
    if (!waitTouchDown(15000, rx, ry, aborted)) {
      drawTouchTarget(tx[i], ty[i], C_FAIL);
      break;                                  // 超时：触摸多半不可用，后面的点不用再等
    }
    drawTouchTarget(tx[i], ty[i], C_OK);
    okPts++;
    if (rx < minX) minX = rx;
    if (rx > maxX) maxX = rx;
    if (ry < minY) minY = ry;
    if (ry > maxY) maxY = ry;
    waitTouchUp(3000);
  }

  // ---------- 收尾：把报告页重画回来，再补上本行的结论 ----------
  logReplay();
  stageNow("Touch");

  Serial.printf("[touch] %u/4 pts%s  X %u..%u  Y %u..%u  (%u ms)\n",
                (unsigned)okPts, aborted ? " ABORTED" : "",
                (unsigned)minX, (unsigned)maxX, (unsigned)minY, (unsigned)maxY,
                (unsigned)(millis() - tAll0));

  if (aborted) {
    reportExt("XPT2046  CS18", ST_NA, "aborted by key");
  } else if (okPts == 4) {
    report("XPT2046  CS18", ST_OK, "4/4 pts");
  } else if (okPts > 0) {
    report("XPT2046  CS18", ST_FAIL, "%u/4 pts", (unsigned)(okPts & 0x0F));
  } else {
    reportExt("XPT2046  CS18", ST_FAIL, "no touch");
  }
}

/** I2C 总线扫描：列出所有应答地址。预期看到 PCF8574(0x20-0x27)。
 *  （IP5306(0x75) 的专门检测已按要求去掉；它若在位，这里仍会把它列出来。） */
static void testI2cScan() {
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setTimeOut(50);
  uint8_t found[16];
  uint8_t n = 0;
  for (uint8_t addr = 0x03; addr <= 0x77; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0 && n < 16) {
      found[n++] = addr;
    }
    yield();
  }
  if (n == 0) {
    reportExt("I2C scan", ST_FAIL, "bus empty");
    return;
  }
  char info[20];
  int len = snprintf(info, sizeof(info), "0x%02X 0x%02X", found[0], found[1]);
  if (n > 2) {
    snprintf(info + len, sizeof(info) - len, "+%d", n - 2);
  }
  reportExt("I2C scan", ST_OK, info);
}

/** PCF8574 按键扩展：报出地址 + **裸读的端口字节**，并逐键列出电平（1 = 松开，0 = 按下）。
 *
 *  ⚠ 原先这里用 `PCF8574` 库的 digitalRead()，有两个问题：
 *    1) 地址是用库的 begin() 探测的，而它在掩码为 0 时恒返回 false ——
 *       于是这一项**一直误报 FAIL "not found"**（芯片明明在）；
 *    2) 库的读法依赖 begin() 设过的内部缓冲，没走 begin() 时会把所有键都报成 LOW。
 *  所以改成与调度器完全相同的裸 I2C 读法：地址用 pcfProbe()，字节用 pcfReadPort()。
 *  这样这一项的结论和屏幕 k= 栏、和按键实际行为三者永远一致。 */
static void testPcf8574() {
  const uint8_t addr = pcfProbe();
  if (!addr) {
    reportExt("PCF8574 keys", ST_FAIL, "not found");
    return;
  }
  const int v = pcfReadPort(addr);
  if (v < 0) {
    reportExt("PCF8574 keys", ST_FAIL, "read fail");
    return;
  }
  char info[32];
  snprintf(info, sizeof(info), "0x%02X %d%d%d%d%d", addr,
           (v >> BTN_UP) & 1, (v >> BTN_DOWN) & 1, (v >> BTN_LEFT) & 1,
           (v >> BTN_RIGHT) & 1, (v >> BTN_SELECT) & 1);
  reportExt("PCF8574 keys", ST_OK, info);
}

/**
 * 外部电源(USB/充电)插入检测 —— 验 IO39 那一路分压电路是否正常。
 * 定义与业务代码 shared.h 的 USB_DETECT_* 完全一致（V2：IO39，插入读到高电平）。
 *
 * 这一路是**纯数字量**（IO39 在 ESP32-S3 上没有 ADC 通道，业务代码也只用 digitalRead）：
 * 板上把 VBUS 经分压网络变成 IO39 上的一个逻辑电平，分压是否正常就体现在这个电平上。
 *
 * 判据（两条一起看）：
 *   ① 电平稳定：分压网络在时，读数是一个确定的逻辑电平。电阻虚焊/开路会让引脚悬空，
 *      读数来回跳 —— 采样 100 ms 统计跳变，跳变多即判 FAIL（分压电路可疑）。
 *   ② 电平极性：插入 USB 时应为高（USB_DETECT_ACTIVE_HIGH = 1），屏幕同时报高电平占比，
 *      插着 USB 跑自检时它应该是 100%。
 *
 * ⚠ 必须用纯 INPUT，**不能开内部上/下拉**：内部 45k 会与板上分压网络并联、改变分压比，
 *   高分压比时足以把高电平拉到逻辑阈值以下，导致恒判"未插入"（shared.h 专门写了这条警告）。
 */
static void testUsbDetect() {
  pinMode(USB_DETECT, INPUT);        // 纯输入，不加内部上下拉
  delay(2);

  uint8_t  hiSamples = 0;
  uint8_t  samples   = 0;
  uint8_t  edges     = 0;
  int last = digitalRead(USB_DETECT);
  const uint32_t t0 = millis();
  while (millis() - t0 < 100) {      // 采样 100 ms
    const int v = digitalRead(USB_DETECT);
    samples++;
    if (v) {
      hiSamples++;
    }
    if (v != last) {
      edges++;
      last = v;
    }
  }

  char info[32];
  if (edges > 2) {
    // 电平在跳：分压电阻虚焊/开路把引脚悬空了（或该脚被别的东西驱动）
    snprintf(info, sizeof(info), "unstable e%u", (unsigned)(edges & 0x3F));
    reportExt("USB det  GPIO39", ST_FAIL, info);
    Serial.printf("[usbdet] UNSTABLE: %u edges / %u samples -> IO39 悬空或分压异常\n",
                  (unsigned)edges, (unsigned)samples);
    return;
  }

  const bool plugged = (last != 0);                                  // ACTIVE_HIGH = 1
  const unsigned pct = samples ? ((hiSamples * 100u) / samples) : 0u;
  snprintf(info, sizeof(info), "%s %u%%", plugged ? "PLUG" : "none", (unsigned)(pct & 0x7Fu));
  reportExt("USB det  GPIO39", ST_OK, info);
  Serial.printf("[usbdet] IO39 = %s  steady (%u edges / %u samples, %u%% high)\n",
                plugged ? "HIGH (USB 已插入)" : "LOW (未插 USB)",
                (unsigned)edges, (unsigned)samples, (unsigned)pct);
}

/** 电池电压：必须在 WiFi/BLE 起来之前采（ADC1 会被无线模块占住）。
 *  注：GPIO2 属于 ADC1_CH1，不是 ADC2 —— 标签按实际写。 */
static void testBattery() {
  analogSetPinAttenuation(BAT_ADC, ADC_11db);
  delay(10);
  const uint32_t mV = analogReadMilliVolts(BAT_ADC);
  const float vbat = (mV / 1000.0f) * BAT_MULT;
  const bool plausible = vbat > 2.50f && vbat < 4.60f;
  report("Batt GPIO2", plausible ? ST_OK : ST_FAIL, "%.2fV %umV", vbat, (unsigned)mV);
}

static void testBle() {
  // 控制器只 init 一次。esp_bt_controller_disable() 之后状态是 INITED 而不是 IDLE，
  // 所以重跑时再调 init 会返回 ESP_ERR_INVALID_STATE —— 旧写法会让第二遍误报 FAIL。
  static bool s_btInitTried = false;
  static bool s_btInited    = false;
  if (!s_btInitTried) {
    s_btInitTried = true;
    esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    s_btInited = (esp_bt_controller_init(&cfg) == ESP_OK);
  }

  bool ok = false;
  if (s_btInited) {
    const esp_bt_controller_status_t st = esp_bt_controller_get_status();
    if (st == ESP_BT_CONTROLLER_STATUS_ENABLED) {
      ok = true;
    } else if (st == ESP_BT_CONTROLLER_STATUS_INITED) {
      ok = (esp_bt_controller_enable(ESP_BT_MODE_BLE) == ESP_OK);
    }
  }

  uint8_t mac[6] = {0};
  if (ok) {
    esp_read_mac(mac, ESP_MAC_BT);
    esp_bt_controller_disable();   // 关掉射频，控制器保持 INITED，下次直接 enable
  }
  report("BLE ctrl", ok ? ST_OK : ST_FAIL, "%02X:%02X:%02X:%02X:%02X:%02X",
         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void testWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  delay(100);
  uint8_t mac[6];
  WiFi.macAddress(mac);
  const int nets = WiFi.scanNetworks(false, true);   // 同步扫描，~1-2 s
  WiFi.scanDelete();
  WiFi.mode(WIFI_OFF);                               // 释放 RF 与内存
  // 判据：scanNetworks 返回 >=0 即算通过（0 表示周围无 AP，不代表 RF 坏了）。
  report("WiFi 2.4G", nets >= 0 ? ST_OK : ST_FAIL, "%u net(s)",
         (unsigned)(nets > 0 ? nets : 0));
  Serial.printf("       STA MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/** GPS 串口：只有真的插了模块才有意义，接不到就标 N/A 而不是 FAIL。 */
static void testGps() {
  HardwareSerial gpsSerial(2);
  gpsSerial.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);
  String line;
  bool gotFix = false;
  uint32_t t0 = millis();
  while (millis() - t0 < 1500) {
    while (gpsSerial.available()) {
      const char c = (char)gpsSerial.read();
      if (c == '\n') {
        if (line.startsWith("$") && line.length() > 20) {
          gotFix = true;
        }
        line = "";
      } else if (line.length() < 120) {
        line += c;
      }
    }
    if (gotFix) {
      break;
    }
    delay(10);
  }
  gpsSerial.end();
  reportExt("GPS UART41/42", gotFix ? ST_OK : ST_NA, gotFix ? "NMEA ok" : "no module");
}

static void testMiscIo() {
  // SD 卡座机械检测脚：0 = 有卡（多数卡座如此），这里只报原始电平避免误判。
  pinMode(SD_CD, INPUT_PULLUP);
  const int cd = digitalRead(SD_CD);
  report("SD CD    GPIO38", ST_NA, cd ? "HIGH (empty?)" : "LOW (inserted)");

  report("Buzzer", ST_NA, "-1 disabled");
  report("IR RX40 / TX46", ST_NA, "not tested");
  report("WS2812  GPIO1", ST_NA, "no driver");
}

/* ---------------- 主流程 ---------------- */

static void setBacklight(uint8_t duty) {
  ledcSetup(0, 5000, 8);
  ledcAttachPin(BACKLIGHT, 0);
  ledcWrite(0, duty);
}

/* ---------------- 自检步骤表 ----------------
 * 一步 = 屏幕上的一个测试项（或一条分组标题）。顺序与原 runAllTests() 完全一致。
 * name 用于屏幕进度行与 RTC 崩溃溯源（须 ≤15 字符）；title 只有分组步骤用。
 */
enum { STEP_DEVICE = 0, STEP_GROUP = 1 };

struct TestStep {
  uint8_t     kind;
  const char* name;
  const char* title;
  void      (*fn)();
};

static const TestStep kSteps[] = {
  { STEP_DEVICE, "Chip",       nullptr, testChip       },
  { STEP_DEVICE, "FlashPsram", nullptr, testFlashPsram },
  { STEP_DEVICE, "Heap",       nullptr, testHeap       },

  { STEP_GROUP,  "GrpFspi",    "[FSPI 11/12/13] SD + CC1101 + NRF24", nullptr },
  { STEP_DEVICE, "SdCard",     nullptr, testSdCard     },
  { STEP_DEVICE, "CC1101",     nullptr, testCc1101     },
  { STEP_DEVICE, "NRF24 #1",   nullptr, testNrf24_1    },
  { STEP_DEVICE, "NRF24 #2",   nullptr, testNrf24_2    },
  { STEP_DEVICE, "NRF24 #3",   nullptr, testNrf24_3    },

  { STEP_GROUP,  "GrpHspi",    "[HSPI 35/36/37] LCD + TOUCH", nullptr },
  { STEP_DEVICE, "LcdId",      nullptr, testDisplayId  },
  { STEP_DEVICE, "Touch",      nullptr, testTouch      },

  { STEP_GROUP,  "GrpI2c",     "[I2C 8/9] KEYPAD", nullptr },
  { STEP_DEVICE, "I2cScan",    nullptr, testI2cScan    },
  { STEP_DEVICE, "Pcf8574",    nullptr, testPcf8574    },

  { STEP_GROUP,  "GrpAnalog",  "[ANALOG / RF / IO]", nullptr },
  { STEP_DEVICE, "Battery",    nullptr, testBattery    },
  { STEP_DEVICE, "UsbDetect",  nullptr, testUsbDetect  },
  { STEP_DEVICE, "BLE",        nullptr, testBle        },
  { STEP_DEVICE, "WiFi",       nullptr, testWifi       },
  { STEP_DEVICE, "GPS",        nullptr, testGps        },
  { STEP_DEVICE, "MiscIo",     nullptr, testMiscIo     },
};
static const int STEP_COUNT = (int)(sizeof(kSteps) / sizeof(kSteps[0]));

/** 开一轮新的自检：清屏、归零计数、重画标题与复位溯源行。 */
static void beginRun() {
  g_row = 0;
  g_ok = g_fail = g_na = 0;

  // 每轮开头重建一次面板状态（约十几毫秒）。这是自愈保险：万一某一轮里把屏留在了
  // 半初始化 / 白屏状态（例如某个操作把 HSPI 切成了读模式），下一轮开头就能恢复画面 ——
  // 按任意键重开一轮即可，不必重新烧录。
  // 第二次调用 tft.init() 是安全的：TFT_eSPI 内部的 SPIClass 已经 _spi 非空，
  // 不会再次去初始化 HSPI 外设（也就不会触发 periph_module_reset）。
  tft.init();
  tft.setRotation(2);          // 与主工程 TFT_ROTATION 一致
  tft.fillScreen(TFT_BLACK);
  drawFrame();                 // 标题 + 复位溯源行，并把 g_row 归到表格首行
  logClear();                  // 行历史也清空（新一轮）

  Serial.println("\n=========== ESP32-DIV V2 hardware self-test ===========");
  Serial.printf("boot#%u  rst=%s  last_stage=%s\n", (unsigned)g_bootCount,
                resetReasonStr(esp_reset_reason()),
                g_lastStage[0] ? g_lastStage : "(none)");
}

/** 只执行第 i 步，不碰调度状态（s_step 由 advanceStep 维护）。 */
static void runStep(int i) {
  const TestStep& s = kSteps[i];
  if (s.kind == STEP_GROUP) {
    group(s.title);
    return;
  }
  stageNow(s.name);          // 屏幕进度行 + RTC 崩溃溯源
  if (s.fn) {
    s.fn();
  }
}

/** 画汇总行，本轮结束。 */
static void finishRun() {
  const int y = 2 + g_row * ROW_H + 2;
  g_row += 2;
  tft.setTextFont(1);
  tft.setTextColor(g_fail == 0 ? C_OK : C_FAIL, TFT_BLACK);
  tft.setCursor(2, y);
  char sum[32];
  snprintf(sum, sizeof(sum), "RESULT OK=%d FAIL=%d N/A=%d", g_ok, g_fail, g_na);
  tft.print(sum);
  tft.setCursor(2, y + 12);
  tft.setTextColor(C_INFO, TFT_BLACK);
  tft.print("Any key = run again");
  Serial.printf("=========== %s ===========\n\n", sum);
}

/** 前进一步；已到末尾则先清屏重开一轮（沿用当前自动/手动模式）。
 *  顺便给每一步计时：耗时打到串口，并在下一次停顿期间显示在屏幕底部。 */
static void advanceStep() {
  if (s_step >= STEP_COUNT) {
    beginRun();
    s_step = 0;
  }
  const TestStep& st = kSteps[s_step];
  strncpy(g_lastStepName, st.name, sizeof(g_lastStepName) - 1);
  g_lastStepName[sizeof(g_lastStepName) - 1] = '\0';
  g_lastKind = st.kind;

  const uint32_t t0 = millis();
  runStep(s_step);
  g_lastStepMs = millis() - t0;

  Serial.printf("[step %2d/%-2d] %-11s %5u ms%s\n", s_step + 1, STEP_COUNT,
                g_lastStepName, (unsigned)g_lastStepMs,
                g_lastStepMs > 300 ? "  <== 偏慢" : "");

  s_step++;
  if (s_step >= STEP_COUNT) {
    finishRun();
  }
}

/** 分组标题行刚画完时不必停顿 —— 它只是个小标题，紧接着的第一项会一起看见；
 *  否则这个组会白等一整拍（HSPI/I2C 组各有 3 个步骤，凑起来就是好几秒的"卡住"感）。 */
static uint32_t nextPauseMs() {
  return (g_lastKind == STEP_GROUP) ? 0 : STEP_PAUSE_MS;
}

/** 手动模式的底部提示：下一步是第几项 / 已跑完，并带上实时按键字节。 */
static void showManualHint() {
  char buf[48];                       // 留足余量：%d 最坏情况也不会被截断
  if (s_step >= STEP_COUNT) {
    snprintf(buf, sizeof(buf), "done - key = restart");
  } else {
    char ks[16];
    keySuffix(ks, sizeof(ks));
    snprintf(buf, sizeof(buf), "next %d/%d  %s", s_step + 1, STEP_COUNT, ks);
  }
  statusLine(buf);
}

void setup() {
  Serial.begin(115200);
  delay(100);

  // ---- 复位溯源：必须放在最前面，才能捕获"上一次是怎么死的" ----
  g_bootCount++;
  const esp_reset_reason_t rr = esp_reset_reason();
  const char* rrStr = resetReasonStr(rr);
  const char* lastStage = g_lastStage[0] ? g_lastStage : "-";

  g_bootColor = (rr == ESP_RST_POWERON)  ? C_INFO
              : (rr == ESP_RST_BROWNOUT) ? C_FAIL
                                         : C_GRP;
  snprintf(g_bootLine, sizeof(g_bootLine), "#%u %s @%s",
           (unsigned)g_bootCount, rrStr, lastStage);

  Serial.printf("\n[boot] HardwareTest start  boot#%u  rst=%s  last_stage=%s\n",
                (unsigned)g_bootCount, rrStr, lastStage);
  if (rr == ESP_RST_BROWNOUT) {
    Serial.println("[boot] !! 上次是欠压复位(BROWNOUT)：本次背光已降到 40% 压低电流。"
                   "请换 USB 供电，或确认电池+保护板能否带动 WiFi 发射的电流尖峰。");
  } else if (rr == ESP_RST_PANIC && g_lastStage[0]) {
    Serial.printf("[boot] !! 上次软件崩溃(PANIC)，崩溃点在测试项 [%s]\n", g_lastStage);
  }

  // 欠压复位时主动降背光：背光是本板最大的常驻电流负载，先把它压下去，
  // 既能让板子活下来，也是"是不是掉电重启"最直接的对照实验。
  setBacklight(rr == ESP_RST_BROWNOUT ? 100 : 200);

  tft.init();
  tft.setRotation(2);          // 与主工程 TFT_ROTATION 一致
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);

  // ⚠ 必须抢在 SD.begin() 之前用自己的引脚把 FSPI 总线占住。
  //   SDFS::begin() 内部会调 spi.begin()（无参）—— ESP32 的 SPIClass::begin()
  //   在 sck/miso/mosi/ss 全为 -1 时，会把 S3 的 FSPI 强制绑到默认引脚
  //   SCK 36 / MISO 37 / MOSI 35，而那正是显示屏所在的总线，屏幕会当场挂掉。
  //   先带参 begin() 一次后，后续无参 begin() 会因为 _spi 非空而直接 return（SPI.cpp:72），
  //   引脚配置得以保持 —— 主工程的 sdSpiInit() 用的是同一个套路。
  fspi.begin(BUS_SCK, BUS_MISO, BUS_MOSI, SD_CS);
  busReleaseAllCs();

  // 按键（PCF8574）必须在第一项自检之前就绪：否则自动流程已经开跑，
  // 中途按不动，就谈不上"按键立刻停止自动流程"。
  keyInit();

  // 上电先跑第一项，之后的节奏（步间停顿 / 等按键）全部交给 loop()。
  beginRun();
  s_step = 0;
  advanceStep();
}

/* ---------------- 调度：自动逐项跑 ⇄ 手动单步 ----------------
 * loop() 每次只做"一件事"，节奏由 s_manual 决定：
 *   自动 —— 先检查按键（按下则立刻停机转手动，且不执行下一项），否则停顿 STEP_PAUSE_MS 后前进一步；
 *   手动 —— 等一次按键沿，按下才前进一步；
 *   已跑完 —— 等按键，按下则清屏重来一轮。
 */
/** 底部状态行**始终**刷新：任何状态下都能实时看到按键端口字节。
 *  之前只有"停顿倒计时"和"手动提示"会写这一行 —— 一旦整轮跑完停在结果页，这行就不再更新，
 *  于是"按键没反应"和"屏幕没刷新"看起来完全一样，没法区分。这里加一道定时刷新堵住这个歧义；
 *  另外每 5 s 在串口打一行心跳，顺带证明固件还活着（没有卡死 / 没有反复重启）。 */
static void statusTick(const char* tag) {
  static uint32_t lastMs  = 0;
  static uint32_t lastLog = 0;
  const uint32_t now = millis();
  if (now - lastMs < 150) {
    return;
  }
  lastMs = now;

  char ks[16];
  keySuffix(ks, sizeof(ks));
  char buf[48];
  snprintf(buf, sizeof(buf), "%s  %s", tag, ks);
  statusLine(buf);

  if (now - lastLog >= 5000) {
    lastLog = now;
    Serial.printf("[key] live %s  usableKeys=0x%02X  (loop alive)\n", ks, g_keyGoodMask);
  }
}

void loop() {
  // 本轮已跑完：停在结果页，按键 = 清屏重来一轮（沿用当前模式）
  if (s_step >= STEP_COUNT) {
    statusTick("done key=restart");
    if (keyEdge()) {
      advanceStep();                                  // 内部检测到超界 → beginRun() + 第 1 项
      if (s_manual && s_step < STEP_COUNT) {
        showManualHint();
      }
    } else {
      delay(KEY_POLL_MS);
    }
    return;
  }

  // 手动单步：每按一次键，才跑一项
  if (s_manual) {
    statusTick("wait key");
    if (keyEdge()) {
      advanceStep();
      if (s_step < STEP_COUNT) {
        showManualHint();
      }
    } else {
      delay(KEY_POLL_MS);
    }
    return;
  }

  // 自动模式：按键 → 立刻停止自动流程，转手动单步；
  // 停顿期间按键同样生效（pauseInterruptible 返回 true）。
  if (keyEdge() || pauseInterruptible(nextPauseMs())) {
    s_manual = true;
    Serial.println("[mode] manual single-step  (auto flow stopped by key)");
    showManualHint();
    return;                                           // 本次按键只用于"停下"，不推进
  }

  advanceStep();
}
