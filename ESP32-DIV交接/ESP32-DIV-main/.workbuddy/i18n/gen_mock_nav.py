# -*- coding: utf-8 -*-
"""导航栏 / 分页按钮 汉化前后对比模拟图（按真机几何：cell=48px，栏高 24px）。"""
from PIL import Image, ImageDraw, ImageFont

W, CELL, BARH = 240, 48, 24
ASC = ImageFont.truetype("C:/Windows/Fonts/consola.ttf", 8)
ZH  = ImageFont.truetype("C:/Windows/Fonts/simsun.ttc", 12, index=0)
_ref = ZH.getbbox("国")
_ux = (12 - (_ref[2]-_ref[0]))//2 - _ref[0]
_uy = (12 - (_ref[3]-_ref[1]))//2 - _ref[1]

BG, FG, LINE, ICON = (16, 18, 26), (235, 235, 240), (60, 64, 76), (120, 190, 250)

def text_w(s, zh):
    return sum((6 if ord(c) < 128 else 12) for c in s) if zh else sum(6 for _ in s)

def draw_text(dr, cx, cy, s, zh, color):
    """MC_DATUM 居中；fg==bg 时的透明绘制用逐点方式模拟"""
    w = text_w(s, zh)
    x = cx - w//2
    fh = 8
    y = cy - fh//2                      # font1 的 MC_DATUM
    for ch in s:
        if ord(ch) < 128:
            dr.text((x, y), ch, font=ASC, fill=color); x += 6
        else:
            dy = (fh - 12)//2
            img = Image.new("L", (12, 12), 0)
            ImageDraw.Draw(img).text((_ux, _uy), ch, font=ZH, fill=255)
            px = img.load()
            for j in range(12):
                for i in range(12):
                    if px[i, j] > 128:
                        dr.point((x+i, y+dy+j), fill=color)
            x += 12

def panel(title, rows, zh):
    h = 22 + len(rows) * (BARH + 10) + 10
    im = Image.new("RGB", (W, h), BG); d = ImageDraw.Draw(im)
    d.text((6, 5), title, font=ASC, fill=(200, 200, 210))
    y = 22
    for labels in rows:
        barY = y + 10
        d.rectangle([0, barY, W, barY+BARH], fill=(28, 31, 42))
        d.line([0, barY, W, barY], fill=LINE)
        for i in range(5):
            cx = i * CELL + CELL//2
            if i:
                d.line([i*CELL, barY+3, i*CELL, barY+BARH-3], fill=LINE)
            lb = labels[i]
            if lb:
                draw_text(d, cx, barY + BARH//2, lb, zh, ICON if i == 2 else FG)
            else:
                d.ellipse([cx-2, barY+BARH//2-2, cx+2, barY+BARH//2+2], outline=ICON)
        y = barY + BARH + 10
    return im, h

nav_sets = [
    ["Mode-", None, "Exit", "Toggle", "Mode+"],
    ["Prev", "Arm", "Exit", "Play", "Next"],
    ["Scan", "Next", "Exit", "Prev", "View"],
]
nav_sets_zh = [
    ["模式-", None, "退出", "切换", "模式+"],
    ["上页", "布防", "退出", "播放", "下页"],
    ["扫描", "下页", "退出", "上页", "查看"],
]
page_sets    = [["Back", None, "Main Menu", None, "Next Page"]]
page_sets_zh = [["返回", None, "主菜单", None, "下一页"]]

a, ha = panel("BEFORE (English)", nav_sets, False)
b, hb = panel("AFTER (中文)", nav_sets_zh, True)
pa, hpa = panel("BEFORE", page_sets, False)
pb, hpb = panel("AFTER", page_sets_zh, True)
H = ha + pa.height + 20
out = Image.new("RGB", (W*2 + 24, H), (6, 7, 10))
out.paste(a, (0, 0)); out.paste(b, (W+24, 0))
out.paste(pa, (0, ha + 10)); out.paste(pb, (W+24, ha + 10))
out = out.resize(((W*2+24)*2, H*2), Image.NEAREST)
out.save("mock_navbar_zh.png")
print("saved: mock_navbar_zh.png")
