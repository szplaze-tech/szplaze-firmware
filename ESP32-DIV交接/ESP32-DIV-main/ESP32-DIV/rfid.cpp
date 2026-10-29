#include "rfid.h"

#include <Adafruit_PN532.h>
#include <SPI.h>
#include <Wire.h>
#include <Arduino.h>
#include <cstdio>
#include <cstring>

#include "SettingsStore.h"
#include "Touchscreen.h"
#include "icon.h"
#include "utils.h"
#include "shared.h"

#ifndef RFID_UID_CLONE
#define RFID_UID_CLONE 1
#endif

/** Passive listen max wait (ms). Old 60000 felt like “~1 minute” when no tag. Override in build flags if needed. */
#ifndef RFID_TAG_LISTEN_TIMEOUT_MS
#define RFID_TAG_LISTEN_TIMEOUT_MS 20000
#endif
/** Clone flow waits for source / blank tag (ms). */
#ifndef RFID_CLONE_PHASE_TIMEOUT_MS
#define RFID_CLONE_PHASE_TIMEOUT_MS 45000
#endif
/** Jam / disrupt: wait for external reader to poll target (ms). 0 = until Stop. */
#ifndef RFID_TARGET_WAIT_TIMEOUT_MS
#define RFID_TARGET_WAIT_TIMEOUT_MS 120000
#endif

#if RFID_USE_I2C
/* I2C 传输：复用板上已有的 I2C 总线（SDA 8 / SCL 9），**零新增引脚**。
 * 库内地址固定 0x24，与 PCF8574(0x20) / IP5306(0x75) 不冲突。
 * IRQ 传 -1：I2C 下库的 isready() 读 RDY 字节、不依赖 IRQ 脚。
 * RST 也传 -1 **不是**因为不用复位 —— 复位交给 RfidNfc::hardwareReset() 自己做。
 * 原因是库在**构造函数**里就会 `pinMode(_reset, OUTPUT)`，而全局对象的构造发生在
 * `initArduino()` **之前**，那时调 pinMode 有踩到未初始化外设管理器的风险
 * （可能开机即挂）。放到 begin 阶段的 hardwareReset() 里做完全等价、且更安全。 */
static Adafruit_PN532 s_nfc(PN532_I2C_IRQ, -1, &Wire);
#else
static Adafruit_PN532 s_nfc(PN532_SCK, PN532_MISO, PN532_MOSI, PN532_SS);
#endif
static bool s_hwOk = false;
static char s_pn532VerStr[28] = "";
static bool s_rfidRetrySession = false;
static FeatureUI::Button s_rfFoot[2];

/* NullTag-style card family */
enum CardType { UNKNOWN, MIFARE_CLASSIC, MIFARE_ULTRALIGHT, NTAG, MIFARE_DESFIRE };

enum class RfidUiEvt { None, Back, Primary };

#if RFID_USE_I2C
/** 临时放宽 I2C 超时：PN532 在唤醒握手 / SAMConfig 期间会 clock stretch，
 *  板上常规的 50ms 可能不够。用完必须恢复，否则总线真卡死时
 *  statusBarTask（每 400ms 轮询 IP5306）会被长时间阻塞。 */
static void rfidI2cWiden()   { Wire.setTimeOut(PN532_I2C_TIMEOUT_MS); }
static void rfidI2cRestore() { Wire.setTimeOut(I2C_BUS_TIMEOUT_MS); }
#else
/* SPI 模式不需要动 I2C 超时，保留空实现让调用点保持单一代码路径。 */
static inline void rfidI2cWiden() {}
static inline void rfidI2cRestore() {}
#endif

/** 给 PN532 一次硬复位（拉低 RSTPDN 再释放）。
 *
 *  为什么必须做：RSTPDN 悬空时 PN532 可能停留在复位态，
 *  **连 I2C 地址都不应答** —— 表现就是总线扫描里 0x24 完全消失，
 *  而同一条总线上的 0x20 / 0x75 正常。这种情况靠软件配置救不回来，
 *  只能由主机把复位脚拉低再释放。
 *
 *  为什么在这里做而不是交给库：见 s_nfc 构造处的注释 ——
 *  库会在全局构造期就 pinMode，早于 initArduino()。 */
static void rfidResetPulse() {
#if RFID_USE_I2C && (PN532_I2C_RST >= 0)
  pinMode(PN532_I2C_RST, OUTPUT);
  digitalWrite(PN532_I2C_RST, LOW);
  delay(5);                       // 复位脉冲（手册下限 20ns，这里给足）
  digitalWrite(PN532_I2C_RST, HIGH);
  delay(150);                     // 释放后给芯片启动时间，再发第一条命令
#endif
}

#if RFID_PROBE_TRACE
/** 探测分步计时：把"卡在哪一步、这一步已经耗了多久"直接写在屏幕上
 *  （画在自检条下方 y=46 起那一行）。排查完把 RFID_PROBE_TRACE 置 0 即可去掉。
 *
 *  只能显示"两次阻塞调用之间"的状态 —— 若卡在某次调用**内部**，
 *  屏上留下的正好是"进入那次调用前"的标记，用来定位是哪一次调用。
 *  配合共享变量 s_probeT0 每轮探测归零。 */
static uint32_t s_probeT0 = 0;
static void rfidProbeStep(const char* stage) {
  tft.fillRect(0, 46, tft.width(), 16, UI_BG);
  uiSetTextSize(1);
  uiSetTextFont(1);
  uiSetTextColor(UI_WARN, UI_BG);
  uiSetCursor(4, 47);
  uiPrint(stage);
  uiPrint("  t=");
  uiPrint((int)(millis() - s_probeT0));
  uiPrint("ms");
}
#else
static inline void rfidProbeStep(const char*) {}
#endif

/** 让共享总线上的其它器件"闭嘴"。
 *  PN532 的 bitbang 会在 GPIO11/12/13 上直接推信号，而这三根同时也是
 *  SD / CC1101 的总线。其它器件的 CS 空闲时本应为高（MISO 高阻），
 *  但只要有一个还停在低（上个功能没恢复干净、或初始化中途被打断），
 *  它的 MISO 就会去驱动我们正在推的那根线 —— 典型后果就是
 *  **完全读不到 PN532，连 StatusRead 都拿不到有效字节**。
 *  只做"拉高片选"，不动 SD 的挂载状态。 */
static void rfidQuiesceSharedBus() {
#ifdef CC1101_CS
  pinMode(CC1101_CS, OUTPUT);
  digitalWrite(CC1101_CS, HIGH);
#endif
#ifdef SD_CS
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
#endif
}

static void rfidAttachBus() {
#if RFID_USE_I2C
  // I2C 模式下**什么都不用做**：
  //   - 模块不占 SPI 总线，SD 卡一直保持挂载，没有"抢总线"这回事；
  //   - init / SAMConfig / 重试次数都已在 RfidNfc::begin() 里配好，
  //     PN532 掉电前会一直保持这些设置。
  // 原来这里每进一个会话都重跑 s_nfc.begin() + SAMConfig() +
  // setPassiveActivationRetries()，那是 SPI 模式（bitbang 每次都要重挂总线）
  // 遗留的做法。在 I2C 下纯属多余，而且握手慢时每个命令要几百 ms，
  // 直接表现为"进了会话半天不出界面"。
  (void)0;
#else
  // Adafruit_PN532 is constructed for software SPI (bitbang). Do NOT call
  // SPI.begin(PN532_*) here: on DIV V2 the PN532 MOSI/MISO pins are swapped
  // vs the SD/CC1101 bus, and remapping hardware SPI that way leaves the SD
  // card dead until something else (e.g. SubGHz) re-inits the bus correctly.
  rfidQuiesceSharedBus();     // 先把别的片选拉高，避免 MISO/MOSI 被争用
  if (s_hwOk) {
    s_nfc.begin();
    s_nfc.SAMConfig();
    s_nfc.setPassiveActivationRetries(PN532_PASSIVE_RETRIES);
  }
#endif
}

/** 离开一个会话时归还总线。
 *  I2C 模式不占 SPI，无需重挂 SD（SD 会一直挂着，反而更好）；
 *  SPI 模式必须归还，否则 SD 卡要等别的功能重新初始化总线才活。 */
static void rfidRestoreBus() {
#if RFID_USE_I2C
  requestStatusBarRedraw();
#else
  restoreSdAfterSharedSpi();
#endif
}

static void rfidPn532VerStr(uint32_t ver) {
  if (!ver) {
    s_pn532VerStr[0] = '\0';
    return;
  }
  snprintf(s_pn532VerStr, sizeof(s_pn532VerStr), "PN532 0x%06lX", (unsigned long)ver);
}

static void rfidReleaseNavButtons() {
  delay(12);
  for (int i = 0; i < 80; i++) {
    if (!isButtonPressed(BTN_SELECT) && !isButtonPressed(BTN_LEFT) &&
        !isButtonPressed(BTN_RIGHT) && !isButtonPressed(BTN_UP) &&
        !isButtonPressed(BTN_DOWN)) {
      break;
    }
    delay(5);
  }
}

static constexpr int kRfidScreenH = 320;
static constexpr int16_t kRfidToolbarY = 20;
static constexpr int16_t kRfidToolbarH = 16;
static constexpr int kRfidToolbarBottom = kRfidToolbarY + kRfidToolbarH;
static constexpr int kRfidIconBackX = 10;
static constexpr int kRfidIconSize = 16;
static bool s_rfidUiDrawn = false;

static int rfidContentBottom() {
  return featureHasTouchNavBar() ? touchNavContentBottomY() : kRfidScreenH;
}

static void rfidClearBody(uint16_t color = FEATURE_BG) {
  if (featureHasTouchNavBar()) {
    featureClearContent(color);
  } else {
    tft.fillScreen(color);
  }
}

static void rfidRedrawNavChrome() {
  redrawTouchButtonBar();
  maintainTouchNavBar();
}

static bool rfidSessionExitRequested() { return feature_exit_requested; }

static void rfidPollToolbar() {
  if (!feature_active) {
    return;
  }
  static unsigned long s_lastToolbarCheck = 0;
  if (millis() - s_lastToolbarCheck < 50) {
    return;
  }
  s_lastToolbarCheck = millis();
  int x = 0;
  int y = 0;
  if (!readTouchXY(x, y)) {
    return;
  }
  if (y > kRfidToolbarY && y < kRfidToolbarBottom && x > kRfidIconBackX &&
      x < (kRfidIconBackX + kRfidIconSize)) {
    feature_exit_requested = true;
  }
}

static bool rfidPumpSessionUi() {
  if (rfidSessionExitRequested()) {
    return true;
  }
  maintainTouchNavBar();
  rfidPollToolbar();
  if (s_rfidUiDrawn) {
    tft.drawFastHLine(0, kRfidToolbarY, TFT_WIDTH, UI_LINE);
    tft.drawFastHLine(0, kRfidToolbarBottom, TFT_WIDTH, UI_LINE);
  }
  return false;
}

static RfidUiEvt rfidPollFooter(FeatureUI::Button* btns, int n, bool mapSelectToPrimary) {
  if (isButtonPressed(BTN_LEFT)) {
    rfidReleaseNavButtons();
    return RfidUiEvt::Back;
  }
  if (n == 1 && isButtonPressed(BTN_SELECT)) {
    rfidReleaseNavButtons();
    return RfidUiEvt::Back;
  }
  if (mapSelectToPrimary && n >= 2 && isButtonPressed(BTN_SELECT)) {
    if (!btns[1].disabled) {
      rfidReleaseNavButtons();
      return RfidUiEvt::Primary;
    }
  }
  if (featureHasTouchNavBar()) {
    if (isTouchNavButtonPressedEdge(BTN_LEFT)) {
      rfidReleaseNavButtons();
      return RfidUiEvt::Back;
    }
    if (isTouchNavButtonPressedEdge(BTN_SELECT)) {
      if (mapSelectToPrimary && n >= 2 && !btns[1].disabled) {
        rfidReleaseNavButtons();
        return RfidUiEvt::Primary;
      }
      rfidReleaseNavButtons();
      return RfidUiEvt::Back;
    }
    return RfidUiEvt::None;
  }
  int x = 0, y = 0;
  if (!readTouchXYDismiss(x, y) && !readTouchXY(x, y)) {
    return RfidUiEvt::None;
  }
  int h = FeatureUI::hit(btns, n, x, y);
  if (h < 0) {
    return RfidUiEvt::None;
  }
  if (btns[h].disabled) {
    return RfidUiEvt::None;
  }
  rfidReleaseNavButtons();
  if (h == 0) {
    return RfidUiEvt::Back;
  }
  return RfidUiEvt::Primary;
}

static constexpr int RF_PAD_X = 10;
static constexpr int RF_RESULT_LINE_PX = 17;
static constexpr int RF_BODY_LINE_PX = 18;
static constexpr int RF_TAG_H = 14;
static constexpr int RF_TAG_GAP = 4;
/** Ignored — kept so session code need not change. */
static constexpr int RF_DYN_TOP_MAIN = 0;
static constexpr int RF_DYN_TOP_PROG = 0;

/** IR Record / 2.4GHz Scanner rhythm */
static constexpr int kRfidToolbarGap = 8;
static constexpr int kRfidBoxHeaderH = 15;
static constexpr int kRfidLineHeight = 12;
static constexpr int kRfidStatusY = kRfidToolbarBottom + kRfidToolbarGap;
static constexpr int kRfidStatusBoxH = 91;
static constexpr int kRfidLogGap = 4;
static constexpr int kRfidLogBoxH = 49;
static constexpr int kRfidLogBoxTop = kRfidStatusY + kRfidStatusBoxH + kRfidLogGap;
static constexpr int kRfidLogStartY = kRfidLogBoxTop + kRfidBoxHeaderH;
static constexpr int kRfidLogEndY = kRfidLogBoxTop + kRfidLogBoxH - 2;
static constexpr int kRfidDetailTop = kRfidLogBoxTop + kRfidLogBoxH + 4;
static constexpr int kRfidStatusLineCount = 6;
static constexpr int kRfidStatusTextY = kRfidStatusY + kRfidBoxHeaderH;
static constexpr int kRfidMaxLogLines = 48;
static constexpr int kRfidDetailPadX = RF_PAD_X;
static constexpr int kRfidDetailInnerW = 220;
static constexpr int kRfidDetailHeaderH = 18;

static int rfidHintY() {
  if (featureHasTouchNavBar()) {
    return rfidContentBottom() - 16;
  }
  return TFT_HEIGHT - FeatureUI::FOOTER_H - 16;
}

static int rfidDetailBottom() {
  if (featureHasTouchNavBar()) {
    return rfidContentBottom() - 8;
  }
  return rfidHintY() - 8;
}

static int rfidEffectiveDetailTop() {
  const int bottom = rfidDetailBottom();
  const int minDetailH = 80;
  int top = kRfidDetailTop;
  if (bottom - top < minDetailH) {
    top = bottom - minDetailH;
    const int logBottom = kRfidLogBoxTop + kRfidLogBoxH;
    if (top < logBottom + 2) {
      top = logBottom + 2;
    }
  }
  return top;
}

static int rfidDetailBodyTop() { return rfidEffectiveDetailTop() + kRfidBoxHeaderH + 2; }

static int rfidDetailLinesPerPage() {
  const int bodyBottom = rfidDetailBottom() - 4;
  const int n = (bodyBottom - rfidDetailBodyTop()) / kRfidLineHeight;
  return n < 1 ? 1 : n;
}

