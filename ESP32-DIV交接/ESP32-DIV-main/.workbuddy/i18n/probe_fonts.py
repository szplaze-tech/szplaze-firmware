# -*- coding: utf-8 -*-
"""对比不同字源在 12px / 16px 下的点阵质量，用于挑选最佳字源。"""
from PIL import Image, ImageDraw, ImageFont

CHARS = "重扫议藏储"
CANDS = [
    ("simsun.ttc#0 @12", "C:/Windows/Fonts/simsun.ttc", 0, 12),
    ("simsun.ttc#0 @16", "C:/Windows/Fonts/simsun.ttc", 0, 16),
    ("simhei @12",       "C:/Windows/Fonts/simhei.ttf", 0, 12),
    ("simhei @16",       "C:/Windows/Fonts/simhei.ttf", 0, 16),
    ("msyh @12",         "C:/Windows/Fonts/msyh.ttc",   0, 12),
    ("msyh @16",         "C:/Windows/Fonts/msyh.ttc",   0, 16),
]

def bitmap(font, ch, size, thr=110):
    img = Image.new("L", (size, size), 0)
    d = ImageDraw.Draw(img)
    bb = font.getbbox(ch)
    gw, gh = bb[2] - bb[0], bb[3] - bb[1]
    ox = (size - gw) // 2 - bb[0]
    oy = (size - gh) // 2 - bb[1]
    d.text((ox, oy), ch, font=font, fill=255)
    px = img.load()
    return [[1 if px[x, y] > thr else 0 for x in range(size)] for y in range(size)]

for label, path, idx, size in CANDS:
    try:
        f = ImageFont.truetype(path, size, index=idx)
    except Exception as e:
        print("SKIP %-18s (%s)" % (label, e)); continue
    print("\n" + "#" * 46)
    print("# %s" % label)
    for ch in CHARS:
        bm = bitmap(f, ch, size)
        print("  --- %s ---" % ch)
        for row in bm:
            print("    " + "".join("#" if v else "." for v in row))
