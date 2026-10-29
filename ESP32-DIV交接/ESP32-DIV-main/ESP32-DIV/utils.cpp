#include <SD.h>
#include <SPI.h>
#include <algorithm>
#include <cmath>
#include <vector>
#include <stdarg.h>
#include "driver/gpio.h"
#include "Touchscreen.h"
#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "icon.h"
#include "gps.h"
#include "shared.h"
#include "utils.h"
#include "ip5306.h"
#include "i18n.h"


bool notificationVisible = false;
/** Charging state read from the IP5306 (V2); false on boards without it. */
bool batteryIsCharging = false;
/** 外部电源(USB)已接入 —— 由 IO39 硬件插入检测得到，不依赖 I2C/IP5306。 */
bool usbPowerPresent = false;
static bool notificationHasSave = false;
// 提示类通知（无 Save 按钮）的自动隐藏超时。
// 原实现里 notificationVisible 只在 hideNotification() 里被清掉，而
// hideNotification() 只由触摸处理调用 ⇒ 一条"已在后台运行"之类的提示会
// **一直挡在界面上，必须手动点一下才消失**。这里给它加超时（由主循环
// 调用下面的 notificationTick()）。带 Save 按钮的确认框不自动消失。
static uint32_t s_notifShownMs = 0;
static constexpr uint32_t kNotifAutoHideMs = 6000;
static int notifX, notifY, notifWidth, notifHeight;
static int closeButtonX, closeButtonY, closeButtonSize = 16;
static int okButtonX, okButtonY, okButtonWidth = 84, okButtonHeight = 24;
static int saveButtonX, saveButtonY, saveButtonWidth = 84, saveButtonHeight = 24;

static char s_notifTitleCache[64];
static char s_notifMsgCache[400];
static bool s_notifShowSaveCache = false;

/** Wrapped line count for modal text (font 1, size 1 — must match draw path). */
static int countWrappedNotifLines(const String& msgIn, int innerPixelW) {
  uiSetTextFont(1);
  uiSetTextSize(1);
  String msg = msgIn;
  msg.trim();
  int lines = 0;
  while (msg.length() > 0 && lines < 48) {
    int lineEnd = msg.length();
    while (lineEnd > 0 && uiTextWidth(msg.substring(0, lineEnd)) > innerPixelW) {
      lineEnd--;
    }
    if (lineEnd <= 0) {
      ++lines;
      break;
    }
    if (lineEnd < msg.length()) {
      const int lastSpace = msg.substring(0, lineEnd).lastIndexOf(' ');
      if (lastSpace > 0) {
        lineEnd = lastSpace;
      }
    }
    ++lines;
    msg = msg.substring(lineEnd);
    msg.trim();
  }
  return lines;
}

static size_t decodeObfTo(char* out, size_t outSize, const uint8_t* in, size_t inLen, uint8_t key) {
  if (!out || outSize == 0) return 0;
  size_t n = inLen;
  if (n > outSize - 1) n = outSize - 1;
  for (size_t i = 0; i < n; ++i) out[i] = (char)(in[i] ^ key);
  out[n] = '\0';
  return n;
}

void tftPrintObf(const uint8_t* data, size_t len, uint8_t key) {
  char buf[96];
  decodeObfTo(buf, sizeof(buf), data, len, key);
  uiPrint(buf);
}

void tftPrintlnObf(const uint8_t* data, size_t len, uint8_t key) {
  char buf[96];
  decodeObfTo(buf, sizeof(buf), data, len, key);
  uiPrintln(buf);
}

void serialPrintObf(const uint8_t* data, size_t len, bool newline, uint8_t key) {
  char buf[128];
  decodeObfTo(buf, sizeof(buf), data, len, key);
  if (newline) Serial.println(buf);
  else Serial.print(buf);
}

static inline bool inRect(int x, int y, int rx, int ry, int rw, int rh) {
  return x >= rx && x < (rx + rw) && y >= ry && y < (ry + rh);
}

static void drawNotificationInternal(const char* title, const char* message, bool showSave) {
    snprintf(s_notifTitleCache, sizeof(s_notifTitleCache), "%s", title ? title : "");
    snprintf(s_notifMsgCache, sizeof(s_notifMsgCache), "%s", message ? message : "");
    s_notifShowSaveCache = showSave;
    notificationHasSave = showSave;

    const int scrW = tft.width();
    const int scrH = tft.height();
    notifWidth = (scrW > 232) ? 224 : (scrW - 16);
    if (notifWidth < 160) {
        notifWidth = 160;
    }

    const int messageBoxWidth = notifWidth - 10;
    const int innerTextW = messageBoxWidth - 6;
    const int lineH = 12;
    const int footH = showSave ? 40 : 30;
    const int topBand = 25;

    uiSetTextFont(1);
    uiSetTextSize(1);
    String msgProbe = message ? String(message) : String("");
    msgProbe.trim();
    int lineCount = countWrappedNotifLines(msgProbe, innerTextW);
    if (lineCount < 2) {
        lineCount = 2;
    }
    const int maxLinesByScreen = (scrH - topBand - footH - 14) / lineH;
    int useLines = lineCount;
    if (useLines > maxLinesByScreen) {
        useLines = maxLinesByScreen > 2 ? maxLinesByScreen : 2;
    }

    const int messageBoxHeight = 8 + useLines * lineH;
    notifHeight = topBand + messageBoxHeight + footH;
    if (notifHeight > scrH - 8) {
        notifHeight = scrH - 8;
        const int availBody = notifHeight - topBand - footH;
        useLines = (availBody - 8) / lineH;
        if (useLines < 2) {
            useLines = 2;
        }
    }
    notifX = (scrW - notifWidth) / 2;
    notifY = (scrH - notifHeight) / 2;
    if (notifY < 4) {
        notifY = 4;
    }

    tft.fillRect(notifX, notifY, notifWidth, notifHeight, LIGHT_GRAY);
    tft.drawRect(notifX, notifY, notifWidth, notifHeight, DARK_GRAY);
    tft.fillRect(notifX, notifY, notifWidth, 20, BLUE);

    uiSetTextColor(WHITE, BLUE);
    uiSetTextSize(1);
    uiSetCursor(notifX + 6, notifY + 5);
    uiPrint(title);

    closeButtonX = notifX + notifWidth - closeButtonSize - 6;
    closeButtonY = notifY + 3;
    tft.fillRect(closeButtonX, closeButtonY, closeButtonSize, closeButtonSize, RED);
    uiSetTextColor(WHITE, RED);
    uiSetCursor(closeButtonX + 5, closeButtonY + 4);
    uiPrint("X");

    int messageBoxX = notifX + 5;
    int messageBoxY = notifY + 25;
    const int messageBoxDrawH = notifHeight - topBand - footH;
    tft.fillRect(messageBoxX, messageBoxY, messageBoxWidth, messageBoxDrawH, WHITE);
    uiSetTextColor(BLACK, WHITE);

    {
      const int maxY = messageBoxY + messageBoxDrawH - lineH;
      String msg = message ? String(message) : String("");
      msg.trim();
      int cursorX = messageBoxX + 3;
      int cursorY = messageBoxY + 4;
      while (msg.length() > 0 && cursorY <= maxY) {
        int lineEnd = msg.length();
        while (lineEnd > 0 && uiTextWidth(msg.substring(0, lineEnd)) > innerTextW) {
          lineEnd--;
        }
        if (lineEnd <= 0) {
          break;
        }
        if (lineEnd < msg.length()) {
          int lastSpace = msg.substring(0, lineEnd).lastIndexOf(' ');
          if (lastSpace > 0) {
            lineEnd = lastSpace;
          }
        }
        uiSetCursor(cursorX, cursorY);
        uiPrint(msg.substring(0, lineEnd));
        msg = msg.substring(lineEnd);
        msg.trim();
        cursorY += lineH;
      }

      if (msg.length() > 0) {
        uiSetCursor(cursorX, maxY);
        uiPrint("...");
      }
    }

    int btnY = notifY + notifHeight - 25;
    if (showSave) {
      okButtonWidth = 72;
      saveButtonWidth = 72;
      okButtonHeight = saveButtonHeight = 20;
      okButtonX = notifX + 10;
      saveButtonX = notifX + notifWidth - 10 - saveButtonWidth;
      okButtonY = saveButtonY = btnY;

      tft.fillRect(okButtonX, okButtonY, okButtonWidth, okButtonHeight, GRAY);
      tft.drawRect(okButtonX, okButtonY, okButtonWidth, okButtonHeight, DARK_GRAY);
      tft.drawLine(okButtonX, okButtonY, okButtonX + okButtonWidth, okButtonY, WHITE);
      tft.drawLine(okButtonX, okButtonY, okButtonX, okButtonY + okButtonHeight, WHITE);
      uiSetTextColor(BLACK, GRAY);
      uiSetCursor(okButtonX + 28, okButtonY + 5);
      uiPrint("OK");

      tft.fillRect(saveButtonX, saveButtonY, saveButtonWidth, saveButtonHeight, GREEN);
      tft.drawRect(saveButtonX, saveButtonY, saveButtonWidth, saveButtonHeight, DARK_GRAY);
      tft.drawLine(saveButtonX, saveButtonY, saveButtonX + saveButtonWidth, saveButtonY, WHITE);
      tft.drawLine(saveButtonX, saveButtonY, saveButtonX, saveButtonY + saveButtonHeight, WHITE);
      uiSetTextColor(BLACK, GREEN);
      uiSetCursor(saveButtonX + 18, saveButtonY + 5);
      uiPrint("SAVE");
    } else {
      okButtonWidth = 72;
      okButtonHeight = 20;
      okButtonX = notifX + (notifWidth - okButtonWidth) / 2;
      okButtonY = btnY;
      tft.fillRect(okButtonX, okButtonY, okButtonWidth, okButtonHeight, GRAY);
      tft.drawRect(okButtonX, okButtonY, okButtonWidth, okButtonHeight, DARK_GRAY);
      tft.drawLine(okButtonX, okButtonY, okButtonX + okButtonWidth, okButtonY, WHITE);
      tft.drawLine(okButtonX, okButtonY, okButtonX, okButtonY + okButtonHeight, WHITE);
      uiSetTextColor(BLACK, GRAY);
      uiSetCursor(okButtonX + 28, okButtonY + 5);
      uiPrint("OK");
    }

    notificationVisible = true;
    s_notifShownMs = millis();
}

void showNotificationActions(const char* title, const char* message, bool showSave) {
    drawNotificationInternal(title, message, showSave);
}

void showNotification(const char* title, const char* message) {
    drawNotificationInternal(title, message, false);
}

void hideNotification() {
    applyThemeToPalette(settings().theme);
    tft.fillRect(notifX, notifY, notifWidth, notifHeight, UI_BG);
    notificationVisible = false;
}

/** 主循环每圈调用一次：无 Save 按钮的提示类通知到点自动隐藏。 */
void notificationTick() {
    if (!notificationVisible || notificationHasSave) {
        return;
    }
    if ((uint32_t)(millis() - s_notifShownMs) >= kNotifAutoHideMs) {
        hideNotification();
    }
}

bool isNotificationVisible() {
    // 顺带处理提示类通知的超时（见上面的 notificationTick）。
    // 所有功能页的模态等待都写成 `if (isNotificationVisible()) { …等触摸… }`，
    // 挂在这里就能让**所有页面**都自动收起提示，不必逐页去加 tick
    // （功能页有自己的 while 循环，不走 loop()）。
    notificationTick();
    return notificationVisible;
}

void notificationRedrawIfVisible() {
    if (!notificationVisible) {
        return;
    }
    drawNotificationInternal(s_notifTitleCache, s_notifMsgCache, s_notifShowSaveCache);
}

NotificationAction notificationHandleTouch(int x, int y) {
  if (!notificationVisible) return NotificationAction::None;

  if (inRect(x, y, closeButtonX, closeButtonY, closeButtonSize, closeButtonSize)) {
    hideNotification();
    return NotificationAction::Close;
  }
  if (inRect(x, y, okButtonX, okButtonY, okButtonWidth, okButtonHeight)) {
    hideNotification();
    return NotificationAction::Ok;
  }
  if (notificationHasSave && inRect(x, y, saveButtonX, saveButtonY, saveButtonWidth, saveButtonHeight)) {
    hideNotification();
    return NotificationAction::Save;
  }
  return NotificationAction::None;
}

void printWrappedText(int x, int y, int maxWidth, const char* text) {
    String message = text;
    int cursorX = x, cursorY = y;

    while (message.length() > 0) {
        int lineEnd = message.length();

        while (uiTextWidth(message.substring(0, lineEnd)) > maxWidth) {
            lineEnd--;
        }

        if (lineEnd < message.length()) {
            int lastSpace = message.substring(0, lineEnd).lastIndexOf(' ');
            if (lastSpace > 0) lineEnd = lastSpace;
        }

        uiSetCursor(cursorX, cursorY);
        uiPrint(message.substring(0, lineEnd));

        message = message.substring(lineEnd);
        message.trim();

        cursorY += 15;
    }
}

namespace FeatureUI {

#ifndef FEATURE_TEXT
#define FEATURE_TEXT ORANGE
#endif

static inline uint16_t btnFill(ButtonStyle style, bool pressed, bool disabled) {
  if (disabled) return UI_LINE;
  if (pressed)  return UI_FG;
  switch (style) {
    case ButtonStyle::Primary:   return FEATURE_TEXT;
    case ButtonStyle::Secondary: return UI_FG;
    case ButtonStyle::Danger:    return UI_WARN;
  }
  return UI_FG;
}

static inline uint16_t btnBorder(ButtonStyle style, bool disabled) {
  if (disabled) return UI_LINE;
  switch (style) {
    case ButtonStyle::Primary:   return FEATURE_TEXT;
    case ButtonStyle::Secondary: return FEATURE_TEXT;
    case ButtonStyle::Danger:    return UI_WARN;
  }
  return FEATURE_TEXT;
}

static inline uint16_t btnText(ButtonStyle style, bool disabled) {
  if (disabled) return UI_TEXT;

  switch (style) {
    case ButtonStyle::Secondary: return UI_ICON;
    case ButtonStyle::Primary:
    case ButtonStyle::Danger:    return FEATURE_BG;
  }
  return UI_ICON;
}

void drawFooterBg() {

  tft.fillRect(0, tft.height() - FOOTER_H, tft.width(), FOOTER_H, FEATURE_BG);
}

void drawButtonRect(int x, int y, int w, int h,
                    const char* label,
                    ButtonStyle style,
                    bool pressed,
                    bool disabled,
                    uint8_t font) {
  if (!label) label = "";
  int r = h / 2;
  if (r > 6) r = 6;
  if (r < 0) r = 0;
  uint16_t fill = btnFill(style, pressed, disabled);
  uint16_t edge = btnBorder(style, disabled);
  uint16_t txt  = btnText(style, disabled);

  tft.fillRoundRect(x, y, w, h, r, fill);
  tft.drawRoundRect(x, y, w, h, r, edge);

  uiSetTextDatum(MC_DATUM);
  uiSetTextColor(txt, fill);

  uiDrawString(label, x + w/2, y + h/2, font);

  uiSetTextDatum(TL_DATUM);
  uiSetTextFont(1);
  uiSetTextSize(1);
  uiSetTextColor(UI_TEXT, FEATURE_BG);
}

void layoutFooter3(Button (&btns)[3],
                   const char* l0, ButtonStyle s0,
                   const char* l1, ButtonStyle s1,
                   const char* l2, ButtonStyle s2,
                   bool d0, bool d1, bool d2) {
  int availW = tft.width() - 2*PAD_X;
  int w = (availW - 2*GAP_X) / 3;
  int y = tft.height() - FOOTER_H + (FOOTER_H - BTN_H)/2;
  int x0 = PAD_X;
  int x1 = x0 + w + GAP_X;
  int x2 = x1 + w + GAP_X;
  btns[0] = {(int16_t)x0,(int16_t)y,(int16_t)w,(int16_t)BTN_H,l0,s0,d0};
  btns[1] = {(int16_t)x1,(int16_t)y,(int16_t)w,(int16_t)BTN_H,l1,s1,d1};
  btns[2] = {(int16_t)x2,(int16_t)y,(int16_t)w,(int16_t)BTN_H,l2,s2,d2};
}

void layoutFooter2(Button (&btns)[2],
                   const char* l0, ButtonStyle s0,
                   const char* l1, ButtonStyle s1,
                   bool d0, bool d1) {
  int availW = tft.width() - 2*PAD_X;
  int w = (availW - GAP_X) / 2;
  int y = tft.height() - FOOTER_H + (FOOTER_H - BTN_H)/2;
  int x0 = PAD_X;
  int x1 = x0 + w + GAP_X;
  btns[0] = {(int16_t)x0,(int16_t)y,(int16_t)w,(int16_t)BTN_H,l0,s0,d0};
  btns[1] = {(int16_t)x1,(int16_t)y,(int16_t)w,(int16_t)BTN_H,l1,s1,d1};
}

void layoutFooter4(Button (&btns)[4],
                   const char* l0, ButtonStyle s0,
                   const char* l1, ButtonStyle s1,
                   const char* l2, ButtonStyle s2,
                   const char* l3, ButtonStyle s3,
                   bool d0, bool d1, bool d2, bool d3) {
  int availW = tft.width() - 2*PAD_X;
  int w = (availW - 3*GAP_X) / 4;
  int y = tft.height() - FOOTER_H + (FOOTER_H - BTN_H)/2;
  int x0 = PAD_X;
  int x1 = x0 + w + GAP_X;
  int x2 = x1 + w + GAP_X;
  int x3 = x2 + w + GAP_X;
  btns[0] = {(int16_t)x0,(int16_t)y,(int16_t)w,(int16_t)BTN_H,l0,s0,d0};
  btns[1] = {(int16_t)x1,(int16_t)y,(int16_t)w,(int16_t)BTN_H,l1,s1,d1};
  btns[2] = {(int16_t)x2,(int16_t)y,(int16_t)w,(int16_t)BTN_H,l2,s2,d2};
  btns[3] = {(int16_t)x3,(int16_t)y,(int16_t)w,(int16_t)BTN_H,l3,s3,d3};
}

void layoutFooter1(Button& btn, const char* label, ButtonStyle style, bool disabled) {
  int x = PAD_X;
  int w = tft.width() - 2*PAD_X;
  int y = tft.height() - FOOTER_H + (FOOTER_H - BTN_H)/2;
  btn = {(int16_t)x,(int16_t)y,(int16_t)w,(int16_t)BTN_H,label,style,disabled};
}

void layoutFooter5(Button (&btns)[5],
                   const char* l0, ButtonStyle s0,
                   const char* l1, ButtonStyle s1,
                   const char* l2, ButtonStyle s2,
                   const char* l3, ButtonStyle s3,
                   const char* l4, ButtonStyle s4,
                   bool d0, bool d1, bool d2, bool d3, bool d4) {
  int availW = tft.width() - 2*PAD_X;
  int w = (availW - 4*GAP_X) / 5;
  int y = tft.height() - FOOTER_H + (FOOTER_H - BTN_H)/2;
  int x0 = PAD_X;
  int x1 = x0 + w + GAP_X;
  int x2 = x1 + w + GAP_X;
  int x3 = x2 + w + GAP_X;
  int x4 = x3 + w + GAP_X;
  btns[0] = {(int16_t)x0,(int16_t)y,(int16_t)w,(int16_t)BTN_H,l0,s0,d0};
  btns[1] = {(int16_t)x1,(int16_t)y,(int16_t)w,(int16_t)BTN_H,l1,s1,d1};
  btns[2] = {(int16_t)x2,(int16_t)y,(int16_t)w,(int16_t)BTN_H,l2,s2,d2};
  btns[3] = {(int16_t)x3,(int16_t)y,(int16_t)w,(int16_t)BTN_H,l3,s3,d3};
  btns[4] = {(int16_t)x4,(int16_t)y,(int16_t)w,(int16_t)BTN_H,l4,s4,d4};
}

int hit(const Button* btns, int n, int x, int y) {
  for (int i = 0; i < n; ++i) {
    const auto& b = btns[i];
    if (x >= b.x && x <= (b.x + b.w) && y >= b.y && y <= (b.y + b.h)) return i;
  }
  return -1;
}

}

unsigned long lastStatusBarUpdate = 0;
const int STATUS_BAR_UPDATE_INTERVAL = 2000;
float lastBatteryVoltage = 0.0;
bool sdCardPresent = false;
bool lastSdCardState = false;

static TaskHandle_t statusBarTaskHandle = nullptr;
static volatile bool statusBarDirty = true;
static constexpr uint32_t kStatusBarWardBlinkHalfMs = 900;

void requestStatusBarRedraw() {
  statusBarDirty = true;
}

const float R1 = 100000.0;
const float R2 = 100000.0;

/* ==================== 无操作自动熄屏（Settings → Screen Off） ====================
 *
 * 为什么挂在输入读取的咽喉上而不是主 loop 里：
 *   每个功能都是 `while(!feature_exit_requested){ X::loop(); }` 阻塞在
 *   handleButtons() 内部（见 ESP32-DIV.ino:handleSettingsSubmenuButtons 等），
 *   主 loop 的尾巴在功能运行期间根本不会执行。所以 screenIdleTick() 必须由
 *   **每条功能循环都会调用的输入读取函数** 来推进 —— 即 readTouchXY() /
 *   readTouchXYDismiss()（Touchscreen.cpp）与 isButtonPressed()（ESP32-DIV.ino）。
 *   主 loop 里另外补一次，覆盖没有功能在跑的空闲情况。
 *
 * 消抖与"唤醒那一下"：
 *   熄屏后的第一次输入只用于点亮屏幕，不算一次有效操作（否则用户在看不见
 *   屏幕时摸到哪就触发哪）。用 s_wakeIgnoreUntil 而不是一个 bool 标志，
 *   是因为同一次按下可能被 isButtonPressed / isButtonPressedEdge 连着问两遍，
 *   布尔标志会被第二问"消费掉"从而漏过闸门。
 */