static bool s_rfidBoxesDrawn = false;
static bool s_rfidStatusStaticDrawn = false;
static bool s_rfidDetailBoxVisible = false;
static char s_featureInfoTitle[32];
static char s_featureInfoBody[640];
static bool s_featureInfoAvailable = false;
static char s_listenCenterLabel[24];
static String s_rfidLogBuffer[kRfidMaxLogLines];
static uint16_t s_rfidLogColor[kRfidMaxLogLines];
static int s_rfidLogIndex = 0;
static String s_rfidStatusLineText[kRfidStatusLineCount];
static uint16_t s_rfidStatusLineColor[kRfidStatusLineCount];

static void rfidPrintWrappedPageBg(int x, int y, int maxWidth, const char* text, int lineStepPx,
                                   uint16_t fg, uint16_t bg, int firstLine, int maxLines);
static int rfidCountWrappedLines(int maxWidth, const char* text);
static bool rfidDetailTagNeedsPill(const char* tagLabel);

static constexpr int kRfidInfoHeaderH = 18;
static constexpr int kRfidInfoTitleH = 15;
static constexpr int kRfidInfoPanelPad = 8;
static constexpr int kRfidInfoLinePx = 16;
/** Match Activity / log box chrome (drawRoundRect at x=4, w=TFT_WIDTH-8). */
static constexpr int kRfidInfoBoxX = 4;
static constexpr int kRfidInfoBoxW = TFT_WIDTH - 8;
static constexpr int kRfidInfoInnerX = 6;
static constexpr int kRfidInfoInnerW = TFT_WIDTH - 12;
static constexpr int kRfidInfoTextW = TFT_WIDTH - RF_PAD_X * 2;

static int rfidInfoPanelPageCountForLines(const char* body, int linesPerPage) {
  int total = rfidCountWrappedLines(kRfidInfoTextW, body);
  int pageCount = (total + linesPerPage - 1) / linesPerPage;
  return pageCount < 1 ? 1 : pageCount;
}

static int rfidDetailOverlayTop() { return kRfidStatusY; }

static int rfidInfoContentTop(bool withFeatureTitle) {
  int y = rfidDetailOverlayTop() + kRfidInfoHeaderH + 1 + 5;
  if (withFeatureTitle) {
    y += kRfidInfoTitleH + 4;
  }
  return y + kRfidInfoPanelPad;
}

static int rfidInfoContentBottom() { return rfidDetailBottom() - 6; }

static int rfidInfoOverlayLinesPerPage(bool withFeatureTitle) {
  const int panelTop = rfidInfoContentTop(withFeatureTitle) - kRfidInfoPanelPad;
  const int panelH = rfidInfoContentBottom() - panelTop;
  const int innerH = panelH - kRfidInfoPanelPad * 2;
  const int n = innerH / kRfidInfoLinePx;
  return n < 1 ? 1 : n;
}

static void rfidDrawInfoPill(int x, int y, int w, const char* label) {
  if (w < 28) {
    w = 28;
  }
  tft.fillRoundRect(x, y, w, RF_TAG_H, 7, DARK_GRAY);
  uiSetTextColor(UI_ICON, DARK_GRAY);
  uiSetCursor(x + 6, y + 3);
  uiPrint(label);
}

static String rfidFitInfoTitle(const char* title) {
  if (!title || !title[0]) {
    return String();
  }
  String t = title;
  const int maxW = TFT_WIDTH - RF_PAD_X * 2;
  const bool needsEllipsis = uiTextWidth(t) > maxW;
  while (t.length() > 0 && uiTextWidth(needsEllipsis ? t + "..." : t) > maxW) {
    t.remove(t.length() - 1);
  }
  if (needsEllipsis && t.length() > 0) {
    t += "...";
  }
  return t;
}

static void rfidDrawInfoOverlay(const char* featureTitle, const char* body, int page, int pageCount,
                                int linesPerPage) {
  const int top = rfidDetailOverlayTop();
  const int bottom = rfidDetailBottom();
  const bool showTitle = featureTitle && featureTitle[0];

  tft.fillRect(0, top - 2, TFT_WIDTH, bottom - top + 2, TFT_BLACK);
  tft.drawFastHLine(0, 19, TFT_WIDTH, UI_LINE);

  uiSetTextFont(1);
  uiSetTextSize(1);

  const int headerY = top + 3;
  uiSetTextColor(UI_TEXT, TFT_BLACK);
  uiDrawString("Info", RF_PAD_X, headerY);

  if (pageCount > 1) {
    char pageLabel[12];
    snprintf(pageLabel, sizeof(pageLabel), "%d/%d", page + 1, pageCount);
    const int pw = (int)uiTextWidth(pageLabel) + 14;
    rfidDrawInfoPill(TFT_WIDTH - RF_PAD_X - pw, headerY, pw, pageLabel);
  }

  int y = top + kRfidInfoHeaderH;
  tft.drawFastHLine(kRfidInfoBoxX, y, kRfidInfoBoxW, UI_LINE);
  y += 5;

  if (showTitle) {
    uiSetTextColor(UI_TEXT, TFT_BLACK);
    uiDrawString(rfidFitInfoTitle(featureTitle), RF_PAD_X, y);
    y += kRfidInfoTitleH;
    tft.drawFastHLine(kRfidInfoBoxX, y - 2, kRfidInfoBoxW, UI_LINE);
    y += 4;
  }

  const int panelY = y;
  const int panelH = bottom - panelY - 4;
  if (panelH > 8) {
    tft.drawRoundRect(kRfidInfoBoxX, panelY, kRfidInfoBoxW, panelH, 3, UI_LINE);
    if (panelH > 4) {
      tft.fillRect(kRfidInfoInnerX, panelY + 2, kRfidInfoInnerW, panelH - 4, DARK_GRAY);
    }
  }

  const int textX = RF_PAD_X;
  const int textY = panelY + kRfidInfoPanelPad;
  const int textBottom = panelY + panelH - kRfidInfoPanelPad;
  const int maxLinesInPanel = (textBottom - textY) / kRfidInfoLinePx;
  const int linesToDraw = (linesPerPage < maxLinesInPanel) ? linesPerPage : maxLinesInPanel;

  rfidPrintWrappedPageBg(textX, textY, kRfidInfoTextW, body, kRfidInfoLinePx, UI_TEXT, DARK_GRAY,
                         page * linesPerPage, linesToDraw);

  if (pageCount > 1 && panelH > 20) {
    uiSetTextColor(UI_DIM_TEXT, DARK_GRAY);
    if (page > 0) {
      uiSetCursor(kRfidInfoInnerX + 4, panelY + panelH - 12);
      uiPrint("< Prev");
    }
    if (page < pageCount - 1) {
      const char* nextHint = "Next >";
      const int nw = uiTextWidth(nextHint);
      uiSetCursor(kRfidInfoInnerX + kRfidInfoInnerW - nw - 4, panelY + panelH - 12);
      uiPrint(nextHint);
    }
  }
}

static void rfidDynamicBand(int topY, const char* line1, const char* line2);
static void rfidTryShowInfo();
static void rfidWaitTwoBoxDismiss();
static void rfidRunToolbar(const char* title);
static void rfidDrawTextBoxes();
static void rfidResetUiState();
static void rfidPrint(const String& text, uint16_t color, bool extraSpace = false);
static void rfidDrawStatusLineIfChanged(int line, const String& text, uint16_t color);

/** Dim one-line caption for NFC Status (listen/progress); cleared in rfidFeatureSetup. */
static char s_rfidBandAux[72];

static void rfidFeatureSetup(const char* title) {
  applyThemeToPalette(settings().theme);
  rfidResetUiState();
  rfidClearBody(TFT_BLACK);
  drawStatusBar(readBatteryVoltage(), true);
  s_rfidBandAux[0] = '\0';
  rfidRunToolbar(title);
  rfidDrawTextBoxes();
  rfidRedrawNavChrome();
}

static void rfidSetDialogNavLabels(const char* primaryLabel) {
  setTouchNavLabels("Back", nullptr, primaryLabel, nullptr, nullptr);
  redrawTouchButtonBar();
}

static void rfidSetListenNavLabels(const char* actionLabel, bool withInfo = false) {
  setTouchNavLabels(nullptr, nullptr, actionLabel, nullptr, withInfo ? "Info" : nullptr);
  redrawTouchButtonBar();
}

static void rfidSetInfoOverlayNavLabels(int pageCount) {
  if (pageCount > 1) {
    setTouchNavLabels("Back", "Prev", "Back", "Next", nullptr);
  } else {
    setTouchNavLabels("Back", nullptr, "Back", nullptr, nullptr);
  }
  redrawTouchButtonBar();
}

static void rfidSetTimeoutNavLabels(bool withInfo) {
  setTouchNavLabels("Exit", nullptr, "Ready", nullptr, withInfo ? "Info" : nullptr);
  redrawTouchButtonBar();
}

static void rfidDrawShellFooter1(const char* actionLabel) {
  FeatureUI::layoutFooter1(s_rfFoot[0], actionLabel, FeatureUI::ButtonStyle::Secondary);
  if (actionLabel && actionLabel[0]) {
    strncpy(s_listenCenterLabel, actionLabel, sizeof(s_listenCenterLabel) - 1);
    s_listenCenterLabel[sizeof(s_listenCenterLabel) - 1] = '\0';
  } else {
    s_listenCenterLabel[0] = '\0';
  }
  if (featureHasTouchNavBar()) {
    rfidSetListenNavLabels(actionLabel, s_featureInfoAvailable);
  } else {
    FeatureUI::drawFooterBg();
    FeatureUI::drawButton(s_rfFoot[0]);
  }
}

static void rfidDrawShellFooter2(const char* primaryLabel, FeatureUI::ButtonStyle primaryStyle,
                                 bool primaryDisabled) {
  FeatureUI::layoutFooter2(s_rfFoot, "Back", FeatureUI::ButtonStyle::Secondary, primaryLabel,
                             primaryStyle, false, primaryDisabled);
  if (featureHasTouchNavBar()) {
    rfidSetDialogNavLabels(primaryLabel);
  } else {
    FeatureUI::drawFooterBg();
    FeatureUI::drawButton(s_rfFoot[0]);
    FeatureUI::drawButton(s_rfFoot[1]);
  }
}

static void rfidSetBandAux(const char* s) {
  if (!s || !s[0]) {
    s_rfidBandAux[0] = '\0';
    return;
  }
  strncpy(s_rfidBandAux, s, sizeof(s_rfidBandAux) - 1);
  s_rfidBandAux[sizeof(s_rfidBandAux) - 1] = '\0';
}

/** IR Record-style toolbar: DARK_GRAY bar, title left, back icon. */
static void rfidRunToolbar(const char* title) {
  if (s_rfidUiDrawn) {
    return;
  }
  tft.fillRect(0, kRfidToolbarY, TFT_WIDTH, kRfidToolbarH, DARK_GRAY);
  tft.drawBitmap(kRfidIconBackX, kRfidToolbarY, bitmap_icon_go_back, kRfidIconSize, kRfidIconSize,
                 UI_ICON);
  uiSetTextColor(UI_TEXT, DARK_GRAY);
  uiSetTextSize(1);
  if (title && title[0]) {
    const int titleX = 35;
    const int maxTitleW = TFT_WIDTH - titleX - 8;
    String t = title;
    const bool needsEllipsis = uiTextWidth(t) > maxTitleW;
    while (t.length() > 0 && uiTextWidth(needsEllipsis ? t + "..." : t) > maxTitleW) {
      t.remove(t.length() - 1);
    }
    if (needsEllipsis) {
      t += "...";
    }
    uiSetCursor(titleX, kRfidToolbarY + 4);
    uiPrint(t);
  }
  tft.drawFastHLine(0, 19, TFT_WIDTH, UI_LINE);
  tft.drawFastHLine(0, kRfidToolbarBottom, TFT_WIDTH, UI_LINE);
  s_rfidUiDrawn = true;
}

static void rfidUpdateToolbarTitle(const char* title) {
  s_rfidUiDrawn = false;
  rfidRunToolbar(title);
}

static void rfidPrintWrappedStepBg(int x, int y, int maxWidth, const char* text, int lineStepPx,
                                   uint16_t fg, uint16_t bg, int bottomY = TFT_HEIGHT) {
  String message = text ? text : "";
  int cursorY = y;
  bool clipped = false;

  while (message.length() > 0) {
    if (cursorY > bottomY - lineStepPx) {
      clipped = true;
      break;
    }

    int nl = message.indexOf('\n');
    String paragraph = (nl >= 0) ? message.substring(0, nl) : message;
    message = (nl >= 0) ? message.substring(nl + 1) : "";

    if (paragraph.length() == 0) {
      cursorY += lineStepPx;
      continue;
    }

    while (paragraph.length() > 0) {
      if (cursorY > bottomY - lineStepPx) {
        clipped = true;
        break;
      }

      int lineEnd = (int)paragraph.length();
      while (uiTextWidth(paragraph.substring(0, lineEnd)) > maxWidth && lineEnd > 1) {
        lineEnd--;
      }
      if (lineEnd < (int)paragraph.length()) {
        int lastSpace = paragraph.substring(0, lineEnd).lastIndexOf(' ');
        if (lastSpace > 0) {
          lineEnd = lastSpace;
        }
      }

      uiSetTextColor(fg, bg);
      uiSetCursor(x, cursorY);
      uiPrint(paragraph.substring(0, lineEnd));
      paragraph = paragraph.substring(lineEnd);
      paragraph.trim();
      cursorY += lineStepPx;
    }
  }

  if (clipped && bottomY >= y + lineStepPx) {
    uiSetTextColor(UI_DIM_TEXT, bg);
    uiSetCursor(x, bottomY - lineStepPx);
    uiPrint("...");
  }
}

static void rfidPrintWrappedStep(int x, int y, int maxWidth, const char* text, int lineStepPx) {
  rfidPrintWrappedStepBg(x, y, maxWidth, text, lineStepPx, UI_TEXT, FEATURE_BG);
}

static int rfidCountWrappedLines(int maxWidth, const char* text) {
  String message = text ? text : "";
  int total = 0;
  while (message.length() > 0) {
    int nl = message.indexOf('\n');
    String paragraph = (nl >= 0) ? message.substring(0, nl) : message;
    message = (nl >= 0) ? message.substring(nl + 1) : "";

    if (paragraph.length() == 0) {
      total++;
      continue;
    }

    while (paragraph.length() > 0) {
      int lineEnd = (int)paragraph.length();
      while (uiTextWidth(paragraph.substring(0, lineEnd)) > maxWidth && lineEnd > 1) {
        lineEnd--;
      }
      if (lineEnd < (int)paragraph.length()) {
        int lastSpace = paragraph.substring(0, lineEnd).lastIndexOf(' ');
        if (lastSpace > 0) {
          lineEnd = lastSpace;
        }
      }
      paragraph = paragraph.substring(lineEnd);
      paragraph.trim();
      total++;
    }
  }
  return total > 0 ? total : 1;
}

static void rfidPrintWrappedPageBg(int x, int y, int maxWidth, const char* text, int lineStepPx,
                                   uint16_t fg, uint16_t bg, int firstLine, int maxLines) {
  String message = text ? text : "";
  int lineNo = 0;
  int drawn = 0;
  while (message.length() > 0 && drawn < maxLines) {
    int nl = message.indexOf('\n');
    String paragraph = (nl >= 0) ? message.substring(0, nl) : message;
    message = (nl >= 0) ? message.substring(nl + 1) : "";

    if (paragraph.length() == 0) {
      if (lineNo >= firstLine) {
        drawn++;
      }
      lineNo++;
      continue;
    }

    while (paragraph.length() > 0 && drawn < maxLines) {
      int lineEnd = (int)paragraph.length();
      while (uiTextWidth(paragraph.substring(0, lineEnd)) > maxWidth && lineEnd > 1) {
        lineEnd--;
      }
      if (lineEnd < (int)paragraph.length()) {
        int lastSpace = paragraph.substring(0, lineEnd).lastIndexOf(' ');
        if (lastSpace > 0) {
          lineEnd = lastSpace;
        }
      }

      if (lineNo >= firstLine) {
        uiSetTextColor(fg, bg);
        uiSetCursor(x, y + drawn * lineStepPx);
        uiPrint(paragraph.substring(0, lineEnd));
        drawn++;
      }
      paragraph = paragraph.substring(lineEnd);
      paragraph.trim();
      lineNo++;
    }
  }
}

