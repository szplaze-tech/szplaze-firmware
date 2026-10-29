# -*- coding: utf-8 -*-
"""全量把 tft 文字 API 替换成 i18n 的 ui 镜像 API。"""
import glob, os

FILES = sorted(glob.glob('ESP32-DIV/*.cpp') + glob.glob('ESP32-DIV/*.ino'))
SKIP = {'ESP32-DIV/i18n.cpp'}          # 自身是转发实现，不能替换

PAIRS = [
    ('tft.println(',          'uiPrintln('),
    ('tft.print(',            'uiPrint('),
    ('tft.setCursor(',        'uiSetCursor('),
    ('tft.setTextColor(',     'uiSetTextColor('),
    ('tft.setTextFont(',      'uiSetTextFont('),
    ('tft.setTextSize(',      'uiSetTextSize('),
    ('tft.setTextDatum(',     'uiSetTextDatum('),
    ('tft.drawString(',       'uiDrawString('),
    ('tft.drawCentreString(', 'uiDrawCentreString('),
    ('tft.drawRightString(',  'uiDrawRightString('),
    ('tft.textWidth(',        'uiTextWidth('),
]

total = {}
for f in FILES:
    key = f.replace(os.sep, '/')
    if key in SKIP:
        continue
    src = open(f, encoding='utf-8', errors='ignore').read()
    n = 0
    for a, b in PAIRS:
        c = src.count(a)
        if c:
            src = src.replace(a, b)
            total[a] = total.get(a, 0) + c
            n += c
    if n:
        open(f, 'w', encoding='utf-8').write(src)
        print("%-24s %d" % (os.path.basename(f), n))

print("\n== 总替换 ==")
for a, b in PAIRS:
    print("  %-24s -> %-24s %d" % (a, b, total.get(a, 0)))
print("合计:", sum(total.values()))