static uint32_t s_lastActivityMs  = 0;
static bool     s_screenOff       = false;
static uint32_t s_wakeIgnoreUntil = 0;
static const uint32_t kScreenWakeIgnoreMs = 250;

static ScreenWakeHook    s_wakeHook      = nullptr;
static ScreenAutoOffGate s_autoOffGate   = nullptr;
static bool           s_repaintWanted  = false;
static int            s_aodPct         = -2;   // -2 = 还没画过；-1 = 电量未知
static bool           s_aodPlug        = false;
static uint32_t       s_aodNextRefresh = 0;
static uint32_t       s_aodNextResync  = 0;

bool screenIsOff() { return s_screenOff; }

void screenNoteActivity() { s_lastActivityMs = millis(); }

void screenSetWakeHook(ScreenWakeHook hook) { s_wakeHook = hook; }

void screenSetAutoOffGate(ScreenAutoOffGate fn) { s_autoOffGate = fn; }

bool screenConsumeRepaintRequest() {
  if (!s_repaintWanted) return false;
  s_repaintWanted = false;
  return true;
}

/* ==================== 电池电压 → 剩余电量(%) ====================
 * 单节锂电的开路电压(OCV) → 电量 对照表，段内线性插值。
 *
 * 为什么**不能**用线性映射（原来三处都在用 `map(v*100, 300, 420, 0, 100)`）：
 * 锂电放电曲线在 3.7~3.9V 有一段很平的平台区，这 0.2V 里藏了约 40% 的容量，
 * 线性映射必然把中段整体估高：
 *     3.90V  线性→75%   实际约 63%
 *     3.74V  线性→62%   实际约 20%   ← 差别巨大，低电量时会误以为还有一半电
 *     4.146V 线性→95%   实际约 94%（高段两者接近，所以高电量时看不出来）
 * 表按电压降序排列（pct 也是降序），写的是"静置电压"，带载会略低属正常。 */
static const struct { uint16_t mv; uint8_t pct; } kLiIonCurve[] = {
  { 4200, 100 }, { 4150,  95 }, { 4110,  90 }, { 4080,  85 },
  { 4020,  80 }, { 3980,  75 }, { 3950,  70 }, { 3910,  65 },
  { 3870,  60 }, { 3850,  55 }, { 3840,  50 }, { 3820,  45 },
  { 3800,  40 }, { 3790,  35 }, { 3770,  30 }, { 3760,  25 },
  { 3740,  20 }, { 3720,  15 }, { 3700,  10 }, { 3680,   5 },
  { 3450,   0 },
};
static const int kLiIonCurveN = (int)(sizeof(kLiIonCurve) / sizeof(kLiIonCurve[0]));

/** 由电池电压估算剩余电量(%)；返回 <0 表示还没测到（上电稳定期内）。 */
int batteryPercentFromVoltage(float v) {
  if (v <= 0.01f) return -1;
  const long mv = (long)(v * 1000.0f + 0.5f);
  if (mv >= kLiIonCurve[0].mv) return 100;
  if (mv <= kLiIonCurve[kLiIonCurveN - 1].mv) return 0;
  for (int i = 0; i < kLiIonCurveN - 1; i++) {
    const uint16_t hi = kLiIonCurve[i].mv, lo = kLiIonCurve[i + 1].mv;
    if (mv <= hi && mv >= lo) {
      const int pHi = kLiIonCurve[i].pct, pLo = kLiIonCurve[i + 1].pct;
      return pLo + (int)((long)(pHi - pLo) * (mv - lo) / (long)(hi - lo));
    }
  }
  return 0;
}

/* ==================== 显示层电量百分比（迟滞 + 充放电分离） ====================
 * 电压层已做 EMA + 峰值保持(load-compensation：把未带载时的高压当代表值，挡住
 * "进功能界面电压跌落→% 掉")。这里在"百分比显示"上再加一层逻辑迟滞，彻底消除
 * 导航/负载波动导致的跳动：
 *   - 放电(未插 USB)：只减不增。负载偶尔反弹不许让 % 上升(防反弹)；真正的下降
 *     需连续 N 次(≈N 秒)都低于当前值才刷新，过滤短时尖峰。
 *   - 充电(插 USB)：只增不减。充电器把端电压抬高，带载跌落不许让 % 下降；
 *     真实充电上升立即反映。
 * 状态机只在 1Hz 电池采样节奏上推进，避免被每帧 drawStatusBar 调用加速。 */
int batteryDisplayPercent(float voltage) {
  int raw = batteryPercentFromVoltage(voltage);
  if (raw < 0) raw = 0;
  raw = constrain(raw, 0, 100);

  static int      s_disp  = -1;   // 当前显示值
  static uint8_t  s_cntLo = 0;    // 连续"低于当前"计数
  static uint32_t s_last  = 0;    // 上次推进状态机的时间
  const uint8_t   N     = 3;      // 连续确认次数(1Hz → ~3s)
  const int       STEP  = 1;      // 偏离 ≥1% 才计入确认

  // 修复(2026-10-10)：voltage 还没测到(≤0.01V，上电稳定期内)时**不要**初始化 s_disp，
  // 保持"未就绪"(-1)。否则冷启动前 5 秒会拿到 voltage=0→raw=0→s_disp 被锁成 0，
  // 之后电压采到真实值(如 40%)时，"放电只减不增"会把它当"反弹"吞掉，屏上永远空框；
  // 只有插 USB 进入"充电只增不减"分支才把 s_disp 顶上来 —— 这正是"插 USB 才亮"的根因。
  if (voltage <= 0.01f) return s_disp;               // 未就绪：返回 -1，不污染状态机
  if (s_disp < 0) { s_disp = raw; return raw; }      // 首次拿到有效电压 → 用真实值初始化
  if (millis() - s_last < 1000) return s_disp;   // 两次采样间冻结显示
  s_last = millis();

  const bool charging = usbPowerPresent;   // IO39 外部电源插入检测
  if (charging) {
    if (raw >= s_disp) { s_disp = raw; s_cntLo = 0; }   // 充电：允许上升；禁止带载跌落导致的下降
  } else {
    if (raw <= s_disp - STEP) {                         // 放电：仅允许(连续确认的)下降
      if (++s_cntLo >= N) { s_disp = raw; s_cntLo = 0; }
    } else {
      s_cntLo = 0;                                      // 反弹/抖动：忽略上升
    }
  }
  return s_disp;
}

/** 息屏电量屏用的百分比；<0 表示还没测到。 */
static int screenPercentFromVoltage() {
  return batteryDisplayPercent(currentBatteryVoltage);
}

/* ---- 息屏电量屏几何（240x320 屏幕，竖版电池）----
 * 拆成"外框 / 电量条 / 数字 / 插头"四块，支持**最小区域刷新**：
 *   电量变化 → 只补/清"高度差那一条细条" + 只清"数字框"
 *   插头变化 → 只动插头那一小块
 * 都不整屏清屏，低亮度下看不出闪。
 *
 * 尺寸约束（决定了能缩多小）：font 6 的数字是**固定 27px/位**，
 *   "100" 就占 81px，加上 '%'(font2, 8px) 与间距 → 数字总宽 90px。
 *   电池内宽 = BAT_W - 12，所以 BAT_W 最小 ~104，取 112 留 5px 余量。 */
static const int AOD_BAT_W   = 112;
static const int AOD_BAT_H   = 148;
static const int AOD_NUB_W   = 30;   // 正极凸点（竖放时在顶部）
static const int AOD_NUB_H   = 9;
static const int AOD_GAP     = 12;   // 电池底 → 插头顶
static const int AOD_PLUG_H  = 34;   // 插脚 8 + 本体 20 + 尾线 6

static inline int aodGroupH()  { return AOD_NUB_H + AOD_BAT_H + AOD_GAP + AOD_PLUG_H; }
static inline int aodBatX()    { return (tft.width() - AOD_BAT_W) / 2; }
static inline int aodBatY()    { return (tft.height() - aodGroupH()) / 2 + AOD_NUB_H; }
static inline int aodPlugY()   { return aodBatY() + AOD_BAT_H + AOD_GAP; }
static inline int aodBarBot()  { return aodBatY() + AOD_BAT_H - 6; }
static inline int aodBarMaxH() { return AOD_BAT_H - 12; }
static inline int aodBarH(int pct) {
  if (pct <= 0) return 0;
  return (int)((long)aodBarMaxH() * pct / 100);
}
static inline uint16_t aodBarColor(int pct) { return (pct > 20) ? GREEN : TFT_RED; }

/** 只画电池外框 + 正极凸点（整屏重画时才需要）。 */
static void aodDrawFrame() {
  const int x = aodBatX(), y = aodBatY();
  tft.fillRoundRect(x + (AOD_BAT_W - AOD_NUB_W) / 2, y - AOD_NUB_H,
                    AOD_NUB_W, AOD_NUB_H + 3, 2, TFT_WHITE);
  tft.drawRoundRect(x, y, AOD_BAT_W, AOD_BAT_H, 8, TFT_WHITE);
  tft.drawRoundRect(x + 1, y + 1, AOD_BAT_W - 2, AOD_BAT_H - 2, 8, TFT_WHITE);
}

/** 整条电量条重画（清该列 + 按新高度填色）。 */
static void aodBarPaintWhole(int h, uint16_t color) {
  tft.fillRect(aodBatX() + 6, aodBatY() + 6, AOD_BAT_W - 12, aodBarMaxH(), TFT_BLACK);
  if (h > 0) tft.fillRect(aodBatX() + 6, aodBarBot() - h, AOD_BAT_W - 12, h, color);
}

/**
 * 电量条增量更新：**只动高度差那一条**（这正是"区域刷新"的关键）。
 * 条用方角矩形，所以差值与交集都是精确矩形，不会出现圆角拼缝。
 * 跨过 20% 变色时无法只改一条 → 退回整条重画（很少发生）。
 */
static void aodBarSet(int oldH, int newH, uint16_t oldC, uint16_t newC) {
  const int x = aodBatX() + 6, w = AOD_BAT_W - 12;
  if (oldC != newC) {
    aodBarPaintWhole(newH, newC);
  } else if (newH > oldH) {
    tft.fillRect(x, aodBarBot() - newH, w, newH - oldH, newC);      // 补长出来的那条
  } else if (newH < oldH) {
    tft.fillRect(x, aodBarBot() - oldH, w, oldH - newH, TFT_BLACK); // 清缩回去的那条
  }
}

/**
 * 只重画数字框：清框 → 把落在框内的电量条补回来 → 画数字。
 * 字体选择是个真实的坑：
 *   TFT_eSPI 的 font 6(Font64rle) 是**数字专用字体**，96 个码位里有 80 个
 *   （含 '%'）在字模表里全部回退成空格字形 → font 6 根本画不出百分号。
 *   font 2(Font16) / font 4(Font32rle) 才是完整的 96 字符 ASCII 表。
 *   ⇒ 数字 font 6（48px 高 / 27px 宽每位），'%' 用 font 2（16px 高 / 8px 宽）做上标。
 * 另外：uiPrint() 对纯 ASCII 会转发给 tft.print()，而 **print 路径不认
 * setTextDatum**，只按光标左上角画 —— 居中必须自己用 tft.textWidth()
 * 量真实宽度（各字体是比例字宽，不是等宽！）再定位。
 */
static void aodDrawNumber(int pct) {
  const int x = aodBatX();

  char num[6];
  if (pct < 0) snprintf(num, sizeof(num), "--");
  else         snprintf(num, sizeof(num), "%d", pct);

  const int wNum = tft.textWidth(num, 6);
  const int wPct = (pct < 0) ? 0 : tft.textWidth("%", 2);
  const int gap  = wPct ? 1 : 0;
  const int x0   = x + AOD_BAT_W / 2 - (wNum + gap + wPct) / 2;
  const int yNum = aodBatY() + AOD_BAT_H / 2 - 24;      // font 6 高 48 → 垂直居中

  // 数字框：按"最宽数字"预留固定大小，任何数字都清得干净
  const int boxW = tft.textWidth("100", 6) + 1 + tft.textWidth("%", 2) + 2;
  const int boxX = x + AOD_BAT_W / 2 - boxW / 2;
  const int boxY = yNum - 1, boxH = 50;
  tft.fillRect(boxX, boxY, boxW, boxH, TFT_BLACK);
  // 数字是透明底压在条上的，所以清完框要把落在框内的条补回来（方角，交集即精确矩形）
  const int barTop = aodBarBot() - aodBarH(pct);
  const int ovTop  = max(boxY, barTop);
  const int ovBot  = min(boxY + boxH, aodBarBot());
  if (ovBot > ovTop) {
    tft.fillRect(x + 6, ovTop, AOD_BAT_W - 12, ovBot - ovTop, aodBarColor(pct));
  }

  uiSetTextSize(1);
  uiSetTextDatum(TL_DATUM);      // print 路径只认左上角，显式定死
  uiSetTextColor(TFT_WHITE);     // 单参 = 透明底

  uiSetTextFont(6);
  uiSetCursor(x0, yNum);
  uiPrint(num);

  if (pct >= 0) {                // 电量未知时不画 '%'，避免出现 "--%"
    uiSetTextFont(2);
    uiSetCursor(x0 + wNum + gap, yNum + 8);   // 靠上做上标
    uiPrint("%");
  }
}

/** 只重画插头那一小块；无插头时只清黑。 */
static void aodDrawPlug(bool on) {
  const int cx = tft.width() / 2;
  const int py = aodPlugY();
  tft.fillRect(cx - 18, py - 3, 36, 42, TFT_BLACK);   // 先清干净，避免残影
  if (!on) return;

  tft.fillRect(cx - 9, py,      5, 8, TFT_YELLOW);    // 插脚左（朝上指向电池）
  tft.fillRect(cx + 4, py,      5, 8, TFT_YELLOW);    // 插脚右
  tft.fillRoundRect(cx - 14, py + 8, 28, 20, 3, TFT_YELLOW);   // 插头本体
  tft.fillRect(cx - 6, py + 28, 12, 6, TFT_YELLOW);   // 尾线
}

/**
 * 息屏后显示的低亮度"电量屏"：竖版电池（数字居中）+ 下方黄色充电插头。
 * 刷新策略（按代价从大到小）：
 *   force / 首次      → 整屏清黑 + 全画
 *   电量变化           → 只补/清"高度差细条" + 只清"数字框"(92x50)
 *   插头变化           → 只重画插头那块(36x42)
 *   跨 20% 变色        → 电量条退回整条重画（少见）
 */
void screenDrawAod(bool force) {
  const int  pct  = screenPercentFromVoltage();
  const bool plug = usbPowerPresent;

  const bool first = (s_aodPct == -2);        // 还没画过任何东西
  if (!force && !first && pct == s_aodPct && plug == s_aodPlug) return;

  const bool whole  = force || first;
  const bool pctChg = whole || (pct != s_aodPct);

  if (whole) {
    tft.fillScreen(TFT_BLACK);
    aodDrawFrame();
    aodBarPaintWhole(aodBarH(pct), aodBarColor(pct));
  } else if (pctChg) {
    aodBarSet(aodBarH(s_aodPct), aodBarH(pct),
              aodBarColor(s_aodPct), aodBarColor(pct));
  }
  if (pctChg)                     aodDrawNumber(pct);
  if (whole || plug != s_aodPlug) aodDrawPlug(plug);

  s_aodPct  = pct;
  s_aodPlug = plug;
}

static void screenSleep() {
  if (s_screenOff) return;
  s_screenOff = true;
  // 先画电量屏再降亮度，避免先暗一下再亮的闪动
  screenDrawAod(true);
  setBrightness(SCREEN_OFF_DIM_LEVEL);
  s_aodNextRefresh = millis() + SCREEN_OFF_AOD_REFRESH_MS;
  s_aodNextResync  = millis() + SCREEN_OFF_AOD_RESYNC_MS;
}

static void screenWake() {
  if (!s_screenOff) return;
  s_screenOff = false;
  setBrightness(settings().brightness);   // 恢复用户设定的亮度，不是某个固定值
  s_aodPct = -2;                          // 电量屏作废，下次睡眠重画
  s_repaintWanted = true;
  // 屏幕刚被电量屏整屏覆盖过，必须整体重绘。菜单/子菜单类界面由应用注册的
  // hook 立刻重建；功能屏留标志，由功能自己的循环消费（见 screenConsumeRepaintRequest）。
  if (s_wakeHook) s_wakeHook();
  requestStatusBarRedraw();
}

/*──────────────────── 按键音（蜂鸣器，Settings 可开关） ────────────────────*/
// 2026-10-09：蜂鸣器飞线 IO2→IO45 后启用按键提示音。LEDC 通道 6（通道 0 是
// 背光、通道 7 是 SubGHz replayBeep），全部非阻塞：响铃到点由 uiBeepPoll() 关闭。
// 触发点集中在 ESP32-DIV.ino 的 isButtonPressed()（全部功能都经它轮询按键，
// 内部做沿检测，一次按键只响一声）。
// 2026-10-21：加 esp_timer 硬兜底。工程里多处功能循环（ir.cpp 三个 loop、
// ino 内数十段手写 dispatch while）绕开 isButtonPressed() 或长时间不回主循环，
// 没人调 uiBeepPoll() 时 LEDC 会一直响到退出功能。一次性定时器由 esp_timer
// 独立任务执行关音，不依赖任何循环配合——从机制上消灭"进入某功能蜂鸣长响"。
#include "esp_timer.h"
#ifdef BUZZER_PIN
static constexpr uint8_t UI_BEEP_LEDC_CH = 6;
static bool     s_uiBeepArmed  = false;
static uint32_t s_uiBeepOffAtMs = 0;
static esp_timer_handle_t s_uiBeepOffTimer = nullptr;

// esp_timer 任务上下文里强制关音（与 uiBeepPoll 同款动作，幂等）
static void uiBeepOffTimerCb(void*) {
  if (!s_uiBeepArmed) return;
  ledcWriteTone(UI_BEEP_LEDC_CH, 0);
  ledcDetachPin(BUZZER_PIN);
  s_uiBeepArmed = false;
}

static void uiBeepArmOffTimer(uint16_t ms) {
  if (!s_uiBeepOffTimer) {
    const esp_timer_create_args_t args = {
      &uiBeepOffTimerCb, nullptr, ESP_TIMER_TASK, "uibeep_off", false
    };
    if (esp_timer_create(&args, &s_uiBeepOffTimer) != ESP_OK) return;
  }
  // 重挂前先停旧的一次性定时器（未在跑时返回 error，忽略即可）
  esp_timer_stop(s_uiBeepOffTimer);
  esp_timer_start_once(s_uiBeepOffTimer, (uint64_t)ms * 1000ULL);
}
#endif

void uiBeep(uint16_t hz, uint16_t ms) {
#ifdef BUZZER_PIN
#if DIAG_PRINTS_ENABLED
  // 诊断：每次按键沿打一行。串口有 [UIBEEP] 而 IO45 无波形 ⇒ 引脚/硬件层问题；
  // 串口无 [UIBEEP] ⇒ 按键触发路径没走到（旧固件 / 直接点屏幕菜单项 / Key Beep 关）。
  DBG("[UIBEEP] req hz=%u ms=%u gate=%d pin=%d\r\n",
      hz, ms, (int)settings().keyBeepEnabled, BUZZER_PIN);
#endif
  if (!settings().keyBeepEnabled) return;    // Settings → Key Beep 关闭时静音
  if (hz == 0) hz = 2200;
  if (ms == 0) ms = 25;                      // 2026-10-20：45→25（与默认参数一致）
  ledcSetup(UI_BEEP_LEDC_CH, 4000, 8);
  ledcAttachPin(BUZZER_PIN, UI_BEEP_LEDC_CH);
  ledcWriteTone(UI_BEEP_LEDC_CH, hz);
  s_uiBeepArmed  = true;
  s_uiBeepOffAtMs = millis() + ms;
  uiBeepArmOffTimer(ms);                     // 硬兜底：到点必关，不等任何循环 poll
#endif
}

void uiBeepPoll() {
#ifdef BUZZER_PIN
  if (!s_uiBeepArmed) return;
  if ((int32_t)(millis() - s_uiBeepOffAtMs) < 0) return;
  ledcWriteTone(UI_BEEP_LEDC_CH, 0);
  ledcDetachPin(BUZZER_PIN);
  s_uiBeepArmed = false;
#endif
}

/* 开机自检音（2026-10-09 诊断用）：无视 Key Beep 开关直接响一声。
 * 分叉判据 —— 开机响 = IO45/蜂鸣器硬件路径 OK，问题在按键触发链路；
 * 开机不响 = IO45 引脚/飞线/模组脚位问题（示波器盯 IO45 应见 300ms 方波）。
 * 硬件验证完成后可整段删除（连同 setup() 末尾的调用）。
 * 2026-10-21：调用点已移除（诊断完成），函数保留；同样挂硬兜底定时器。 */