static String rfidFitStatusText(const String& text) {
  const int maxWidth = TFT_WIDTH - 16;
  uiSetTextSize(1);
  if (uiTextWidth(text) <= maxWidth) {
    return text;
  }
  String out = text;
  while (out.length() > 1 && uiTextWidth(out + "...") > maxWidth) {
    out.remove(out.length() - 1);
  }
  if (out.length() > 0) {
    out += "...";
  }
  return out;
}

static constexpr int kRfidLogTextX = 10;
static constexpr int kRfidLogTextW = TFT_WIDTH - kRfidLogTextX * 2;

static String rfidFitLogText(const String& text) {
  uiSetTextSize(1);
  if (uiTextWidth(text) <= kRfidLogTextW) {
    return text;
  }
  String out = text;
  while (out.length() > 1 && uiTextWidth(out + "...") > kRfidLogTextW) {
    out.remove(out.length() - 1);
  }
  if (out.length() > 0) {
    out += "...";
  }
  return out;
}

static void rfidScrollLog() {
  for (int i = 0; i < kRfidMaxLogLines - 1; i++) {
    s_rfidLogBuffer[i] = s_rfidLogBuffer[i + 1];
    s_rfidLogColor[i] = s_rfidLogColor[i + 1];
  }
}

static int rfidLogAreaBottom() {
  if (s_rfidDetailBoxVisible) {
    return kRfidLogEndY;
  }
  const int logH = rfidDetailBottom() - kRfidLogBoxTop;
  return kRfidLogBoxTop + logH - 3;
}

static int rfidLogBoxDrawHeight() {
  if (s_rfidDetailBoxVisible) {
    return kRfidLogBoxH;
  }
  return rfidDetailBottom() - kRfidLogBoxTop;
}

static void rfidSetDetailBoxVisible(bool visible) {
  if (s_rfidDetailBoxVisible == visible) {
    return;
  }
  s_rfidDetailBoxVisible = visible;
  s_rfidBoxesDrawn = false;
}

static int rfidLogVisibleLines() {
  const int h = rfidLogAreaBottom() - kRfidLogStartY;
  if (h <= 0 || kRfidLineHeight <= 0) {
    return 1;
  }
  return h / kRfidLineHeight;
}

static void rfidRedrawActivityLog() {
  const int visible = rfidLogVisibleLines();
  const int logBottom = rfidLogAreaBottom();
  tft.fillRect(6, kRfidLogStartY, TFT_WIDTH - 12, logBottom - kRfidLogStartY, TFT_BLACK);
  if (visible <= 0 || s_rfidLogIndex <= 0) {
    return;
  }
  const int start = (s_rfidLogIndex > visible) ? (s_rfidLogIndex - visible) : 0;
  for (int row = 0; row < visible; row++) {
    const int bufIndex = start + row;
    if (bufIndex >= s_rfidLogIndex) {
      break;
    }
    if (s_rfidLogBuffer[bufIndex].length() == 0) {
      continue;
    }
    const int yPos = kRfidLogStartY + row * kRfidLineHeight;
    uiSetTextSize(1);
    uiSetTextColor(s_rfidLogColor[bufIndex], TFT_BLACK);
    uiSetCursor(kRfidLogTextX, yPos);
    uiPrint(rfidFitLogText(s_rfidLogBuffer[bufIndex]));
  }
}

static void rfidAppendLogLine(const String& line, uint16_t color) {
  if (line.length() == 0) {
    return;
  }
  if (s_rfidLogIndex >= kRfidMaxLogLines) {
    rfidScrollLog();
    s_rfidLogIndex = kRfidMaxLogLines - 1;
  }
  s_rfidLogBuffer[s_rfidLogIndex] = rfidFitLogText(line);
  s_rfidLogColor[s_rfidLogIndex] = color;
  s_rfidLogIndex++;
}

static void rfidPrint(const String& text, uint16_t color, bool extraSpace) {
  uiSetTextSize(1);
  String remaining = text;
  while (remaining.length() > 0) {
    const int nl = remaining.indexOf('\n');
    if (nl >= 0) {
      rfidAppendLogLine(remaining.substring(0, nl), color);
      remaining = remaining.substring(nl + 1);
      continue;
    }
    if (uiTextWidth(remaining) <= kRfidLogTextW) {
      rfidAppendLogLine(remaining, color);
      break;
    }
    int end = (int)remaining.length();
    while (end > 1 && uiTextWidth(remaining.substring(0, end)) > kRfidLogTextW) {
      end--;
    }
    if (end < (int)remaining.length()) {
      const int sp = remaining.substring(0, end).lastIndexOf(' ');
      if (sp > 0) {
        end = sp;
      }
    }
    String line = remaining.substring(0, end);
    line.trim();
    remaining = remaining.substring(end);
    remaining.trim();
    if (line.length() == 0 && remaining.length() > 0) {
      line = rfidFitLogText(remaining);
      remaining = "";
    }
    rfidAppendLogLine(line, color);
  }
  if (extraSpace && s_rfidLogIndex < kRfidMaxLogLines) {
    s_rfidLogBuffer[s_rfidLogIndex] = "";
    s_rfidLogColor[s_rfidLogIndex] = TFT_WHITE;
    s_rfidLogIndex++;
  }
  rfidRedrawActivityLog();
}

static void rfidDrawStatusLine(int line, const String& text, uint16_t color) {
  const int y = kRfidStatusTextY + line * kRfidLineHeight;
  const String fitted = rfidFitStatusText(text);
  tft.fillRect(8, y - UI_CJK_TOP_PAD, TFT_WIDTH - 16,
                kRfidLineHeight + UI_CJK_TOP_PAD, TFT_BLACK);
  uiSetTextSize(1);
  uiSetTextColor(color, TFT_BLACK);
  uiSetCursor(8, y);
  uiPrint(fitted);
}

static void rfidDrawStatusLineIfChanged(int line, const String& text, uint16_t color) {
  if (line < 0 || line >= kRfidStatusLineCount) {
    return;
  }
  if (s_rfidStatusLineText[line] == text && s_rfidStatusLineColor[line] == color) {
    return;
  }
  s_rfidStatusLineText[line] = text;
  s_rfidStatusLineColor[line] = color;
  rfidDrawStatusLine(line, text, color);
}

/** Repaint NFC Status lines after overlay/box chrome cleared the framebuffer. */
static void rfidRepaintStatusFromCache() {
  for (int i = 0; i < kRfidStatusLineCount; i++) {
    if (s_rfidStatusLineText[i].length() > 0) {
      rfidDrawStatusLine(i, s_rfidStatusLineText[i], s_rfidStatusLineColor[i]);
    }
  }
}

static void rfidDrawTextBoxes() {
  const int detailBottom = rfidDetailBottom();
  const int detailTop = rfidEffectiveDetailTop();
  const int logH = rfidLogBoxDrawHeight();
  tft.fillRect(0, kRfidStatusY - 2, TFT_WIDTH, detailBottom - kRfidStatusY + 2, TFT_BLACK);
  tft.drawFastHLine(0, 19, TFT_WIDTH, UI_LINE);
  tft.drawRoundRect(4, kRfidLogBoxTop, TFT_WIDTH - 8, logH, 3, UI_LINE);
  uiSetTextSize(1);
  uiSetTextColor(UI_DIM_TEXT, TFT_BLACK);
  uiDrawString("NFC Status", 8, kRfidStatusY + 3);
  uiDrawString("Activity", 8, kRfidLogBoxTop + 3);
  if (s_rfidDetailBoxVisible) {
    const int detailH = detailBottom - detailTop;
    if (detailH > 8) {
      tft.drawRoundRect(4, detailTop, TFT_WIDTH - 8, detailH, 3, UI_LINE);
    }
    uiDrawString("Tag Details", 8, detailTop + 3);
  } else {
    const int innerH = logH - kRfidBoxHeaderH - 2;
    if (innerH > 0) {
      tft.fillRect(6, kRfidLogBoxTop + kRfidBoxHeaderH, TFT_WIDTH - 12, innerH, TFT_BLACK);
    }
  }
  s_rfidBoxesDrawn = true;
}

static void rfidDrawStaticStatusLines() {
  if (s_rfidStatusStaticDrawn) {
    return;
  }
  rfidDrawStatusLineIfChanged(4, "Protocol: ISO14443A", UI_DIM_TEXT);
  if (s_pn532VerStr[0] != '\0') {
    rfidDrawStatusLineIfChanged(5, String("Reader: ") + s_pn532VerStr, UI_DIM_TEXT);
  } else {
    rfidDrawStatusLineIfChanged(5, "Reader: PN532", UI_DIM_TEXT);
  }
  s_rfidStatusStaticDrawn = true;
}

static void rfidResetUiState() {
  s_rfidUiDrawn = false;
  s_rfidBoxesDrawn = false;
  s_rfidStatusStaticDrawn = false;
  s_rfidDetailBoxVisible = false;
  s_featureInfoAvailable = false;
  s_featureInfoTitle[0] = '\0';
  s_featureInfoBody[0] = '\0';
  s_listenCenterLabel[0] = '\0';
  s_rfidLogIndex = 0;
  for (int i = 0; i < kRfidMaxLogLines; i++) {
    s_rfidLogBuffer[i] = "";
    s_rfidLogColor[i] = 0;
  }
  for (int i = 0; i < kRfidStatusLineCount; i++) {
    s_rfidStatusLineText[i] = "";
    s_rfidStatusLineColor[i] = 0;
  }
}

static char s_dynPrev1[72];
static char s_dynPrev2[72];
static char s_dynPrevAux[72];
static char s_dynPrevPill[16];
static char s_dynPillLabel[16] = "LIVE";
static uint16_t s_dynPillColor = UI_WARN;

static unsigned s_listenSec = 0xFFFFFFFFu;
static uint8_t s_listenSpin = 0;
static unsigned long s_listenSpinMs = 0;

#if RFID_PROBE_TRACE
/* 监听循环的每帧耗时探针（只在 RFID_PROBE_TRACE=1 时编译）。
 *   P = 上一次 readPassiveTargetID 花了多久
 *   F = 上一整帧花了多久
 * 用途：把"读卡慢"变成两个可读数字 ——
 *   P 接近几百 ms ⇒ 瓶颈是 PN532 的阻塞轮询（库的 waitready/无界重试）；
 *   P 很小而 F 很大 ⇒ 瓶颈在 UI / 输入读取侧，跟 PN532 无关。 */
#endif

/* ⚠ 这两个统计量必须**无条件**定义：RFID_PROBE_TRACE=0 只是不把它们显示出来，
 * 但会话看门狗（s_pollMs > RFID_BUS_STALL_MS）与状态带的 P=/F= 都要用。
 * （改宏时把它们一起关掉过一次，链接期才报错。） */
static uint32_t s_pollMs  = 0;
static uint32_t s_frameMs = 0;

static void rfidListenShellReset() {
  s_listenSec = 0xFFFFFFFFu;
  s_listenSpin = 0;
  s_listenSpinMs = 0;
}

static void rfidDynClearPrev() {
  s_dynPrev1[0] = '\0';
  s_dynPrev2[0] = '\0';
  s_dynPrevAux[0] = '\0';
  s_dynPrevPill[0] = '\0';
}

static void rfidSetDynamicPill(const char* label, uint16_t color) {
  if (!label) {
    return;
  }
  strncpy(s_dynPillLabel, label, sizeof(s_dynPillLabel) - 1);
  s_dynPillLabel[sizeof(s_dynPillLabel) - 1] = '\0';
  s_dynPillColor = color;
}

static void rfidSetFeatureInfo(const char* title, const char* body) {
  if (title && title[0]) {
    strncpy(s_featureInfoTitle, title, sizeof(s_featureInfoTitle) - 1);
    s_featureInfoTitle[sizeof(s_featureInfoTitle) - 1] = '\0';
  }
  if (!body) {
    body = "";
  }
  strncpy(s_featureInfoBody, body, sizeof(s_featureInfoBody) - 1);
  s_featureInfoBody[sizeof(s_featureInfoBody) - 1] = '\0';
  s_featureInfoAvailable = s_featureInfoBody[0] != '\0';
}

static int rfidInfoPanelPageCount(const char* body) {
  return rfidInfoPanelPageCountForLines(body, rfidDetailLinesPerPage());
}

static bool rfidDetailTagNeedsPill(const char* tagLabel) {
  if (!tagLabel || !tagLabel[0]) {
    return false;
  }
  return strcmp(tagLabel, "INFO") != 0 && strcmp(tagLabel, "PROGRESS") != 0 &&
         strcmp(tagLabel, "SCAN") != 0 && strcmp(tagLabel, "RESULT") != 0;
}

static void rfidDrawInfoPanel(const char* tagLabel, const char* body, int page, int pageCount,
                              int linesPerPage) {
  const int detailBottom = rfidDetailBottom();
  const int detailTop = rfidEffectiveDetailTop();
  const int bodyTop = rfidDetailBodyTop();
  tft.fillRect(5, detailTop + kRfidBoxHeaderH, TFT_WIDTH - 10, detailBottom - detailTop - kRfidBoxHeaderH - 1,
               TFT_BLACK);
  uiSetTextFont(1);
  const int tagTop = detailTop + kRfidBoxHeaderH + 2;
  int textTop = bodyTop;
  if (pageCount > 1) {
    tft.fillRoundRect(kRfidDetailPadX, tagTop, 62, RF_TAG_H, 7, DARK_GRAY);
    uiSetTextColor(UI_ICON, DARK_GRAY);
    uiSetCursor(kRfidDetailPadX + 6, tagTop + 3);
    tft.printf("%d/%d", page + 1, pageCount);
    textTop = tagTop + RF_TAG_H + RF_TAG_GAP;
  } else if (rfidDetailTagNeedsPill(tagLabel)) {
    int pillW = (int)uiTextWidth(tagLabel) + 14;
    if (pillW < 48) {
      pillW = 48;
    }
    tft.fillRoundRect(kRfidDetailPadX, tagTop, pillW, RF_TAG_H, 7, DARK_GRAY);
    uiSetTextColor(UI_ICON, DARK_GRAY);
    uiSetCursor(kRfidDetailPadX + 6, tagTop + 3);
    uiPrint(tagLabel);
    textTop = tagTop + RF_TAG_H + RF_TAG_GAP;
  }
  rfidPrintWrappedPageBg(kRfidDetailPadX, textTop, kRfidDetailInnerW, body, kRfidLineHeight, UI_TEXT,
                         TFT_BLACK, page * linesPerPage, linesPerPage);
}

static void rfidRefreshInfoPanel(const char* tagLabel, const char* body) {
  (void)tagLabel;
  (void)body;
}

static void rfidDrawHint(bool twoActions, int padX = RF_PAD_X) {
  if (featureHasTouchNavBar()) {
    return;
  }
  const int hintY = rfidHintY();
  tft.fillRect(0, hintY - 3, TFT_WIDTH, 17, FEATURE_BG);
  uiSetTextFont(1);
  uiSetTextColor(UI_DIM_TEXT, FEATURE_BG);
  uiSetCursor(padX, hintY);
  uiPrint(twoActions ? "LEFT: Back   SELECT: action" : "LEFT / SELECT: close");
}

