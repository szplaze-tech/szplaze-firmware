#ifndef UTILS_H
#define UTILS_H

#include <SD.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <XPT2046_Touchscreen.h>
#include "SettingsStore.h"
#include "Touchscreen.h"
#include "shared.h"

#ifdef TFT_GREEN
#undef TFT_GREEN
#endif
#define TFT_GREEN GREEN

extern TFT_eSPI tft;

// Obfuscated-string helpers (XOR decode, then print).
void tftPrintObf(const uint8_t* data, size_t len, uint8_t key = k0());
void tftPrintlnObf(const uint8_t* data, size_t len, uint8_t key = k0());
void serialPrintObf(const uint8_t* data, size_t len, bool newline = false, uint8_t key = k0());

void updateStatusBar();
/** Stop GPS wardriver background task before WiFi/BLE features use the radio. */
void pauseBackgroundRadioTasks();
float readBatteryVoltage();
/** 电池电压 → 剩余电量(%)，走单节锂电 OCV 曲线插值（**不是**线性映射）。
 *  返回 <0 = 还没测到。状态栏 / 息屏电量屏 / 变化检测三处共用同一口径，
 *  避免各写一份线性映射导致同一个电压在不同地方显示不同的电量。
 *  实现与"为什么不能线性"的理由见 utils.cpp。 */
int batteryPercentFromVoltage(float v);
/** 显示层电量百分比：在 batteryPercentFromVoltage() 之上做迟滞 + 充放电分离
 *  （放电只减不增防反弹、连续 N 次确认才更新、充电只增不减）。状态栏/息屏屏/
 *  statusBarTask 统一走这里，避免各界面电量跳动不一致。 */
int batteryDisplayPercent(float voltage);
/** 读 IO39 的外部电源(USB)插入检测（USB_DETECT_PIN，插入=高）。
 *  纯 GPIO，不碰 ADC/I2C，上电即可用。配置见 shared.h。 */
bool readUsbPowerPresent();

/** 上电即把 PN532 的 RSTPDN 驱动为高、释放芯片复位。
 *  ⚠ **必须在 setup 最早期调用**（早于任何 I2C 访问 / initPcf8574Buttons）。
 *  不驱动它会让 PN532 从上电起就吊在复位态，进而拖坏 I2C 总线 ——
 *  表现是五个按键全部按不动（原因与被坑过的现场见 utils.cpp 实现处注释）。 */
void bootReleasePn532Reset();
/** I2C 总线锁死恢复：SCL 打 9 个脉冲 + 生成 STOP，逼把 SDA 拉住不放的从机松手。
 *  返回 true = 检测到 SDA 被拉低并执行了恢复。
 *  **这是"不用拔电源就能恢复"的唯一软件手段** —— 主机复位不会让从机释放总线。
 *  在 setup 里（Wire.begin 之前）和检测到总线异常时各调一次。 */
bool i2cBusRecover();
/** 启动时的 I2C 体检：扫描 0x03..0x77 + 读 PCF8574 原始字节，画在屏幕上停留一会儿。
 *  目的：把"按键挂了"从"看不见"变成"一眼可见"。开关见 shared.h 的 BOOT_I2C_SELFTEST。 */
void bootI2cSelftest();

/* ---- 无操作自动熄屏（Settings → Screen Off，超时预设见 SettingsStore.h）----
 * 所有输入读取函数都应经过 screenFilterInput()；所有会被反复调用的输入轮询点
 * 都应顺带调 screenIdleTick()。为什么这样挂，见 utils.cpp 里的长注释。 */
bool screenIsOff();
/** 刷新"最近一次操作"时间戳（确认是有效操作时调用）。 */
void screenNoteActivity();
/** 唤醒后"整体重绘当前界面"的回调，应用启动时注册一次。
 *  屏幕被息屏电量屏整屏覆盖过，菜单类界面靠它立刻重建。 */
typedef void (*ScreenWakeHook)();
void screenSetWakeHook(ScreenWakeHook hook);