void uiBeepSelfTest(uint16_t hz, uint16_t ms) {
#ifdef BUZZER_PIN
  if (hz == 0) hz = 2400;
  if (ms == 0) ms = 300;
  ledcSetup(UI_BEEP_LEDC_CH, 4000, 8);
  ledcAttachPin(BUZZER_PIN, UI_BEEP_LEDC_CH);
  ledcWriteTone(UI_BEEP_LEDC_CH, hz);
  s_uiBeepArmed  = true;
  s_uiBeepOffAtMs = millis() + ms;
  uiBeepArmOffTimer(ms);
#if DIAG_PRINTS_ENABLED
  DBG("[UIBEEP] selftest hz=%u ms=%u pin=%d\r\n", hz, ms, BUZZER_PIN);
#endif
#endif
}

void screenIdleTick() {
  if (s_screenOff) {
    // 睡眠期间也要让电量/插头保持最新 —— statusBarTask 是独立任务，
    // 即使主流程被功能屏的 while 卡住，currentBatteryVoltage / usbPowerPresent 仍在更新。
    if ((int32_t)(millis() - s_aodNextRefresh) >= 0) {
      s_aodNextRefresh = millis() + SCREEN_OFF_AOD_REFRESH_MS;
      // 每 AOD_RESYNC 周期强制整屏重画一次：万一有别的地方（未加闸门的功能屏）
      // 在电量屏上画了东西，最多这么多秒内自愈。
      const bool resync = ((int32_t)(millis() - s_aodNextResync) >= 0);
      if (resync) s_aodNextResync = millis() + SCREEN_OFF_AOD_RESYNC_MS;
      screenDrawAod(resync);
    }
    return;                                      // 已熄屏，等输入来唤醒
  }

  // 2026-10-09：息屏超时功能整体移除（用户判定无用，Settings 里的 Screen Off
  // 行一并删除）。不再做无操作超时判定，屏幕只随输入保持常亮。
  // screenSleep()/AOD 电量屏基础设施保留（screenSleep 已无调用点，取地址防
  // -Werror=unused-function），万一以后要恢复息屏逻辑直接接回来即可。
  (void)&screenSleep;
}

bool screenFilterInput() {
  // ★ 能走到这里就说明确实有输入 —— 无论最终是否被丢弃，先记一次"有人在操作"。
  //   这一行同时修掉一个真实 bug：唤醒分支原先不刷新时间戳，唤醒后
  //   s_screenOff 已变 false，下一拍 screenIdleTick() 却仍用几分钟前的旧时间戳
  //   判定"已超时" → 屏幕亮一下立刻又黑，表现为"点屏幕像卡住了"。
  screenNoteActivity();

  if (s_screenOff) {                             // 熄屏中的第一下：只点亮
    // 先把闸门武装好，再点亮并重绘 —— hook 内部若也会读输入，才会被正确吞掉
    s_wakeIgnoreUntil = millis() + kScreenWakeIgnoreMs;
    screenWake();
    return true;
  }
  if ((int32_t)(millis() - s_wakeIgnoreUntil) < 0) return true;   // 唤醒余振
  return false;
}

/**
 * 外部电源(USB)插入检测 —— 纯数字量，走 IO39（见 shared.h 的 USB_DETECT_*）。
 * 与电池电量测量无关：不碰 ADC、不碰 I2C，所以上电即可读，
 * 不受 BATTERY_START_DELAY_MS 与 WiFi 抢占 ADC1 的影响。
 */
bool readUsbPowerPresent()
{
#if USB_DETECT_PIN >= 0
  static bool s_pinReady = false;

  if (!s_pinReady) {
    s_pinReady = true;
    // 默认纯 INPUT：板上检测电路自带分压/下拉，内部上下拉会并联进去改变分压比，
    // 高阻分压时可能把电平压到逻辑高阈值以下，所以不主动开。
#if USB_DETECT_PULL_MODE == 1
    pinMode(USB_DETECT_PIN, INPUT_PULLUP);
#elif USB_DETECT_PULL_MODE == 2
    pinMode(USB_DETECT_PIN, INPUT_PULLDOWN);
#else
    pinMode(USB_DETECT_PIN, INPUT);
#endif
  }

  const int lvl = digitalRead(USB_DETECT_PIN);
#if USB_DETECT_ACTIVE_HIGH
  return lvl == HIGH;
#else
  return lvl == LOW;
#endif
#else
  return false;
#endif
}

// Last battery level source used by readBatteryVoltage(): 'A' = ADC divider,
// 'I' = IP5306 steps, '?' = not read yet. Shown in the [IP5306] diag line.
static char s_battSrc = '?';
// Last raw GPIO2 ADC probe in mV (0 = pin reads dead, -1 = not attempted yet).
static int s_adcProbeMv = -1;

float readBatteryVoltage()
{
  // 上电稳定前不做任何电池测量（ADC / IP5306 I2C 都不动），
  // 见 shared.h 的 BATTERY_START_DELAY_MS。状态栏此期间显示 "--"。
  if (millis() < BATTERY_START_DELAY_MS) {
    return 0.0f;
  }

#if defined(BATTERY_USE_IP5306) && !BATTERY_LEVEL_USE_ADC
  // Strict IP5306 mode (BATTERY_LEVEL_USE_ADC 0): 0/25/50/75/100% steps only.
  {
    int8_t pct = ip5306_getBatteryPercent();
    if (pct >= 0)
    {
      s_battSrc = 'I';
      // Synthesize a voltage in the 3.0..4.2 V range so the existing
      // drawStatusBar() 300..420 -> 0..100% mapping reproduces the step.
      return 3.0f + (pct / 100.0f) * 1.2f;
    }
  }
  return 0.0f;
#else
  // ADC mode (default, BATTERY_LEVEL_USE_ADC 1): continuous voltage through
  // the on-board 100k/100k divider (VBAT -> GPIO2), IP5306 as fallback.
  //
  // The IDF ADC1 lock is held while WiFi initializes/scans; calling
  // analogReadMilliVolts() in that window spams
  // "adc1 lock release called before acquire" and can return garbage.
  // Mitigation: wait out the boot-time WiFi init, validate the first sample
  // against a plausible pack range, and back off 5 s whenever it is off.
  float adcVolt = 0.0f;
#if BATTERY_ADC_PIN >= 0
  static uint32_t s_adcNextTryMs = 2000; // 首次尝试：配合 BATTERY_START_DELAY_MS=2000，色块约 2s 出现
  static bool     s_adcEverValid = false; // ADC 是否**曾经**读到过有效值
  static float    s_adcLastVolt  = 0.0f;  // 最近一次有效值(峰值保持后)
  static float    s_emaVolt      = -1.0f; // EMA 滤波状态(<0 = 未初始化)
  static float    s_peakVolt     = -1.0f; // 峰值保持：屏蔽带载跌落，仅缓慢回落(代表 SoC)

  if (millis() >= s_adcNextTryMs)
  {
    static bool adcInitialized = false;

    if (!adcInitialized)
    {
      analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db);
      adcInitialized = true;
    }

    // 初检取 **5 次采样的中位数**。
    // 原来这里是"取 3 次最小值"，注释假设瞬态只会把读数顶高；但实测
    // ESP32-S3 的 ADC 在 IDF 的 ADC1 锁被 WiFi 占用时会**返回 0 附近**，
    // 只要 3 次里有 1 次掉零，最小值就≈0 → 整次读数被判定不可信 →
    // 退回 IP5306 的四档值(0/25/50/75/100)。屏上就出现"电压 4.146V 却显示 75%"
    // 这种自相矛盾的现象。中位数对**向上和向下**尖峰都免疫，5 取 3 足够稳。
    uint32_t s[5];
    for (uint8_t i = 0; i < 5; i++) {
      s[i] = analogReadMilliVolts(BATTERY_ADC_PIN);
      delayMicroseconds(200);
    }
    for (uint8_t i = 1; i < 5; i++) {          // 5 个元素插入排序
      const uint32_t k = s[i];
      int j = i - 1;
      while (j >= 0 && s[j] > k) { s[j + 1] = s[j]; j--; }
      s[j + 1] = k;
    }
    const uint32_t probeMv = s[2];

    s_adcProbeMv = (int)probeMv; // 诊断：原始 GPIO2 读数（mV），打印在 [IP5306] 行
    const float probeVolt = (probeMv / 1000.0f) * BATTERY_VDIV_MULT;

    if (probeVolt > 2.5f && probeVolt < 4.6f)
    {
      uint32_t sum = 0;

      for (uint8_t i = 0; i < 16; i++) {
        sum += analogReadMilliVolts(BATTERY_ADC_PIN);
        delayMicroseconds(200);
      }

      adcVolt = (sum / 16.0f / 1000.0f) * BATTERY_VDIV_MULT;
      // 时间滤波：ESP32-S3 的 ADC 本身有噪声，且工程常驻 WifiScan/BleScan
      // 后台扫描周期拉电流，使电池分压随负载起伏；两者叠加后若不平滑，
      // 状态栏百分比在低电量区(≤25%)会每 1% 变化就重绘一次，肉眼见"跳动"。
      // 做法：采样节流到 1Hz(21 次采样已够稳)，采样间用 EMA 输出平滑值，
      // 同时省掉原先每帧 21 次采样的 CPU 开销。
      if (s_emaVolt < 0.01f) s_emaVolt = adcVolt;                       // 首采用真值初始化
      else                    s_emaVolt = s_emaVolt * 0.6f + adcVolt * 0.4f;  // α=0.4，时间常数≈2.5s
      s_adcEverValid = true;
      // 峰值保持 + 慢衰减：带载(点亮屏幕/拉起 RF)时电池端电压会真实跌落，而 OCV
      // 曲线在 3.76~3.80V 段极陡(约 32mV≈14%)，导致"进功能界面电量 38%→24%"的假放电。
      // SoC 不该因开背光而下降，故取近期最高平滑电压作代表值；仅负载移除或真实放电时缓慢回落。
      if (s_peakVolt < 0.01f)            s_peakVolt = s_emaVolt;                       // 首采初始化
      else if (s_emaVolt >= s_peakVolt)  s_peakVolt = s_emaVolt;                       // 回升/充电：立即跟高
      else                               s_peakVolt = s_peakVolt + (s_emaVolt - s_peakVolt) * 0.003f; // 带载/放电：极慢衰减(~330s)，屏蔽短暂负载跌落
      s_adcLastVolt  = s_peakVolt;
      s_battSrc = 'A';
      s_adcNextTryMs = millis() + 1000;   // 采样节流到 1Hz（原设计首次后不再推进→每帧重采）
      return s_adcLastVolt;
    }

    adcVolt = 0.0f;
    // 无效(多半是 WiFi/BLE 后台扫描占着 ADC1 锁)→ 快速重试 250ms。
    // 旧值 2000ms 是在"BATTERY_START_DELAY_MS=5s 门限"时代定的：那时首采在 5s，
    // 扫描早已结束，退避几乎从不触发；改 2s 门限后首采(2s)正好撞进扫描窗口，
    // 每次失败都被放大成 2s → 色块 5~7s 才出现。锁是瞬态的，快速重试即可。
    s_adcNextTryMs = millis() + 250;
  }

  // 曾经读到过有效值：失败/退避期间**沿用上次有效值**，绝不退回 IP5306 的四档值
  // —— 否则屏上会在 94% 和 75% 之间来回跳。
  if (s_adcEverValid) return s_adcLastVolt;
#endif

#ifdef BATTERY_USE_IP5306
  // ADC missing or implausible: fall back to the IP5306 0/25/50/75/100% steps.
  {
    int8_t pct = ip5306_getBatteryPercent();
    if (pct >= 0)
    {
      s_battSrc = 'I';
      // Synthesize a voltage in the 3.0..4.2 V range so the existing
      // drawStatusBar() 300..420 -> 0..100% mapping reproduces the step.
      return 3.0f + (pct / 100.0f) * 1.2f;
    }
  }
#endif

#if BATTERY_ADC_PIN >= 0
  return adcVolt;
#else
  return 0.0f;
#endif
#endif
}

float readInternalTemperature() {
  float temperature = temperatureRead();
  return temperature;
}

void updateSdCardStatus() {

#ifdef SD_CD
  bool cardDetected = !digitalRead(SD_CD);
#else
  bool cardDetected = true;
#endif

  if (cardDetected != lastSdCardState) {
    sdCardPresent = cardDetected;
    lastSdCardState = cardDetected;

    Serial.println(sdCardPresent ? "SD Card detected" : "SD Card removed");
    requestStatusBarRedraw();
  }
}

static bool statusBarBatteryMeaningfulChange(int newPct, int oldPct, bool force) {
  if (force) {
    return true;
  }
  // -1 = 未测到。"未测到↔测到(含 0%)"的切换必须重绘——否则 0% 电池开机时
  // -1→0 被当成 0→0，色块要等别的图标变化连带重绘才出现。
  if (newPct < 0 || oldPct < 0) {
    return newPct != oldPct;
  }
  if (newPct <= 25 || oldPct <= 25) {
    return newPct != oldPct;
  }
  const int d = newPct - oldPct;
  return (d >= 3 || d <= -3);
}

static int statusBarTempBand(float t) {
  if (fabs(static_cast<double>(t) - 53.33) < 0.51) {
    return 1;
  }
  if (t > 55.f) {
    return 2;
  }
  return 0;
}

void drawStatusBar(float batteryVoltage, bool forceUpdate, bool bottomSeparator) {
  // ⚠ 息屏期间禁止画状态栏。
  //   原因：statusBarTask 是独立 FreeRTOS 任务，每 400ms 因 WiFi/BLE 计数等变化
  //   置 statusBarDirty；主 loop 的 updateStatusBar() 就会把状态栏画出来，
  //   直接盖在息屏电量屏上（实测 bug：息屏后状态栏还在刷新）。
  //   脏标志一并清掉：唤醒时 screenWake() 会 requestStatusBarRedraw() 重新置位。
  if (screenIsOff()) {
    statusBarDirty = false;
    return;
  }

  static int lastBatteryPercentage = -1;
  static int lastWifiHalf          = -100000;
  static int lastBleHalf           = -100000;
  static int lastTempBand          = -100;
  static int lastSdSnap            = -1;
  static bool lastWardGpsIcon      = false;
  static bool lastUsbPresent       = false;
  static uint32_t lastWardBlinkPhase = 0;

  // 走锂电曲线 + 显示层迟滞（防反弹/连续确认/充放电分离），见 batteryDisplayPercent()
  // rawPct: -1=未测到(电压无效)，>=0=真实百分比。门控(battCh)必须用 rawPct 区分
  // "未测到→0%"这个关键切换——若先钳到 0，0%电池 时 -1→0 被当成 0→0"无变化"，
  // 色块直到别的图标(wifi/ble)变化连带重绘才出现（2026-10-10 实测踩坑）。
  const int rawPct = batteryDisplayPercent(batteryVoltage);
  int batteryPercentage = rawPct < 0 ? 0 : constrain(rawPct, 0, 100);

  int wifiDevices = 0;
  int bleDevices  = 0;

  wifiDevices = WifiScan::getLastCount();
  bleDevices  = BleScan::getLastCount();

  const int wifiHalf = wifiDevices / 2;
  const int bleHalf  = bleDevices / 2;

  const bool wardGpsIcon = GpsWardriver::statusBarGpsIconActive();
  const uint32_t wardBlinkPhase =
      wardGpsIcon ? (millis() / kStatusBarWardBlinkHalfMs) : 0u;
  const bool wardSatVisible =
      wardGpsIcon && ((wardBlinkPhase & 1u) == 0u);

  const float internalTemp = readInternalTemperature();
  const int tempBand         = statusBarTempBand(internalTemp);
  const int sdSnap           = sdCardPresent ? 1 : 0;

  const bool battCh =
      statusBarBatteryMeaningfulChange(rawPct, lastBatteryPercentage, forceUpdate);
  const bool wardBlinkOnly =
      !forceUpdate && wardGpsIcon && lastWardGpsIcon &&
      (wardBlinkPhase != lastWardBlinkPhase) && !battCh && wifiHalf == lastWifiHalf &&
      bleHalf == lastBleHalf && tempBand == lastTempBand && sdSnap == lastSdSnap;

  if (wardBlinkOnly) {
    constexpr int kBarH   = 20;
    constexpr int kY      = 4;
    constexpr int kIconY  = kY - 2;
    constexpr int kIconW  = 16;
    constexpr int kGap    = 3;
    constexpr int kBleIx  = 130;
    constexpr int kBleDrawX = kBleIx + 25;
    const int wardGpsX    = kBleDrawX - kGap - kIconW;
    if (wardSatVisible) {
      tft.drawBitmap(wardGpsX, kIconY, bitmap_icon_satellite, kIconW, kIconW, TFT_ORANGE);
    } else {
      tft.fillRect(wardGpsX, kIconY, kIconW, kIconW, UI_LABLE);
    }
    if (bottomSeparator) {
      tft.drawFastHLine(0, kBarH - 1, tft.width(), TFT_WHITE);
    }
    lastWardBlinkPhase = wardBlinkPhase;
    return;
  }

  if (battCh || wifiHalf != lastWifiHalf || bleHalf != lastBleHalf || tempBand != lastTempBand ||
      sdSnap != lastSdSnap || wardGpsIcon != lastWardGpsIcon ||
      usbPowerPresent != lastUsbPresent ||
      (wardGpsIcon && wardBlinkPhase != lastWardBlinkPhase) || forceUpdate) {
    int barHeight = 20;
    int x = 7;
    int y = 4;

    tft.fillRect(0, 0, tft.width(), barHeight, UI_LABLE);

    // 电池：4 格色块（按电量点亮格数），前两格绿、第三格黄、第四格红，无百分比文字。
    // 电量 0% 时 4 格全空框；25%/50%/75%/100% 对应亮 1/2/3/4 格。
    tft.drawRoundRect(x, y, 28, 10, 2, TFT_WHITE);
    tft.fillRect(x + 28, y + 3, 2, 4, TFT_WHITE);

    if (currentBatteryVoltage <= 0.01f) {
      // 上电稳定前（BATTERY_START_DELAY_MS 内）未测量：4 格全空框
      const int cellW = 5, cellH = 6, cellGap = 1, cellY = y + 2, cellX0 = x + 2;
      for (int i = 0; i < 4; i++) {
        tft.drawRect(cellX0 + i * (cellW + cellGap), cellY, cellW, cellH, TFT_WHITE);
      }
    } else {
      // 点亮的格数：0..4（每 25% 一格，向上取整）
      int lit = (batteryPercentage + 24) / 25;
      if (lit < 0) lit = 0;
      if (lit > 4) lit = 4;
      // 0% 时点亮第 1 格（红）：让"空框"只表示"未测到"，与"0% 没电"可区分。
      // （2026-10-10 教训：电池耗尽到 3.4V 以下是 0%→0 格，与未测到的空框长得
      //  一模一样，被误判为"色块不显示/读不到"。）
      if (batteryPercentage >= 0 && lit < 1) lit = 1;
      // 各格固定配色（从左到右）：第1格红、第2格黄、第3格绿、第4格绿
      static const uint16_t cellColor[4] = { TFT_RED, TFT_YELLOW, GREEN, GREEN };
      const int cellW = 5, cellH = 6, cellGap = 1, cellY = y + 2, cellX0 = x + 2;
      for (int i = 0; i < 4; i++) {
        const int cx = cellX0 + i * (cellW + cellGap);
        if (i < lit) tft.fillRect(cx, cellY, cellW, cellH, cellColor[i]);
        else        tft.drawRect(cx, cellY, cellW, cellH, TFT_WHITE);
      }
    }

    const int iconW         = 16;
    const int gap           = 3;
    const int wifiBarsWidth = 24;

    const int bleIconX      = 130;
    const int bleTextX      = bleIconX + iconW + gap;
    const int wifiBarsX     = bleTextX + 12 + gap;
    const int tempIconX     = wifiBarsX + wifiBarsWidth + gap;
    const int sdIconX       = tempIconX + iconW + gap;
    int iconY               = y - 2;

    int clearWidth = tft.width() - bleIconX;
    tft.fillRect(bleIconX, 0, clearWidth, barHeight, UI_LABLE);

    const int bleDrawX = bleIconX + 25;
    if (wardSatVisible) {
      const int wardGpsX = bleDrawX - gap - iconW;
      tft.drawBitmap(wardGpsX, iconY, bitmap_icon_satellite, iconW, iconW, TFT_ORANGE);
    }

    uint16_t wifiColor = (wifiDevices > 0) ? GREEN : TFT_WHITE;
    uint16_t bleColor  = (bleDevices  > 0) ? TFT_CYAN  : TFT_WHITE;

    int wifiStrength = 0;
    if (wifiDevices > 0) {

      wifiStrength = constrain(::map(wifiDevices, 0, 15, 0, 100), 0, 100);
    }

    int wifiX = wifiBarsX + 10;
    int wifiY = y + 11;

    for (int i = 0; i < 4; i++) {
      const int sigBarH = (i + 1) * 3;
      const int barWidth  = 4;
      const int barX      = wifiX + i * 6;

      if (wifiStrength > i * 25) {
        tft.fillRoundRect(barX, wifiY - sigBarH, barWidth, sigBarH, 1, GREEN);
      } else {
        tft.drawRoundRect(barX, wifiY - sigBarH, barWidth, sigBarH, 1, TFT_WHITE);
      }
    }

    tft.drawBitmap(bleIconX + 25, iconY, bitmap_icon_ble, 16, 16, bleColor);
    uiSetTextColor(bleColor, UI_LABLE);

    if (internalTemp == 53.33f) {
      tft.drawBitmap(tempIconX + 10, y - 2, bitmap_icon_temp, 16, 16, TFT_YELLOW);
    } else if (internalTemp > 55) {
      tft.drawBitmap(tempIconX + 10, y - 2, bitmap_icon_temp, 16, 16, TFT_RED);
    } else {
      tft.drawBitmap(tempIconX + 10, y - 2, bitmap_icon_temp, 16, 16, GREEN);
    }

    if (sdCardPresent) {
      tft.drawBitmap(sdIconX + 10, y - 2, bitmap_icon_sdcard, 16, 16, GREEN);
    } else {
      tft.drawBitmap(sdIconX + 10, y - 2, bitmap_icon_nullsdcard, 16, 16, TFT_RED);
    }

    // 外部电源已接入：电池框右侧画黄色"插头"标。
    // 判据取自 IO39 硬件插入检测（USB_DETECT_PIN），不再依赖 IP5306 的充电位 ——
    // 插入检测反映"是否有外部电源"，比充电位更直接，也不怕 IP5306 不在线。
    if (usbPowerPresent) {
      const int plugX = x + 32;
      const int plugY = y + 1;
      // 两根插脚（指向电池方向）
      tft.fillRect(plugX + 0, plugY + 1, 4, 2, TFT_YELLOW);
      tft.fillRect(plugX + 0, plugY + 6, 4, 2, TFT_YELLOW);
      // 插头本体
      tft.fillRoundRect(plugX + 4, plugY - 1, 8, 10, 2, TFT_YELLOW);
      // 尾线
      tft.fillRect(plugX + 12, plugY + 3, 3, 2, TFT_YELLOW);
    }

    if (bottomSeparator) {
      tft.drawFastHLine(0, barHeight - 1, tft.width(), TFT_WHITE);
    }

    lastBatteryPercentage = rawPct;   // 记录原始值(-1=未测到)，供门控区分"未测到↔0%"
    lastWifiHalf          = wifiHalf;
    lastBleHalf           = bleHalf;
    lastTempBand          = tempBand;
    lastSdSnap            = sdSnap;
    lastWardGpsIcon       = wardGpsIcon;
    lastUsbPresent        = usbPowerPresent;
    lastWardBlinkPhase    = wardGpsIcon ? wardBlinkPhase : 0u;
  }
}