static void rfidDynamicBand(int topY, const char* line1, const char* line2) {
  (void)topY;
  const char* l1 = line1 ? line1 : "";
  const char* l2 = line2 ? line2 : "";
  if (strcmp(s_dynPrev1, l1) == 0 && strcmp(s_dynPrev2, l2) == 0 &&
      strcmp(s_dynPrevAux, s_rfidBandAux) == 0 && strcmp(s_dynPrevPill, s_dynPillLabel) == 0) {
    return;
  }

  rfidDrawStaticStatusLines();
  const String modeLine = String("Mode: ") + s_dynPillLabel;
  rfidDrawStatusLineIfChanged(0, modeLine, s_dynPillColor);
  rfidDrawStatusLineIfChanged(1, l1, UI_TEXT);
  rfidDrawStatusLineIfChanged(2, l2, UI_DIM_TEXT);
  if (s_rfidBandAux[0]) {
    rfidDrawStatusLineIfChanged(3, s_rfidBandAux, UI_DIM_TEXT);
  }

  strncpy(s_dynPrev1, l1, sizeof(s_dynPrev1) - 1);
  s_dynPrev1[sizeof(s_dynPrev1) - 1] = '\0';
  strncpy(s_dynPrev2, l2, sizeof(s_dynPrev2) - 1);
  s_dynPrev2[sizeof(s_dynPrev2) - 1] = '\0';
  strncpy(s_dynPrevAux, s_rfidBandAux, sizeof(s_dynPrevAux) - 1);
  s_dynPrevAux[sizeof(s_dynPrevAux) - 1] = '\0';
  strncpy(s_dynPrevPill, s_dynPillLabel, sizeof(s_dynPrevPill) - 1);
  s_dynPrevPill[sizeof(s_dynPrevPill) - 1] = '\0';
}

static void rfidRedrawListenChrome() {
  rfidSetDetailBoxVisible(false);
  rfidDrawTextBoxes();
  rfidRepaintStatusFromCache();
  rfidRedrawActivityLog();
  if (featureHasTouchNavBar() && s_listenCenterLabel[0]) {
    rfidSetListenNavLabels(s_listenCenterLabel, s_featureInfoAvailable);
  }
}

/** Wait for Exit on 2-box chrome; feature info stays on Info nav. */
static void rfidWaitTwoBoxDismiss() {
  rfidSetDetailBoxVisible(false);
  rfidDrawShellFooter1("Exit");
  rfidDrawTextBoxes();
  rfidRepaintStatusFromCache();
  rfidRedrawActivityLog();
  if (featureHasTouchNavBar()) {
    rfidSetListenNavLabels("Exit", s_featureInfoAvailable);
  }

  for (;;) {
    if (rfidPumpSessionUi()) {
      return;
    }
    rfidTryShowInfo();
    if (rfidPollFooter(s_rfFoot, 1, false) != RfidUiEvt::None) {
      return;
    }
    delay(8);
  }
}

/** Timeout dismiss: Exit leaves, Ready retries. @return true when Ready pressed. */
static bool rfidWaitTimeoutDismiss() {
  rfidSetDetailBoxVisible(false);
  FeatureUI::layoutFooter2(s_rfFoot, "Exit", FeatureUI::ButtonStyle::Secondary, "Ready",
                           FeatureUI::ButtonStyle::Primary, false, false);
  rfidDrawTextBoxes();
  rfidRepaintStatusFromCache();
  rfidRedrawActivityLog();
  if (featureHasTouchNavBar()) {
    rfidSetTimeoutNavLabels(s_featureInfoAvailable);
  } else {
    FeatureUI::drawFooterBg();
    FeatureUI::drawButton(s_rfFoot[0]);
    FeatureUI::drawButton(s_rfFoot[1]);
  }

  for (;;) {
    if (rfidPumpSessionUi()) {
      return false;
    }
    rfidTryShowInfo();
    RfidUiEvt e = rfidPollFooter(s_rfFoot, 2, true);
    if (e == RfidUiEvt::Primary) {
      return true;
    }
    if (e == RfidUiEvt::Back) {
      return false;
    }
    delay(8);
  }
}

/** Timeout — stay on 2-box chrome; feature info on Info nav. @return true to retry. */
static bool rfidDismissTimeoutPanel(const char* statusLine, const char* message) {
  rfidSetDynamicPill("WARN", UI_WARN);
  rfidSetBandAux(nullptr);
  rfidDynClearPrev();
  rfidDynamicBand(0, statusLine, "No tag detected");

  String logLine = message;
  const int nl = logLine.indexOf('\n');
  if (nl >= 0) {
    logLine = logLine.substring(0, nl);
  }
  rfidPrint(String("[!] ") + logLine, UI_WARN, false);
  return rfidWaitTimeoutDismiss();
}

static void rfidShowInfoPanel() {
  const bool showTitle = s_featureInfoTitle[0] != '\0';
  const int linesPerPage = rfidInfoOverlayLinesPerPage(showTitle);
  const int pageCount = rfidInfoPanelPageCountForLines(s_featureInfoBody, linesPerPage);
  int page = 0;
  bool redraw = false;
  rfidSetInfoOverlayNavLabels(pageCount);
  rfidDrawInfoOverlay(s_featureInfoTitle, s_featureInfoBody, page, pageCount, linesPerPage);

  for (;;) {
    if (rfidPumpSessionUi()) {
      rfidRedrawListenChrome();
      return;
    }
    if (redraw) {
      rfidDrawInfoOverlay(s_featureInfoTitle, s_featureInfoBody, page, pageCount, linesPerPage);
      redraw = false;
    }

    if (isButtonPressed(BTN_LEFT)) {
      rfidReleaseNavButtons();
      break;
    }
    if (pageCount == 1 && isButtonPressed(BTN_SELECT)) {
      rfidReleaseNavButtons();
      break;
    }
    if (pageCount > 1 && isButtonPressed(BTN_UP) && page < pageCount - 1) {
      rfidReleaseNavButtons();
      page++;
      redraw = true;
      continue;
    }
    if (pageCount > 1 && isButtonPressed(BTN_DOWN) && page > 0) {
      rfidReleaseNavButtons();
      page--;
      redraw = true;
      continue;
    }

    if (featureHasTouchNavBar()) {
      if (isTouchNavButtonPressedEdge(BTN_LEFT) || isTouchNavButtonPressedEdge(BTN_SELECT)) {
        rfidReleaseNavButtons();
        break;
      }
      if (pageCount > 1 && isTouchNavButtonPressedEdge(BTN_UP) && page < pageCount - 1) {
        rfidReleaseNavButtons();
        page++;
        redraw = true;
        continue;
      }
      if (pageCount > 1 && isTouchNavButtonPressedEdge(BTN_DOWN) && page > 0) {
        rfidReleaseNavButtons();
        page--;
        redraw = true;
        continue;
      }
    }

    delay(8);
  }

  rfidRedrawListenChrome();
}

static void rfidTryShowInfo() {
  if (!s_featureInfoAvailable || !s_featureInfoBody[0]) {
    return;
  }

  bool open = false;
  if (featureHasTouchNavBar()) {
    if (isTouchNavButtonPressedEdge(BTN_RIGHT)) {
      rfidReleaseNavButtons();
      open = true;
    }
  } else if (isButtonPressed(BTN_RIGHT)) {
    rfidReleaseNavButtons();
    open = true;
  }
  if (!open) {
    return;
  }

  rfidShowInfoPanel();
}

static void rfidListenTick(const char* statusLine, unsigned long t0) {
  const char* spin = "|/-\\";
  unsigned sec = (unsigned)((millis() - t0) / 1000UL);
  char detail[64];
  if (sec != s_listenSec) {
    s_listenSec = sec;
#if RFID_PROBE_TRACE
    snprintf(detail, sizeof(detail), "%u s  P=%lums F=%lums", sec,
             (unsigned long)s_pollMs, (unsigned long)s_frameMs);
#else
    snprintf(detail, sizeof(detail), "%u s", sec);
#endif
  } else {
    if (millis() - s_listenSpinMs > 320) {
      s_listenSpin = (uint8_t)((s_listenSpin + 1) % 4);
      s_listenSpinMs = millis();
    }
#if RFID_PROBE_TRACE
    snprintf(detail, sizeof(detail), "%u s %c  P=%lums F=%lums", sec, spin[s_listenSpin],
             (unsigned long)s_pollMs, (unsigned long)s_frameMs);
#else
    snprintf(detail, sizeof(detail), "%u s  %c", sec, spin[s_listenSpin]);
#endif
  }
  rfidDynamicBand(0, statusLine, detail);
}

/** Full chrome once — listening: NFC Status + Activity (info via nav button). */
static void rfidShellListen(const char* title, const char* footerLabel, const char* infoBody) {
  rfidListenShellReset();
  rfidDynClearPrev();
  rfidSetDynamicPill("SCAN", UI_WARN);
  rfidFeatureSetup(title);
  rfidSetDetailBoxVisible(false);
  const char* sub = (s_pn532VerStr[0] != '\0') ? s_pn532VerStr : nullptr;
  rfidSetBandAux(sub);
  const char* info = (infoBody && infoBody[0]) ? infoBody : "Waiting for tag...";
  rfidSetFeatureInfo(title, info);
  rfidDrawShellFooter1(footerLabel);
  rfidDrawHint(false);
  rfidDrawStaticStatusLines();
  rfidDynamicBand(0, "Ready", "");
  rfidPrint("[*] Waiting for tag...", UI_DIM_TEXT, false);
}

/** Full chrome once — progress: NFC Status + Activity (info via nav button). */
static void rfidShellProgress(const char* mainTitle, const char* stepLine, const char* footerLabel,
                              const char* infoBody = nullptr) {
  rfidListenShellReset();
  rfidDynClearPrev();
  rfidSetDynamicPill("WORK", UI_OK);
  rfidFeatureSetup(mainTitle);
  rfidSetDetailBoxVisible(false);
  rfidDrawTextBoxes();
  rfidSetBandAux(stepLine);
  const char* info = (infoBody && infoBody[0]) ? infoBody : stepLine;
  rfidSetFeatureInfo(mainTitle, info);
  rfidDrawShellFooter1(footerLabel);
  rfidDrawHint(false);
  rfidDrawStaticStatusLines();
  rfidDynamicBand(0, stepLine, "");
  rfidPrint(String("[!] ") + stepLine, UI_TEXT, false);
  rfidRedrawNavChrome();
}

/** Switch listen → progress without wiping chrome or activity log. */
static void rfidTransitionToProgress(const char* title, const char* stepLine, const char* footerLabel,
                                     const char* infoBody) {
  (void)infoBody;
  if (title && title[0]) {
    rfidUpdateToolbarTitle(title);
  }
  rfidListenShellReset();
  rfidDynClearPrev();
  rfidSetDynamicPill("WORK", UI_OK);
  rfidSetDetailBoxVisible(false);
  rfidSetBandAux(stepLine);
  rfidDrawShellFooter1(footerLabel);
  rfidDrawTextBoxes();
  rfidDrawStaticStatusLines();
  rfidDynamicBand(0, stepLine, "");
  rfidPrint(String("[!] ") + stepLine, UI_TEXT, false);
}

static void rfidLayoutFull(const char* title, const char* subtitleOrNull, const char* body,
                           bool twoButtons, const char* primaryLabel,
                           FeatureUI::ButtonStyle primaryStyle, bool primaryDisabled) {
  applyThemeToPalette(settings().theme);
  rfidResetUiState();
  rfidClearBody(TFT_BLACK);
  drawStatusBar(readBatteryVoltage(), true);
  s_rfidBandAux[0] = '\0';
  rfidRunToolbar(title);

  const int boxY = kRfidStatusY;
  const int boxBottom = rfidDetailBottom();
  const int boxH = boxBottom - boxY;
  if (boxH > 12) {
    tft.drawRoundRect(4, boxY, TFT_WIDTH - 8, boxH, 3, UI_LINE);
    uiSetTextSize(1);
    uiSetTextColor(UI_DIM_TEXT, TFT_BLACK);
    uiDrawString("Details", 8, boxY + 3);
    const int innerX = kRfidDetailPadX;
    const int innerW = kRfidDetailInnerW;
    const bool hasSub = subtitleOrNull && subtitleOrNull[0];
    int tagTop = boxY + kRfidBoxHeaderH + 4;
    int bodyTop = tagTop + RF_TAG_H + RF_TAG_GAP;
    if (hasSub) {
      uiSetTextFont(1);
      uiSetTextColor(UI_DIM_TEXT, TFT_BLACK);
      String sub = subtitleOrNull;
      const bool wasLong = uiTextWidth(sub) > innerW;
      while (sub.length() > 0 && uiTextWidth(wasLong ? sub + "..." : sub) > innerW) {
        sub.remove(sub.length() - 1);
      }
      if (wasLong) {
        sub += "...";
      }
      uiSetCursor(innerX, boxY + kRfidBoxHeaderH + 2);
      uiPrint(sub);
      tagTop = boxY + kRfidBoxHeaderH + 16;
      bodyTop = tagTop + RF_TAG_H + RF_TAG_GAP;
    }
    const char* tag = (primaryStyle == FeatureUI::ButtonStyle::Danger) ? "CAUTION" : "DETAILS";
    uiSetTextFont(1);
    tft.fillRoundRect(innerX, tagTop, 52, RF_TAG_H, 7, DARK_GRAY);
    uiSetTextColor(UI_ICON, DARK_GRAY);
    uiSetCursor(innerX + 6, tagTop + 3);
    uiPrint(tag);
    rfidPrintWrappedStepBg(innerX, bodyTop, innerW, body, RF_BODY_LINE_PX, UI_TEXT, TFT_BLACK,
                           boxBottom - 4);
  }

  rfidRedrawNavChrome();

  if (twoButtons && primaryLabel) {
    rfidDrawShellFooter2(primaryLabel, primaryStyle, primaryDisabled);
    rfidDrawHint(true);
  } else {
    rfidDrawShellFooter1("Back");
    rfidDrawHint(false);
  }
}

static bool rfidSubtitleIsFailure(const char* sub) {
  if (!sub || !sub[0]) {
    return false;
  }
  return strstr(sub, "fail") || strstr(sub, "Fail") || strstr(sub, "Timeout") ||
         strstr(sub, "Skipped") || strstr(sub, "No tag") || strstr(sub, "Unsupported") ||
         strstr(sub, "Aborted") || strstr(sub, "Stopped") || strstr(sub, "No write") ||
         strstr(sub, "lost") || strstr(sub, "Lost") || strstr(sub, "Incomplete");
}

/** Result dismiss: Exit only on success; Exit + Ready when failed. */
static void rfidWaitResultDismiss(bool offerRetry) {
  if (offerRetry) {
    if (rfidWaitTimeoutDismiss()) {
      s_rfidRetrySession = true;
    }
    return;
  }
  rfidWaitTwoBoxDismiss();
}

