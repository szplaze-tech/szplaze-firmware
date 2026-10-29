# -*- coding: utf-8 -*-
"""用与 zh_font.h 相同的参数渲染两套字号的真机布局模拟图。"""
from PIL import Image, ImageDraw, ImageFont

FONT_PATH = "C:/Windows/Fonts/simhei.ttf"
ASC = ImageFont.truetype("C:/Windows/Fonts/consola.ttf", 14)   # 近似 font2(8x16) ASCII

def render(size, ascii_adv, draw_fn):
    """size: 汉字点阵字号; ascii_adv: ASCII 每字符步进"""
    f = ImageFont.truetype(FONT_PATH, size)
    def draw(dr, x, y, s, fg, fh):
        dy = (fh - size) // 2
        cx = x
        for ch in s:
            if ord(ch) < 128:
                dr.text((cx, y), ch, font=ASC, fill=fg)
                cx += ascii_adv
            else:
                bb = f.getbbox(ch)
                gw, gh = bb[2] - bb[0], bb[3] - bb[1]
                ox = (size - gw) // 2 - bb[0]
                oy = (size - gh) // 2 - bb[1]
                dr.text((cx + ox, y + dy + oy), ch, font=f, fill=fg)
                cx += size
        return cx
    return draw

# ── 图 1：子菜单，16×16（font2 行高 16、行距 30）──
W, H = 240, 250
a = Image.new("RGB", (W, H), (16, 18, 26))
da = ImageDraw.Draw(a)
da.rectangle([0, 0, W, 20], fill=(26, 30, 42))
da.text((6, 2), "[IP5306] 58%", font=ASC, fill=(120, 200, 140))
d16 = render(16, 8, None)
for i, it in enumerate(["无线扫描", "协议干扰", "ESB嗅探", "ESB重放", "鼠标注入", "鼠标扫描", "返回主菜单"]):
    y = 30 + i * 30
    da.rectangle([10, y, 26, y + 16], outline=(150, 150, 165))
    da.text((30, y + 1), "|", font=ASC, fill=(230, 230, 235))
    d16(da, 40, y, it, (235, 235, 240), 16)

# ── 图 2：功能内界面，12×12（font1 行高 8，ASCII 步进 6）──
b = Image.new("RGB", (W, H), (16, 18, 26))
db = ImageDraw.Draw(b)
db.rectangle([0, 0, W, 20], fill=(26, 30, 42))
db.text((6, 2), "[IP5306] 58%", font=ASC, fill=(120, 200, 140))
d12 = render(12, 6, None)
lines = [
    (12, "重放攻击", 0),
    (36, "频率：418.000MHz", 0),
    (52, "协议：1  位长：24", 0),
    (68, "RSSI：-100 dBm", 0),
    (84, "值：0x1A2B3C", 0),
    (116, "未捕获到信号！", 0),
    (140, "扫描中…", 0),
    (164, "已存档案：3", 0),
    (188, "触摸重试或返回", 0),
]
small = ImageFont.truetype("C:/Windows/Fonts/consola.ttf", 8)
for y, txt, _ in lines:
    d12(db, 12, y, txt, (220, 220, 230), 8)

out = Image.new("RGB", (W * 2 + 30, H), (6, 7, 10))
out.paste(a, (0, 0))
out.paste(b, (W + 30, 0))
out.save("mock_both_sizes.png")
out.resize(((W * 2 + 30) * 2, H * 2), Image.NEAREST).save("mock_both_sizes_2x.png")
print("saved: mock_both_sizes_2x.png  (左=菜单16x16 / 右=功能内12x12)")
