# -*- coding: utf-8 -*-
"""对比 12x12 中文点阵：旧方案(黑体缩到12px + 逐字居中) vs 新方案(宋体点阵 + 统一基线)。"""
from PIL import Image, ImageDraw, ImageFont

LINES = ["重放攻击", "未捕获到信号！", "扫描中…", "已存档案：3", "触摸重试或返回", "频率：418.000MHz"]
ASC = ImageFont.truetype("C:/Windows/Fonts/consola.ttf", 8)   # 近似 font1(6x8)

def make(size, font_path, idx, thr, unified):
    f = ImageFont.truetype(font_path, size, index=idx)
    ref = f.getbbox("国")
    rw, rh = ref[2]-ref[0], ref[3]-ref[1]
    ux = (size-rw)//2 - ref[0]
    uy = (size-rh)//2 - ref[1]
    def draw(dr, x, y, s, fg):
        cx = x
        for ch in s:
            if ord(ch) < 128:
                dr.text((cx, y), ch, font=ASC, fill=fg); cx += 6
            else:
                if unified:
                    ox, oy = ux, uy
                else:
                    bb = f.getbbox(ch)
                    gw, gh = bb[2]-bb[0], bb[3]-bb[1]
                    ox = (size-gw)//2 - bb[0]; oy = (size-gh)//2 - bb[1]
                # 逐点判断（与固件一致：阈值二值化，只画亮点）
                img = Image.new("L", (size*2, size*2), 0)
                ImageDraw.Draw(img).text((size//2+ox, size//2+oy), ch, font=f, fill=255)
                px = img.load()
                for j in range(size):
                    for i in range(size):
                        if px[i+size//2-0, j+size//2-0] > thr:
                            dr.point((cx+i, y+j), fill=fg)
                cx += size
    return draw

W, H = 250, 200
panels = [
    ("改造前：黑体@12 + 逐字居中", make(12, "C:/Windows/Fonts/simhei.ttf", 0, 110, False)),
    ("改造后：宋体点阵@12 + 统一基线", make(12, "C:/Windows/Fonts/simsun.ttc", 0, 128, True)),
]
out = Image.new("RGB", (W*2+24, H), (6, 7, 10))
for k, (title, d12) in enumerate(panels):
    p = Image.new("RGB", (W, H), (16, 18, 26))
    dp = ImageDraw.Draw(p)
    dp.rectangle([0, 0, W, 18], fill=(26, 30, 42))
    dp.text((6, 2), title, font=ASC, fill=(200, 200, 210))
    for i, ln in enumerate(LINES):
        d12(dp, 12, 34 + i*26, ln, (225, 225, 235))
    out.paste(p, (k*(W+24), 0))
out = out.resize(((W*2+24)*2, H*2), Image.NEAREST)
out.save("mock_before_after.png")
print("saved: mock_before_after.png")
