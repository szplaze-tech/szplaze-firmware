// i18n.h —— 运行时中英文切换 + 中文点阵绘制
//
// 设计：
//   1) T("English") 按当前语言返回译文；英文模式直接返回原指针（零开销）。
//   2) TFT_eSPI 内置字体不含汉字，因此所有文字绘制都要走 ui* 镜像 API：
//        tft.print("Scanner")   -> uiPrint("Scanner")     // 自动 T()
//        tft.println("Ready")   -> uiPrintln("Ready")
//        tft.setCursor(x, y)    -> uiSetCursor(x, y)
//        tft.setTextColor(f,b)  -> uiSetTextColor(f, b)
//        tft.setTextFont(n)     -> uiSetTextFont(n)
//        tft.setTextSize(s)     -> uiSetTextSize(s)
//        tft.drawString(s,x,y,f)-> uiDrawString(s, x, y, f)
//      英文模式下这些函数全部等价转发给 tft，行为与改造前完全一致。
//   3) 汉字按当前字高自动选字号：字高 >= 16 用 16x16（菜单），否则用 12x12（功能内）。
//   4) 汉字只画"前景点"（drawPixel），不填背景 → 任何底色上都正确，
//      也绕开了 TFT_eSPI pushImage 在小 buffer 上的字节序问题。
#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>
#include "zh_font.h"

// 供下方的模板兜底直接转发使用（定义在 ESP32-DIV.ino）
extern TFT_eSPI tft;

#define LANG_EN 0
#define LANG_ZH 1

// 当前语言（设置菜单可切换，存入 SettingsStore）
extern uint8_t g_lang;

// 取译文。英文模式或未收录的字符串返回原指针。
const char* T(const char* en);

// ── 汉字绘制内核（tft 与 TFT_eSprite 通用）────────────────────
//
// 为什么用模板而不是 TFT_eSPI* 基类指针：
//   TFT_eSprite 派生自 TFT_eSPI，把 drawPixel / drawChar / fillRect / setWindow /
//   pushColor 声明为 **virtual** 后重写（像素写进自己的缓冲区，见 Extensions/Sprite.h）。
//   模板按实参类型静态分发，直接命中派生类重写、不依赖虚表，语义最直白；
//   更关键的是 startWrite / endWrite 恰恰**没有**被重写 —— Sprite.h 里根本没有这两个
//   名字，照调会落到 TFT_eSPI 的实现，白白 begin_tft_write() 一次（拉低 CS、锁事务）。
//   模板 + 编译期 trait 才能把这一条也区分开，见下方 I18nScreenTxn。
//
// 注意：TFT_eSprite 不提供读取"当前文字状态"的方法，所以泛型入口把
// fg/bg/font/datum/size 全部作为显式参数传入，不依赖对象内部状态。

static inline int i18nGlyphIndex(uint16_t cp) {
  int lo = 0, hi = ZH_FONT_COUNT - 1;
  while (lo <= hi) {
    const int mid = (lo + hi) >> 1;
    const uint16_t u = zhUnicode[mid];
    if (u == cp) return mid;
    if (u < cp) lo = mid + 1; else hi = mid - 1;
  }
  return -1;
}

static inline int i18nUtf8Next(const char* s, uint16_t* cp) {
  const uint8_t c = (uint8_t)s[0];
  if (c < 0x80) { *cp = c; return 1; }
  if ((c & 0xE0) == 0xC0) {
    *cp = (uint16_t)(((c & 0x1F) << 6) | ((uint8_t)s[1] & 0x3F));
    return 2;
  }
  if ((c & 0xF0) == 0xE0) {
    *cp = (uint16_t)(((c & 0x0F) << 12) | (((uint8_t)s[1] & 0x3F) << 6) |
                     ((uint8_t)s[2] & 0x3F));
    return 3;
  }
  *cp = '?'; return 1;
}

static inline bool i18nIsAsciiOnly(const char* s) {
  for (; *s; ++s) {
    if ((uint8_t)*s >= 0x80) return false;
  }
  return true;
}

static inline int16_t i18nFontHeightOf(uint8_t f) {
  switch (f) {
    case 2:  return 16;
    case 4:  return 26;
    case 6:  return 48;
    case 7:  return 48;
    case 8:  return 75;
    default: return 8;      // font 1
  }
}
static inline int16_t i18nFontWidthOf(uint8_t f) {
  switch (f) {
    case 2:  return 8;
    case 4:  return 12;
    case 6:  return 24;
    case 7:  return 16;
    case 8:  return 38;
    default: return 6;      // font 1
  }
}