static void rfidPresentResult(const char* title, const char* sub, const char* body,
                              const char* detailTag = "RESULT", const char* pillLabel = nullptr) {
  (void)detailTag;
  if (!s_rfidBoxesDrawn) {
    rfidFeatureSetup(title ? title : "RFID");
  } else if (title && title[0]) {
    rfidUpdateToolbarTitle(title);
  }

  rfidListenShellReset();
  rfidDynClearPrev();

  const bool failed = rfidSubtitleIsFailure(sub);
  if (pillLabel && pillLabel[0]) {
    rfidSetDynamicPill(pillLabel, failed ? UI_WARN : UI_OK);
  } else {
    rfidSetDynamicPill(failed ? "WARN" : "DONE", failed ? UI_WARN : UI_OK);
  }
  rfidSetBandAux(nullptr);
  rfidDrawStaticStatusLines();
  rfidDynamicBand(0, title ? title : "Done", sub && sub[0] ? sub : "Complete");
  if (sub && sub[0]) {
    rfidPrint(String("[") + (failed ? "!" : "+") + "] " + sub, failed ? UI_WARN : UI_OK, false);
  }
  if (body && body[0]) {
    String remaining = body;
    int lines = 0;
    while (remaining.length() > 0 && lines < 8) {
      const int nl = remaining.indexOf('\n');
      String line = (nl >= 0) ? remaining.substring(0, nl) : remaining;
      remaining = (nl >= 0) ? remaining.substring(nl + 1) : "";
      line.trim();
      if (line.length() > 0) {
        rfidPrint(line, failed ? UI_WARN : UI_TEXT, false);
        lines++;
      }
    }
  }
  rfidWaitResultDismiss(failed);
}

static void rfidResultAndDismiss(const char* title, const char* sub, const char* body) {
  rfidPresentResult(title, sub, body);
}

/** @return true = primary (right) action, false = back */
static bool rfidRunTwoButtonDialog(const char* title, const char* sub, const char* body,
                                   const char* primaryLabel, FeatureUI::ButtonStyle primaryStyle) {
  rfidLayoutFull(title, sub, body, true, primaryLabel, primaryStyle, false);
  for (;;) {
    if (rfidPumpSessionUi()) {
      return false;
    }
    RfidUiEvt e = rfidPollFooter(s_rfFoot, 2, true);
    if (e == RfidUiEvt::Back) {
      return false;
    }
    if (e == RfidUiEvt::Primary) {
      return true;
    }
    delay(8);
  }
}

/** 2-box confirm: Back cancels, primary continues. */
static bool rfidRunTwoBoxConfirm(const char* title, const char* statusLine, const char* statusDetail,
                                 const char* activityMsg, const char* infoBody,
                                 const char* primaryLabel) {
  rfidListenShellReset();
  rfidDynClearPrev();
  rfidSetDynamicPill("READY", UI_OK);
  rfidFeatureSetup(title);
  rfidSetDetailBoxVisible(false);
  rfidSetFeatureInfo(title, infoBody ? infoBody : statusLine);
  rfidSetBandAux(nullptr);
  rfidDynamicBand(0, statusLine, statusDetail ? statusDetail : "");
  if (activityMsg && activityMsg[0]) {
    rfidPrint(activityMsg, UI_TEXT, false);
  }
  rfidDrawShellFooter2(primaryLabel, FeatureUI::ButtonStyle::Primary, false);
  rfidDrawHint(true);

  for (;;) {
    if (rfidPumpSessionUi()) {
      return false;
    }
    rfidTryShowInfo();
    RfidUiEvt e = rfidPollFooter(s_rfFoot, 2, true);
    if (e == RfidUiEvt::Back) {
      return false;
    }
    if (e == RfidUiEvt::Primary) {
      return true;
    }
    delay(8);
  }
}

static bool rfidPollListenUi() {
  if (rfidPumpSessionUi()) {
    return true;
  }
  rfidTryShowInfo();
  return rfidPollFooter(s_rfFoot, 1, false) != RfidUiEvt::None;
}

static bool rfidPollCancel() {
  return rfidPollListenUi();
}

static bool tryMagicBackdoor() {
  uint8_t commands[][2] = {{0x40, 0x00}, {0x43, 0x00}};
  uint8_t response[32];
  uint8_t responseLength = 32;
  for (int i = 0; i < 2; i++) {
    if (s_nfc.inDataExchange(commands[i], sizeof(commands[i]), response, &responseLength)) {
      return true;
    }
  }
  return false;
}

/** True if the same ISO14443A tag is still on the coil (short poll). */
static bool rfidTagStillPresent(const uint8_t* uid, uint8_t uidLen) {
  uint8_t uidNow[7] = {0};
  uint8_t lenNow = 0;
  if (!s_nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uidNow, &lenNow,
                                 PN532_READ_POLL_TIMEOUT_MS)) {
    return false;
  }
  if (lenNow != uidLen) {
    return false;
  }
  return memcmp(uid, uidNow, uidLen) == 0;
}

static bool rfidDumpTagLost(const uint8_t* uid, uint8_t uidLen, unsigned blockOrPage) {
  if (rfidTagStillPresent(uid, uidLen)) {
    return false;
  }
  rfidRestoreBus();
  char body[160];
  snprintf(body, sizeof(body),
           "Tag left the field during dump\n(at block/page %u).\n\n"
           "Keep the tag on the coil until reading finishes.",
           blockOrPage);
  rfidResultAndDismiss("Dump", "Tag lost", body);
  return true;
}

static CardType detectCardType(uint8_t* uid, uint8_t uidLength) {
  if (uidLength != 4 && uidLength != 7) {
    return UNKNOWN;
  }
  uint8_t keyA[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  if (s_nfc.mifareclassic_AuthenticateBlock(uid, uidLength, 0, 0, keyA)) {
    return MIFARE_CLASSIC;
  }
  uint8_t data[16];
  if (s_nfc.mifareultralight_ReadPage(4, data)) {
    uint8_t cmd[] = {0x60};
    uint8_t response[32];
    uint8_t responseLength = 32;
    if (s_nfc.inDataExchange(cmd, sizeof(cmd), response, &responseLength) &&
        responseLength >= 8) {
      if (response[0] == 0x00 && response[1] == 0x04) {
        return NTAG;
      }
    }
    return MIFARE_ULTRALIGHT;
  }
  uint8_t cmd[] = {0x5A, 0x00, 0x00, 0x00};
  uint8_t response[32];
  uint8_t responseLength = 32;
  if (s_nfc.inDataExchange(cmd, sizeof(cmd), response, &responseLength) && responseLength > 0) {
    return MIFARE_DESFIRE;
  }
  return UNKNOWN;
}

static const char* cardTypeStr(CardType t) {
  switch (t) {
    case MIFARE_CLASSIC:
      return "MIFARE Classic";
    case MIFARE_ULTRALIGHT:
      return "Ultralight";
    case NTAG:
      return "NTAG";
    case MIFARE_DESFIRE:
      return "DESFire";
    default:
      return "Unknown";
  }
}

static void uidToHex(char* out, size_t outLen, uint8_t* uid, uint8_t uidLen) {
  size_t pos = 0;
  out[0] = '\0';
  for (uint8_t i = 0; i < uidLen && i < 7u && pos + 3 < outLen; i++) {
    pos += (size_t)snprintf(out + pos, outLen - pos, "%02X", uid[i]);
  }
}

static void formatClassicBlock0(char* out, size_t outLen, const uint8_t* d) {
  snprintf(out, outLen,
           "Sector 0 block 0 (read with key A = FF FF FF FF FF FF):\n"
           "%02X %02X %02X %02X %02X %02X %02X %02X\n"
           "%02X %02X %02X %02X %02X %02X %02X %02X",
           d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9], d[10], d[11], d[12], d[13],
           d[14], d[15]);
}

namespace RfidNfc {

/** 把 PN532 硬复位一次（拉低 RSTPDN 再释放，等芯片启动）。
 *  进功能前**必须先调**：RSTPDN 悬空的模块会停在复位态、连 0x24 都不应答，
 *  不先复位就直接探测必然报 "not found"。
 *  SPI 模式或未接 RSTPDN（PN532_I2C_RST < 0）时是空操作。 */
void hardwareReset() {
  rfidResetPulse();
}

bool begin() {
  if (s_hwOk) {
    return true;
  }
#if RFID_PROBE_TRACE
  s_probeT0 = millis();
#endif
  rfidProbeStep("1/5 attach bus");
  rfidAttachBus();
  rfidI2cWiden();   // I2C：探测期超时（SPI 模式下是空实现）

  // 这一步最可疑：库内部会 reset()→delay(10)→wakeup()→SAMConfig()，
  // 而 SAMConfig 里的 waitready 是按 10ms 步进空转到超时的。
  rfidProbeStep("2/5 nfc.begin()");
  s_nfc.begin();

  rfidProbeStep("3/5 getFirmwareVersion");
  uint32_t ver = s_nfc.getFirmwareVersion();
  if (!ver) {
    rfidI2cRestore();
#if DIAG_PRINTS_ENABLED
  #if RFID_USE_I2C
    Serial.println("[RFID] PN532 no response on I2C (addr 0x24, SDA=GPIO8/SCL=GPIO9).");
  #if (PN532_I2C_RST >= 0)
    Serial.printf("       Already pulsed RSTPDN (GPIO%d). Check: 1) SEL=I2C  2) SDA/SCL not swapped"
                  "  3) RSTPDN really wired to GPIO%d  4) VCC/GND\n", PN532_I2C_RST, PN532_I2C_RST);
  #else
    Serial.println("       Check: 1) SEL=I2C  2) SDA/SCL not swapped  3) VCC/GND"
                   "  4) RSTPDN floating (set PN532_I2C_RST to a spare pin)");
  #endif
  #else
    Serial.printf("[RFID] PN532 no response on SPI. SCK=%d MISO=%d MOSI=%d SS=%d\n",
                  PN532_SCK, PN532_MISO, PN532_MOSI, PN532_SS);
  #endif
#endif
    rfidPn532VerStr(0);
    rfidProbeStep("fail: no firmware version");
    rfidRestoreBus();
    return false;
  }
  rfidPn532VerStr(ver);
#if DIAG_PRINTS_ENABLED
  Serial.printf("[RFID] PN532 online: %s\n", s_pn532VerStr);
#endif
  rfidProbeStep("4/5 SAMConfig");
  s_nfc.SAMConfig();
  rfidProbeStep("5/5 setPassiveActivationRetries");
  s_nfc.setPassiveActivationRetries(PN532_PASSIVE_RETRIES);
  rfidI2cRestore();
  s_hwOk = true;
  rfidProbeStep("done - PN532 online");
  rfidRestoreBus();
  return true;
}

bool hardwareOk() { return s_hwOk; }

/** Clear retry flag before starting a session (menu loop). */
void clearSessionRetry() { s_rfidRetrySession = false; }

/** @return true once if the user chose Ready on a failure result screen. */
bool consumeSessionRetry() {
  if (!s_rfidRetrySession) {
    return false;
  }
  s_rfidRetrySession = false;
  return true;
}

void resetCloneBuffer() {}

static bool rfidTargetSendResponse(const uint8_t* payload, uint8_t payloadLen) {
  uint8_t tx[64];
  if (payloadLen + 1u > sizeof(tx)) {
    payloadLen = (uint8_t)(sizeof(tx) - 1u);
  }
  tx[0] = 0x8E;
  memcpy(tx + 1, payload, payloadLen);
  return s_nfc.setDataTarget(tx, (uint8_t)(payloadLen + 1u)) != 0;
}

/** Wait until an external reader polls us in ISO14443A target mode. */
static bool rfidTargetWaitReader(const char* tickMsg, unsigned long timeoutMs, uint8_t* rx,
                                 uint8_t* rxLen, bool showWaitLog) {
  for (;;) {
    unsigned long t0 = millis();
    if (showWaitLog) {
      rfidListenShellReset();
      rfidPrint("[*] Waiting for external reader...", UI_DIM_TEXT, false);
    }

    for (;;) {
      if (rfidPumpSessionUi()) {
        return false;
      }
      if (rfidPollCancel()) {
        return false;
      }
      rfidListenTick(tickMsg, t0);

      if (s_nfc.AsTarget()) {
        const uint8_t maxLen = *rxLen;
        *rxLen = maxLen;
        if (s_nfc.getDataTarget(rx, rxLen)) {
          return true;
        }
      }

      if (timeoutMs > 0 && (millis() - t0) >= timeoutMs) {
        char tmsg[128];
        snprintf(tmsg, sizeof(tmsg),
                 "No external reader in %lu s.\nTap Ready to wait again.",
                 timeoutMs / 1000UL);
        if (!rfidDismissTimeoutPanel("Timeout", tmsg)) {
          return false;
        }
        rfidPrint("[*] Ready — waiting for reader...", UI_DIM_TEXT, false);
        break;
      }
      delay(25);
    }
  }
}

#if RFID_USE_I2C
/* PN532 在 I2C 下的"结果就绪"判据：读 1 个字节，0x01 表示有数据可取。
 * 为什么自己读：库的 isready() 是 private，而它的公共读卡 API 是**阻塞式**的
 * （readPassiveTargetID → sendCommandCheckAck → waitready 按 10ms 步进空转到超时）。
 * 这在实测里代价极大：按键和触摸都在主循环里轮询，每次读卡都可能吃掉
 * 几十~几百 ms，于是整机表现成"触屏和按键基本卡死"。
 * 单字节读只要几百微秒 —— 用它把"等结果"变成每帧一次的快查。
 * 地址/就绪值与库一致（PN532_I2C_ADDRESS=0x24 / PN532_I2C_READY=0x01）。 */
static bool rfidI2cReadyBit() {
  Wire.requestFrom((uint8_t)PN532_I2C_ADDRESS, (uint8_t)1);
  if (Wire.available() <= 0) {
    return false;
  }
  return (Wire.read() == PN532_I2C_READY);
}
#endif

/**
 * 非阻塞"看有没有卡"：拆成「发一次命令」+「每帧只查 RDY 位」。
 *   返回 true  = 这一拍读到卡（uid / uidLen 有效）
 *   返回 false = 暂时没有 —— **它几乎立刻返回**，不会把 UI 拖住
 *
 * 与库的 readPassiveTargetID() 的区别（也是必须自己写的原因）：
 *   库版本一次调用里包含 waitready 空转，是"阻塞到出结果"；
 *   这里每帧只做一次单字节读，出结果才去读那 21 字节。
 *
 * 另外它自带两处自愈：
 *   - 命令太久没回音（RFID_TAG_CMD_RETRY_MS）→ 丢弃并重发
 *   - s_pollMs 记录"命令已等了多久"，供上层看门狗判总线异常
 */
static bool rfidPollTag(uint8_t* uid, uint8_t* uidLen) {
#if RFID_USE_I2C
  static bool     pending      = false;
  static uint32_t pendingSince = 0;

  if (!pending) {
    if (!s_nfc.startPassiveTargetIDDetection(PN532_MIFARE_ISO14443A)) {
      return false;                        // 发命令失败：下一帧再试
    }
    pending      = true;
    pendingSince = millis();
    s_pollMs     = 0;
    return false;
  }

  const uint32_t waited = millis() - pendingSince;
  s_pollMs = waited;

  if (waited > RFID_TAG_CMD_RETRY_MS) {    // 太久没回音 → 丢弃，下一帧重发
    pending = false;
    return false;
  }

  if (!rfidI2cReadyBit()) {
    return false;                          // 结果还没好 —— 立刻让出 CPU
  }

  pending = false;
  return s_nfc.readDetectedPassiveTargetID(uid, uidLen) != 0;
#else
  // SPI 模式（非 V2）没有可用的 RDY 轮询入口，保持原阻塞调用，行为不变
  const uint32_t t0 = millis();
  const bool hit = s_nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, uidLen,
                                             PN532_READ_POLL_TIMEOUT_MS);
  s_pollMs = millis() - t0;
  return hit;
#endif
}