/** "此刻是否允许自动息屏"的判断，应用启动时注册一次。
 *  返回 false 时 screenIdleTick() 直接跳过（不息屏），并持续刷新计时 ——
 *  用于把"有动态画面的功能"排除在息屏保护之外：活画面不该被电量屏打断，
 *  功能退出后也从零开始计满一个完整超时，不会"一退出就立刻黑屏"。 */
typedef bool (*ScreenAutoOffGate)();
void screenSetAutoOffGate(ScreenAutoOffGate fn);
/** 熄屏唤醒后请求整体重绘：能自行重绘的功能屏在循环里查这个标志（消费即清除）。
 *  主菜单/子菜单类界面由 wake hook 处理，不用查。 */
bool screenConsumeRepaintRequest();
/** 画息屏电量屏。force=true 强制重画，false 仅在数值变化时重画。 */
void screenDrawAod(bool force);
/** 推进超时判断：到点就灭背光。极廉价，可放在紧循环里反复调。 */
void screenIdleTick();
/** 输入闸门：返回 true = 丢弃本次输入（熄屏后的第一下只用于点亮屏幕）。 */
bool screenFilterInput();

float readInternalTemperature();
bool isSDCardAvailable();
/** After SPI is used for another device (nRF24 / CC1101 / PN532), restore pins,
 *  deselect other chip-selects, and remount SD. No GPIO pin remapping. */
void restoreSdAfterSharedSpi();
/** Reclaim shared SPI pins for CC1101/nRF without mounting SD (SD.begin raises
 *  the SPI clock and can leave the bus unusable for SubGHz until reboot). */
void reclaimSharedSpiBus();
/** Hold SD CS high and set a CC1101-safe SPI rate on the shared bus. */
void holdSdInactiveOnSharedSpi();
void drawStatusBar(float batteryVoltage, bool forceUpdate = false, bool bottomSeparator = false);
void startStatusBarTask();
/** Request a status bar pass on the next update (e.g. after SD or ward state changes). */
void requestStatusBarRedraw();

/** Init NimBLE once (releases unused Classic BT RAM first). Safe to call repeatedly. */
bool ensureBleStackReady();

/** Allow another SD mount attempt (clears v1 boot "gave up" latch). */
void sdRetryMount();

extern bool feature_exit_requested;

extern void setBrightness(uint8_t value);
bool isButtonPressed(int buttonPin);
/** True while the PCF8574 button for this pin is held (no touch nav). */
bool isPhysicalButtonPressed(int buttonPin);
/** True while a touch nav slot for this pin is held (ignores physical buttons). */
bool isTouchNavButtonPressed(int buttonPin);
/** True once per touch nav press (ignores physical buttons). */
bool isTouchNavButtonPressedEdge(int buttonPin);
/** True once per press (touch nav edge or PCF falling edge). */
bool isButtonPressedEdge(int buttonPin);
/** Exit/back: level-sensitive so slow feature loops still catch PCF + touch nav. */
bool featureExitButtonPressed();
void setTouchButtonInputEnabled(bool enabled);
void drawTouchButtonCue();
void invalidateTouchButtonCue();
void redrawTouchButtonBar();
/** Clear touch-nav edge state after a full-screen overlay (keyboard, etc.). */
void resetTouchNavHeldState();
/** Draw the touch nav bar once (no-op if already drawn). */
void maintainTouchNavBar();
/** Pixels reserved at the bottom for the touch nav bar (0 when hidden). */
int16_t touchNavReservedHeight();
/** Lowest Y coordinate features should draw into (exclusive of nav bar). */
int16_t touchNavContentBottomY();
/** Clear feature body only; leaves the touch nav bar intact. */
void featureClearContent(uint16_t color = BLACK);
/** True when the on-screen touch nav bar is active for the current feature. */
bool featureHasTouchNavBar();
/** Override nav slots with text labels (nullptr = default icon for that slot). */
void setTouchNavLabels(const char* left, const char* down, const char* center,
                       const char* up, const char* right);
bool initPcf8574Buttons();
uint8_t getPcf8574Address();
void sdSpiInit();
bool sdMountChipSelect(uint8_t cs);