// 汉字用哪套字号：字高 >= 16 用 16x16（菜单），否则 12x12（功能内文字）
static inline bool i18nUseGlyph16(int16_t fh) { return fh >= 16 && fh < 75; }

// 是否需要 SPI 事务包裹：屏幕需要（批量写更高效），sprite 不需要。
// TFT_eSprite 没有重写 startWrite/endWrite，照调会落到 TFT_eSPI 的实现上，
// 白白 begin_tft_write()（拉低 CS、锁定事务）再恢复一次 —— 既无意义，
// 也会在别人正占用 SPI 总线时产生干扰。故用编译期 trait 区分。
template <typename Gfx> struct I18nScreenTxn { static const bool value = true;  };
template <>             struct I18nScreenTxn<TFT_eSprite> { static const bool value = false; };

// 画一个汉字：只画前景点（bg == fg 时背景透明），按 size 放大
template <typename Gfx>
static inline void i18nDrawGlyphGx(Gfx& gfx, int16_t x, int16_t y, uint16_t cp,
                                   uint16_t fg, uint16_t bg, int16_t fh,
                                   uint8_t size) {
  const int idx = i18nGlyphIndex(cp);
  if (idx < 0) {
    // 字库未收录：画一个空心方块占位，便于发现漏字
    gfx.drawRect(x + 1, y + 1, 10, 10, fg);
    return;
  }
  const bool big = i18nUseGlyph16(fh);
  const uint8_t* bm = big ? zhFont16[idx] : zhFont12[idx];
  const int w = big ? ZH16_W : ZH12_W;
  const int h = big ? ZH16_H : ZH12_H;
  const int n = w * h;
  const int sc = (size > 4) ? 4 : (size == 0 ? 1 : size);

  if (I18nScreenTxn<Gfx>::value) gfx.startWrite();
  // 先用自己的格子底色清一遍：ASCII 走 drawChar 时会填底色，汉字也必须一样，
  // 否则原地更新的数值文字会残留上一个字的笔画（残留/乱码）。
  if (bg != fg) {
    gfx.fillRect(x, y, w * sc, h * sc, bg);
  }
  for (int i = 0; i < n; ++i) {
    if ((bm[i >> 3] >> (7 - (i & 7))) & 1) {
      const int px = x + (i % w) * sc;
      const int py = y + (i / w) * sc;
      if (sc == 1) {
        gfx.drawPixel(px, py, fg);
      } else {
        gfx.fillRect(px, py, sc, sc, fg);
      }
    }
  }
  if (I18nScreenTxn<Gfx>::value) gfx.endWrite();
}

// 混排绘制：ASCII 走内置字体，汉字走点阵；返回绘制后的 x
template <typename Gfx>
static inline int16_t i18nDrawMixedGx(Gfx& gfx, int16_t x, int16_t y, const char* s,
                                      uint16_t fg, uint16_t bg, int16_t fw,
                                      int16_t fh, uint8_t size) {
  const bool big = i18nUseGlyph16(fh);
  const int gh = big ? ZH16_H : ZH12_H;
  const int gw = big ? ZH16_W : ZH12_W;
  const int sc = (size > 4) ? 4 : (size == 0 ? 1 : size);
  const int16_t dy = (int16_t)((fh - gh) / 2);

  int16_t cx = x;
  while (*s) {
    uint16_t cp;
    const int n = i18nUtf8Next(s, &cp);
    if (cp < 0x80) {
      gfx.drawChar(cx, y, (uint16_t)cp, fg, bg, size);
      cx += (int16_t)(fw * sc);
    } else {
      i18nDrawGlyphGx(gfx, cx, (int16_t)(y + dy), cp, fg, bg, fh, size);
      cx += (int16_t)(gw * sc);
    }
    s += n;
  }
  return cx;
}

static inline int16_t i18nMixedWidth(const char* s, int16_t fw, int16_t fh,
                                     uint8_t size) {
  const bool big = i18nUseGlyph16(fh);
  const int gw = big ? ZH16_W : ZH12_W;
  const int sc = (size > 4) ? 4 : (size == 0 ? 1 : size);
  int16_t w = 0;
  while (*s) {
    uint16_t cp;
    const int n = i18nUtf8Next(s, &cp);
    w += (int16_t)((cp < 0x80 ? fw : gw) * sc);
    s += n;
  }
  return w;
}