static void statusBarTask(void* ) {
  static int prevBattPct    = -1;
  static int prevWifiHalf   = -100000;
  static int prevBleHalf    = -100000;
  static int prevTempBand   = -100;
  static int prevSdSnap     = -1;
  static bool prevWardIcon  = false;
  static uint32_t prevWardPhase = 0;
  static bool prevCharging  = false;
  static bool prevUsb       = false;

  for (;;) {
    // 外部电源(USB)插入检测：纯 GPIO，不碰 ADC/I2C，所以不受下面
    // BATTERY_START_DELAY_MS 的门限限制，上电立刻就能反映真实状态。
    usbPowerPresent = readUsbPowerPresent();

    // 电量采样自身的上电稳定门在 readBatteryVoltage() 内部
    // （BATTERY_START_DELAY_MS），此处无需再做电池在线判断。
    // （2026-10-10：batteryOnline 变量与 IP5306 总线扫描一并移除——V2 板无该芯片。）

    updateSdCardStatus();
    const float v = readBatteryVoltage();
    currentBatteryVoltage = v;
    // 2026-10-10：V2 板无 IP5306（BATTERY_USE_IP5306 已在 shared.h 注释掉），
    // 充电状态恒为 false；USB 插入检测走 IO39（usbPowerPresent），与 I2C 无关。
    // 原此处按 BATTERY_USE_IP5306 轮询 0x70/0x78 寄存器并打印长诊断行（超出屏幕
    // 调试行宽 36 字符，adc 字段被截断看不到），现换成下面的短行诊断。
    batteryIsCharging = false;

#if DIAG_PRINTS_ENABLED
    // 诊断：每 3 秒一行（≤36 字符，适配屏幕调试行宽）：
    //   V   = readBatteryVoltage() 返回值(mV，峰值保持后喂给显示层)
    //   src = A:ADC 曾读到有效值  -:尚无有效值
    //   adc = 最近一次 IO2 初检中位数(mV，未乘分压比)
    //   usb = IO39 USB 插入检测(1=插入)
    {
      static uint32_t s_lastDiag = 0;
      if (millis() - s_lastDiag > 3000) {
        s_lastDiag = millis();
        DBG("[BAT] V=%d src=%c adc=%d usb=%d\n",
            (int)(v * 1000.0f), s_battSrc, s_adcProbeMv, usbPowerPresent ? 1 : 0);
      }
    }
#endif

    int pct = batteryDisplayPercent(v);
    if (pct < 0) pct = 0;                          // 与 drawStatusBar 同一套口径
    const int wifi  = WifiScan::getLastCount();
    const int ble   = BleScan::getLastCount();
    const int wifiH = wifi / 2;
    const int bleH  = ble / 2;
    const int tBand = statusBarTempBand(readInternalTemperature());
    const int sdSn  = sdCardPresent ? 1 : 0;
    const bool ward = GpsWardriver::statusBarGpsIconActive();
    const uint32_t wPh = ward ? (millis() / kStatusBarWardBlinkHalfMs) : 0u;

    bool need = false;
    if (statusBarBatteryMeaningfulChange(pct, prevBattPct, false)) {
      need = true;
    }
    if (wifiH != prevWifiHalf || bleH != prevBleHalf) {
      need = true;
    }
    if (tBand != prevTempBand || sdSn != prevSdSnap) {
      need = true;
    }
    if (ward != prevWardIcon) {
      need = true;
    }
    if (ward && wPh != prevWardPhase) {
      need = true;
    }
    if (batteryIsCharging != prevCharging) {
      need = true;
    }
    if (usbPowerPresent != prevUsb) {
      need = true;   // 插拔 USB → 立刻重绘，黄色插头跟着出现/消失
    }

    if (need) {
      statusBarDirty = true;
      prevBattPct     = pct;
      prevWifiHalf    = wifiH;
      prevBleHalf     = bleH;
      prevTempBand    = tBand;
      prevSdSnap      = sdSn;
      prevWardIcon    = ward;
      prevWardPhase   = ward ? wPh : 0u;
      prevCharging    = batteryIsCharging;
      prevUsb         = usbPowerPresent;
    }

#if DIAG_PRINTS_ENABLED
    // 现场核对 IO39 极性的最直接手段：插拔 USB 看 raw 变不变、present 跟不跟得上。
    {
      static uint32_t s_lastUsbDiag = 0;
      if (millis() - s_lastUsbDiag > 3000) {
        s_lastUsbDiag = millis();
#if USB_DETECT_PIN >= 0
        DBG("[USB] IO%d raw=%d present=%d (active_%s)\n",
                      USB_DETECT_PIN, (int)digitalRead(USB_DETECT_PIN), (int)usbPowerPresent,
#if USB_DETECT_ACTIVE_HIGH
                      "high");
#else
                      "low");
#endif
#else
        DBGln("[USB] insert detect disabled on this board");
#endif
      }
    }
#endif

    vTaskDelay(pdMS_TO_TICKS(400));
  }
}

void startStatusBarTask() {
  if (statusBarTaskHandle != nullptr) return;
  xTaskCreatePinnedToCore(
    statusBarTask,
    "statusBar",
    4096,   // 2048 会爆栈：诊断 printf 浮点格式化（dtoa）需要约 1.5KB + 常规绘制开销
    nullptr,
    1,
    &statusBarTaskHandle,
    0
  );
}

void pauseBackgroundRadioTasks() {
  GpsWardriver::stopBackgroundIfRunning();
}

void updateStatusBar() {

  // 息屏期间不做状态栏刷新（见 drawStatusBar 顶部注释）
  if (screenIsOff()) return;

  if (statusBarTaskHandle != nullptr) {
    if (statusBarDirty) {
      drawStatusBar(currentBatteryVoltage, false);
      statusBarDirty = false;
    }
    return;
  }

  const unsigned long currentMillis = millis();
  const bool wardOn = GpsWardriver::statusBarGpsIconActive();
  static uint32_t s_noTaskLastWardPhase = UINT32_MAX;
  const uint32_t wardPh =
      wardOn ? (currentMillis / kStatusBarWardBlinkHalfMs) : 0u;
  const bool wardBlinkTick =
      wardOn && (s_noTaskLastWardPhase == UINT32_MAX || wardPh != s_noTaskLastWardPhase);
  if (wardOn) {
    s_noTaskLastWardPhase = wardPh;
  } else {
    s_noTaskLastWardPhase = UINT32_MAX;
  }

  if (currentMillis - lastStatusBarUpdate > STATUS_BAR_UPDATE_INTERVAL || wardBlinkTick) {
    float batteryVoltage = readBatteryVoltage();
    updateSdCardStatus();
    currentBatteryVoltage = batteryVoltage;
    drawStatusBar(batteryVoltage, false);
    lastBatteryVoltage = batteryVoltage;
    lastStatusBarUpdate = currentMillis;
  }
}

/* ── I2C 总线锁死恢复（bus recovery）─────────────────────────────────────
 * I2C 有一个不对称的坑：**从机拉住 SDA 不放时，主机复位解不开它。**
 * 从机在传输中途被打断（时钟被干扰、上电时序错、芯片半死）后会停在
 * "还剩几位没吐完"的状态继续拉低 SDA；此后主机发不出 START，每次事务
 * 只能吃满超时。整机表现：越来越慢 → UI 卡死，而且**按复位键重启无效**，
 * 必须给整个板子断电（从机掉电才会松手）。
 * 标准解法（本函数）：把 SCL 当普通 GPIO 打 9 个时钟，让从机把剩余位吐完
 * 进入空闲；再在 SCL 为高时做一个 SDA 低→高的 STOP。
 * 返回 true = 当时确实检测到 SDA 被拉低并执行了恢复。 */
bool i2cBusRecover() {
#if I2C_BUS_RECOVER
  pinMode(I2C_SDA_PIN, INPUT_PULLUP);
  pinMode(I2C_SCL_PIN, INPUT_PULLUP);
  delayMicroseconds(10);

  // ⚠ 判据必须**两条线都查**：只有 SDA/SCL 都为高才是真正的空闲。
  // 最初只查了 SDA —— 结果漏掉了最典型的一种锁死：从机 clock stretch
  // 时把 **SCL** 拉住不放（PN532 就是这种）。那种情况下 SDA 仍是高，
  // 旧判据直接 return false、根本不做恢复 ⇒ 用户只能断电。
  const bool sdaLow = (digitalRead(I2C_SDA_PIN) == LOW);
  const bool sclLow = (digitalRead(I2C_SCL_PIN) == LOW);
  if (!sdaLow && !sclLow) {
    return false;                       // 两条线都高 ⇒ 总线空闲，别去动它
  }

  pinMode(I2C_SCL_PIN, OUTPUT);
  digitalWrite(I2C_SCL_PIN, HIGH);
  for (int i = 0; i < 9; i++) {         // 9 个时钟：足够吐完一个字节 + ACK 位
    digitalWrite(I2C_SCL_PIN, LOW);
    delayMicroseconds(5);
    digitalWrite(I2C_SCL_PIN, HIGH);
    delayMicroseconds(5);
  }

  pinMode(I2C_SDA_PIN, OUTPUT);         // STOP：SCL 高电平期间 SDA 由低变高
  digitalWrite(I2C_SDA_PIN, LOW);
  delayMicroseconds(5);
  digitalWrite(I2C_SCL_PIN, HIGH);
  delayMicroseconds(5);
  digitalWrite(I2C_SDA_PIN, HIGH);
  delayMicroseconds(5);

  pinMode(I2C_SDA_PIN, INPUT_PULLUP);   // 交还给 I2C 外设前先放成开漏空闲态
  pinMode(I2C_SCL_PIN, INPUT_PULLUP);
  delayMicroseconds(10);
  return true;
#else
  return false;
#endif
}

#if HAS_PCF8574_BUTTONS
static uint8_t s_pcf8574Addr = 0;

uint8_t getPcf8574Address() {
  return s_pcf8574Addr;
}

bool initPcf8574Buttons() {
  // Prevent I2C bus hangs (no ACK / missing pull-ups) from tripping the task WDT
  // and rebooting right after the intro on classic ESP32.
  Wire.begin();                          // ESP32-S3 变体默认 SDA=GPIO8 / SCL=GPIO9
  Wire.setClock(I2C_BUS_CLOCK_HZ);       // 外接模块走长线时可降到 50k，见 shared.h
  Wire.setTimeOut(I2C_BUS_TIMEOUT_MS);

  pcf.pinMode(BTN_UP, INPUT_PULLUP);
  pcf.pinMode(BTN_DOWN, INPUT_PULLUP);
  pcf.pinMode(BTN_LEFT, INPUT_PULLUP);
  pcf.pinMode(BTN_RIGHT, INPUT_PULLUP);
  pcf.pinMode(BTN_SELECT, INPUT_PULLUP);

#if PCF8574_AUTO_DETECT
  for (uint8_t addr = PCF8574_ADDR_MIN; addr <= PCF8574_ADDR_MAX; addr++) {
    yield();
    if (pcf.begin(addr)) {
      s_pcf8574Addr = addr;
      Serial.printf("[PCF8574] auto-detected at 0x%02X\n", addr);
      break;
    }
  }
  if (s_pcf8574Addr == 0) {
    Serial.println("[PCF8574] not found (scanned 0x20-0x27)");
    return false;
  }
#else
  if (!pcf.begin(PCF8574_I2C_ADDR)) {
    Serial.printf("[PCF8574] not found at fixed address 0x%02X\n", PCF8574_I2C_ADDR);
    return false;
  }
  s_pcf8574Addr = PCF8574_I2C_ADDR;
  Serial.printf("[PCF8574] using fixed address 0x%02X\n", s_pcf8574Addr);
#endif

  return true;
}

/* ── 启动 I2C 体检 ────────────────────────────────────────────────────────
 * 起因：接上 PN532 之后"上电按键全按不动，但屏幕看不出任何异常"。
 * 那种情况下唯一能判断的入口就是这里 —— 把总线上到底有什么、键盘芯片
 * 能不能真读出数据，直接打在屏幕上。
 * 注意它读的是**原始字节**而不是只做 ACK：PCF8574 库在 I2C 读失败时会
 * 保持上一次的缓冲值，digitalRead() 于是返回 LOW（= 判定为"按下"）。
 * 所以"能 ACK"和"能正确读数据"是两件事，必须分开测。 */
