# -*- coding: utf-8 -*-
"""SubGHz「干扰检测」界面：汉化 + 布局调整 前后对比（按真机几何 240x320）。

改造要点：
  · 面板标题 Monitor -> 监视器（标题区 14 -> 17px，留出 12px 汉字高度）
  · 行高 12 -> 14px（12px 汉字竖直居中不挤）
  · 第 2 列值 x 162 -> 168（避开「占空比」3 字 36px 的右边界 164，原来会贴上「比」）
  · 状态框 30 -> 26px，把高度还给波形区
  · 状态文字 CLEAR/ACTIVITY/JAMMING DETECTED -> 无干扰/有活动/检测到干扰
  · 网格标题 Signal -> 信号；Log 的 on/off -> 开/关
"""
from PIL import Image, ImageDraw, ImageFont

W, H = 240, 320
ASC   = ImageFont.truetype("C:/Windows/Fonts/consola.ttf", 8)
ASC2  = ImageFont.truetype("C:/Windows/Fonts/consola.ttf", 16)
ZH    = ImageFont.truetype("C:/Windows/Fonts/simsun.ttc", 12, index=0)
_ref  = ZH.getbbox("国")
_ux = (12 - (_ref[2]-_ref[0]))//2 - _ref[0]
_uy = (12 - (_ref[3]-_ref[1]))//2 - _ref[1]

BG, FG, DIM, LINE, ACC = (0, 0, 0), (235, 235, 240), (120, 124, 136), (60, 64, 76), (120, 190, 250)

def txt(d, x, y, s, color, fh=8):
    """按 font1(6px ASCII / 12px 汉字, 统一基线) 绘制，只画亮点（透明背景）"""
    dy = (fh - 12)//2
    for ch in s:
        if ord(ch) < 128:
            d.text((x, y), ch, font=ASC, fill=color); x += 6
        else:
            img = Image.new("L", (12, 12), 0)
            ImageDraw.Draw(img).text((_ux, _uy), ch, font=ZH, fill=255)
            px = img.load()
            for j in range(12):
                for i in range(12):
                    if px[i, j] > 128:
                        d.point((x+i, y+dy+j), fill=color)
            x += 12

def tw(s, fh=8):
    return sum(6 if ord(c) < 128 else 12 for c in s)

def panel(zh):
    im = Image.new("RGB", (W, H), BG); d = ImageDraw.Draw(im)
    # 顶部栏
    d.rectangle([0, 0, W-1, 35], fill=(10, 12, 16)); d.line([0, 35, W-1, 35], fill=LINE)
    txt(d, 6, 6, "干扰检测" if zh else "Jamming Detector", FG)
    txt(d, 6, 20, "SubGHz", DIM)

    kJdPad, kJdPanelW, kJdRadius = 4, 232, 3
    infoY, infoH, header, lineH = 40, (52 if zh else 46), (17 if zh else 14), (14 if zh else 12)
    row1 = infoY + header + (3 if zh else 2)
    row2 = row1 + lineH + 2
    statusY, statusH = infoY + infoH + 6, (26 if zh else 30)
    waveY = statusY + statusH + 6

    # 信息面板
    d.rounded_rectangle([kJdPad, infoY, kJdPad+kJdPanelW, infoY+infoH], kJdRadius, outline=LINE)
    txt(d, 10, infoY + 3, "监视器" if zh else "Monitor", DIM)

    col2LabelX, col2ValueX = 128, (168 if zh else 162)
    if zh:
        rows = [(10, row1, "频率", 44, "418.000 MHz"),
                (10, row2, "RSSI", 44, "-72/-95 dBm"),
                (col2LabelX, row1, "占空比", col2ValueX, "58% E:3"),
                (col2LabelX, row2, "记录", col2ValueX, "开")]
    else:
        rows = [(10, row1, "Freq", 44, "418.000 MHz"),
                (10, row2, "RSSI", 44, "-72/-95 dBm"),
                (col2LabelX, row1, "Duty", col2ValueX, "58% E:3"),
                (col2LabelX, row2, "Log", col2ValueX, "on")]
    for lx, ly, lab, vx, val in rows:
        txt(d, lx, ly, lab, DIM)
        txt(d, vx, ly, val, FG)

    # 状态框
    bgc = (200, 120, 30)
    d.rounded_rectangle([kJdPad, statusY, kJdPad+kJdPanelW, statusY+statusH], kJdRadius, fill=bgc, outline=LINE)
    st = "检测到干扰" if zh else "JAMMING DETECTED"
    sz = 2
    if tw(st) * sz > kJdPanelW - 8: sz = 1
    # 居中：按 textSize 缩放绘制
    big = Image.new("L", (tw(st)*sz, 12*sz), 0)
    tmp = ImageDraw.Draw(big)
    txt(tmp, 0, 0, st, 255)
    big = big.resize((tw(st)*sz, 11*sz), Image.NEAREST)
    im.paste(Image.new("RGB", big.size, (0, 0, 0)), (0, 0))  # 占位避免 lint
    bx = kJdPad + (kJdPanelW - tw(st)*sz)//2
    by = statusY + (statusH - 11*sz)//2
    px = big.load()
    for j in range(big.height):
        for i in range(big.width):
            if px[i, j] > 128:
                d.point((bx+i, by+j), fill=(0, 0, 0))

    # 波形面板
    waveH = 290 - waveY
    d.rounded_rectangle([kJdPad, waveY, kJdPad+kJdPanelW, waveY+waveH], kJdRadius, outline=LINE)
    txt(d, 10, waveY + 3, "信号" if zh else "Signal", DIM)
    plotTop = waveY + 16
    plotH = waveH - 20
    plotX, plotW = kJdPad + 4, kJdPanelW - 8
    for g in range(1, 4):
        d.line([plotX+1, plotTop + plotH*g//4, plotX+plotW-1, plotTop + plotH*g//4], fill=(38, 40, 48))
        d.line([plotX + plotW*g//4, plotTop+1, plotX + plotW*g//4, plotTop+plotH-1], fill=(38, 40, 48))
    import math
    prev = None
    for x in range(plotW):
        t = x / 18.0
        v = -78 + 18*math.sin(t) + (10 if (x // 26) % 2 else 0)
        y = plotTop + int(((-35 - v) / 65.0) * plotH)
        y = max(plotTop+1, min(plotTop+plotH-1, y))
        if prev is not None:
            d.line([plotX+x-1, prev, plotX+x, y], fill=ACC)
        prev = y
    return im

a = panel(False).resize((W*2, H*2), Image.NEAREST)
b = panel(True).resize((W*2, H*2), Image.NEAREST)
out = Image.new("RGB", (W*4 + 30, H*2), (6, 7, 10))
out.paste(a, (0, 0)); out.paste(b, (W*2 + 30, 0))
out.save("mock_jdetector.png")
print("saved: mock_jdetector.png  (左=改造前 / 右=改造后)")
