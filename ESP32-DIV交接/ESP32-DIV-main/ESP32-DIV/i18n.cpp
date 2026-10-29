// i18n.cpp —— 运行时中英文切换 + 中文点阵绘制实现
#include "i18n.h"
#include "utils.h"          // TFT_eSPI tft
#include "zh_font.h"
#include "i18n_table.h"     // 自动生成：kI18n[]
#include <string.h>
#include <stdio.h>

uint8_t g_lang = LANG_EN;

// ── T()：英文模式直接返回原指针（零开销）；中文模式查表 ──
const char* T(const char* en) {
  if (!en) return en;
  if (g_lang != LANG_ZH) return en;

  const char c0 = en[0];

  // 1) 精确匹配（含表里带尾随空格的条目）
  for (size_t i = 0; i < kI18nCount; ++i) {
    if (kI18n[i].en[0] == c0 && strcmp(kI18n[i].en, en) == 0) {
      return kI18n[i].zh;
    }
  }

  // 2) 尾随空白不敏感：代码里常用 "Idle   " / "MAN " 这类尾部补空格做列对齐，
  //    而翻译表里只存 "Idle" / "MAN"，这里再比一次。
  size_t n = strlen(en);
  while (n > 0 && (en[n - 1] == ' ' || en[n - 1] == '\t')) --n;
  if (n == 0 || n == strlen(en)) {
    return en;                       // 无尾随空白 → 上面已精确比过
  }
  for (size_t i = 0; i < kI18nCount; ++i) {
    if (kI18n[i].en[0] == c0 &&
        strncmp(kI18n[i].en, en, n) == 0 && kI18n[i].en[n] == '\0') {
      return kI18n[i].zh;
    }
  }
  return en;  // 未收录：原样返回
}

// ── 当前文字状态（镜像 TFT_eSPI） ──────────────────────────
static uint8_t  s_font   = 1;
static int16_t  s_fontH  = 8;
static int16_t  s_fontW  = 6;
static uint8_t  s_size   = 1;
static uint8_t  s_datum  = TL_DATUM;
static int16_t  s_curX   = 0;
static int16_t  s_curY   = 0;
static uint16_t s_fg     = TFT_WHITE;
static uint16_t s_bg     = TFT_BLACK;

// 本文件内部沿用原签名的转发：真正的实现已统一到 i18n.h 的泛型内核
// （i18nFontHeightOf / i18nMixedWidth / i18nDrawMixedGx ……），
// 这里只把目标固定为 tft、并补上本文件的字号状态 s_size。
static inline int16_t fontHeightOf(uint8_t f) { return i18nFontHeightOf(f); }
static inline int16_t fontWidthOf(uint8_t f)  { return i18nFontWidthOf(f); }
static inline bool     useGlyph16(int16_t fh) { return i18nUseGlyph16(fh); }
static inline bool     isAsciiOnly(const char* s) { return i18nIsAsciiOnly(s); }
static inline int      utf8Next(const char* s, uint16_t* cp) { return i18nUtf8Next(s, cp); }

static inline int16_t mixedWidth(const char* s, int16_t fw, int16_t fh) {
  return i18nMixedWidth(s, fw, fh, s_size);
}

static inline int16_t drawMixed(int16_t x, int16_t y, const char* s, uint16_t fg,
                                uint16_t bg, int16_t fw, int16_t fh) {
  return i18nDrawMixedGx(tft, x, y, s, fg, bg, fw, fh, s_size);
}

// ── 镜像 TFT_eSPI 的状态设置 ────────────────────────────────
void uiSetTextFont(uint8_t f) {
  s_font  = f;
  s_fontH = fontHeightOf(f);
  s_fontW = fontWidthOf(f);
  tft.setTextFont(f);
}
void uiSetTextSize(uint8_t s) {
  s_size = (s == 0) ? 1 : s;
  tft.setTextSize(s);
}
void uiSetTextDatum(uint8_t datum) {
  s_datum = datum;
  tft.setTextDatum(datum);
}
void uiSetCursor(int16_t x, int16_t y) {
  s_curX = x;
  s_curY = y;
  tft.setCursor(x, y);
}
void uiSetTextColor(uint16_t fg, uint16_t bg) {
  s_fg = fg;
  s_bg = bg;
  tft.setTextColor(fg, bg);
}
void uiSetTextColor(uint16_t fg) {
  s_fg = fg;
  s_bg = fg;              // 与 TFT_eSPI 一致：fg==bg 表示背景透明
  tft.setTextColor(fg);
}

// ── print 家族 ──────────────────────────────────────────────
void uiPrint(const char* s) {
  if (!s) return;
  s = T(s);
  if (g_lang != LANG_ZH || isAsciiOnly(s)) {
    tft.print(s);                        // 英文模式 / 纯 ASCII：行为与原来一致
    return;
  }
  s_curX = drawMixed(s_curX, s_curY, s, s_fg, s_bg, s_fontW, s_fontH);
  tft.setCursor(s_curX, s_curY);
}
void uiPrint(const String& s) { uiPrint(s.c_str()); }
void uiPrint(char c)          { char b[2] = { c, 0 }; uiPrint(b); }

void uiPrint(int v)               { char b[16]; snprintf(b, sizeof(b), "%d", v);   uiPrint(b); }
void uiPrint(unsigned int v)      { char b[16]; snprintf(b, sizeof(b), "%u", v);   uiPrint(b); }
void uiPrint(long v)              { char b[24]; snprintf(b, sizeof(b), "%ld", v);  uiPrint(b); }
void uiPrint(unsigned long v)     { char b[24]; snprintf(b, sizeof(b), "%lu", v);  uiPrint(b); }
void uiPrint(long long v)         { char b[28]; snprintf(b, sizeof(b), "%lld", v); uiPrint(b); }
void uiPrint(unsigned long long v){ char b[28]; snprintf(b, sizeof(b), "%llu", v); uiPrint(b); }
void uiPrint(double v)            { char b[28]; snprintf(b, sizeof(b), "%.2f", v); uiPrint(b); }