// 泛型绘制入口：把中英混排文本画到任意 Gfx（tft 或 TFT_eSprite）。
// 语义与 TFT_eSPI 的 drawString 对齐：x/y 按 datum 解释。
// 纯 ASCII（英文模式或本来就无汉字）时直接转发给目标的 drawString，零开销。
template <typename Gfx>
inline void uiGfxDrawString(Gfx& gfx, const char* s, int32_t x, int32_t y,
                            uint16_t fg, uint16_t bg, uint8_t font = 1,
                            uint8_t datum = TL_DATUM, uint8_t size = 1) {
  if (!s) return;
  s = T(s);
  if (g_lang != LANG_ZH || i18nIsAsciiOnly(s)) {
    gfx.drawString(s, x, y, font);
    return;
  }

  const int16_t fh = i18nFontHeightOf(font);
  const int16_t fw = i18nFontWidthOf(font);
  const int16_t w  = i18nMixedWidth(s, fw, fh, size);
  int32_t dx = 0, dy = 0;
  switch (datum) {
    case TC_DATUM: case MC_DATUM: case BC_DATUM: dx = -(w / 2); break;
    case TR_DATUM: case MR_DATUM: case BR_DATUM: dx = -w;       break;
    default: break;
  }
  switch (datum) {
    case ML_DATUM: case MC_DATUM: case MR_DATUM: dy = -(fh / 2); break;
    case BL_DATUM: case BC_DATUM: case BR_DATUM: dy = -fh;       break;
    default: break;
  }
  i18nDrawMixedGx(gfx, (int16_t)(x + dx), (int16_t)(y + dy), s, fg, bg, fw, fh,
                  size);
}

// ── 镜像 TFT_eSPI 的绘制 API ────────────────────────────────
void uiSetTextFont(uint8_t f);
void uiSetTextSize(uint8_t s);
void uiSetTextDatum(uint8_t datum);
void uiSetCursor(int16_t x, int16_t y);
void uiSetTextColor(uint16_t fg, uint16_t bg);
void uiSetTextColor(uint16_t fg);            // 单参：背景透明（与 TFT_eSPI 语义一致）

void uiPrint(const char* s);
void uiPrint(const String& s);
void uiPrint(char c);
void uiPrint(int v);
void uiPrint(unsigned int v);
void uiPrint(long v);
void uiPrint(unsigned long v);
void uiPrint(long long v);
void uiPrint(unsigned long long v);
void uiPrint(double v);

void uiPrintln(const char* s);
void uiPrintln(const String& s);

// 兜底：TFT_eSPI 继承自 Arduino Print，可接受 IPAddress 等 Printable、
// 以及 print(float, 小数位) 这类调用。这些内容无需翻译，直接转发。
template <typename T> inline void uiPrint(const T& v)   { tft.print(v); }
template <typename T> inline void uiPrintln(const T& v) { tft.println(v); }
inline void uiPrint(double v, int dp)                   { tft.print(v, dp); }
inline void uiPrintln()                                 { tft.println(); }

void uiDrawString(const char* s, int32_t x, int32_t y, uint8_t font = 1);
void uiDrawString(const String& s, int32_t x, int32_t y, uint8_t font = 1);
void uiDrawCentreString(const char* s, int32_t cx, int32_t y, uint8_t font = 1);
void uiDrawCentreString(const String& s, int32_t cx, int32_t y, uint8_t font = 1);
void uiDrawRightString(const char* s, int32_t rx, int32_t y, uint8_t font = 1);
void uiDrawRightString(const String& s, int32_t rx, int32_t y, uint8_t font = 1);

// 中英混排绘制（左上角基准，透明背景）
void    uiTextDraw(int16_t x, int16_t y, const char* s, uint16_t fg);

// 文本像素宽度（按当前字号估算）
int16_t uiTextWidth(const char* s);
int16_t uiTextWidth(const String& s);
int16_t uiTextWidth(const char* s, uint8_t font);
int16_t uiTextWidth(const String& s, uint8_t font);

// 按像素宽度裁剪：**先翻译再裁剪**，且按 UTF-8 字符（不会截出半个汉字），
// 超出时追加 "..."。用于导航栏/列表等空间受限处。
String  uiFitText(const char* s, int16_t maxW);
String  uiFitText(const String& s, int16_t maxW);

// 12px 汉字在 8px 的 font1 行里会**向上溢出 2px**（见 i18n.cpp 的 zhDy()）。
// 因此任何"先 fillRect 清行、再写字"的地方，清屏矩形上边都要上移这么多、
// 高度同步加高 —— 否则会切掉汉字顶部、或留下上一个字的残影（残留/乱码）。
#define UI_CJK_TOP_PAD 3

// 方便宏：把字面量换成译文
#define TT(s) T(s)