static bool rfidListenIso14443a(const char* title, const char* footer, uint8_t* uid,
                                uint8_t* uidLenOut, unsigned long timeoutMs, const char* tickMsg,
                                const char* infoBody = nullptr) {
  const char* info = (infoBody && infoBody[0]) ? infoBody : tickMsg;
  int stallRuns = 0;                      // 连续"这条命令在吃满 I2C 超时"的次数
  for (;;) {
    unsigned long t0 = millis();
    rfidShellListen(title, footer, info);
    for (;;) {
      const unsigned long tFrame = millis();   // 总是统计：看门狗要用，不再只是诊断
      if (rfidPumpSessionUi()) {
        return false;
      }
      rfidListenTick(tickMsg, t0);
      if (rfidPollCancel()) {
        return false;
      }
      const bool hit = rfidPollTag(uid, uidLenOut);   // 非阻塞：没结果就立刻回来
      s_frameMs = millis() - tFrame;

      if (hit) {
        return true;
      }

      // ── 总线看门狗 ──
      // 单次读卡耗时远大于正常轮询 = 这条命令在吃满 I2C 超时（时钟被从机拉住、
      // 芯片半死）。不拦的话 UI 一直排在 I2C 后面，观感就是"触屏和按键全卡死"，
      // 而且会越拖越死（用户实测：卡死后再按复位键重启都进不去，只能断电）。
      // 连续 RFID_BUS_STALL_LIMIT 次就解一次总线并中止会话，把控制权还给界面。
      if (s_pollMs > RFID_BUS_STALL_MS) {
        if (++stallRuns >= RFID_BUS_STALL_LIMIT) {
          i2cBusRecover();
          rfidPrint("[!] I2C bus stalled - session aborted", TFT_RED, false);
          delay(1200);
          return false;
        }
      } else {
        stallRuns = 0;
      }

      if (millis() - t0 > timeoutMs) {
        char tmsg[96];
        snprintf(tmsg, sizeof(tmsg),
                 "No ISO14443A tag in %lu s.\nTap Ready to listen again.",
                 (unsigned long)(timeoutMs / 1000UL));
        if (!rfidDismissTimeoutPanel("Timeout", tmsg)) {
          return false;
        }
        rfidPrint("[*] Ready — listening again...", UI_DIM_TEXT, false);
        break;
      }
      delay(12);
    }
  }
}