extern float currentBatteryVoltage;
/** True while the IP5306 reports the battery is charging (V2 only). */
extern bool batteryIsCharging;
/** True while external power (USB) is plugged in — IO39 insert detect.
 *  Drives the yellow plug next to the battery gauge; independent of the IP5306. */
extern bool usbPowerPresent;

void initDisplay();
void showNotification(const char* title, const char* message);
void hideNotification();

enum class NotificationAction : uint8_t { None, Close, Ok, Save };
void showNotificationActions(const char* title, const char* message, bool showSave);
bool isNotificationVisible();
/** Call from the main loop: auto-hides prompt-only notifications after a timeout. */
void notificationTick();
/** Repaint the modal after a full-screen or body redraw covered it (same cached title/message). */
void notificationRedrawIfVisible();
NotificationAction notificationHandleTouch(int x, int y);
void printWrappedText(int x, int y, int maxWidth, const char* text);
void loading(int frameDelay, uint16_t color, int16_t x, int16_t y, int repeats, bool center);
void displayLogo(uint16_t color, int displayTime);
void initSDCard();

/* 按键音（蜂鸣器）：非阻塞短鸣 + 到点关闭。开关在 Settings → Key Beep。
 * 触发集中在 isButtonPressed() 内部沿检测；主 loop 每圈调 uiBeepPoll()。
 * 2026-10-20：默认时长 45→25ms（用户反馈偏长）。 */
void uiBeep(uint16_t hz = 2200, uint16_t ms = 25);
void uiBeepPoll();
void uiBeepSelfTest(uint16_t hz = 2400, uint16_t ms = 300);  // 开机自检音（无视 Key Beep 开关，诊断用）

namespace AppSettingsUI{ void setup(); void loop(); }
namespace TouchCalib{ void setup(); void loop(); }

namespace Terminal {
  void terminalSetup();
  void terminalLoop();
}

namespace SdFileManager {
  void setup();
  void loop();
}

namespace FeatureUI {
  enum class ButtonStyle : uint8_t { Primary, Secondary, Danger };

  struct Button {
    int16_t x, y, w, h;
    const char* label;
    ButtonStyle style;
    bool disabled;
  };

  constexpr int16_t FOOTER_H = 34;
  constexpr int16_t BTN_H    = 26;
  constexpr int16_t PAD_X    = 8;
  constexpr int16_t GAP_X    = 8;

  void drawFooterBg();

  void drawButtonRect(int x, int y, int w, int h,
                      const char* label,
                      ButtonStyle style,
                      bool pressed = false,
                      bool disabled = false,
                      uint8_t font = 2);

  inline void drawButton(const Button& b, bool pressed = false) {
    drawButtonRect(b.x, b.y, b.w, b.h, b.label, b.style, pressed, b.disabled);
  }

  void layoutFooter3(Button (&btns)[3],
                     const char* l0, ButtonStyle s0,
                     const char* l1, ButtonStyle s1,
                     const char* l2, ButtonStyle s2,
                     bool d0=false, bool d1=false, bool d2=false);
  void layoutFooter2(Button (&btns)[2],
                     const char* l0, ButtonStyle s0,
                     const char* l1, ButtonStyle s1,
                     bool d0=false, bool d1=false);
  void layoutFooter4(Button (&btns)[4],
                     const char* l0, ButtonStyle s0,
                     const char* l1, ButtonStyle s1,
                     const char* l2, ButtonStyle s2,
                     const char* l3, ButtonStyle s3,
                     bool d0=false, bool d1=false, bool d2=false, bool d3=false);
  void layoutFooter1(Button& btn, const char* label, ButtonStyle style, bool disabled=false);
  void layoutFooter5(Button (&btns)[5],
                     const char* l0, ButtonStyle s0,
                     const char* l1, ButtonStyle s1,
                     const char* l2, ButtonStyle s2,
                     const char* l3, ButtonStyle s3,
                     const char* l4, ButtonStyle s4,
                     bool d0=false, bool d1=false, bool d2=false, bool d3=false, bool d4=false);

  int hit(const Button* btns, int n, int x, int y);
}

#endif
 