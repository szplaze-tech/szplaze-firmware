# -*- coding: utf-8 -*-
"""把"先 fillRect 清行、再写文字"的清屏矩形统一上移并加高（见 UI_CJK_TOP_PAD）。

只做精确文本替换，不做正则猜测；未命中的会打印出来以便人工核对。
"""
import os

PATCHES = [
    ('ESP32-DIV/ir.cpp',
     'tft.fillRect(8, y, tft.width() - 16, kIrLineHeight, TFT_BLACK);',
     'tft.fillRect(8, y - UI_CJK_TOP_PAD, tft.width() - 16,\n                kIrLineHeight + UI_CJK_TOP_PAD, TFT_BLACK);'),
    ('ESP32-DIV/rfid.cpp',
     'tft.fillRect(8, y, TFT_WIDTH - 16, kRfidLineHeight, TFT_BLACK);',
     'tft.fillRect(8, y - UI_CJK_TOP_PAD, TFT_WIDTH - 16,\n                kRfidLineHeight + UI_CJK_TOP_PAD, TFT_BLACK);'),
    ('ESP32-DIV/subghz.cpp',
     'tft.fillRect(kValueX, y, kValueW, kLineH - 1, TFT_BLACK);',
     'tft.fillRect(kValueX, y - UI_CJK_TOP_PAD, kValueW,\n                  kLineH - 1 + UI_CJK_TOP_PAD, TFT_BLACK);'),
    ('ESP32-DIV/subghz.cpp',
     'tft.fillRect(kLabelX, y, kValueX - kLabelX - 2, kLineH - 1, TFT_BLACK);',
     'tft.fillRect(kLabelX, y - UI_CJK_TOP_PAD, kValueX - kLabelX - 2,\n                  kLineH - 1 + UI_CJK_TOP_PAD, TFT_BLACK);'),
    ('ESP32-DIV/subghz.cpp',
     'tft.fillRect(kLabelX, statusY, kPanelW - 16, kLineH - 1, TFT_BLACK);',
     'tft.fillRect(kLabelX, statusY - UI_CJK_TOP_PAD, kPanelW - 16,\n                  kLineH - 1 + UI_CJK_TOP_PAD, TFT_BLACK);'),
    ('ESP32-DIV/subghz.cpp',
     'tft.fillRect(kLabelX, countY, kPanelW - 16, kLineH - 1, TFT_BLACK);',
     'tft.fillRect(kLabelX, countY - UI_CJK_TOP_PAD, kPanelW - 16,\n                  kLineH - 1 + UI_CJK_TOP_PAD, TFT_BLACK);'),
]

for path, old, new in PATCHES:
    src = open(path, encoding='utf-8').read()
    if old not in src:
        print("MISS  %-22s %s" % (os.path.basename(path), old[:52]))
        continue
    open(path, 'w', encoding='utf-8').write(src.replace(old, new, 1))
    print("OK    %-22s %s" % (os.path.basename(path), old[:52]))