void uiPrintln(const char* s) {
  if (!s) { tft.println(); return; }
  const char* t = T(s);
  if (g_lang != LANG_ZH || isAsciiOnly(t)) { tft.println(t); return; }
  drawMixed(s_curX, s_curY, t, s_fg, s_bg, s_fontW, s_fontH);
  s_curX = 0;                            // 与 TFT_eSPI 的 println 一致：回到行首、下移一行
  s_curY = (int16_t)(s_curY + s_fontH * s_size);
  tft.setCursor(s_curX, s_curY);
}
void uiPrintln(const String& s) { uiPrintln(s.c_str()); }

// ── drawString 家族（按当前 datum 对齐）──────────────────────
void uiDrawString(const char* s, int32_t x, int32_t y, uint8_t font) {
  if (!s) return;
  s = T(s);
  if (g_lang != LANG_ZH || isAsciiOnly(s)) { tft.drawString(s, x, y, font); return; }

  const int16_t fh = fontHeightOf(font);
  const int16_t fw = fontWidthOf(font);
  const int16_t w  = mixedWidth(s, fw, fh);
  int32_t dx = 0, dy = 0;
  switch (s_datum) {
    case TC_DATUM: case MC_DATUM: case BC_DATUM: dx = -(w / 2); break;
    case TR_DATUM: case MR_DATUM: case BR_DATUM: dx = -w;       break;
    default: break;
  }
  switch (s_datum) {
    case ML_DATUM: case MC_DATUM: case MR_DATUM: dy = -(fh / 2); break;
    case BL_DATUM: case BC_DATUM: case BR_DATUM: dy = -fh;       break;
    default: break;
  }
  drawMixed((int16_t)(x + dx), (int16_t)(y + dy), s, s_fg, s_bg, fw, fh);
}
void uiDrawString(const String& s, int32_t x, int32_t y, uint8_t font) {
  uiDrawString(s.c_str(), x, y, font);
}
void uiDrawCentreString(const char* s, int32_t cx, int32_t y, uint8_t font) {
  if (!s) return;
  s = T(s);
  if (g_lang != LANG_ZH || isAsciiOnly(s)) { tft.drawCentreString(s, cx, y, font); return; }
  const int16_t fh = fontHeightOf(font);
  const int16_t w  = mixedWidth(s, fontWidthOf(font), fh);
  drawMixed((int16_t)(cx - w / 2), (int16_t)y, s, s_fg, s_bg, fontWidthOf(font), fh);
}
void uiDrawCentreString(const String& s, int32_t cx, int32_t y, uint8_t font) {
  uiDrawCentreString(s.c_str(), cx, y, font);
}
void uiDrawRightString(const char* s, int32_t rx, int32_t y, uint8_t font) {
  if (!s) return;
  s = T(s);
  if (g_lang != LANG_ZH || isAsciiOnly(s)) { tft.drawRightString(s, rx, y, font); return; }
  const int16_t fh = fontHeightOf(font);
  const int16_t w  = mixedWidth(s, fontWidthOf(font), fh);
  drawMixed((int16_t)(rx - w), (int16_t)y, s, s_fg, s_bg, fontWidthOf(font), fh);
}
void uiDrawRightString(const String& s, int32_t rx, int32_t y, uint8_t font) {
  uiDrawRightString(s.c_str(), rx, y, font);
}

// ── 独立绘制 / 测量 ────────────────────────────────────────
void uiTextDraw(int16_t x, int16_t y, const char* s, uint16_t fg) {
  if (!s) return;
  drawMixed(x, y, s, fg, fg, s_fontW, s_fontH);
}

int16_t uiTextWidth(const char* s) {
  if (!s) return 0;
  return mixedWidth(T(s), s_fontW, s_fontH);
}
int16_t uiTextWidth(const String& s) { return uiTextWidth(s.c_str()); }
int16_t uiTextWidth(const char* s, uint8_t font) {
  if (!s) return 0;
  return mixedWidth(T(s), fontWidthOf(font), fontHeightOf(font));
}
int16_t uiTextWidth(const String& s, uint8_t font) { return uiTextWidth(s.c_str(), font); }

// 按像素宽度裁剪（先翻译 → 再按 UTF-8 整字裁剪 → 追加 "..."）
String uiFitText(const char* s, int16_t maxW) {
  if (!s) return String();
  const char* t = T(s);
  if (mixedWidth(t, s_fontW, s_fontH) <= maxW) return String(t);

  const int sc   = (s_size > 4) ? 4 : s_size;
  const int gw   = useGlyph16(s_fontH) ? ZH16_W : ZH12_W;
  const int16_t ellW = (int16_t)(s_fontW * 3 * sc);        // "..." 的宽度

  String out;
  int16_t w = 0;
  while (*t) {
    uint16_t cp;
    const int n = utf8Next(t, &cp);
    const int16_t cw = (int16_t)((cp < 0x80 ? s_fontW : gw) * sc);
    if ((int32_t)w + cw > (int32_t)maxW - ellW) break;
    for (int i = 0; i < n; ++i) out += t[i];                // 整字拷贝，绝不切断 UTF-8
    w += cw;
    t += n;
  }
  out += "...";
  return out;
}
String uiFitText(const String& s, int16_t maxW) { return uiFitText(s.c_str(), maxW); }