static bool bootI2cAck(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

/** 读一个字节。返回 -1 = 事务失败（**不是** 0x00，两者含义完全相反）。 */
static int bootI2cPeek(uint8_t addr) {
  if (!bootI2cAck(addr)) {
    return -1;
  }
  if (Wire.requestFrom((uint8_t)addr, (uint8_t)1) != 1) {
    return -1;
  }
  if (Wire.available() <= 0) {
    return -1;
  }
  return (int)Wire.read();
}

void bootI2cSelftest() {
#if BOOT_I2C_SELFTEST
  Wire.begin();                        // 可重入：已初始化时只打印一条 warning
  Wire.setClock(I2C_BUS_CLOCK_HZ);
  Wire.setTimeOut(I2C_BUS_TIMEOUT_MS);
  delay(2);

  const bool sdaIdle = (digitalRead(I2C_SDA_PIN) == HIGH);
  const bool sclIdle = (digitalRead(I2C_SCL_PIN) == HIGH);

  // ① 先只问板上**常驻**器件（键盘）——它在 ⇒ 总线可用。
  //    这一步同时是**闸门**：总线坏的时候绝不做全扫。117 个地址挨个吃满
  //    50ms 超时 ≈ 5.9s 纯黑屏，外加下面的停留时间，会被当成"死机"。
  //    这个坑真踩过（用户反馈："卡死之后重启，屏幕也卡住"）。
  char   ackLine[96] = "";
  size_t used = 0;
  int    ackN = 0;
  bool   hostOk    = bootI2cAck(0x20);
  bool   recovered = false;

  // ② 空闲态 SDA **或 SCL** 被拉低 = 从机把总线锁住了 → 打时钟把它逼放。
  //    两条线都要查：PN532 卡住时拉低的往往是 SCL（clock stretch）。
  //    这是"不用拔电源就能恢复"的唯一软件手段。
  if (!hostOk || !sdaIdle || !sclIdle) {
    recovered = i2cBusRecover();
    hostOk    = bootI2cAck(0x20);
  }

  if (hostOk) {
    for (uint8_t a = 0x03; a <= 0x77; a++) {
      if (bootI2cAck(a)) {
        ackN++;
        if (used + 4 < sizeof(ackLine)) {
          const int w = snprintf(ackLine + used, sizeof(ackLine) - used, "%02X ", a);
          if (w > 0) {
            used += (size_t)w;
          }
        }
      }
      yield();
    }
  }

  const uint8_t pcfAddr = getPcf8574Address();
  const int     raw     = pcfAddr ? bootI2cPeek(pcfAddr) : -1;
  const bool    keysOk  = (pcfAddr != 0 && raw > 0);

  char l2[64];
  char l3[64];
  if (hostOk) {
    snprintf(l2, sizeof(l2), "SDA=%d SCL=%d  found(%d): %s",
             sdaIdle ? 1 : 0, sclIdle ? 1 : 0, ackN, ackN ? ackLine : "-");
  } else {
    // 总线坏 → 全扫已跳过（否则要黑屏近 6 秒）
    snprintf(l2, sizeof(l2), "SDA=%d SCL=%d  BUS STUCK - scan skipped",
             sdaIdle ? 1 : 0, sclIdle ? 1 : 0);
  }

  if (pcfAddr == 0) {
    // 探测阶段就失败 ⇒ isPhysicalButtonPressed() 会直接 return false，五个键全废
    snprintf(l3, sizeof(l3), "KEYPAD 0x20-27 ABSENT  >> KEYS DEAD");
  } else if (raw < 0) {
    snprintf(l3, sizeof(l3), "KEYPAD 0x%02X ACK ok  but READ FAIL", pcfAddr);
  } else if (raw == 0x00) {
    // 空闲态 P3..P7 全为 0 只可能是"5 个键同时按下"或总线被拖 —— 后者远可能
    snprintf(l3, sizeof(l3), "KEYPAD 0x%02X raw=00 STUCK (bus pulled?)", pcfAddr);
  } else {
    snprintf(l3, sizeof(l3), "KEYPAD 0x%02X raw=%02X idle ok", pcfAddr, (unsigned)raw);
  }

  // ── 一切正常就不打扰 ──
  // 正常路径下这两行没有信息量，却要让每次开机多等 2.5 秒。
  // 想看就把 shared.h 的 BOOT_I2C_SELFTEST 改成 2（每次都显示）。
  const bool abnormal = recovered || !hostOk || !keysOk;
#if BOOT_I2C_SELFTEST != 2
  if (!abnormal) {
#if DIAG_PRINTS_ENABLED
    DBG("[I2C] ok  found: %s\n", ackN ? ackLine : "-");
    DBG("[I2C] %s\n", l3);
#endif
    return;
  }
#endif

  tft.fillRect(0, 0, tft.width(), 46, TFT_BLACK);
  tft.setTextFont(1);
  tft.setTextSize(1);
  tft.setTextColor(recovered ? TFT_YELLOW : TFT_CYAN, TFT_BLACK);
  tft.setCursor(4, 4);
  tft.print(recovered ? "Boot I2C: bus was STUCK -> recovered"
                      : "Boot I2C self-test  (SDA8/SCL9)");
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(4, 18);
  tft.print(l2);
  tft.setTextColor(keysOk ? TFT_GREEN : TFT_RED, TFT_BLACK);
  tft.setCursor(4, 32);
  tft.print(l3);

#if DIAG_PRINTS_ENABLED
  DBG("[I2C] sdaIdle=%d sclIdle=%d recovered=%d\n",
                sdaIdle ? 1 : 0, sclIdle ? 1 : 0, recovered ? 1 : 0);
  DBG("[I2C] scan: %s\n", ackN ? ackLine : "(skipped or none)");
  DBG("[I2C] %s\n", l3);
#endif

  // 走到这里说明是异常（正常路径已在上面提前返回），多停一会儿 ——
  // 此时这两行是唯一线索来源。但也别太久，否则会被误读成"死机"。
  delay(abnormal ? 4000 : 2500);
#endif
}

#else
uint8_t getPcf8574Address() {
  return 0;
}

bool initPcf8574Buttons() {
  return false;
}
/* 无 PCF8574 的板（CYD）：体检没有意义，给个空实现让 setup 的调用点保持单一。 */
void bootI2cSelftest() {}
#endif

/* ── 上电释放 PN532 复位（RSTPDN）────────────────────────────────────────
 * 现场（一次真实的"按键全废"）：
 *   本固件**从不初始化 GPIO1**（全工程零 pinMode(1,...)），而 PN532 模块的
 *   RSTPDN 就接在 GPIO1 上。于是从上电到第一次进 RFID 功能之间，这个脚一直是
 *   **高阻输入** —— 对 PN532 来说等同于 RSTPDN 悬空，芯片停在复位态。
 *   复位态的 PN532 会扰动 I2C 总线，导致紧随其后的 initPcf8574Buttons()
 *   地址探测失败（s_pcf8574Addr 保持 0）；而 isPhysicalButtonPressed() 开头就是
 *     if (getPcf8574Address() != 0) { ... }
 *     return false;
 *   ⇒ **五个按键全部返回"未按下"，屏幕上却看不出任何异常。**
 *   进一次 RFID 功能时 hardwareReset() 会 pulse 这个脚、把芯片放出来，
 *   总线随即恢复正常 —— 这就是"进过 RFID 之后按键又能用了"的原因。
 * 修法：别让芯片吊在复位态，setup 一上来就把它放出来。
 * （WS2812 也挂在 GPIO1：长期高电平不点灯；只有进 RFID 那一次 LOW 脉冲
 *   会让它收到一个"复位"位型，同样不发光，安全。） */
void bootReleasePn532Reset() {
#if BOOT_RELEASE_PN532 && (PN532_I2C_RST >= 0)
  pinMode(PN532_I2C_RST, OUTPUT);
  digitalWrite(PN532_I2C_RST, HIGH);
  delay(5);                      // 给芯片启动时间，再让后面的 I2C 探测去碰它
#if DIAG_PRINTS_ENABLED
  Serial.printf("[boot] PN532 RSTPDN released (GPIO%d = HIGH)\n", PN532_I2C_RST);
#endif
#endif
}

#if TOUCH_SHARES_TFT_SPI
#if defined(CONFIG_IDF_TARGET_ESP32S3)
static SPIClass s_sdSpi(FSPI);
#elif defined(CONFIG_IDF_TARGET_ESP32)
static SPIClass s_sdSpi(VSPI);
#else
static SPIClass s_sdSpi(FSPI);
#endif
#endif

/** Tracks whether SD.begin() currently owns a live mount on the shared SPI bus. */
static bool s_sdFsMounted = false;

/** GPIO CS that last mounted SD (-1 = unknown). Tried first to avoid long SD.begin on wrong CS. */
static int8_t s_sdLastGoodCs = -1;

/** After a failed mount on classic ESP32, do not keep retrying (SD.begin can WDT). */
static bool s_sdMountGaveUp = false;

void sdRetryMount() {
  s_sdMountGaveUp = false;
}

/** Deselect every other SPI slave that shares the SD bus so none holds MISO. */
static void sdRaiseCsPin(int pin) {
  if (pin < 0) {
    return;
  }
  pinMode(pin, OUTPUT);
  digitalWrite(pin, HIGH);
}

static void sdReleaseOtherChipSelects() {
#if defined(CC1101_CS)
  sdRaiseCsPin(CC1101_CS);
#endif
#if defined(PN532_SS) && !RFID_USE_I2C
  // 仅 SPI 模式的 PN532 有独立片选要抬高。
  // I2C 模式下 PN532_SS(5) 其实就是 CC1101_CS，上面已处理，无需重复。
  sdRaiseCsPin(PN532_SS);
#endif
#if defined(CSN_PIN_1)
  sdRaiseCsPin(CSN_PIN_1);
#endif
#if defined(CSN_PIN_2)
  sdRaiseCsPin(CSN_PIN_2);
#endif
#if defined(CSN_PIN_3)
  sdRaiseCsPin(CSN_PIN_3);
#endif
  // Scanner bit-bangs CE/CSN on these pins; keep CE low / CSN high after leaving.
#if defined(CE_PIN_3)
  pinMode(CE_PIN_3, OUTPUT);
  digitalWrite(CE_PIN_3, LOW);
#endif
}

void sdSpiInit() {
#if defined(SD_SCLK) && defined(SD_MISO) && defined(SD_MOSI) && defined(SD_CS)
#if TOUCH_SHARES_TFT_SPI
  s_sdSpi.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
#else
  // On ESP32-S3 (v2), RFID bitbang remaps these pins — reset before reclaim.
  // On classic ESP32 (v1), SD often shares SPI with TFT_eSPI; gpio_reset_pin
  // after tft.init() can WDT/reboot during boot SD mount.
#if BOARD_HAS_ESP32S3
  gpio_reset_pin((gpio_num_t)SD_SCLK);
  gpio_reset_pin((gpio_num_t)SD_MISO);
  gpio_reset_pin((gpio_num_t)SD_MOSI);
  gpio_reset_pin((gpio_num_t)SD_CS);
#endif
  SPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
  SPI.setDataMode(SPI_MODE0);
  SPI.setBitOrder(MSBFIRST);
  SPI.setFrequency(4000000);
#endif
#endif
}

bool sdMountChipSelect(uint8_t cs) {
#if TOUCH_SHARES_TFT_SPI
  return SD.begin(cs, s_sdSpi, 4000000);
#else
  return SD.begin(cs, SPI, 4000000);
#endif
}

void initSDCard() {

#ifdef SD_CD
  pinMode(SD_CD, INPUT_PULLUP);
#endif

#if !BOARD_HAS_ESP32S3
  // Classic ESP32: raise CS only. Full SD.begin is deferred — boot mount was
  // rebooting right after the intro ("3 sd").
#if defined(SD_CS)
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
#endif
  sdReleaseOtherChipSelects();
  // Block auto-mount from settingsLoad / status bar until an SD feature asks.
  s_sdMountGaveUp = true;
  updateSdCardStatus();
  return;
#else
  restoreSdAfterSharedSpi();
  updateSdCardStatus();
#endif
}

static bool sdPinIsOkForSdMount(uint8_t pin) {
#if defined(SD_CS)
  if (pin == SD_CS) {
    return true;
  }
#endif
#if defined(SD_CS_PIN)
#if defined(CC1101_CS)
  if (SD_CS_PIN != CC1101_CS && pin == SD_CS_PIN) {
    return true;
  }
#else
  if (pin == SD_CS_PIN) {
    return true;
  }
#endif
#endif
  return false;
}

static void sdAddPinUnique(uint8_t* pins, int* nTry, uint8_t p) {
  if (!sdPinIsOkForSdMount(p)) {
    return;
  }
  for (int j = 0; j < *nTry; j++) {
    if (pins[j] == p) {
      return;
    }
  }
  if (*nTry < 3) {
    pins[(*nTry)++] = p;
  }
}

/** SPI must already be SD wiring. Returns true if SD.begin succeeded. */
static bool sdTryBeginOrder() {
  uint8_t tryPins[3];
  int nTry = 0;
  if (s_sdLastGoodCs >= 0) {
    sdAddPinUnique(tryPins, &nTry, (uint8_t)s_sdLastGoodCs);
  }
#if defined(SD_CS)
  sdAddPinUnique(tryPins, &nTry, SD_CS);
#endif
#if defined(SD_CS_PIN)
#if defined(CC1101_CS)
  if (SD_CS_PIN != CC1101_CS) {
    sdAddPinUnique(tryPins, &nTry, SD_CS_PIN);
  }
#else
  sdAddPinUnique(tryPins, &nTry, SD_CS_PIN);
#endif
#endif
  for (int i = 0; i < nTry; i++) {
    if (sdMountChipSelect(tryPins[i])) {
      s_sdLastGoodCs = (int8_t)tryPins[i];
      return true;
    }
  }
  s_sdLastGoodCs = -1;
  return false;
}

/** Soft remount: keep current SPI pinmux. Safe while CC1101/nRF24 still need the bus
 *  (same SCK/MISO/MOSI as SD on DIV V2). Only raises other chip-selects and remounts FatFS. */
static bool sdRemountSoft() {
  sdReleaseOtherChipSelects();

#if defined(SD_SCLK) && defined(SD_MISO) && defined(SD_MOSI) && defined(SD_CS)
#if TOUCH_SHARES_TFT_SPI
  s_sdSpi.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
#else
  // Do not SPI.end()/gpio_reset here — that tears down CC1101 after SubGHz Init.
  SPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
  SPI.setDataMode(SPI_MODE0);
  SPI.setBitOrder(MSBFIRST);
#endif
#endif

  SD.end();
  s_sdFsMounted = false;
  delay(2);
  if (sdTryBeginOrder()) {
    s_sdFsMounted = true;
#if !TOUCH_SHARES_TFT_SPI
    // SD.begin() often leaves the bus at 16–40 MHz; CC1101 cannot use that.
    SPI.setFrequency(4000000);
#endif
    return true;
  }
  return false;
}

bool isSDCardAvailable() {
#ifdef SD_CD
  updateSdCardStatus();
  if (!sdCardPresent) {
    return false;
  }
#endif

  // FatFS "exists(/)" can stay true after another feature stole the SPI bus.
  // cardType() actually talks to the card and catches a dead mount.
  if (s_sdFsMounted) {
    if (SD.cardType() != CARD_NONE) {
      return true;
    }
    s_sdFsMounted = false;
    SD.end();
  }

#if !BOARD_HAS_ESP32S3
  if (s_sdMountGaveUp) {
    return false;
  }
#endif

  // Prefer a soft remount so SubGHz (CC1101 on the same SPI pins) stays alive.
  if (sdRemountSoft()) {
    return true;
  }

  // Soft failed — pins may still be in RFID bitbang / Scanner remapped state.
  // Only do the destructive reclaim when no radio feature owns the bus.
  if (!feature_active) {
    restoreSdAfterSharedSpi();
#if !BOARD_HAS_ESP32S3
    if (!s_sdFsMounted) {
      s_sdMountGaveUp = true;
      Serial.println("[sd] mount failed — will not retry until reboot");
    }
#endif
    return s_sdFsMounted;
  }
  return false;
}

void holdSdInactiveOnSharedSpi() {
#if defined(SD_CS)
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
#endif
#if !TOUCH_SHARES_TFT_SPI
  SPI.setDataMode(SPI_MODE0);
  SPI.setBitOrder(MSBFIRST);
  SPI.setFrequency(4000000);
#endif
}

void reclaimSharedSpiBus() {
  // Reset pinmux after RFID bitbang / Scanner remaps, but do NOT SD.begin().
  // Mounting SD here is what broke SubGHz: SD.begin raises SPI clock and parks
  // the card on the same MISO line CC1101 needs.
  sdReleaseOtherChipSelects();
#if defined(SD_CS)
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
#endif
#if defined(CC1101_CS)
  pinMode(CC1101_CS, OUTPUT);
  digitalWrite(CC1101_CS, HIGH);
#endif
#if defined(PN532_SS) && !RFID_USE_I2C
  pinMode(PN532_SS, OUTPUT);
  digitalWrite(PN532_SS, HIGH);
#endif

  SD.end();
  s_sdFsMounted = false;

#if !TOUCH_SHARES_TFT_SPI
  SPI.end();
#if BOARD_HAS_ESP32S3
#if defined(SD_SCLK) && defined(SD_MISO) && defined(SD_MOSI)
  gpio_reset_pin((gpio_num_t)SD_SCLK);
  gpio_reset_pin((gpio_num_t)SD_MISO);
  gpio_reset_pin((gpio_num_t)SD_MOSI);
#endif
#if !RFID_USE_I2C
  // PN532 software-SPI may have bitbanged these (on DIV V2 MOSI/MISO are
  // swapped vs SD/CC1101). Reset them even when they overlap SD pins.
  // I2C 模式下 PN532 不碰这些脚，整段跳过；且 5 号脚属 CC1101，下面会单独处理。
#if defined(PN532_SCK)
  gpio_reset_pin((gpio_num_t)PN532_SCK);
#endif
#if defined(PN532_MISO)
  gpio_reset_pin((gpio_num_t)PN532_MISO);
#endif
#if defined(PN532_MOSI)
  gpio_reset_pin((gpio_num_t)PN532_MOSI);
#endif
#if defined(PN532_SS)
  gpio_reset_pin((gpio_num_t)PN532_SS);
  pinMode(PN532_SS, OUTPUT);
  digitalWrite(PN532_SS, HIGH);
#endif
#endif // !RFID_USE_I2C
#if defined(SD_CS)
  gpio_reset_pin((gpio_num_t)SD_CS);
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
#endif
#if defined(CC1101_CS)
  gpio_reset_pin((gpio_num_t)CC1101_CS);
  pinMode(CC1101_CS, OUTPUT);
  digitalWrite(CC1101_CS, HIGH);
#endif
#endif // BOARD_HAS_ESP32S3
#if defined(SD_SCLK) && defined(SD_MISO) && defined(SD_MOSI) && defined(SD_CS)
#if defined(CC1101_SCK) && defined(CC1101_MISO) && defined(CC1101_MOSI) && defined(CC1101_CS)
  // Prefer CC1101 CS as SPI SS — same data pins as SD on DIV V2, but matches
  // what ELECHOUSE SpiStart() will bind on the next Init().
  SPI.begin(CC1101_SCK, CC1101_MISO, CC1101_MOSI, CC1101_CS);
#else
  SPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
#endif
  SPI.setDataMode(SPI_MODE0);
  SPI.setBitOrder(MSBFIRST);
  SPI.setFrequency(4000000);
#endif
#else
#if defined(SD_SCLK) && defined(SD_MISO) && defined(SD_MOSI) && defined(SD_CS)
  s_sdSpi.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
#endif
#endif
  delay(2);
}

void restoreSdAfterSharedSpi() {
  // Full reclaim + remount for menu/SD features after leaving SPI radios.
  reclaimSharedSpiBus();
  delay(5);
  if (sdTryBeginOrder()) {
    s_sdFsMounted = true;
#if !TOUCH_SHARES_TFT_SPI
    SPI.setFrequency(4000000);
#endif
  }
  requestStatusBarRedraw();
}

void loading(int frameDelay, uint16_t color, int16_t x, int16_t y, int repeats, bool center) {
  int16_t bitmapWidth = 100;
  int16_t bitmapHeight = 120;
  int16_t logoX = x;
  int16_t logoY = y;

  if (center) {
    int16_t screenWidth = tft.width();
    int16_t screenHeight = tft.height();
    logoX = (screenWidth - bitmapWidth) / 2;
    logoY = (screenHeight - bitmapHeight) / 2;
  }

  const unsigned char* bitmaps[] = {
    bitmap_icon_skull_loading_1,
    bitmap_icon_skull_loading_2,
    bitmap_icon_skull_loading_3,
    bitmap_icon_skull_loading_4,
    bitmap_icon_skull_loading_5,
    bitmap_icon_skull_loading_6,
    bitmap_icon_skull_loading_7,
    bitmap_icon_skull_loading_8,
    bitmap_icon_skull_loading_9,
    bitmap_icon_skull_loading_10
  };
  const int numFrames = 10;

  for (int r = 0; r < repeats; r++) {
    for (int i = 0; i < numFrames; i++) {
      tft.fillRect(logoX, logoY, bitmapWidth, bitmapHeight, TFT_BLACK);
      tft.drawBitmap(logoX, logoY, bitmaps[i], bitmapWidth, bitmapHeight, color);
      delay(frameDelay);
    }
  }
}

void displayLogo(uint16_t color, int displayTime) {
  int16_t bitmapWidth = 150;
  int16_t bitmapHeight = 150;
  int16_t screenWidth = tft.width();
  int16_t screenHeight = tft.height();
  int16_t logoX = (screenWidth - bitmapWidth) / 2;
  int16_t logoY = (screenHeight - bitmapHeight) / 2 - 20;

  tft.fillRect(logoX, logoY, bitmapWidth, bitmapHeight, TFT_BLACK);
  tft.drawBitmap(logoX, logoY, bitmap_icon_cifer, bitmapWidth, bitmapHeight, color);

  uiSetTextColor(color);
  uiSetTextFont(1);

  uiSetTextSize(2);
  int16_t textX = screenWidth / 3.5;
  int16_t textY = logoY + bitmapHeight + 10;
  uiSetCursor(textX, textY);
  tftPrintObf(OBF_PN, sizeof(OBF_PN));

  uiSetTextSize(1);
  textX = screenWidth / 3.5;
  textY += 20;
  uiSetCursor(textX, textY);
  uiPrint("by ");
  tftPrintObf(OBF_DN, sizeof(OBF_DN));

  textX = screenWidth / 2.5;
  textY += 50;
  uiSetCursor(textX, textY);
  // Version is intentionally NOT obfuscated.
  uiPrint(ESP32DIV_VERSION);

  Serial.println("==================================");
  serialPrintObf(OBF_PN, sizeof(OBF_PN), true);
  Serial.print("Developed by: "); serialPrintObf(OBF_DN, sizeof(OBF_DN), true);
  // Version is intentionally NOT obfuscated.
  Serial.print("Version:      "); Serial.println(ESP32DIV_VERSION);
  // 2026-10-10：Contact/GitHub 已按需求移除（OBF_EM/OBF_GH 定义已删）。
  Serial.print("Website:      "); serialPrintObf(OBF_WB, sizeof(OBF_WB), true);
  Serial.println("==================================");

  delay(displayTime);
}

namespace Terminal {

#define TEXT_HEIGHT 16
#define BOT_FIXED_AREA 0
#define TOP_FIXED_AREA 86
#define DISPLAY_WIDTH 240
#define DISPLAY_HEIGHT 320
#define SCREEN_WIDTH 240
#define SCREENHEIGHT 320

static bool uiDrawn = false;

uint16_t yStart = TOP_FIXED_AREA;
uint16_t yArea = DISPLAY_HEIGHT - TOP_FIXED_AREA - BOT_FIXED_AREA;
uint16_t yDraw = DISPLAY_HEIGHT - BOT_FIXED_AREA - TEXT_HEIGHT;

uint16_t xPos = 0;

byte data = 0;

boolean change_colour = 1;
boolean selected = 1;
boolean terminalActive = true;

int blank[19];

long baudRates[] = {9600, 19200, 38400, 57600, 115200};
byte baudIndex = 0;

static int terminalContentBottom() {
  return featureHasTouchNavBar() ? (int)touchNavContentBottomY() : DISPLAY_HEIGHT;
}

static int terminalBotFixedArea() {
  const int bottom = terminalContentBottom();
  return (bottom < DISPLAY_HEIGHT) ? (DISPLAY_HEIGHT - bottom) : BOT_FIXED_AREA;
}

static void terminalUpdateNavLabels() {
  if (!featureHasTouchNavBar()) {
    return;
  }
  setTouchNavLabels("Exit", nullptr, terminalActive ? "Pause" : "Active", nullptr, "Baud");
  redrawTouchButtonBar();
}

static void terminalCycleBaud() {
  if (terminalActive) {
    terminalActive = false;
    terminalUpdateNavLabels();
    return;
  }
  baudIndex = (baudIndex + 1) % 5;
  Serial.end();
  delay(100);
  Serial.begin(baudRates[baudIndex]);
  tft.fillRect(0, 37, DISPLAY_WIDTH, 16, ORANGE);
  uiSetTextColor(TFT_WHITE, TFT_WHITE);
  String baudMsg = " Serial Terminal - " + String(baudRates[baudIndex]) + " baud ";
  uiDrawCentreString(baudMsg, DISPLAY_WIDTH / 2, 37, 2);
  delay(10);
}

static void terminalSetActive(bool active) {
  terminalActive = active;
  delay(10);
  tft.fillRect(0, 37, DISPLAY_WIDTH, 16, ORANGE);
  uiSetTextColor(TFT_WHITE, TFT_WHITE);
  if (active) {
    uiDrawCentreString(" Serial Terminal Active ", DISPLAY_WIDTH / 2, 37, 2);
  } else {
    String baudMsg = " Serial Terminal - " + String(baudRates[baudIndex]) + " baud ";
    uiDrawCentreString(baudMsg, DISPLAY_WIDTH / 2, 37, 2);
  }
  terminalUpdateNavLabels();
}

static void terminalHandleNavButtons() {
  if (!featureHasTouchNavBar()) {
    return;
  }
  if (isTouchNavButtonPressedEdge(BTN_LEFT)) {
    feature_exit_requested = true;
    return;
  }
  if (isTouchNavButtonPressedEdge(BTN_RIGHT)) {
    terminalCycleBaud();
    return;
  }
  if (isTouchNavButtonPressedEdge(BTN_SELECT)) {
    terminalSetActive(!terminalActive);
  }
}

void runUI() {

    #define STATUS_BAR_Y_OFFSET 20
    #define STATUS_BAR_HEIGHT 16
    #define ICON_SIZE 16
    #define ICON_NUM 3

    static int iconX[ICON_NUM] = {210, 170, 10};
    static int iconY = STATUS_BAR_Y_OFFSET;

    static const unsigned char* icons[ICON_NUM] = {
        bitmap_icon_sort_up_plus,
        bitmap_icon_power,
        bitmap_icon_go_back
    };

    if (!uiDrawn) {
        tft.drawLine(0, 19, 240, 19, TFT_WHITE);
        tft.fillRect(0, STATUS_BAR_Y_OFFSET, SCREEN_WIDTH, STATUS_BAR_HEIGHT, DARK_GRAY);

        for (int i = 0; i < ICON_NUM; i++) {
            if (icons[i] != NULL) {
                tft.drawBitmap(iconX[i], iconY, icons[i], ICON_SIZE, ICON_SIZE, TFT_WHITE);
            }
        }
        tft.drawLine(0, STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT, SCREEN_WIDTH, STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT, ORANGE);
        uiDrawn = true;
    }

    static unsigned long lastAnimationTime = 0;
    static int animationState = 0;
    static int activeIcon = -1;

    if (animationState > 0 && millis() - lastAnimationTime >= 150) {
        if (animationState == 1) {
            tft.drawBitmap(iconX[activeIcon], iconY, icons[activeIcon], ICON_SIZE, ICON_SIZE, TFT_WHITE);
            animationState = 2;

            switch (activeIcon) {
                case 0:
                  if (terminalActive) {
                    terminalActive = false;
                  } else if (!terminalActive) {
                    baudIndex = (baudIndex + 1) % 5;
                    Serial.end();
                    delay(100);
                    Serial.begin(baudRates[baudIndex]);
                    tft.fillRect(0, 37, DISPLAY_WIDTH, 16, ORANGE);
                    uiSetTextColor(TFT_WHITE, TFT_WHITE);
                    String baudMsg = " Serial Terminal - " + String(baudRates[baudIndex]) + " baud ";
                    uiDrawCentreString(baudMsg, DISPLAY_WIDTH / 2, 37, 2);
                    delay(10);
                  }
                    break;
                case 1:
                    delay(10);
                    tft.fillRect(0, 37, DISPLAY_WIDTH, 16, ORANGE);
                    uiSetTextColor(TFT_WHITE, TFT_WHITE);
                    uiDrawCentreString(" Serial Terminal Active ", DISPLAY_WIDTH / 2, 37, 2);
                    terminalActive = true;
                    break;

                case 2:
                    feature_exit_requested = true;
                    break;
            }
        } else if (animationState == 2) {
            animationState = 0;
            activeIcon = -1;
        }
        lastAnimationTime = millis();
    }

    static unsigned long lastTouchCheck = 0;
    const unsigned long touchCheckInterval = 50;

    if (millis() - lastTouchCheck >= touchCheckInterval) {
        int x, y;
        if (!featureHasTouchNavBar() && feature_active && readTouchXY(x, y)) {
            if (y > STATUS_BAR_Y_OFFSET && y < STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT) {
                for (int i = 0; i < ICON_NUM; i++) {
                    if (x > iconX[i] && x < iconX[i] + ICON_SIZE) {
                        if (icons[i] != NULL && animationState == 0) {
                            tft.drawBitmap(iconX[i], iconY, icons[i], ICON_SIZE, ICON_SIZE, TFT_BLACK);
                            animationState = 1;
                            activeIcon = i;
                            lastAnimationTime = millis();
                        }
                        break;
                    }
                }
            }
        }
        lastTouchCheck = millis();
    }
}

void scrollAddress(uint16_t vsp) {
  tft.writecommand(ILI9341_VSCRSADD);
  tft.writedata(vsp >> 8);
  tft.writedata(vsp);
}

int scroll_line() {
  int yTemp = yStart;
  tft.fillRect(0, yStart, blank[(yStart - TOP_FIXED_AREA) / TEXT_HEIGHT], TEXT_HEIGHT, TFT_BLACK);

  yStart += TEXT_HEIGHT;
  const int scrollLimit = DISPLAY_HEIGHT - terminalBotFixedArea();
  if (yStart >= scrollLimit) {
    yStart = TOP_FIXED_AREA + (yStart - scrollLimit);
  }
  scrollAddress(yStart);
  delay(1);
  return yTemp;
}

void setupScrollArea(uint16_t tfa, uint16_t bfa) {
  tft.writecommand(ILI9341_VSCRDEF);
  tft.writedata(tfa >> 8);
  tft.writedata(tfa);
  tft.writedata((DISPLAY_HEIGHT - tfa - bfa) >> 8);
  tft.writedata(DISPLAY_HEIGHT - tfa - bfa);
  tft.writedata(bfa >> 8);
  tft.writedata(bfa);
}

void terminalSetup() {

  setupTouchscreen();
  tft.fillScreen(TFT_BLACK);

  tft.fillRect(0, 37, DISPLAY_WIDTH, 16, ORANGE);
  uiSetTextColor(TFT_WHITE, TFT_WHITE);
  String baudMsg = " Serial Terminal - " + String(baudRates[baudIndex]) + " baud ";
  uiDrawCentreString(baudMsg, DISPLAY_WIDTH / 2, 37, 2);

  float currentBatteryVoltage = readBatteryVoltage();

  drawStatusBar(currentBatteryVoltage, true);

  uiDrawn = false;

  Serial.begin(baudRates[baudIndex]);

  const int bfa = terminalBotFixedArea();
  yArea = DISPLAY_HEIGHT - TOP_FIXED_AREA - bfa;
  yDraw = DISPLAY_HEIGHT - bfa - TEXT_HEIGHT;
  setupScrollArea(TOP_FIXED_AREA, bfa);

  for (byte i = 0; i < 19; i++) blank[i] = 0;

  terminalUpdateNavLabels();
}

void terminalLoop() {

  updateStatusBar();
  if (featureHasTouchNavBar()) {
    maintainTouchNavBar();
  }
  terminalHandleNavButtons();
  runUI();

  if (terminalActive) {
    byte charCount = 0;
    while (Serial.available() && charCount < 10) {
      data = Serial.read();
      if (data == '\r' || xPos > 231) {
        xPos = 0;
        yDraw = scroll_line();
      }
      if (data > 31 && data < 128) {
        xPos += tft.drawChar(data, xPos, yDraw, 2);
        blank[(18 + (yStart - TOP_FIXED_AREA) / TEXT_HEIGHT) % 19] = xPos;
      }
      charCount++;
      }
    }
  }
}

namespace AppSettingsUI {

static const int SCREEN_W = 240;
static const int BAR_H    = 22;
static const int TITLE_Y  = BAR_H + 4;
static const int TITLE_H  = 16;
// 行高 32 → 28：第 7 行（Screen Off）从 y=240 起、止于 268，
// 而底部 Back/Save 按钮从 y=288 开始，不重叠。（7*28 + 6*4 = 220px）
static const int ROW_H    = 28;
static const int GAP_Y    = 4;
static const int PAD_X    = 12;
static const int LABEL_W  = 92;
static const int RADIUS   = 8;

static inline int rowY(int i) { return TITLE_Y + TITLE_H + 6 + i * (ROW_H + GAP_Y); }

struct Rect { int x,y,w,h; };
static inline Rect makeRect(int x,int y,int w,int h){ return {x,y,w,h}; }
static inline void fillRound(const Rect& r, uint16_t c){ tft.fillRoundRect(r.x,r.y,r.w,r.h,RADIUS,c); }
static inline void drawRound(const Rect& r, uint16_t c){ tft.drawRoundRect(r.x,r.y,r.w,r.h,RADIUS,c); }

static uint16_t cardBG, cardEdge, textDim, textStrong, accentSoft, btnBg, btnFg, btnStroke;

static uint16_t blend565(uint16_t a, uint16_t b, uint8_t k ) {
  uint32_t rb = (((a & 0xF81F) * (255-k)) + ((b & 0xF81F) * k)) >> 8;
  uint32_t g  = (((a & 0x07E0) * (255-k)) + ((b & 0x07E0) * k)) >> 8;
  return (rb & 0xF81F) | (g & 0x07E0);
}
static void buildPalette() {

  cardBG     = UI_FG;
  cardEdge   = UI_LINE;
  textDim    = UI_TEXT;
  textStrong = UI_TEXT;
  accentSoft = UI_ICON;

  btnBg      = UI_BG;
  btnFg      = UI_FG;
  btnStroke  = UI_LINE;
}

static int  sel = 0;
static bool dirtySettings = false;
static bool uiDirty = false;

static const char* items[] = {"Brightness", "Theme", "Accent", "Key Beep", "Auto Scan",
                              "Language"};   // 2026-10-09：移除 NeoPixel / Screen Off，新增 Key Beep
static const int N = sizeof(items)/sizeof(items[0]);
static const int ROW_KEY_BEEP = 3;

static uint8_t  last_brightness;
static Theme    last_theme;
static uint8_t  last_accent;
static bool     last_keyBeep;
static bool     last_autoScan;
static bool     last_langZh;
static int      last_sel;

static bool dragging = false;

static uint32_t lastChangeMs = 0;

static Rect rowRect(int i) { return makeRect(PAD_X, rowY(i), SCREEN_W - PAD_X*2, ROW_H); }

static void setTitleFont() { uiSetTextFont(2); }
static void setLabelFont() { uiSetTextFont(2); }

static void drawTitle() {
  setTitleFont();
  uiSetTextColor(textStrong, UI.bg);
  uiSetCursor(PAD_X, TITLE_Y);
  uiPrint("Settings");
}

static void drawCardStatic(int i, bool selected) {
  Rect r = rowRect(i);

  tft.fillRect(0, r.y, SCREEN_W, r.h, UI_BG);

  if (selected) {
    tft.fillRect(0, r.y, 3, r.h, UI.accent);
  }

  setLabelFont();
  uiSetTextColor(textDim, UI_BG);
  int ty = r.y + (r.h/2 - 6);
  uiSetCursor(r.x, ty);
  uiPrint(items[i]);

  tft.drawLine(PAD_X, r.y + r.h - 1, SCREEN_W - PAD_X, r.y + r.h - 1, UI_LINE);
}

static void wipeBrightnessWidgetArea() {
  Rect r = rowRect(0);
  tft.fillRect(r.x + LABEL_W, r.y + 2, r.w - LABEL_W - 6, r.h - 4, UI_BG);
}
static void wipeThemeWidgetArea() {
  Rect r = rowRect(1);
  tft.fillRect(r.x + LABEL_W, r.y + 2, r.w - LABEL_W - 6, r.h - 4, UI_BG);
}
static void wipeAccentWidgetArea() {
  Rect r = rowRect(2);
  tft.fillRect(r.x + LABEL_W, r.y + 2, r.w - LABEL_W - 6, r.h - 4, UI_BG);
}
static void wipeSwitchWidgetArea(int row) {
  Rect r = rowRect(row);
  tft.fillRect(r.x + LABEL_W, r.y + 2, r.w - LABEL_W - 6, r.h - 4, UI_BG);
}

static Rect rBrightTrack(){
  Rect r = rowRect(0);
  int left  = r.x + LABEL_W + 6;
  int right = r.x + r.w - 44;
  int w     = max(70, right - left);
  return makeRect(left, r.y + (r.h/2 - 8), w, 16);
}
static Rect rBrightKnob(uint8_t v) {
  Rect tr = rBrightTrack();
  int d = 18;
  int x = tr.x + (int)((uint32_t)v * (tr.w - d) / 255u);
  int y = tr.y + tr.h/2 - d/2;
  return makeRect(x, y, d, d);
}
static void drawBrightnessWidget(uint8_t v, bool selected) {
  tft.startWrite();
  wipeBrightnessWidgetArea();

  Rect tr = rBrightTrack();

  tft.fillRoundRect(tr.x, tr.y, tr.w, tr.h, tr.h/2, UI_BG);
  tft.drawRoundRect(tr.x, tr.y, tr.w, tr.h, tr.h/2, cardEdge);

  int fw = (int)((uint32_t)v * tr.w / 255u);
  tft.fillRoundRect(tr.x, tr.y, fw+2, tr.h, tr.h/2, UI_ICON);

  Rect kb = rBrightKnob(v);
  tft.fillCircle(kb.x + kb.w/2, kb.y + kb.h/2, kb.w/2, UI_FG);
  tft.drawCircle(kb.x + kb.w/2, kb.y + kb.h/2, kb.w/2, selected ? UI_ICON : UI_FG);

  int bx = tr.x + tr.w + 6, by = tr.y - 2, bw = 34, bh = tr.h + 4;
  tft.fillRoundRect(bx+1, by+1, bw, bh, 4, blend565(UI_BG, UI_FG, 28));
  tft.fillRoundRect(bx,   by,   bw, bh, 4, cardBG);
  tft.drawRoundRect(bx,   by,   bw, bh, 4, cardEdge);
  setLabelFont();
  uiSetTextColor(textStrong, cardBG);
  char buf[8]; snprintf(buf,sizeof(buf),"%3u", v);
  uiSetCursor(bx+5, by+2);
  uiPrint(buf);

  tft.endWrite();
}
static void drawBrightness(uint8_t v, bool selected) {
  drawCardStatic(0, selected);
  drawBrightnessWidget(v, selected);
}

static Rect rThemeDark()  {
  Rect r = rowRect(1);
  int right = r.x + r.w - 6;
  uiSetTextFont(2);
  int wD = (int)uiTextWidth("[Dark]");
  int wL = (int)uiTextWidth("Light");
  int gap = 6;
  return makeRect(right - wD - gap - wL, r.y + 8, wD, r.h - 16);
}
static Rect rThemeLight() {
  Rect r = rowRect(1);
  int right = r.x + r.w - 6;
  uiSetTextFont(2);
  int wL = (int)uiTextWidth("[Light]");
  return makeRect(right - wL, r.y + 8, wL, r.h - 16);
}

static void drawThemeWidget(Theme th, bool ) {

  tft.startWrite();
  wipeThemeWidgetArea();

  Rect r = rowRect(1);
  int right = r.x + r.w - 6;
  int ty    = r.y + (r.h / 2 - 6);

  setLabelFont();

  const char* darkLabel  = (th == Theme::Dark)  ? "[Dark]"  : "Dark";
  const char* lightLabel = (th == Theme::Light) ? "[Light]" : "Light";

  int wD = (int)uiTextWidth(darkLabel);
  int wL = (int)uiTextWidth(lightLabel);
  int gap = 6;

  int lx = right - wL;
  int dx = lx - gap - wD;

  uiSetTextColor(textStrong, UI_BG);
  uiSetCursor(dx, ty);
  uiPrint(darkLabel);
  uiSetCursor(lx, ty);
  uiPrint(lightLabel);

  tft.endWrite();
}
static void drawTheme(Theme th, bool selected) {
  drawCardStatic(1, selected);
  drawThemeWidget(th, selected);
}

static Rect rAccentSwatch() {
  Rect r = rowRect(2);
  int sw = 18;
  int sx = r.x + r.w - sw - 6;
  int sy = r.y + (r.h - sw) / 2;
  return makeRect(sx, sy, sw, sw);
}
static void drawAccentWidget(uint8_t preset, bool ) {
  tft.startWrite();
  wipeAccentWidgetArea();

  Rect r = rowRect(2);
  int right = r.x + r.w - 6;
  int ty = r.y + (r.h / 2 - 6);

  setLabelFont();
  const char* name = accentPresetName(preset);
  int nameW = (int)uiTextWidth(name);
  Rect sw = rAccentSwatch();
  int nameX = sw.x - 8 - nameW;

  uiSetTextColor(textStrong, UI_BG);
  uiSetCursor(nameX, ty);
  uiPrint(name);

  tft.fillRoundRect(sw.x, sw.y, sw.w, sw.h, 4, accentColor565(preset));
  tft.drawRoundRect(sw.x, sw.y, sw.w, sw.h, 4, cardEdge);

  tft.endWrite();
}
static void drawAccent(uint8_t preset, bool selected) {
  drawCardStatic(2, selected);
  drawAccentWidget(preset, selected);
}

static Rect rSwitchTrack(int row){
  Rect r = rowRect(row);
  const int w = 34;
  const int h = 14;
  int x = r.x + r.w - w - 10;
  int y = r.y + (r.h - h) / 2;
  return makeRect(x, y, w, h);
}
static Rect rSwitchKnob(bool on, int row) {
  Rect tr = rSwitchTrack(row);
  int d = tr.h - 4;
  int x = on ? (tr.x + tr.w - d - 2)
             : (tr.x + 2);
  int y = tr.y + (tr.h - d) / 2;
  return makeRect(x, y, d, d);
}
static void drawSwitchWidgetRow(bool on, bool , int row,
                                const char* onLabel = "ON",
                                const char* offLabel = "OFF") {
  tft.startWrite();
  wipeSwitchWidgetArea(row);

  Rect tr = rSwitchTrack(row);

  tft.fillRoundRect(tr.x, tr.y, tr.w, tr.h, tr.h/2, UI_BG);
  tft.drawRoundRect(tr.x, tr.y, tr.w, tr.h, tr.h/2, cardEdge);

  if (on) {
    tft.fillRoundRect(tr.x+1, tr.y+1, tr.w-2, tr.h-2, tr.h/2, UI_ICON);
  }

  Rect kb = rSwitchKnob(on, row);
  uint16_t knobBody  = UI_FG;
  uint16_t knobEdge  = on ? UI.accent : UI_FG;
  tft.fillCircle(kb.x+kb.w/2, kb.y+kb.h/2, kb.w/2, knobBody);
  tft.drawCircle(kb.x+kb.w/2, kb.y+kb.h/2, kb.w/2, knobEdge);

  setLabelFont();
  uiSetTextColor(textStrong, UI_BG);
  int labelX = tr.x - 26;
  int labelY = tr.y + 1;
  uiSetCursor(labelX, labelY);
  uiPrint(on ? onLabel : offLabel);

  tft.endWrite();
}
static void drawSwitchRow(bool on, bool selected, int row) {
  drawCardStatic(row, selected);
  drawSwitchWidgetRow(on, selected, row);
}

static void drawKeyBeepRow(bool on, bool selected) { drawSwitchRow(on, selected, ROW_KEY_BEEP); }
static void drawAutoScan(bool on, bool selected) { drawSwitchRow(on, selected, 4); }

// 语言行：开关形态，右侧标签用 ZH / EN 比 ON / OFF 直观
static void drawLanguageWidget(bool zh, bool selected) {
  drawCardStatic(5, selected);
  drawSwitchWidgetRow(zh, selected, 5, "ZH", "EN");
}
static void drawLanguage(bool zh, bool selected) { drawLanguageWidget(zh, selected); }

static Rect backRect(){
  int h = tft.height();
  int bwTotal = SCREEN_W - PAD_X*2;
  int bh = 24;
  int bx = PAD_X;
  int by = h - bh - 8;
  const int gap = 8;
  int bw = (bwTotal - gap) / 2;
  return makeRect(bx, by, bw, bh);
}
static Rect saveRect(){
  Rect b = backRect();
  const int gap = 8;
  return makeRect(b.x + b.w + gap, b.y, b.w, b.h);
}
static void drawFooterButton(const Rect& b, const char* label, uint16_t body, uint16_t edge){

  FeatureUI::ButtonStyle style =
    (body == UI.accent) ? FeatureUI::ButtonStyle::Primary : FeatureUI::ButtonStyle::Secondary;
  FeatureUI::drawButtonRect(b.x, b.y, b.w, b.h, label, style);
}
static void footerToast(const char* msg, uint16_t color){
  Rect b = backRect();
  int y = b.y - 18;
  int h = 14;
  tft.fillRect(PAD_X, y, SCREEN_W - PAD_X*2, h, UI_BG);
  uiSetTextColor(color, UI_BG);
  uiSetTextFont(1);
  uiSetTextSize(1);
  uiSetCursor(PAD_X, y + 3);
  uiPrint(msg);
}
static void drawFooter(bool backPressed=false, bool savePressed=false){
  Rect b = backRect();
  Rect s = saveRect();

  int clearX = PAD_X - 2;
  int clearW = SCREEN_W - (PAD_X*2) + 4;
  tft.fillRect(clearX, b.y-2, clearW, b.h+4, UI.bg);

  FeatureUI::drawButtonRect(b.x, b.y, b.w, b.h, "Back",
                            backPressed ? FeatureUI::ButtonStyle::Primary : FeatureUI::ButtonStyle::Secondary);

  FeatureUI::drawButtonRect(s.x, s.y, s.w, s.h, "Save",
                            FeatureUI::ButtonStyle::Primary);
}
static bool touchInRect(const Rect& r, int x, int y) {
  return (x>=r.x && x<=r.x+r.w && y>=r.y && y<=r.y+r.h);
}

static void drawAll() {
  tft.fillScreen(UI_BG);
  buildPalette();

  drawStatusBar(currentBatteryVoltage, true);
  setTitleFont();
  drawTitle();

  auto& s = settings();
  drawBrightness(s.brightness, sel==0);
  drawTheme(s.theme, sel==1);
  drawAccent(s.accentColor, sel==2);
  drawKeyBeepRow(s.keyBeepEnabled, sel==ROW_KEY_BEEP);
  bool autoScan = (s.autoWifiScan || s.autoBleScan);
  drawAutoScan(autoScan, sel==4);
  drawLanguageWidget(g_lang == LANG_ZH, sel==5);

  drawFooter(false, false);

  last_sel        = sel;
  last_brightness = s.brightness;
  last_theme      = s.theme;
  last_accent     = s.accentColor;
  last_keyBeep    = s.keyBeepEnabled;
  last_autoScan     = autoScan;
  last_langZh     = (g_lang == LANG_ZH);
  uiDirty = false;
}

static void redrawIfChanged() {
  auto& s = settings();

  // 语言切换：整屏重绘（所有标签都要换成新语言）
  if ((g_lang == LANG_ZH) != last_langZh) {
    drawAll();
    return;
  }

  if (s.theme != last_theme || s.accentColor != last_accent) {
    applyThemeToPalette(s.theme);
    buildPalette();
    drawAll();
    return;
  }

  if (sel != last_sel) {
    drawCardStatic(0, sel==0);  drawBrightnessWidget(s.brightness, sel==0);
    drawCardStatic(1, sel==1);  drawThemeWidget(s.theme, sel==1);
    drawCardStatic(2, sel==2);  drawAccentWidget(s.accentColor, sel==2);
    drawCardStatic(ROW_KEY_BEEP, sel==ROW_KEY_BEEP);  drawSwitchWidgetRow(s.keyBeepEnabled, sel==ROW_KEY_BEEP, ROW_KEY_BEEP);
    bool autoScan = (s.autoWifiScan || s.autoBleScan);
    drawCardStatic(4, sel==4);  drawSwitchWidgetRow(autoScan, sel==4, 4);
    drawCardStatic(5, sel==5);  drawSwitchWidgetRow(g_lang == LANG_ZH, sel==5, 5, "ZH", "EN");
    last_sel = sel;
  } else {
    if (s.brightness != last_brightness) {
      drawBrightnessWidget(s.brightness, sel==0);
      last_brightness = s.brightness;
    }
    if (s.keyBeepEnabled != last_keyBeep) {
      drawSwitchWidgetRow(s.keyBeepEnabled, sel==ROW_KEY_BEEP, ROW_KEY_BEEP);
      last_keyBeep = s.keyBeepEnabled;
    }
    bool autoScan = (s.autoWifiScan || s.autoBleScan);
    if (autoScan != last_autoScan) {
      drawSwitchWidgetRow(autoScan, sel==4, 4);
      last_autoScan = autoScan;
    }
    if (s.theme != last_theme) {
      drawThemeWidget(s.theme, sel==1);
      last_theme = s.theme;
    }
  }

  uiDirty = false;
}

static bool applyBrightness(uint8_t v){
  if (v > 255) v = 255;
  auto& s = settings();
  if (s.brightness == v) return false;
  s.brightness = v;
  ::setBrightness(v);
  dirtySettings = true;
  uiDirty = true;
  lastChangeMs = millis();
  return true;
}
static bool applyTheme(Theme t){
  auto& s = settings();
  if (s.theme == t) return false;
  s.theme = t;
  applyThemeToPalette(t);
  buildPalette();
  dirtySettings = true;
  uiDirty = true;
  lastChangeMs = millis();
  return true;
}
static bool applyAccent(uint8_t preset){
  auto& s = settings();
  preset = accentPresetClamp(preset);
  if (s.accentColor == preset) return false;
  s.accentColor = preset;
  applyThemeToPalette(s.theme);
  buildPalette();
  dirtySettings = true;
  uiDirty = true;
  lastChangeMs = millis();
  return true;
}
// 按键音开关（立即生效；Save 后写入 settings.json）
static bool applyKeyBeep(bool en){
  auto& s = settings();
  if (s.keyBeepEnabled == en) return false;
  s.keyBeepEnabled = en;
  dirtySettings = true;
  uiDirty = true;
  lastChangeMs = millis();
  return true;
}

static bool applyAutoScan(bool en){
  auto& s = settings();
  if (s.autoWifiScan == en && s.autoBleScan == en) return false;
  s.autoWifiScan = en;
  s.autoBleScan  = en;
  dirtySettings = true;
  uiDirty = true;
  lastChangeMs = millis();
  return true;
}

// 切换界面语言（立即生效，整屏重绘；Save 后写入 settings.json）
static bool applyLanguage(bool zh){
  const uint8_t want = zh ? LANG_ZH : LANG_EN;
  if (g_lang == want) return false;
  g_lang = want;
  settings().lang = want;
  dirtySettings = true;
  uiDirty = true;
  lastChangeMs = millis();
  return true;
}

static void handleTouch() {
  int tx, ty;
  static uint32_t lastToggleMs = 0;
  static bool accentArmed = true;
  // 开关行沿触发（Key Beep / Auto Scan / Language）：一次按下只翻一次。
  // 之前只有 120ms 时间去抖，手指按住超 120ms 会再翻一次——点一下翻了两次，
  // 状态弹回原值，Save 时变 No changes，看起来就是"开关没存住"。
  static bool switchArmed = true;
  if (!readTouchXY(tx, ty)) {
    dragging = false;
    accentArmed = true;
    switchArmed = true;
    return;
  }

  Rect br = backRect();
  Rect sr = saveRect();
  if (touchInRect(br, tx, ty)) {
    drawFooter(true, false);
    delay(25);
    drawFooter(false, false);

    feature_exit_requested = true;
    return;
  }
  if (touchInRect(sr, tx, ty)) {
    drawFooter(false, true);
    delay(25);
    drawFooter(false, false);
    if (dirtySettings) {
      bool ok = settingsSave();
      if (ok) {
        dirtySettings = false;
        footerToast("Saved", UI.ok);
        delay(250);
        footerToast("      ", UI_BG);
      } else {
        footerToast("Save FAILED", UI.warn);
      }
    } else {
      footerToast("No changes", UI_TEXT);
      delay(250);
      footerToast("         ", UI_BG);
    }
    return;
  }

  for (int i=0;i<N;++i){
    Rect rr = rowRect(i);
    if (ty >= rr.y && ty <= rr.y+rr.h) { sel = i; break; }
  }

  auto& s = settings();

  if (sel == 0) {
    Rect tr = rBrightTrack();
    Rect kb = rBrightKnob(s.brightness);

    bool inTrack = (tx >= tr.x && tx <= tr.x+tr.w && ty >= tr.y-10 && ty <= tr.y+tr.h+10);
    bool inKnob  = (tx >= kb.x && tx <= kb.x+kb.w && ty >= kb.y && ty <= kb.y+kb.h);

    if (inTrack || inKnob) {
      dragging = true;
      long rel = tx - tr.x;
      if (rel < 0) rel = 0;
      if (rel > tr.w-1) rel = tr.w-1;
      uint8_t v = (uint8_t)((rel * 255L) / (tr.w - 1));
      applyBrightness(v);
    }
  } else if (sel == 1) {
    Rect d = rThemeDark();
    Rect l = rThemeLight();
    uint32_t now = millis();
    if (tx >= d.x && tx <= d.x+d.w && ty >= d.y && ty <= d.y+d.h) {
      if (now - lastToggleMs > 200) {
        applyTheme(Theme::Dark);
        lastToggleMs = now;
      }
    } else if (tx >= l.x && tx <= l.x+l.w && ty >= l.y && ty <= l.y+l.h) {
      if (now - lastToggleMs > 200) {
        applyTheme(Theme::Light);
        lastToggleMs = now;
      }
    }
  } else if (sel == 2) {
    Rect sw = rAccentSwatch();
    const bool onSwatch =
        (tx >= sw.x - 80 && tx <= sw.x + sw.w && ty >= sw.y - 8 && ty <= sw.y + sw.h + 8);
    if (onSwatch) {
      uint32_t now = millis();
      if (accentArmed && (now - lastToggleMs > 250)) {
        uint8_t next = (s.accentColor + 1) % ACCENT_PRESET_COUNT;
        applyAccent(next);
        lastToggleMs = now;
        accentArmed = false;
      }
    } else {
      accentArmed = true;
    }
  } else if (sel == ROW_KEY_BEEP) {
    Rect tr = rSwitchTrack(ROW_KEY_BEEP);
    if (tx >= tr.x && tx <= tr.x+tr.w && ty >= tr.y-10 && ty <= tr.y+tr.h+10) {
      uint32_t now = millis();
      if (switchArmed && (now - lastToggleMs > 120)) {
        switchArmed = false;
        applyKeyBeep(!s.keyBeepEnabled);
        if (s.keyBeepEnabled) uiBeep(2400, 25);   // 开启瞬间给一声反馈（时长与按键音一致）
        lastToggleMs = now;
      }
    }
  } else if (sel == 4) {
    Rect tr = rSwitchTrack(4);
    if (tx >= tr.x && tx <= tr.x+tr.w && ty >= tr.y-10 && ty <= tr.y+tr.h+10) {
      uint32_t now = millis();
      if (switchArmed && (now - lastToggleMs > 120)) {
        switchArmed = false;
        bool autoScan = (s.autoWifiScan || s.autoBleScan);
        applyAutoScan(!autoScan);
        lastToggleMs = now;
      }
    }
  } else if (sel == 5) {
    Rect tr = rSwitchTrack(5);
    if (tx >= tr.x && tx <= tr.x+tr.w && ty >= tr.y-10 && ty <= tr.y+tr.h+10) {
      uint32_t now = millis();
      if (switchArmed && (now - lastToggleMs > 200)) {
        switchArmed = false;
        applyLanguage(g_lang != LANG_ZH);
        lastToggleMs = now;
      }
    }
  }
}

void setup(){

  applyThemeToPalette(settings().theme);
  buildPalette();
  ::setBrightness(settings().brightness);
  sel = 0; dirtySettings = false; uiDirty = false; dragging = false;
  drawAll();
}

void loop(){
  bool changedByButtons=false;

  static bool upWasDown     = false;
  static bool downWasDown   = false;
  static bool leftWasDown   = false;
  static bool rightWasDown  = false;
  static bool selectWasDown = false;
  static uint32_t lastNavMs = 0;
  static uint32_t lastActionMs = 0;
  const uint32_t NAV_DEBOUNCE_MS    = 140;
  const uint32_t ACTION_DEBOUNCE_MS = 140;

  // 从息屏电量屏唤醒：整屏被覆盖过，先整体重绘再继续处理输入
  if (screenConsumeRepaintRequest()) {
    drawAll();
  }

  uint32_t now = millis();

  bool upNow     = isButtonPressed(BTN_UP);
  bool downNow   = isButtonPressed(BTN_DOWN);
  bool leftNow   = isButtonPressed(BTN_LEFT);
  bool rightNow  = isButtonPressed(BTN_RIGHT);
  bool selectNow = isButtonPressed(BTN_SELECT);

  if (selectNow && !selectWasDown && (now - lastActionMs > ACTION_DEBOUNCE_MS)) {
    feature_exit_requested = true;
    lastActionMs = now;
    return;
  }

  if (upNow && !upWasDown && (now - lastNavMs > NAV_DEBOUNCE_MS)) {
    sel=(sel+N-1)%N; changedByButtons=true;
    lastNavMs = now;
  }
  if (downNow && !downWasDown && (now - lastNavMs > NAV_DEBOUNCE_MS)) {
    sel=(sel+1)%N;   changedByButtons=true;
    lastNavMs = now;
  }

  if (leftNow && !leftWasDown && (now - lastActionMs > ACTION_DEBOUNCE_MS)){
    auto& s=settings();
    if (sel==0 && s.brightness>0)      { applyBrightness(s.brightness>8? s.brightness-8:0); }
    else if (sel==1)                   { applyTheme(Theme::Dark); }
    else if (sel==2)                   { applyAccent((s.accentColor + ACCENT_PRESET_COUNT - 1) % ACCENT_PRESET_COUNT); }
    else if (sel==3)                   { applyKeyBeep(false); }
    else if (sel==4)                   { applyAutoScan(false); }
    else if (sel==5)                   { applyLanguage(false); }
    changedByButtons=true;
    lastActionMs = now;
  }
  if ((rightNow && !rightWasDown) && (now - lastActionMs > ACTION_DEBOUNCE_MS)){
    auto& s=settings();
    if (sel==0 && s.brightness<255)    { applyBrightness(s.brightness+8); }
    else if (sel==1)                   { applyTheme(Theme::Light); }
    else if (sel==2)                   { applyAccent((s.accentColor + 1) % ACCENT_PRESET_COUNT); }
    else if (sel==3)                   { applyKeyBeep(true); }
    else if (sel==4)                   { applyAutoScan(true); }
    else if (sel==5)                   { applyLanguage(true); }
    changedByButtons=true;
    lastActionMs = now;
  }

  upWasDown     = upNow;
  downWasDown   = downNow;
  leftWasDown   = leftNow;
  rightWasDown  = rightNow;
  selectWasDown = selectNow;

  handleTouch();

  if (changedByButtons || uiDirty) redrawIfChanged();

  delay(2);
}

}

namespace SdFileManager {

using FeatureUI::Button;
using FeatureUI::ButtonStyle;

static constexpr uint16_t COL_BG   = FEATURE_BG;
static constexpr int STATUS_BAR_H = 20;
static constexpr int HEADER_H = 54;

static String ellipsize(const String& s, int maxW, uint8_t font) {
  if (uiTextWidth(s, font) <= maxW) return s;
  String out = s;
  while (out.length() > 0 && uiTextWidth(out + "...", font) > maxW) {
    out.remove(out.length() - 1);
  }
  return out + "...";
}

struct Entry {
  String name;
  String path;
  bool isDir = false;
  uint32_t size = 0;
  bool isUp = false; // synthetic ".."
};

enum class Page : uint8_t { Browser, Info, ConfirmDelete };

static Page page = Page::Browser;
static std::vector<Entry> entries;
static String cwd = "/";
static int sel = 0;
static int listStart = 0;
static bool uiDirty = true;
static bool touchActive = false;
static bool touchDragging = false;
static int touchStartX = 0;
static int touchStartY = 0;
static int touchLastY = 0;
static int scrollAccumY = 0;

static Button browserBtns[4];
static Button infoBtns[2];
static Button confirmBtns[2];

static String lastErr;

static int sdFmContentBottom() {
  return featureHasTouchNavBar() ? (int)touchNavContentBottomY()
                                 : (int)(tft.height() - FeatureUI::FOOTER_H - 2);
}

static void sdFmRestoreNavChrome() {
  if (!featureHasTouchNavBar()) {
    return;
  }
  invalidateTouchButtonCue();
  maintainTouchNavBar();
}

static char s_sdFmNavCache[5][12] = {{0}};

static void sdFmResetNavLabelCache() {
  for (int i = 0; i < 5; ++i) {
    s_sdFmNavCache[i][0] = '\0';
  }
}

static void sdFmApplyNavLabels(const char* left, const char* down, const char* center,
                               const char* up, const char* right) {
  if (!featureHasTouchNavBar()) {
    return;
  }
  const char* src[5] = {left, down, center, up, right};
  bool same = true;
  for (int i = 0; i < 5; ++i) {
    const char* s = (src[i] && src[i][0]) ? src[i] : "";
    if (strcmp(s_sdFmNavCache[i], s) != 0) {
      same = false;
      break;
    }
  }
  if (same) {
    return;
  }
  for (int i = 0; i < 5; ++i) {
    const char* s = (src[i] && src[i][0]) ? src[i] : "";
    strncpy(s_sdFmNavCache[i], s, sizeof(s_sdFmNavCache[i]) - 1);
    s_sdFmNavCache[i][sizeof(s_sdFmNavCache[i]) - 1] = '\0';
  }
  setTouchNavLabels(left, down, center, up, right);
  redrawTouchButtonBar();
}

static void sdFmUpdateBrowserNavLabels() {
  sdFmApplyNavLabels("Exit", "Next", "Refresh", "Prev", "Open");
}

static void sdFmUpdateInfoNavLabels() {
  sdFmApplyNavLabels("Back", nullptr, "Delete", nullptr, nullptr);
}

static void sdFmUpdateConfirmNavLabels() {
  sdFmApplyNavLabels("Cancel", nullptr, "Delete", nullptr, nullptr);
}

static void clampSel();
static bool reloadDir(const String& path, String* errOut);
static void resetListDrawCache();
static void drawBrowserPage(bool full = true);
static void drawConfirmDeletePage();
static void openSelected();
static bool deleteSelected(String* errOut);

static void sdFmHandleNavButtons() {
  if (!featureHasTouchNavBar()) {
    return;
  }
  if (page == Page::Browser) {
    if (isTouchNavButtonPressedEdge(BTN_LEFT)) {
      feature_exit_requested = true;
      return;
    }
    if (isTouchNavButtonPressedEdge(BTN_UP) && !entries.empty()) {
      sel--;
      clampSel();
      drawBrowserPage(false);
      return;
    }
    if (isTouchNavButtonPressedEdge(BTN_DOWN) && !entries.empty()) {
      sel++;
      clampSel();
      drawBrowserPage(false);
      return;
    }
    if (isTouchNavButtonPressedEdge(BTN_RIGHT)) {
      openSelected();
      return;
    }
    if (isTouchNavButtonPressedEdge(BTN_SELECT)) {
      reloadDir(cwd, nullptr);
      drawBrowserPage(true);
    }
  } else if (page == Page::Info) {
    if (isTouchNavButtonPressedEdge(BTN_LEFT)) {
      drawBrowserPage();
      return;
    }
    if (isTouchNavButtonPressedEdge(BTN_SELECT)) {
      drawConfirmDeletePage();
    }
  } else if (page == Page::ConfirmDelete) {
    if (isTouchNavButtonPressedEdge(BTN_LEFT)) {
      drawBrowserPage();
      return;
    }
    if (isTouchNavButtonPressedEdge(BTN_SELECT)) {
      String err;
      const bool ok = deleteSelected(&err);
      if (!ok) {
        showNotification("Delete", err.c_str());
        delay(500);
      }
      reloadDir(cwd, nullptr);
      drawBrowserPage();
    }
  }
}

static File sdOpenCompat(const String& path) {
  File f = SD.open(path.c_str());
  if (f) return f;
  if (path.length() > 1 && path[0] == '/') {
    f = SD.open(path.substring(1).c_str());
  }
  return f;
}

static bool sdRemoveCompat(const String& path) {
  if (SD.remove(path.c_str())) return true;
  if (path.length() > 1 && path[0] == '/') return SD.remove(path.substring(1).c_str());
  return false;
}

static bool sdRmdirCompat(const String& path) {
  if (SD.rmdir(path.c_str())) return true;
  if (path.length() > 1 && path[0] == '/') return SD.rmdir(path.substring(1).c_str());
  return false;
}

static String normalizePath(const String& path) {
  if (path.length() == 0) return "/";
  String p = path;
  if (p[0] != '/') p = "/" + p;

  String out;
  out.reserve(p.length());
  bool lastSlash = false;
  for (size_t i = 0; i < p.length(); ++i) {
    char c = p[i];
    if (c == '/') {
      if (!lastSlash) out += c;
      lastSlash = true;
    } else {
      out += c;
      lastSlash = false;
    }
  }

  while (out.length() > 1 && out.endsWith("/")) {
    out.remove(out.length() - 1);
  }
  return out;
}

static void clampSel() {
  if (entries.empty()) { sel = 0; return; }
  if (sel < 0) sel = 0;
  if (sel >= (int)entries.size()) sel = (int)entries.size() - 1;
}

static void listLayout(int& top, int& bottom, int& rowH, int& maxVisible) {
  top = STATUS_BAR_H + HEADER_H + 6;
  bottom = sdFmContentBottom();
  rowH = 22;
  maxVisible = max(1, (bottom - top) / rowH);
}

static void clampListStart(int maxVisible) {
  int maxStart = max(0, (int)entries.size() - maxVisible);
  if (listStart < 0) listStart = 0;
  if (listStart > maxStart) listStart = maxStart;
}

static void ensureSelectionVisible(int maxVisible) {
  if (entries.empty()) { listStart = 0; return; }
  if (sel < listStart) listStart = sel;
  if (sel >= listStart + maxVisible) listStart = sel - maxVisible + 1;
  clampListStart(maxVisible);
}

static String parentPath(const String& p) {
  String np = normalizePath(p);
  if (np == "/") return "/";
  int slash = np.lastIndexOf('/');
  if (slash <= 0) return "/";
  return np.substring(0, slash);
}

static bool reloadDir(const String& path, String* errOut = nullptr) {
  entries.clear();
  lastErr = "";

  if (!isSDCardAvailable()) {
    if (errOut) *errOut = "SD not mounted";
    lastErr = "SD not mounted";
    return false;
  }

  String openPath = normalizePath(path);

  File dir = sdOpenCompat(openPath);
  if (!dir) {
    if (errOut) *errOut = "Open dir failed";
    lastErr = "Open dir failed";
    return false;
  }
  if (!dir.isDirectory()) {
    dir.close();
    if (errOut) *errOut = "Not a directory";
    lastErr = "Not a directory";
    return false;
  }

  cwd = openPath;

  if (cwd != "/") {
    Entry up;
    up.name = "..";
    up.path = parentPath(cwd);
    up.isDir = true;
    up.isUp = true;
    entries.push_back(up);
  }

  for (;;) {
    File f = dir.openNextFile();
    if (!f) break;

    Entry e;
    {
      String raw = String(f.name());
      while (raw.endsWith("/")) raw.remove(raw.length() - 1);
      int slash = raw.lastIndexOf('/');
      String base = (slash >= 0) ? raw.substring(slash + 1) : raw;
      while (base.startsWith("/")) base.remove(0, 1);
      while (base.endsWith("/")) base.remove(base.length() - 1);
      e.name = base.length() ? base : raw;
      while (e.name.startsWith("/")) e.name.remove(0, 1);
      while (e.name.endsWith("/")) e.name.remove(e.name.length() - 1);
      if (e.name.length() == 0) { f.close(); continue; }
    }
    e.isDir = f.isDirectory();
    e.size = e.isDir ? 0 : (uint32_t)f.size();
    if (cwd == "/") e.path = "/" + e.name;
    else e.path = cwd + "/" + e.name;
    e.path = normalizePath(e.path);

    entries.push_back(e);
    f.close();
    yield();
  }
  dir.close();

  // Sort, keeping ".." first when present.
  int start = (entries.size() && entries[0].isUp) ? 1 : 0;
  std::sort(entries.begin() + start, entries.end(), [](const Entry& a, const Entry& b) {
    if (a.isDir != b.isDir) return a.isDir > b.isDir;
    return a.name < b.name;
  });

  sel = 0;
  listStart = 0;
  uiDirty = true;
  resetListDrawCache();
  return true;
}

static void drawHeader(const char* title) {
  const int w = tft.width();
  const int y0 = STATUS_BAR_H;
  tft.fillRect(0, y0, w, HEADER_H, FEATURE_BG);

  uiSetTextFont(2);
  uiSetTextSize(1);
  uiSetTextColor(UI_ICON, FEATURE_BG);
  uiSetCursor(8, y0 + 4);
  uiPrint(title);

  uiSetTextFont(1);
  uiSetTextColor(UI_TEXT, FEATURE_BG);
  String pathLine = ellipsize(cwd, w - 16, 1);
  uiSetCursor(8, y0 + 24);
  uiPrint(pathLine);

  String right = entries.empty()
    ? String("0/0")
    : String(sel + 1) + "/" + String((int)entries.size());
  String left = String("Items: ") + String((int)entries.size());
  int rightW = uiTextWidth(right, 1);
  uiSetCursor(8, y0 + 40);
  uiPrint(left);
  uiSetCursor(w - 8 - rightW, y0 + 40);
  uiPrint(right);

  tft.drawLine(0, y0 + HEADER_H, w, y0 + HEADER_H, UI_LINE);
}

static void drawHeaderSelCounter() {
  const int w = tft.width();
  const int y0 = STATUS_BAR_H;
  String right = entries.empty()
    ? String("0/0")
    : String(sel + 1) + "/" + String((int)entries.size());
  int rightW = uiTextWidth(right, 1);
  tft.fillRect(w - 8 - max(rightW, 40), y0 + 38, max(rightW, 40) + 4, 14, FEATURE_BG);
  uiSetTextFont(1);
  uiSetTextColor(UI_TEXT, FEATURE_BG);
  uiSetCursor(w - 8 - rightW, y0 + 40);
  uiPrint(right);
}

static void drawListRow(int idx, int y, int rowH, bool selected) {
  const int x = 8;
  const int w = tft.width() - 16;
  uint16_t bg = selected ? UI_FG : FEATURE_BG;
  uint16_t fg = selected ? UI_ICON : UI_TEXT;

  tft.fillRect(x, y, w, rowH - 1, bg);
  tft.drawLine(x, y + rowH - 1, x + w, y + rowH - 1, UI_LINE);

  uiSetTextFont(1);
  uiSetTextColor(fg, bg);
  String label = entries[idx].name;
  if (entries[idx].isDir && !entries[idx].isUp) label = label + "/";
  label = ellipsize(label, w - 80, 1);
  uiSetCursor(x + 4, y + 6);
  uiPrint(label);

  String right = entries[idx].isDir ? "DIR" : "";
  if (!entries[idx].isDir && !entries[idx].isUp) {
    char sbuf[16];
    uint32_t kb = (entries[idx].size + 1023) / 1024;
    snprintf(sbuf, sizeof(sbuf), "%lukB", (unsigned long)kb);
    right = sbuf;
  }
  if (right.length()) {
    int tw = uiTextWidth(right, 1);
    uiSetCursor(x + w - tw - 6, y + 6);
    uiPrint(right);
  }
}

static void drawListScrollbar(int top, int bottom, int start, int maxVisible) {
  if ((int)entries.size() <= maxVisible) {
    return;
  }
  int scrollBarX = tft.width() - 6;
  int scrollBarHeight = bottom - top - 6;
  int scrollBarY = top + 3;
  int indicatorHeight = (maxVisible * scrollBarHeight) / (int)entries.size();
  if (indicatorHeight < 6) indicatorHeight = 6;
  int indicatorY = scrollBarY;
  indicatorY = scrollBarY + (start * (scrollBarHeight - indicatorHeight)) /
                            max(1, (int)entries.size() - maxVisible);
  tft.fillRect(scrollBarX, scrollBarY, 3, scrollBarHeight, UI_LINE);
  tft.fillRect(scrollBarX, indicatorY, 3, indicatorHeight, UI_ACCENT);
}

static int s_lastListSel = -1;
static int s_lastListStart = -1;

static void resetListDrawCache() {
  s_lastListSel = -1;
  s_lastListStart = -1;
}

static void drawList(bool full = true) {
  int top, bottom, rowH, maxVisible;
  listLayout(top, bottom, rowH, maxVisible);

  if (!isSDCardAvailable()) {
    tft.fillRect(0, top, tft.width(), bottom - top, COL_BG);
    uiSetTextFont(2);
    uiSetTextColor(UI_WARN, COL_BG);
    uiDrawCentreString("SD not mounted", tft.width() / 2, top + 30, 2);
    resetListDrawCache();
    return;
  }

  if (entries.empty()) {
    tft.fillRect(0, top, tft.width(), bottom - top, COL_BG);
    uiSetTextFont(2);
    uiSetTextColor(UI_TEXT, COL_BG);
    uiDrawCentreString("Empty folder", tft.width() / 2, top + 30, 2);
    resetListDrawCache();
    return;
  }

  clampSel();
  ensureSelectionVisible(maxVisible);

  int start = listStart;
  int end = min((int)entries.size(), start + maxVisible);

  const bool windowChanged = full || s_lastListSel < 0 || s_lastListStart != start;
  if (windowChanged) {
    tft.fillRect(0, top, tft.width(), bottom - top, COL_BG);
    for (int i = start; i < end; ++i) {
      int y = top + (i - start) * rowH;
      drawListRow(i, y, rowH, i == sel);
    }
    drawListScrollbar(top, bottom, start, maxVisible);
    drawHeaderSelCounter();
    s_lastListSel = sel;
    s_lastListStart = start;
    return;
  }

  if (s_lastListSel != sel) {
    drawHeaderSelCounter();
    const int oldSel = s_lastListSel;
    const int newSel = sel;
    if (oldSel >= start && oldSel < end) {
      drawListRow(oldSel, top + (oldSel - start) * rowH, rowH, false);
    }
    if (newSel >= start && newSel < end) {
      drawListRow(newSel, top + (newSel - start) * rowH, rowH, true);
    }
    s_lastListSel = sel;
  }
}

static void drawBrowserFooter() {
  if (featureHasTouchNavBar()) {
    sdFmUpdateBrowserNavLabels();
    return;
  }
  bool sdOk = isSDCardAvailable();
  bool hasSel = !entries.empty();
  bool selIsUp = false;
  if (hasSel) {
    clampSel();
    selIsUp = entries[sel].isUp;
  }

  FeatureUI::drawFooterBg();
  FeatureUI::layoutFooter4(
    browserBtns,
    "Exit",    ButtonStyle::Secondary,
    "Open",    ButtonStyle::Primary,
    "Delete",  ButtonStyle::Danger,
    "Refresh", ButtonStyle::Secondary,
    false,
    (!sdOk || !hasSel),
    (!sdOk || !hasSel || selIsUp),
    false
  );
  for (auto& b : browserBtns) FeatureUI::drawButton(b);
}

static void drawBrowserPage(bool full) {
  page = Page::Browser;
  if (full) {
    featureClearContent(COL_BG);
    currentBatteryVoltage = readBatteryVoltage();
    drawStatusBar(currentBatteryVoltage, true);
    drawHeader("SD File Manager");
    drawBrowserFooter();
    drawList(true);
    sdFmRestoreNavChrome();
    uiDirty = false;
    return;
  }

  drawList(false);
  uiDirty = false;
}

static void drawInfoPage() {
  page = Page::Info;
  featureClearContent(COL_BG);
  currentBatteryVoltage = readBatteryVoltage();
  drawStatusBar(currentBatteryVoltage, true);
  drawHeader("File info");

  if (featureHasTouchNavBar()) {
    sdFmUpdateInfoNavLabels();
  } else {
    FeatureUI::drawFooterBg();
    FeatureUI::layoutFooter2(
      infoBtns,
      "Back",   ButtonStyle::Secondary,
      "Delete", ButtonStyle::Danger,
      false, (entries.empty() || !isSDCardAvailable())
    );
    for (auto& b : infoBtns) FeatureUI::drawButton(b);
  }

  const int top = STATUS_BAR_H + HEADER_H + 6;
  const int bottom = sdFmContentBottom();
  tft.fillRect(0, top, tft.width(), bottom - top, COL_BG);

  if (entries.empty()) {
    uiSetTextFont(2);
    uiSetTextColor(UI_TEXT, COL_BG);
    uiDrawCentreString("No selection", tft.width()/2, top + 24, 2);
    uiDirty = false;
    return;
  }

  clampSel();
  const Entry& e = entries[sel];

  uiSetTextFont(2);
  uiSetTextColor(UI_TEXT, COL_BG);
  uiSetCursor(8, top + 10);
  uiPrint("Name:");
  uiSetCursor(70, top + 10);
  uiPrint(e.name);

  uiSetCursor(8, top + 34);
  uiPrint("Type:");
  uiSetCursor(70, top + 34);
  uiPrint(e.isDir ? "Folder" : "File");

  if (!e.isDir) {
    uiSetCursor(8, top + 58);
    uiPrint("Size:");
    uiSetCursor(70, top + 58);
    uiPrint(String(e.size) + " B");
  }

  uiSetTextFont(1);
  uiSetCursor(8, top + 88);
  uiPrint("Path:");
  uiSetCursor(8, top + 102);
  uiPrint(e.path);

  sdFmRestoreNavChrome();
  uiDirty = false;
}

static void drawConfirmDeletePage() {
  page = Page::ConfirmDelete;
  featureClearContent(COL_BG);
  currentBatteryVoltage = readBatteryVoltage();
  drawStatusBar(currentBatteryVoltage, true);
  drawHeader("Confirm delete");

  if (featureHasTouchNavBar()) {
    sdFmUpdateConfirmNavLabels();
  } else {
    FeatureUI::drawFooterBg();
    FeatureUI::layoutFooter2(
      confirmBtns,
      "Cancel", ButtonStyle::Secondary,
      "Delete", ButtonStyle::Danger,
      false, false
    );
    for (auto& b : confirmBtns) FeatureUI::drawButton(b);
  }

  const int top = STATUS_BAR_H + HEADER_H + 6;
  const int bottom = sdFmContentBottom();
  tft.fillRect(0, top, tft.width(), bottom - top, COL_BG);

  uiSetTextFont(2);
  uiSetTextColor(UI_WARN, COL_BG);
  uiDrawCentreString("Delete selected item?", tft.width()/2, top + 18, 2);

  if (!entries.empty()) {
    const Entry& e = entries[sel];
    uiSetTextFont(1);
    uiSetTextColor(UI_TEXT, COL_BG);
    uiDrawCentreString(e.name.c_str(), tft.width()/2, top + 48, 1);
  }

  sdFmRestoreNavChrome();
  uiDirty = false;
}

static bool deleteSelected(String* errOut = nullptr) {
  if (entries.empty()) { if (errOut) *errOut = "Nothing selected"; return false; }
  clampSel();
  const Entry e = entries[sel];
  if (e.isUp) { if (errOut) *errOut = "Cannot delete .."; return false; }

  if (!isSDCardAvailable()) { if (errOut) *errOut = "SD not mounted"; return false; }

  bool ok = false;
  if (e.isDir) {
    // ESP32 FS supports rmdir. If directory is not empty it will fail.
    ok = sdRmdirCompat(e.path);
  } else {
    ok = sdRemoveCompat(e.path);
  }
  if (!ok) {
    if (errOut) *errOut = "Delete failed";
    return false;
  }
  return true;
}

static void openSelected() {
  if (entries.empty()) return;
  clampSel();
  const Entry& e = entries[sel];

  if (e.isDir) {
    String err;
    if (!reloadDir(normalizePath(e.path), &err)) {
      drawBrowserPage();
      String msg = err.length() ? err : "Failed";
      msg += "\n";
      msg += e.path;
      showNotification("Open", msg.c_str());
      delay(600);
      return;
    }
    drawBrowserPage();
    return;
  }
  drawInfoPage();
}

static bool hitListRow(int x, int y, int& outIdx) {
  int top, bottom, rowH, maxVisible;
  listLayout(top, bottom, rowH, maxVisible);
  if (y < top || y >= bottom) return false;

  ensureSelectionVisible(maxVisible);
  int start = listStart;
  int end = min((int)entries.size(), start + maxVisible);

  int row = (y - top) / rowH;
  int idx = start + row;
  if (idx < start || idx >= end) return false;
  outIdx = idx;
  return true;
}

static void handleTap(int x, int y) {
  if (page == Page::Browser) {
    int idx = -1;
    if (hitListRow(x, y, idx)) {
      if (idx != sel) {
        sel = idx;
        clampSel();
        drawBrowserPage(false);
      } else {
        openSelected();
      }
      delay(140);
      return;
    }

    int f = -1;
    if (!featureHasTouchNavBar()) {
      f = FeatureUI::hit(browserBtns, 4, x, y);
    }
    if (f >= 0) {
      FeatureUI::drawButton(browserBtns[f], true);
      delay(90);
      FeatureUI::drawButton(browserBtns[f], false);
      delay(90);

      if (f == 0) { feature_exit_requested = true; return; }          // Exit
      if (f == 1) { openSelected(); delay(120); return; }             // Open
      if (f == 2) {                                                   // Delete
        if (!entries.empty() && isSDCardAvailable() && !entries[sel].isUp) {
          drawConfirmDeletePage();
        }
        delay(120);
        return;
      }
      if (f == 3) { reloadDir(cwd, nullptr); drawBrowserPage(true); delay(120); return; } // Refresh
    }
  }
  else if (page == Page::Info) {
    int f = -1;
    if (!featureHasTouchNavBar()) {
      f = FeatureUI::hit(infoBtns, 2, x, y);
    }
    if (f >= 0) {
      FeatureUI::drawButton(infoBtns[f], true);
      delay(90);
      FeatureUI::drawButton(infoBtns[f], false);
      delay(90);

      if (f == 0) { drawBrowserPage(); delay(120); return; }         // Back
      if (f == 1) { drawConfirmDeletePage(); delay(120); return; }   // Delete
    }
  }
  else if (page == Page::ConfirmDelete) {
    int f = -1;
    if (!featureHasTouchNavBar()) {
      f = FeatureUI::hit(confirmBtns, 2, x, y);
    }
    if (f >= 0) {
      FeatureUI::drawButton(confirmBtns[f], true);
      delay(90);
      FeatureUI::drawButton(confirmBtns[f], false);
      delay(90);

      if (f == 0) {                                                  // Cancel
        drawBrowserPage();
        delay(120);
        return;
      }
      if (f == 1) {                                                  // Delete
        String err;
        bool ok = deleteSelected(&err);
        if (!ok) {
          showNotification("Delete", err.c_str());
          delay(500);
        }
        reloadDir(cwd, nullptr);
        drawBrowserPage();
        delay(120);
        return;
      }
    }
  }
}

void setup() {
  sdRetryMount();
  sdFmResetNavLabelCache();
  page = Page::Browser;
  cwd = "/";
  sel = 0;
  uiDirty = true;
  reloadDir("/", nullptr);
  currentBatteryVoltage = readBatteryVoltage();
  drawStatusBar(currentBatteryVoltage, true);
  drawBrowserPage();
}

void loop() {
  updateStatusBar();
  if (featureHasTouchNavBar()) {
    maintainTouchNavBar();
  }
  sdFmHandleNavButtons();
  if (feature_exit_requested) {
    return;
  }
  if (uiDirty) {
    switch (page) {
      case Page::Browser:        drawBrowserPage(true); break;
      case Page::Info:           drawInfoPage(); break;
      case Page::ConfirmDelete:  drawConfirmDeletePage(); break;
    }
  }

  // Physical navigation (kept for devices without touch nav bar).
  if (!featureHasTouchNavBar() && isButtonPressed(BTN_UP)) {
    if (page == Page::Browser && !entries.empty()) { sel--; clampSel(); drawBrowserPage(false); }
    delay(160);
    return;
  }
  if (!featureHasTouchNavBar() && isButtonPressed(BTN_DOWN)) {
    if (page == Page::Browser && !entries.empty()) { sel++; clampSel(); drawBrowserPage(false); }
    delay(160);
    return;
  }

  int x, y;
  bool touched = readTouchXY(x, y);
  if (!touched) {
    if (touchActive && !touchDragging) {
      handleTap(touchStartX, touchStartY);
    }
    touchActive = false;
    touchDragging = false;
    scrollAccumY = 0;
    return;
  }

  if (!touchActive) {
    touchActive = true;
    touchDragging = false;
    touchStartX = x;
    touchStartY = y;
    touchLastY = y;
    scrollAccumY = 0;
    return;
  }

  if (page == Page::Browser) {
    int top, bottom, rowH, maxVisible;
    listLayout(top, bottom, rowH, maxVisible);
    bool canScroll = (touchStartY >= top && touchStartY < bottom);

    int dy = y - touchLastY;
    touchLastY = y;

    if (!touchDragging && canScroll) {
      if (abs(y - touchStartY) > 8) {
        touchDragging = true;
      }
    }

    if (touchDragging && canScroll) {
      scrollAccumY += dy;
      int steps = scrollAccumY / rowH;
      if (steps != 0) {
        listStart -= steps;
        clampListStart(maxVisible);
        if (!entries.empty()) {
          if (sel < listStart) sel = listStart;
          if (sel >= listStart + maxVisible) sel = min((int)entries.size() - 1, listStart + maxVisible - 1);
        }
        drawBrowserPage(false);
        scrollAccumY -= steps * rowH;
      }
    }
  }

  return;
}

} // namespace SdFileManager

namespace TouchCalib {
static int stepIdx = 0;
static uint16_t xs[4], ys[4];
static const int pts[4][2] = { {20,20}, {TFT_WIDTH-20,20}, {TFT_WIDTH-20,TFT_HEIGHT-20}, {20,TFT_HEIGHT-20} };

static void drawTarget(int x,int y){
  tft.fillScreen(UI_BG);
  tft.drawCircle(x,y,10,UI_ICON);
  tft.drawLine(x-14,y, x+14,y, UI_ICON);
  tft.drawLine(x,y-14, x,y+14, UI_ICON);
  uiSetCursor(70, 8);
  uiSetTextColor(UI_TEXT, UI_BG);
  uiPrint("Touch the target");
}

void setup(){
  stepIdx=0;
  drawTarget(pts[0][0], pts[0][1]);
}

void loop(){
  if (stepIdx>=4){
    uint16_t xMin = min(xs[0], xs[3]);
    uint16_t xMax = max(xs[1], xs[2]);
    uint16_t yMin = min(ys[0], ys[1]);
    uint16_t yMax = max(ys[2], ys[3]);
    auto& s = settings();
    s.touchXMin = xMin; s.touchXMax = xMax;
#if defined(BOARD_CYD)
    s.touchYMin = yMin; s.touchYMax = yMax;
#else
    s.touchYMin = yMax; s.touchYMax = yMin;
#endif

    bool ok = settingsSave();

    tft.fillScreen(UI_BG);
    uiSetTextColor(ok ? UI.ok : UI.warn, UI_BG);
    uiSetCursor(70, 8);
    uiPrint(ok ? "Calibration Saved" : "Save FAILED");

    uiSetTextColor(UI_TEXT, UI_BG);
    uiSetCursor(70,28);
    tft.printf("X:[%u..%u] Y:[%u..%u]", xMin,xMax,yMin,yMax);

    delay(1200);
    feature_exit_requested = true;
    return;
  }

  int16_t rx = 0, ry = 0;
  if (readTouchRawXY(rx, ry)) {
    xs[stepIdx] = (uint16_t)rx;
    ys[stepIdx] = (uint16_t)ry;
    stepIdx++;
    if (stepIdx<4) drawTarget(pts[stepIdx][0], pts[stepIdx][1]);
  }
  delay(100);
}
}

/* ===== 临时屏幕调试日志层（SCREEN_DEBUG，2026-10-10）=====
 * 串口芯片损坏时的替代查看手段：DBG() 把调试日志写入本环形缓冲，
 * loop() 每帧调用 screenDbgRedraw() 把最近 SCREEN_DBG_N 行绘到屏幕底部黑底区域。
 * 撤除：shared.h 的 SCREEN_DEBUG 改 0 即恢复纯串口；彻底移除见 shared.h 注释。 */
#ifndef SCREEN_DBG_N
#define SCREEN_DBG_N   6
#define SCREEN_DBG_W   36
#define SCREEN_DBG_ROW 12
#endif
static char    s_dbgBuf[SCREEN_DBG_N][SCREEN_DBG_W + 1];
static int     s_dbgHead = 0;   // 指向最新一行
static int     s_dbgCnt  = 0;

void screenDbgPush(const char* s) {
  if (!s) return;
  int len = 0;
  while (s[len] && len < SCREEN_DBG_W) len++;
  while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) len--;  // 去结尾换行
  int i = (s_dbgHead + 1) % SCREEN_DBG_N;
  s_dbgHead = i;
  if (s_dbgCnt < SCREEN_DBG_N) s_dbgCnt++;
  for (int k = 0; k < len; k++) s_dbgBuf[i][k] = s[k];
  s_dbgBuf[i][len] = '\0';
}

void screenDbgPushf(const char* fmt, ...) {
  char buf[SCREEN_DBG_W + 1];
  va_list ap; va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  screenDbgPush(buf);
}

void screenDbgRedraw() {
#if SCREEN_DEBUG
  const int h = tft.height();
  const int w = tft.width();
  const int panelH = SCREEN_DBG_N * SCREEN_DBG_ROW + 4;
  const int y0 = h - panelH;
  tft.fillRect(0, y0, w, panelH, TFT_BLACK);
  tft.setTextFont(1);
  tft.setTextSize(1);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  int start = (s_dbgCnt < SCREEN_DBG_N) ? 0 : (s_dbgHead + 1) % SCREEN_DBG_N;  // 最旧一行
  for (int r = 0; r < s_dbgCnt; r++) {
    int idx = (start + r) % SCREEN_DBG_N;
    tft.setCursor(2, y0 + 2 + r * SCREEN_DBG_ROW);
    tft.print(s_dbgBuf[idx]);
  }
#endif
}
