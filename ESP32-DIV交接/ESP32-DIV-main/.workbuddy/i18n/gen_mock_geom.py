# -*- coding: utf-8 -*-
"""图示两个根因（注释用宋体画，避免默认字体无汉字）。
左：垂直清屏范围不足 → 残留；右：水平清屏越界 → 遮字。"""
from PIL import Image, ImageDraw, ImageFont

ZH12 = ImageFont.truetype("C:/Windows/Fonts/simsun.ttc", 12, index=0)
ZH11 = ImageFont.truetype("C:/Windows/Fonts/simsun.ttc", 11, index=0)
ASC  = ImageFont.truetype("C:/Windows/Fonts/consola.ttf", 8)
_ref = ZH12.getbbox("国")
_ux = (12 - (_ref[2]-_ref[0]))//2 - _ref[0]
_uy = (12 - (_ref[3]-_ref[1]))//2 - _ref[1]

def ctext(d, x, y, s, color, size=11):
    """中文用宋体、ASCII 用等宽，基线对齐"""
    f = ZH11 if size == 11 else ZH12
    for ch in s:
        if ord(ch) < 128:
            d.text((x, y), ch, font=ASC, fill=color); x += 6
        else:
            bb = f.getbbox(ch)
            gw, gh = bb[2]-bb[0], bb[3]-bb[1]
            ox = (f.size - gw)//2 - bb[0]
            d.text((x + ox, y - 1), ch, font=f, fill=color)
            x += f.size
    return x

def glyph(d, x, y, ch, color):
    img = Image.new("L", (12, 12), 0)
    ImageDraw.Draw(img).text((_ux, _uy), ch, font=ZH12, fill=255)
    px = img.load()
    for j in range(12):
        for i in range(12):
            if px[i, j] > 128:
                d.point((x+i, y-2+j), fill=color)

W, H = 320, 260
im = Image.new("RGB", (W, H), (14, 15, 20)); d = ImageDraw.Draw(im)

ctext(d, 8, 6, "垂直：清屏矩形从 y 起只有 11px，汉字占 y-2 ~ y+10", (215, 215, 225))

# ── 上：改造前 ──
y = 34
d.rectangle([16, y, 150, y+11], outline=(200, 60, 60))
glyph(d, 22, y, "协", (235, 235, 240))
glyph(d, 36, y, "议", (235, 235, 240))
glyph(d, 50, y, "：", (235, 235, 240))
ctext(d, 170, y-2, "顶部 2 行清不到 → 残留", (240, 120, 120))
ctext(d, 16, y+18, "改造前：y .. y+11", (190, 190, 205))

# ── 下：改造后 ──
y = 92
d.rectangle([16, y-3, 150, y+11], outline=(80, 200, 120))
glyph(d, 22, y, "协", (235, 235, 240))
glyph(d, 36, y, "议", (235, 235, 240))
glyph(d, 50, y, "：", (235, 235, 240))
ctext(d, 170, y-4, "y-3 .. y+11 全覆盖 ✓", (120, 220, 140))
ctext(d, 16, y+18, "改造后：y-3 .. y+11", (190, 190, 205))

d.line([8, 148, W-8, 148], fill=(50, 54, 64))
ctext(d, 8, 156, "水平：清屏宽度按英文标签算，越界压住中文标签", (215, 215, 225))

# ── 上：越界 ──
y = 186
d.text((16, y-2), "Freq:", font=ASC, fill=(190, 190, 200))
d.rectangle([52, y-3, 164, y+11], outline=(200, 60, 60))
ctext(d, 170, y-2, "值格 40..136 越界", (240, 120, 120))
glyph(d, 158, y, "模", (235, 235, 240))
glyph(d, 172, y, "式", (235, 235, 240))
ctext(d, 16, y+18, "改造前：「模」左 6 列被抹掉", (240, 120, 120))

# ── 下：不越界 ──
y = 232
d.text((16, y-2), "Freq:", font=ASC, fill=(190, 190, 200))
d.rectangle([52, y-3, 154, y+11], outline=(80, 200, 120))
ctext(d, 170, y-2, "值格 40..126 不越界 ✓", (120, 220, 140))
glyph(d, 158, y, "模", (235, 235, 240))
glyph(d, 172, y, "式", (235, 235, 240))

im.resize((W*3, H*3), Image.NEAREST).save("mock_cell_geometry.png")
print("saved: mock_cell_geometry.png")