void sessionErase() {
  static const char kIntro[] =
      "Clears MIFARE Classic data block 4 (16 bytes to 00) using default key A.\n\n"
      "Ultralight / NTAG are not erased by this mode.\n\n"
      "Hold a Classic tag on the coil.";

  rfidAttachBus();
  uint8_t uid[7] = {0};
  uint8_t uidLength = 0;
  if (!rfidListenIso14443a("Erase", "Cancel", uid, &uidLength, (unsigned long)RFID_TAG_LISTEN_TIMEOUT_MS,
                           "Classic tag — block 4 erase", kIntro)) {
    rfidRestoreBus();
    return;
  }

  char uidStr[24] = "";
  uidToHex(uidStr, sizeof(uidStr), uid, uidLength);
  rfidSetDynamicPill("OK", UI_OK);
  rfidSetBandAux(nullptr);
  rfidDynamicBand(0, "Tag detected", uidStr);
  rfidPrint(String("[+] Tag detected ") + uidStr, UI_OK, false);

  CardType ct = detectCardType(uid, uidLength);
  if (ct != MIFARE_CLASSIC) {
    rfidRestoreBus();
    rfidPresentResult("Erase", "Skipped", "This erase only targets MIFARE Classic.");
    return;
  }

  uint8_t keyA[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  uint8_t emptyBlock[16] = {0};
  if (!s_nfc.mifareclassic_AuthenticateBlock(uid, uidLength, 4, 0, keyA)) {
    rfidRestoreBus();
    rfidResultAndDismiss("Erase", "Auth failed", "Could not authenticate block 4 with key A.");
    return;
  }
  if (!s_nfc.mifareclassic_WriteDataBlock(4, emptyBlock)) {
    rfidRestoreBus();
    rfidResultAndDismiss("Erase", "Write failed", "Block 4 write was rejected.");
    return;
  }

  rfidRestoreBus();
  char body[180];
  snprintf(body, sizeof(body), "UID %s\nBlock 4 cleared (16 bytes zero).", uidStr);
  rfidResultAndDismiss("Erase", "Done", body);
}

void sessionDump() {
  static const char kIntro[] =
      "Reads Classic blocks 0–63 or UL/NTAG user pages.\n\n"
      "Summary here; full hex prints on Serial Monitor.\n\n"
      "Present tag on the coil.";

  rfidAttachBus();
  uint8_t uid[7] = {0};
  uint8_t uidLength = 0;
  if (!rfidListenIso14443a("Dump", "Cancel", uid, &uidLength, (unsigned long)RFID_TAG_LISTEN_TIMEOUT_MS,
                           "Present tag to dump", kIntro)) {
    rfidRestoreBus();
    return;
  }

  char uidStr[24] = "";
  uidToHex(uidStr, sizeof(uidStr), uid, uidLength);
  CardType ct = detectCardType(uid, uidLength);
  rfidSetDynamicPill("OK", UI_OK);
  rfidSetBandAux(nullptr);
  rfidDynamicBand(0, "Tag detected", uidStr);
  rfidPrint(String("[+] Tag detected ") + uidStr, UI_OK, false);

  uint8_t keyA[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  uint8_t data[16];
  int blocksWithData = 0;
  int blocksRead = 0;
  int segmentsExpected = 0;

  if (ct == MIFARE_CLASSIC) {
    segmentsExpected = 64;
    rfidTransitionToProgress("Dump", "Reading Classic blocks", "Cancel",
                             "Reading MIFARE Classic blocks 0–63.\nFull hex on Serial Monitor.");
    Serial.println(F("\n--- RFID Dump Classic ---"));
    for (uint8_t block = 0; block < 64; block++) {
      char l2[48];
      snprintf(l2, sizeof(l2), "Block %u / 63", (unsigned)block);
      rfidDynamicBand(RF_DYN_TOP_PROG, "Dumping MIFARE Classic", l2);
      char infoLine[72];
      snprintf(infoLine, sizeof(infoLine), "Reading block %u of 63.\nFull hex on Serial Monitor.",
               (unsigned)block);
      rfidRefreshInfoPanel("PROGRESS", infoLine);
      if (rfidPollCancel()) {
        rfidRestoreBus();
        if (!rfidSessionExitRequested()) {
          rfidResultAndDismiss("Dump", "Stopped", "Dump aborted.");
        }
        return;
      }
      const bool ok = s_nfc.mifareclassic_AuthenticateBlock(uid, uidLength, block, 0, keyA) &&
                      s_nfc.mifareclassic_ReadDataBlock(block, data);
      if (ok) {
        blocksRead++;
        bool nz = false;
        for (int i = 0; i < 16; i++) {
          if (data[i] != 0) {
            nz = true;
          }
        }
        if (nz) {
          blocksWithData++;
        }
        Serial.printf("B%u:", (unsigned)block);
        for (int i = 0; i < 16; i++) {
          Serial.printf(" %02X", data[i]);
        }
        Serial.println();
      } else if (rfidDumpTagLost(uid, uidLength, (unsigned)block)) {
        return;
      }
      delay(25);
    }
  } else if (ct == MIFARE_ULTRALIGHT || ct == NTAG) {
    uint8_t maxPages = (ct == MIFARE_ULTRALIGHT) ? 16u : 36u;
    segmentsExpected = (int)maxPages - 4;
    rfidTransitionToProgress("Dump", "Reading user pages", "Cancel",
                             "Reading Ultralight / NTAG user pages.\nFull hex on Serial Monitor.");
    Serial.println(F("\n--- RFID Dump Ultralight / NTAG ---"));
    for (uint8_t page = 4; page < maxPages; page++) {
      char l2[48];
      snprintf(l2, sizeof(l2), "Page %u / %u", (unsigned)page, (unsigned)(maxPages - 1));
      rfidDynamicBand(RF_DYN_TOP_PROG, "Dumping pages", l2);
      char infoLine[72];
      snprintf(infoLine, sizeof(infoLine), "Reading page %u of %u.\nFull hex on Serial Monitor.",
               (unsigned)page, (unsigned)(maxPages - 1));
      rfidRefreshInfoPanel("PROGRESS", infoLine);
      if (rfidPollCancel()) {
        rfidRestoreBus();
        if (!rfidSessionExitRequested()) {
          rfidResultAndDismiss("Dump", "Stopped", "Dump aborted.");
        }
        return;
      }
      uint8_t pg[4];
      if (s_nfc.mifareultralight_ReadPage(page, pg)) {
        blocksRead++;
        bool nz = false;
        for (int i = 0; i < 4; i++) {
          if (pg[i] != 0) {
            nz = true;
          }
        }
        if (nz) {
          blocksWithData++;
        }
        Serial.printf("P%u:", (unsigned)page);
        for (int i = 0; i < 4; i++) {
          Serial.printf(" %02X", pg[i]);
        }
        Serial.println();
      } else if (rfidDumpTagLost(uid, uidLength, (unsigned)page)) {
        return;
      }
      delay(18);
    }
  } else {
    rfidRestoreBus();
    rfidResultAndDismiss("Dump", "Unsupported",
                         "Dump targets Classic or Ultralight/NTAG.\nTag did not match.");
    return;
  }

  const bool tagGoneAtEnd = segmentsExpected > 0 && blocksRead > 0 && blocksRead < segmentsExpected &&
                            !rfidTagStillPresent(uid, uidLength);

  rfidRestoreBus();

  if (blocksRead == 0) {
    rfidResultAndDismiss("Dump", "Read failed", "No blocks/pages could be read.\nCheck tag position and keys.");
    return;
  }

  if (tagGoneAtEnd) {
    char body[160];
    snprintf(body, sizeof(body),
             "UID: %s\nType: %s\n\nOnly %d of %d blocks read before tag left the field.",
             uidStr, cardTypeStr(ct), blocksRead, segmentsExpected);
    rfidResultAndDismiss("Dump", "Tag lost", body);
    return;
  }

  char body[420];
  const bool partial = segmentsExpected > 0 && blocksRead < segmentsExpected;
  snprintf(body, sizeof(body),
           "UID: %s\nType: %s\n\n"
           "Segments read OK: %d%s\nBlocks/pages with any non-zero byte: %d\n\n"
           "%sOpen Serial Monitor for full hex.",
           uidStr, cardTypeStr(ct), blocksRead,
           partial ? " (partial — some blocks need other keys)" : "",
           blocksWithData, partial ? "Some blocks were skipped (auth/key).\n" : "");
  rfidResultAndDismiss("Dump", "Done", body);
}

void sessionDecodeAccess() {
  static const char kIntro[] =
      "Reads MIFARE Classic sector 1 trailer (block 7) and shows raw access bytes.\n"
      "Uses default keys A/B.\n\n"
      "Present a Classic tag on the coil.";

  rfidAttachBus();
  uint8_t uid[7] = {0};
  uint8_t uidLength = 0;
  if (!rfidListenIso14443a("Decode Access", "Cancel", uid, &uidLength,
                           (unsigned long)RFID_TAG_LISTEN_TIMEOUT_MS, "Classic tag", kIntro)) {
    rfidRestoreBus();
    return;
  }

  char uidStr[24] = "";
  uidToHex(uidStr, sizeof(uidStr), uid, uidLength);
  rfidSetDynamicPill("OK", UI_OK);
  rfidSetBandAux(nullptr);
  rfidDynamicBand(0, "Tag detected", uidStr);
  rfidPrint(String("[+] Tag detected ") + uidStr, UI_OK, false);

  CardType ct = detectCardType(uid, uidLength);
  if (ct != MIFARE_CLASSIC) {
    rfidRestoreBus();
    rfidPresentResult("Decode Access", "Skipped", "Only MIFARE Classic sector trailers.");
    return;
  }

  uint8_t keyA[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  uint8_t keyB[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  uint8_t blk[16] = {0};
  bool authenticated = false;
  (void)tryMagicBackdoor();
  if (s_nfc.mifareclassic_AuthenticateBlock(uid, uidLength, 7, 0, keyA)) {
    authenticated = true;
  } else if (s_nfc.mifareclassic_AuthenticateBlock(uid, uidLength, 7, 1, keyB)) {
    authenticated = true;
  }

  if (!authenticated || !s_nfc.mifareclassic_ReadDataBlock(7, blk)) {
    rfidRestoreBus();
    rfidResultAndDismiss("Decode Access", "Failed", "Could not read block 7 (sector 1 trailer).");
    return;
  }

  uint8_t a6 = blk[6], a7 = blk[7], a8 = blk[8];
  uint8_t c1 = (uint8_t)((a7 >> 5) & 0x01);
  uint8_t c2 = (uint8_t)((a8 >> 1) & 0x01);
  uint8_t c3 = (uint8_t)((a8 >> 5) & 0x01);
  uint8_t accessCond = (uint8_t)((c1 << 2) | (c2 << 1) | c3);

  rfidRestoreBus();

  char body[420];
  snprintf(body, sizeof(body),
           "UID %s\nSector 1 trailer (block 7):\n"
           "%02X %02X %02X %02X %02X %02X %02X %02X\n"
           "%02X %02X %02X %02X %02X %02X %02X %02X\n\n"
           "Access bytes [6..8]: %02X %02X %02X\n"
           "Condition bits C1..C3 (nibble-style): %u %u %u → code %u\n\n"
           "(Interpret with MFC access-bit tables for your sector.)",
           uidStr, blk[0], blk[1], blk[2], blk[3], blk[4], blk[5], blk[6], blk[7], blk[8], blk[9],
           blk[10], blk[11], blk[12], blk[13], blk[14], blk[15], a6, a7, a8, (unsigned)c1,
           (unsigned)c2, (unsigned)c3, (unsigned)accessCond);
  rfidResultAndDismiss("Decode Access", "Block 7", body);
}

void sessionJamReader() {
  static const char kIntro[] =
      "Puts the PN532 into ISO14443A target mode and sends rapid replies toward "
      "an external reader.\n\nKeep antennas close to the reader coil.\n\n"
      "Stop ends the session.";

  rfidAttachBus();
  rfidShellProgress("Jam Reader", "Preparing target mode...", "Stop", kIntro);

  uint8_t rx[64];
  uint8_t rxLen = sizeof(rx);
  if (!rfidTargetWaitReader("Waiting for reader", RFID_TARGET_WAIT_TIMEOUT_MS, rx, &rxLen, true)) {
    rfidRestoreBus();
    return;
  }

  rfidSetDynamicPill("JAM", UI_WARN);
  rfidSetBandAux(nullptr);
  rfidDynamicBand(0, "Jam active", "Flooding external reader");
  rfidPrint("[+] Reader detected", UI_OK, false);

  static const uint8_t kJamResp[] = {0xFF, 0xFF, 0xFF, 0xFF};
  unsigned long lastUiMs = 0;
  bool pendingRx = true;

  for (;;) {
    if (rfidPumpSessionUi()) {
      break;
    }
    if (rfidPollCancel()) {
      break;
    }

    if (pendingRx) {
      if (!rfidTargetSendResponse(kJamResp, sizeof(kJamResp))) {
        rfidDynamicBand(0, "Jam active", "Waiting for reader...");
        pendingRx = rfidTargetWaitReader("Waiting for reader", 0, rx, &rxLen, false);
        if (!pendingRx) {
          break;
        }
        rfidPrint("[+] Reader reconnected", UI_OK, false);
        continue;
      }
      pendingRx = false;
    }

    rxLen = sizeof(rx);
    if (s_nfc.getDataTarget(rx, &rxLen)) {
      pendingRx = true;
      const unsigned long now = millis();
      if (now - lastUiMs >= 200) {
        rfidDynamicBand(0, "Jam active", "Reader RF collision / flood");
        lastUiMs = now;
      }
    } else {
      const unsigned long now = millis();
      if (now - lastUiMs >= 200) {
        rfidDynamicBand(0, "Jam active", "Waiting for reader...");
        lastUiMs = now;
      }
      pendingRx = rfidTargetWaitReader("Waiting for reader", 0, rx, &rxLen, false);
      if (!pendingRx) {
        break;
      }
      rfidPrint("[+] Reader reconnected", UI_OK, false);
    }
    delay(5);
  }

  s_nfc.SAMConfig();
  s_nfc.setPassiveActivationRetries(PN532_PASSIVE_RETRIES);
  rfidRestoreBus();
  if (rfidSessionExitRequested()) {
    return;
  }
  rfidResultAndDismiss("Jam Reader", "Stopped", "SAM reconfigured; reader mode restored.");
}

void sessionDisruptEmulate() {
  static const char kIntro[] =
      "Target mode: cycles payloads (FF / 00 / random) on each exchange.\n"
      "Use only on gear you own or are permitted to test.\n\n"
      "Stop ends the session.";

  rfidAttachBus();
  rfidShellProgress("Disrupt Emulate", "Preparing target mode...", "Stop", kIntro);

  uint8_t rx[64];
  uint8_t rxLen = sizeof(rx);
  if (!rfidTargetWaitReader("Waiting for reader", RFID_TARGET_WAIT_TIMEOUT_MS, rx, &rxLen, true)) {
    rfidRestoreBus();
    return;
  }

  rfidSetDynamicPill("TX", UI_WARN);
  rfidSetBandAux(nullptr);
  rfidDynamicBand(0, "Disrupt active", "FF / 00 / random payloads");
  rfidPrint("[+] Reader detected", UI_OK, false);

  uint8_t txPayload[16];
  uint8_t cycle = 0;
  unsigned long lastUiMs = 0;
  bool pendingRx = true;

  for (;;) {
    if (rfidPumpSessionUi()) {
      break;
    }
    if (rfidPollCancel()) {
      break;
    }

    if (pendingRx) {
      if (cycle % 3 == 0) {
        memset(txPayload, 0xFF, sizeof(txPayload));
      } else if (cycle % 3 == 1) {
        memset(txPayload, 0x00, sizeof(txPayload));
      } else {
        for (unsigned i = 0; i < sizeof(txPayload); i++) {
          txPayload[i] = (uint8_t)(random(256));
        }
      }
      cycle++;

      if (!rfidTargetSendResponse(txPayload, sizeof(txPayload))) {
        rfidDynamicBand(0, "Disrupt active", "Waiting for reader...");
        pendingRx = rfidTargetWaitReader("Waiting for reader", 0, rx, &rxLen, false);
        if (!pendingRx) {
          break;
        }
        rfidPrint("[+] Reader reconnected", UI_OK, false);
        continue;
      }
      pendingRx = false;
    }

    rxLen = sizeof(rx);
    if (s_nfc.getDataTarget(rx, &rxLen)) {
      pendingRx = true;
      const unsigned long now = millis();
      if (now - lastUiMs >= 200) {
        rfidDynamicBand(0, "Disrupt active", "FF / 00 / random payloads");
        lastUiMs = now;
      }
    } else {
      const unsigned long now = millis();
      if (now - lastUiMs >= 200) {
        rfidDynamicBand(0, "Disrupt active", "Waiting for reader...");
        lastUiMs = now;
      }
      pendingRx = rfidTargetWaitReader("Waiting for reader", 0, rx, &rxLen, false);
      if (!pendingRx) {
        break;
      }
      rfidPrint("[+] Reader reconnected", UI_OK, false);
    }
    delay(5);
  }

  s_nfc.SAMConfig();
  s_nfc.setPassiveActivationRetries(PN532_PASSIVE_RETRIES);
  rfidRestoreBus();
  if (rfidSessionExitRequested()) {
    return;
  }
  rfidResultAndDismiss("Disrupt Emulate", "Stopped", "SAM reconfigured.");
}

void sessionCardReader() {
  static const char kIntro[] =
      "Passive ISO14443A read.\n\n"
      "Hold the tag flat on the coil. UID, tag type, and memory samples appear here after scan.";

  uint8_t uid[7] = {0};
  uint8_t uidLength = 0;
  rfidAttachBus();

  if (!rfidListenIso14443a("Card Reader", "Cancel", uid, &uidLength,
                           (unsigned long)RFID_TAG_LISTEN_TIMEOUT_MS,
                           "Listening — present ISO14443A tag", kIntro)) {
    rfidRestoreBus();
    return;
  }

  char uidStr[24] = "";
  uidToHex(uidStr, sizeof(uidStr), uid, uidLength);
  CardType ct = detectCardType(uid, uidLength);

  char classicBlk[200] = "";
  if (ct == MIFARE_CLASSIC) {
    uint8_t keyA[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    uint8_t data[16];
    if (s_nfc.mifareclassic_AuthenticateBlock(uid, uidLength, 0, 0, keyA) &&
        s_nfc.mifareclassic_ReadDataBlock(0, data)) {
      formatClassicBlock0(classicBlk, sizeof(classicBlk), data);
    } else {
      snprintf(classicBlk, sizeof(classicBlk),
               "Sector 0 block 0 was not readable using default key A (FF FF FF FF FF FF).");
    }
  }

  char ulSample[240] = "";
  if (ct == MIFARE_ULTRALIGHT || ct == NTAG) {
    uint8_t pg[4][4];
    bool ok = true;
    for (int pi = 0; pi < 4; pi++) {
      if (!s_nfc.mifareultralight_ReadPage((uint8_t)(4 + pi), pg[pi])) {
        ok = false;
      }
    }
    if (ok) {
      snprintf(ulSample, sizeof(ulSample),
               "User memory sample (pages 4–7, 4 bytes each):\n"
               "P4  %02X %02X %02X %02X\n"
               "P5  %02X %02X %02X %02X\n"
               "P6  %02X %02X %02X %02X\n"
               "P7  %02X %02X %02X %02X",
               pg[0][0], pg[0][1], pg[0][2], pg[0][3], pg[1][0], pg[1][1], pg[1][2], pg[1][3],
               pg[2][0], pg[2][1], pg[2][2], pg[2][3], pg[3][0], pg[3][1], pg[3][2], pg[3][3]);
    } else {
      snprintf(ulSample, sizeof(ulSample), "Pages 4–7 could not all be read.");
    }
  }

  const char* fw = s_pn532VerStr[0] ? s_pn532VerStr : "PN532 (version not cached)";
  char body[640];
  snprintf(body, sizeof(body),
           "Reader IC: %s\n\n"
           "UID (hex): %s\n"
           "UID length: %u bytes\n"
           "Detected type: %s\n"
           "Type code (internal): %d\n",
           fw, uidStr, (unsigned)uidLength, cardTypeStr(ct), (int)ct);

  if (ct == MIFARE_CLASSIC && classicBlk[0]) {
    size_t len = strlen(body);
    snprintf(body + len, sizeof(body) - len, "\n%s", classicBlk);
  }
  if ((ct == MIFARE_ULTRALIGHT || ct == NTAG) && ulSample[0]) {
    size_t len = strlen(body);
    snprintf(body + len, sizeof(body) - len, "\n\n%s", ulSample);
  }
  if (ct == MIFARE_DESFIRE) {
    size_t len = strlen(body);
    snprintf(body + len, sizeof(body) - len, "\n\n"
             "DESFire / ISO-DEP-style tag detected.\n"
             "This tool only dumps UID here; full DESFire ops need other APDUs.");
  }
  if (ct == UNKNOWN) {
    size_t len = strlen(body);
    snprintf(body + len, sizeof(body) - len, "\n\n"
             "Family could not be determined from quick probes.");
  }

  rfidRestoreBus();

  rfidSetDynamicPill("OK", UI_OK);
  rfidSetBandAux(nullptr);
  rfidDynamicBand(0, "Tag detected", uidStr);
  rfidPrint(String("[+] ") + cardTypeStr(ct) + "  " + uidStr, UI_OK, false);
  if (ct == MIFARE_CLASSIC && classicBlk[0]) {
    rfidPrint(classicBlk, UI_DIM_TEXT, false);
  }
  (void)body;
  rfidWaitTwoBoxDismiss();
}

static uint8_t s_srcData[16][3][16];
static uint8_t s_srcPages[ESP32DIV_RFID_SRC_PAGES][4];
// 2026-10-12：按扇区记录源卡可用密钥，支持"部分克隆"——门禁卡常有个别扇区使用
// 非默认密钥，旧逻辑任一扇区鉴权失败就一票否决整个克隆（"Some blocks/pages did
// not read"）。现在逐扇区独立处理：能读的扇区克隆，读不了的跳过并在结果里注明。
static uint8_t s_srcKeyA[16][6];
static uint8_t s_srcKeyB[16][6];
static bool    s_srcKeyAValid[16];
static bool    s_srcKeyBValid[16];
static bool    s_srcSectorOk[16];

// 常见出厂/公共密钥表（工厂默认 / 全零 / MAD / NDEF / 常见工程公共值）。
// 读源卡时逐扇区按 A 类、B 类依次尝试。
static const uint8_t kCommonKeys[][6] = {
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},  // factory default
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00},  // zero
    {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5},  // MAD key A
    {0xD3, 0xF7, 0xD3, 0xF7, 0xD3, 0xF7},  // NDEF
    {0x4D, 0x3A, 0x99, 0xC3, 0x51, 0xDD},
    {0x1A, 0x98, 0x2C, 0x7E, 0x45, 0x9A},
    {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF},
    {0x71, 0x4C, 0x5C, 0x88, 0x6E, 0x09},
    {0x58, 0x7E, 0xE5, 0xF9, 0x35, 0x0F},
    {0xB5, 0xFF, 0x67, 0xBA, 0xA2, 0x5A},
    {0x84, 0x08, 0xC4, 0x9C, 0x15, 0x4C},
    {0xB0, 0x13, 0x71, 0xD0, 0x93, 0x35},
};
static const uint8_t kCommonKeyCount =
    (uint8_t)(sizeof(kCommonKeys) / sizeof(kCommonKeys[0]));

void sessionClone() {
  uint8_t uid[7] = {0};
  uint8_t uidLength = 0;
  uint8_t keyA[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  uint8_t keyB[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  uint8_t keyZero[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  uint8_t data[16];
  uint8_t srcUidCopy[7];

#if RFID_UID_CLONE
  const char* introFoot =
      "Common key list (A/B).\nLocked sectors skipped; UID clone on Gen2 \"magic\" cards.";
#else
  const char* introFoot = "Common key list (A/B). Locked sectors skipped on write.";
#endif

  char introBody[220];
  snprintf(introBody, sizeof(introBody),
           "Two-tag flow: read source, confirm, then write a blank of the same type.\n\n%s\n\n"
           "Step 1/4: present the SOURCE tag on the coil.",
           introFoot);

  rfidAttachBus();
  if (!rfidListenIso14443a("Clone", "Cancel", uid, &uidLength,
                           (unsigned long)RFID_CLONE_PHASE_TIMEOUT_MS,
                           "Step 1/4 — Present SOURCE tag", introBody)) {
    rfidRestoreBus();
    return;
  }

  memcpy(srcUidCopy, uid, uidLength);
  char uidStr[24] = "";
  uidToHex(uidStr, sizeof(uidStr), uid, uidLength);
  rfidSetDynamicPill("OK", UI_OK);
  rfidSetBandAux(nullptr);
  rfidDynamicBand(0, "Source tag detected", uidStr);
  rfidPrint(String("[+] Source ") + uidStr, UI_OK, false);

  CardType srcType = detectCardType(uid, uidLength);
  if (srcType == UNKNOWN) {
    rfidRestoreBus();
    rfidResultAndDismiss("Clone", "Unsupported", "Could not classify tag.\nNeed Classic, Ultralight, or NTAG.");
    return;
  }

  memset(s_srcData, 0, sizeof(s_srcData));
  memset(s_srcPages, 0, sizeof(s_srcPages));
  memset(s_srcKeyA, 0, sizeof(s_srcKeyA));
  memset(s_srcKeyB, 0, sizeof(s_srcKeyB));
  memset(s_srcKeyAValid, 0, sizeof(s_srcKeyAValid));
  memset(s_srcKeyBValid, 0, sizeof(s_srcKeyBValid));
  memset(s_srcSectorOk, 0, sizeof(s_srcSectorOk));
  bool readOk = true;
  uint8_t sectorsOkCount = 0;  // Classic：成功读全的扇区数（部分克隆容忍个别扇区密钥未知）

  if (srcType == MIFARE_CLASSIC) {
    rfidTransitionToProgress("Clone", "Step 2/4 — Read source", "Cancel",
                             "Reading source Classic data blocks 0–47.");
    for (uint8_t sector = 0; sector < 16; sector++) {
      char l1[48];
      char l2[48];
      snprintf(l1, sizeof(l1), "Classic — reading data blocks");
      snprintf(l2, sizeof(l2), "Sector %u / 15", (unsigned)sector);
      rfidDynamicBand(RF_DYN_TOP_PROG, l1, l2);
      char infoLine[64];
      snprintf(infoLine, sizeof(infoLine), "Reading sector %u of 15.", (unsigned)sector);
      rfidRefreshInfoPanel("PROGRESS", infoLine);
      if (rfidPollCancel()) {
        rfidRestoreBus();
        return;
      }

      // 该扇区首个数据块上做密钥发现：常见密钥表逐个试 A 类、再 B 类
      const uint8_t firstBlock = (uint8_t)(sector * 4);
      bool solved = false;
      uint8_t useKey[6];
      uint8_t useType = 0;  // 0=key A, 1=key B
      for (uint8_t k = 0; k < kCommonKeyCount && !solved; k++) {
        if (s_nfc.mifareclassic_AuthenticateBlock(
                uid, uidLength, firstBlock, 0,
                const_cast<uint8_t*>(kCommonKeys[k]))) {
          memcpy(useKey, kCommonKeys[k], 6);
          useType = 0;
          solved = true;
        } else if (s_nfc.mifareclassic_AuthenticateBlock(
                       uid, uidLength, firstBlock, 1,
                       const_cast<uint8_t*>(kCommonKeys[k]))) {
          memcpy(useKey, kCommonKeys[k], 6);
          useType = 1;
          solved = true;
        }
        delay(3);  // 失败的鉴权间隔一小拍，减轻 PN532/卡的压力
      }

      bool sectorOk = false;
      if (solved) {
        if (useType == 0) {
          memcpy(s_srcKeyA[sector], useKey, 6);
          s_srcKeyAValid[sector] = true;
        } else {
          memcpy(s_srcKeyB[sector], useKey, 6);
          s_srcKeyBValid[sector] = true;
        }
        sectorOk = true;
        for (uint8_t block = 0; block < 3 && sectorOk; block++) {
          uint8_t blockNum = (uint8_t)(sector * 4 + block);
          // 认证按扇区生效，这里用已知密钥重认证代价极小
          bool auth = s_nfc.mifareclassic_AuthenticateBlock(uid, uidLength,
                                                            blockNum, useType, useKey);
          if (auth && s_nfc.mifareclassic_ReadDataBlock(blockNum, data)) {
            memcpy(s_srcData[sector][block], data, 16);
          } else {
            // 读不全的扇区不克隆（写半截数据可能破坏卡上应用数据）
            sectorOk = false;
          }
          delay(18);
        }
      }
      s_srcSectorOk[sector] = sectorOk;
      if (sectorOk) {
        sectorsOkCount++;
      }
    }
  } else if (srcType == MIFARE_ULTRALIGHT || srcType == NTAG) {
    uint8_t maxPages = (srcType == MIFARE_ULTRALIGHT) ? 16u : 36u;
    rfidTransitionToProgress("Clone", "Step 2/4 — Read source", "Cancel",
                             "Reading source Ultralight / NTAG user pages.");
    for (uint8_t page = 4; page < maxPages; page++) {
      char l2[40];
      snprintf(l2, sizeof(l2), "Page %u / %u", (unsigned)page, (unsigned)(maxPages - 1));
      rfidDynamicBand(RF_DYN_TOP_PROG, "Ultralight / NTAG — user pages", l2);
      char infoLine[64];
      snprintf(infoLine, sizeof(infoLine), "Reading page %u of %u.", (unsigned)page,
               (unsigned)(maxPages - 1));
      rfidRefreshInfoPanel("PROGRESS", infoLine);
      if (rfidPollCancel()) {
        rfidRestoreBus();
        return;
      }
      if (s_nfc.mifareultralight_ReadPage(page, data)) {
        memcpy(s_srcPages[page], data, 4);
      } else {
        readOk = false;
      }
      delay(12);
    }
  }

  rfidRestoreBus();

  if (srcType == MIFARE_CLASSIC && sectorsOkCount == 0) {
    // 一个扇区都进不去：卡用的是自定义密钥，密钥表打不开
    rfidResultAndDismiss("Clone", "Read failed",
                         "No sector key matched (common keys tried).\n"
                         "Card uses custom keys — data unreadable.");
    return;
  }
  if (srcType != MIFARE_CLASSIC && !readOk) {
    rfidResultAndDismiss("Clone", "Read failed", "Some pages did not read.\nCheck keys or tag damage.");
    return;
  }
  if (srcType == MIFARE_CLASSIC && sectorsOkCount < 16) {
    char partialMsg[80];
    snprintf(partialMsg, sizeof(partialMsg),
             "[!] Partial read: %u/15 sectors. Rest use custom keys.", (unsigned)sectorsOkCount);
    rfidPrint(partialMsg, UI_WARN, false);
  }

  static const char kConfirmInfo[] =
      "Source data is stored in device memory.\n\n"
      "Remove the source tag from the coil, then tap Clone to present a blank tag "
      "of the same type.\n\n"
      "Back cancels without writing anything.";

  char confirmDetail[56];
  snprintf(confirmDetail, sizeof(confirmDetail), "%s  %s", uidStr, cardTypeStr(srcType));
  char confirmLog[128];
  snprintf(confirmLog, sizeof(confirmLog),
           "[+] Source copied (%s, %s).\n[*] Remove source — tap Clone.",
           uidStr, cardTypeStr(srcType));

  if (!rfidRunTwoBoxConfirm("Clone", "Step 3/4 — Confirm clone", confirmDetail, confirmLog,
                            kConfirmInfo, "Clone")) {
    return;
  }

  static const char kBlankInfo[] =
      "Step 4/4: present a blank tag of the same type as the source.\n\n"
      "Writing starts automatically after the blank tag is detected.";

  rfidAttachBus();
  if (!rfidListenIso14443a("Clone", "Cancel", uid, &uidLength,
                           (unsigned long)RFID_CLONE_PHASE_TIMEOUT_MS,
                           "Step 4/4 — Present BLANK tag", kBlankInfo)) {
    rfidRestoreBus();
    return;
  }

  char blankUidStr[24] = "";
  uidToHex(blankUidStr, sizeof(blankUidStr), uid, uidLength);
  rfidSetDynamicPill("OK", UI_OK);
  rfidSetBandAux(nullptr);
  rfidDynamicBand(0, "Blank tag detected", blankUidStr);
  rfidPrint(String("[+] Blank ") + blankUidStr, UI_OK, false);

  CardType blankType = detectCardType(uid, uidLength);
  if (blankType != srcType) {
    rfidRestoreBus();
    rfidResultAndDismiss("Clone", "Type mismatch", "Blank must match source family.");
    return;
  }

  bool cloneOk = true;
  int blocksDone = 0;
  int sectorsSkipped = 0;  // 源卡密钥未知而跳过的扇区数
  int sectorsWritten = 0;  // 实际全部写成功的扇区数（区别于"尝试写入"）
  bool uidWritten = false; // RFID_UID_CLONE 下源 UID 是否已写入 magic 卡
#if RFID_UID_CLONE
  bool gen2MagicDetected = false;
#endif

  if (blankType == MIFARE_CLASSIC) {
    bool magic = tryMagicBackdoor();
#if RFID_UID_CLONE
    gen2MagicDetected = magic;
#else
    (void)magic;
#endif
#if RFID_UID_CLONE
    if (magic && s_srcSectorOk[0]) {  // UID 克隆要用源块 0 的 SAK/ATQA 字节，扇区 0 未读上则跳过
      bool ok = s_nfc.mifareclassic_AuthenticateBlock(uid, uidLength, 0, 0, keyA);
      if (!ok) {
        ok = s_nfc.mifareclassic_AuthenticateBlock(uid, uidLength, 0, 1, keyB);
      }
      if (!ok) {
        ok = s_nfc.mifareclassic_AuthenticateBlock(uid, uidLength, 0, 0, keyZero);
      }
      if (ok) {
        uint8_t block0Data[16];
        memcpy(block0Data, srcUidCopy, uidLength);
        block0Data[4] =
            (uint8_t)(block0Data[0] ^ block0Data[1] ^ block0Data[2] ^ block0Data[3]);
        memcpy(&block0Data[5], &s_srcData[0][0][5], 11);
        if (s_nfc.mifareclassic_WriteDataBlock(0, block0Data)) {
          blocksDone++;
          uidWritten = true;
        } else {
          cloneOk = false;
        }
      } else {
        cloneOk = false;
      }
    }
#else
    (void)magic;
    (void)uidWritten;
#endif
    rfidTransitionToProgress("Clone", "Writing — Classic", "Cancel",
                             "Writing copied Classic blocks to blank tag.");
    for (uint8_t sector = 0; sector < 16; sector++) {
      char l2[40];
      snprintf(l2, sizeof(l2), "Sector %u / 15", (unsigned)sector);
      rfidDynamicBand(RF_DYN_TOP_PROG, "Writing copied blocks", l2);
      char infoLine[64];
      snprintf(infoLine, sizeof(infoLine), "Writing sector %u of 15.", (unsigned)sector);
      rfidRefreshInfoPanel("PROGRESS", infoLine);
      if (rfidPollCancel()) {
        rfidRestoreBus();
        if (!rfidSessionExitRequested()) {
          rfidResultAndDismiss("Clone", "Stopped", "Clone aborted mid-write.");
        }
        return;
      }
      // 源卡该扇区密钥未知（读阶段未能读全）→ 跳过，保留目标卡出厂内容
      if (!s_srcSectorOk[sector]) {
        sectorsSkipped++;
        rfidPrint(String("[!] Sector ") + (int)sector + " skipped (key unknown).", UI_DIM_TEXT, false);
        continue;
      }
      bool sectorAllOk = true;  // 该扇区所有写入是否成功（用于真实"写入扇区"统计）
      for (uint8_t block = 0; block < 3; block++) {
        uint8_t blockNum = (uint8_t)(sector * 4 + block);
#if RFID_UID_CLONE
        if (magic && uidWritten && sector == 0 && block == 0) {
          continue;
        }
#endif
        // 目标空白卡通常仍是出厂密钥；源密钥作为后备（目标曾被克隆过的场景）
        bool auth =
            s_nfc.mifareclassic_AuthenticateBlock(uid, uidLength, blockNum, 0, keyA);
        if (!auth) {
          auth = s_nfc.mifareclassic_AuthenticateBlock(uid, uidLength, blockNum, 1, keyB);
        }
        if (!auth) {
          auth = s_nfc.mifareclassic_AuthenticateBlock(uid, uidLength, blockNum, 0, keyZero);
        }
        if (!auth && s_srcKeyAValid[sector]) {
          auth = s_nfc.mifareclassic_AuthenticateBlock(uid, uidLength, blockNum, 0,
                                                       s_srcKeyA[sector]);
        }
        if (!auth && s_srcKeyBValid[sector]) {
          auth = s_nfc.mifareclassic_AuthenticateBlock(uid, uidLength, blockNum, 1,
                                                       s_srcKeyB[sector]);
        }
        if (auth) {
          if (s_nfc.mifareclassic_WriteDataBlock(blockNum, s_srcData[sector][block])) {
            blocksDone++;
          } else {
            cloneOk = false;
            sectorAllOk = false;
          }
        } else {
          cloneOk = false;
          sectorAllOk = false;
        }
        delay(28);
      }
      if (sectorAllOk) {
        sectorsWritten++;
      }
    }
  } else if (blankType == MIFARE_ULTRALIGHT || blankType == NTAG) {
    uint8_t maxPages = (blankType == MIFARE_ULTRALIGHT) ? 16u : 36u;
    rfidTransitionToProgress("Clone", "Writing — Ultralight / NTAG", "Cancel",
                             "Writing copied pages to blank tag.");
    for (uint8_t page = 4; page < maxPages; page++) {
      char l2[40];
      snprintf(l2, sizeof(l2), "Page %u / %u", (unsigned)page, (unsigned)(maxPages - 1));
      rfidDynamicBand(RF_DYN_TOP_PROG, "Writing copied pages", l2);
      char infoLine[64];
      snprintf(infoLine, sizeof(infoLine), "Writing page %u of %u.", (unsigned)page,
               (unsigned)(maxPages - 1));
      rfidRefreshInfoPanel("PROGRESS", infoLine);
      if (rfidPollCancel()) {
        rfidRestoreBus();
        if (!rfidSessionExitRequested()) {
          rfidResultAndDismiss("Clone", "Stopped", "Clone aborted mid-write.");
        }
        return;
      }
      if (s_nfc.mifareultralight_WritePage(page, s_srcPages[page])) {
        blocksDone++;
      } else {
        cloneOk = false;
      }
      delay(18);
    }
  }

  rfidRestoreBus();

  char summary[512];
  const char* fw = s_pn532VerStr[0] ? s_pn532VerStr : "PN532";
#if RFID_UID_CLONE
  snprintf(summary, sizeof(summary),
           "%s\n\n"
           "Source UID: %s\n"
           "Blank UID:    %s\n"
           "Tag type:     %s\n"
           "Reader IC:    %s\n\n"
           "Approx. write ops OK: %d\n"
           "Gen2 magic UID path: %s",
           cloneOk ? "Clone finished." : "Clone finished with some errors.",
           uidStr, blankUidStr, cardTypeStr(srcType), fw, blocksDone,
           gen2MagicDetected ? "detected (UID block attempted)" : "not detected");
#else
  snprintf(summary, sizeof(summary),
           "%s\n\n"
           "Source UID: %s\n"
           "Blank UID:    %s\n"
           "Tag type:     %s\n"
           "Reader IC:    %s\n\n"
           "Approx. write ops OK: %d",
           cloneOk ? "Clone finished." : "Clone finished with some errors.",
           uidStr, blankUidStr, cardTypeStr(srcType), fw, blocksDone);
#endif

  if (srcType == MIFARE_CLASSIC) {
    size_t slen = strlen(summary);
    snprintf(summary + slen, sizeof(summary) - slen,
             "\nSectors written: %u/15 (skipped %u, key unknown)",
             (unsigned)sectorsWritten, (unsigned)sectorsSkipped);
#if RFID_UID_CLONE
    if (!uidWritten) {
      slen = strlen(summary);
      snprintf(summary + slen, sizeof(summary) - slen,
               "\n[!] UID NOT cloned: blank card is not magic.");
    }
#endif
  }

  rfidResultAndDismiss("Clone", "Done", summary);
}

void sessionTagDisrupt() {
  static const char kIntro[] =
      "Writes sector trailer blocks on MIFARE Classic (default key A).\n"
      "Bad access bits can brick a tag.\n\n"
      "Only use tags you own or are allowed to modify.\n\n"
      "Present a Classic tag (~12 s window).";

  uint8_t uid[7] = {0};
  uint8_t uidLength = 0;
  uint8_t maliciousData[16];
  uint8_t key[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  static const uint8_t sectorTrailers[] = {3,  7,  11, 15, 19, 23, 27, 31,
                                           35, 39, 43, 47, 51, 55, 59, 63};
  bool any = false;
  int trailersOk = 0;
  int authFail = 0;

  rfidAttachBus();
  if (!rfidListenIso14443a("Tag Disrupt", "Cancel", uid, &uidLength, 12000UL,
                           "Classic tag — hold on coil (~12 s window)", kIntro)) {
    rfidRestoreBus();
    return;
  }

  CardType ct = detectCardType(uid, uidLength);
  char uidStr[24] = "";
  uidToHex(uidStr, sizeof(uidStr), uid, uidLength);
  rfidSetDynamicPill("OK", UI_OK);
  rfidSetBandAux(nullptr);
  rfidDynamicBand(0, "Tag detected", uidStr);
  rfidPrint(String("[+] Tag detected ") + uidStr, UI_OK, false);

  if (ct != MIFARE_CLASSIC) {
    rfidRestoreBus();
    char msg[120];
    snprintf(msg, sizeof(msg), "Tag types as %s.\nDisrupt targets Classic trailers only.",
             cardTypeStr(ct));
    rfidResultAndDismiss("Tag Disrupt", "Skipped", msg);
    return;
  }

  rfidTransitionToProgress("Tag Disrupt", "Writing sector trailers", "Cancel",
                           "Rotating trailer write patterns.\n16 sector trailers targeted.");
  rfidSetDynamicPill("WRITE", UI_WARN);
  for (uint8_t i = 0; i < 16; i++) {
    uint8_t block = sectorTrailers[i];
    char l2[48];
    snprintf(l2, sizeof(l2), "Trailer block %u — sector %u", (unsigned)block, (unsigned)i);
    rfidDynamicBand(RF_DYN_TOP_PROG, "Rotating trailer pattern", l2);
    char infoLine[72];
    snprintf(infoLine, sizeof(infoLine), "Writing trailer block %u (sector %u).", (unsigned)block,
             (unsigned)i);
    rfidRefreshInfoPanel("PROGRESS", infoLine);
    if (rfidPollCancel()) {
      rfidRestoreBus();
      if (!rfidSessionExitRequested()) {
        rfidResultAndDismiss("Tag Disrupt", "Stopped", "Aborted mid-write.");
      }
      return;
    }

    if (i % 3 == 0) {
      memset(maliciousData, 0xFF, 16);
    } else if (i % 3 == 1) {
      memset(maliciousData, 0x00, 16);
    } else {
      for (uint8_t j = 0; j < 16; j++) {
        maliciousData[j] = (uint8_t)random(256);
      }
    }
    maliciousData[6] = 0x00;
    maliciousData[7] = 0x00;
    maliciousData[8] = 0x00;

    if (!s_nfc.mifareclassic_AuthenticateBlock(uid, uidLength, block, 0, key)) {
      authFail++;
    } else if (s_nfc.mifareclassic_WriteDataBlock(block, maliciousData)) {
      any = true;
      trailersOk++;
    }
    delay(35);
  }

  rfidRestoreBus();

  char msg[320];
  const char* fw = s_pn532VerStr[0] ? s_pn532VerStr : "PN532";
  snprintf(msg, sizeof(msg),
           "Tag UID: %s\n"
           "Reader IC: %s\n\n"
           "Sector trailers touched: %d / 16\n"
           "Key A auth failures:    %d\n\n"
           "%s\n\n"
           "Patterns alternate FF / 00 / random on trailers.\n"
           "Access bits may brick the tag.",
           uidStr, fw, trailersOk, authFail,
           any ? "At least one trailer write succeeded." : "No trailer writes succeeded.");

  rfidResultAndDismiss("Tag Disrupt", any ? "Finished" : "No writes", msg);
}

} // namespace RfidNfc
