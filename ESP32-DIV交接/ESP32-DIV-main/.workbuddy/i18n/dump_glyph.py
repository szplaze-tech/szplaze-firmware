# -*- coding: utf-8 -*-
"""把 zh_font.cpp 里的点阵解回 ASCII 图，用来目视检查字形质量。
用法：python dump_glyph.py 重扫议   [--size 12|16|both]
"""
import re, sys, os

SKETCH = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), 'ESP32-DIV')
SRC = os.path.join(SKETCH, 'zh_font.cpp')

src = open(SRC, encoding='utf-8').read()

# 解析 zhUnicode[...] = { ... };
uni_blk = re.search(r'zhUnicode\[ZH_FONT_COUNT\]\s*=\s*\{(.*?)\};', src, re.S).group(1)
uni = [int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]{4})', uni_blk)]

def parse_table(name):
    m = re.search(name + r'\[ZH_FONT_COUNT\]\[(\d+)\]\s*=\s*\{(.*?)\n\};', src, re.S)
    bw = int(m.group(1)); body = m.group(2)
    rows = re.findall(r'\{([^}]*)\}', body)
    out = []
    for r in rows:
        b = bytes(int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]{2})', r))
        out.append(b)
    return bw, out

BW12, T12 = parse_table('zhFont12')
BW16, T16 = parse_table('zhFont16')
print("解析：zhUnicode=%d 条, zhFont12 每字 %d B, zhFont16 每字 %d B" % (len(uni), BW12, BW16))

def art(bm, size):
    lines = []
    for y in range(size):
        ln = ""
        for x in range(size):
            k = y * size + x
            ln += "#" if (bm[k >> 3] >> (7 - (k & 7))) & 1 else "."
        lines.append(ln)
    return lines

chars = sys.argv[1] if len(sys.argv) > 1 else "重扫议"
which = sys.argv[2] if len(sys.argv) > 2 else "both"

for ch in chars:
    cp = ord(ch)
    if cp not in uni:
        print("!! 字库未收录: %s (U+%04X)" % (ch, cp)); continue
    i = uni.index(cp)
    print("\n" + "=" * 40)
    print("%s  U+%04X  index=%d" % (ch, cp, i))
    if which in ("12", "both"):
        print("--- 12x12 (zhFont12[%d]) ---" % i)
        for l in art(T12[i], 12): print("   " + l)
    if which in ("16", "both"):
        print("--- 16x16 (zhFont16[%d]) ---" % i)
        for l in art(T16[i], 16): print("   " + l)
