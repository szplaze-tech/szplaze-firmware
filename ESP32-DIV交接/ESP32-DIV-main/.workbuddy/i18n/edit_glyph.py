# -*- coding: utf-8 -*-
"""单字点阵编辑器 —— 直接改 zh_font.cpp 里某一字符的数组。

设计：把 zh_font.cpp 当前内容读成「权威数据」，只改目标字，再写回。
      ⇒ 不会覆盖你手工改过的其它字形；也不会丢字。

用法：
  # 1) 只看某个字（不改）
  python edit_glyph.py 重

  # 2) 用字体重新渲染某个字（可微调偏移），并写回
  python edit_glyph.py 重 --dy -1 --thr 120
  python edit_glyph.py 议 --font C:/Windows/Fonts/simhei.ttf --size 12

  # 3) 直接用手画的字符画覆盖（# = 亮点，. = 空）——最直观的"改数组"
  #    先导出模板：python edit_glyph.py 议 --export art.txt
  #    编辑 art.txt 后回写：python edit_glyph.py 议 --art art.txt

  # 4) 只改 12x12 或只改 16x16
  python edit_glyph.py 议 --size 12 --art art12.txt
"""
import re, os, sys, shutil, argparse

HERE = os.path.dirname(os.path.abspath(__file__))
SKETCH = os.path.join(os.path.dirname(os.path.dirname(HERE)), 'ESP32-DIV')
CPP = os.path.join(SKETCH, 'zh_font.cpp')

FONT_PATH, FONT_INDEX, FONT_THR, REF_CHAR = "C:/Windows/Fonts/simsun.ttc", 0, 128, "国"

# ───────────────────────── 解析现有字库 ─────────────────────────
def parse(src):
    uni_blk = re.search(r'zhUnicode\[ZH_FONT_COUNT\]\s*=\s*\{(.*?)\};', src, re.S).group(1)
    uni = [int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]{4})', uni_blk)]

    def table(name):
        m = re.search(name + r'\[ZH_FONT_COUNT\]\[(\d+)\]\s*=\s*\{(.*?)\n\};', src, re.S)
        bw, body = int(m.group(1)), m.group(2)
        rows = [bytes(int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]{2})', r))
                for r in re.findall(r'\{([^}]*)\}', body)]
        return bw, rows
    bw12, t12 = table('zhFont12')
    bw16, t16 = table('zhFont16')
    return uni, bw12, t12, bw16, t16

def emit(uni, bw12, t12, bw16, t16):
    out = ['#include "zh_font.h"', '']
    out.append('const uint16_t zhUnicode[ZH_FONT_COUNT] = {')
    for i in range(0, len(uni), 12):
        out.append("  " + ", ".join("0x%04X" % u for u in uni[i:i+12]) + ",")
    out.append('};')
    out.append('')
    for name, bw, tab in (('zhFont12', bw12, t12), ('zhFont16', bw16, t16)):
        out.append('const uint8_t %s[ZH_FONT_COUNT][%d] = {' % (name, bw))
        for i, d in enumerate(tab):
            ch = chr(uni[i]) if i < len(uni) else '?'
            out.append("  {" + ",".join("0x%02X" % x for x in d) + "}, // %s" % ch)
        out.append('};')
        out.append('')
    return "\n".join(out)

# ───────────────────────── 位图 <-> 字符画 ─────────────────────────
def art_of(bm, size):
    return ["".join("#" if (bm[(y*size+x) >> 3] >> (7 - ((y*size+x) & 7))) & 1 else "." for x in range(size))
            for y in range(size)]

def bm_of(art, size):
    data = bytearray((size*size + 7)//8)
    for y in range(size):
        line = art[y] if y < len(art) else ""
        for x in range(size):
            c = line[x] if x < len(line) else "."
            if c not in (".", " ", "0"):
                k = y*size + x
                data[k >> 3] |= (0x80 >> (k & 7))
    return bytes(data)

def render(ch, size, font_path, index, thr, dx, dy):
    from PIL import Image, ImageDraw, ImageFont
    font = ImageFont.truetype(font_path, size, index=index)
    ref = font.getbbox(REF_CHAR)
    rw, rh = ref[2]-ref[0], ref[3]-ref[1]
    ox = (size - rw)//2 - ref[0] + dx
    oy = (size - rh)//2 - ref[1] + dy
    img = Image.new("L", (size, size), 0)
    ImageDraw.Draw(img).text((ox, oy), ch, font=font, fill=255)
    px = img.load()
    data = bytearray((size*size + 7)//8)
    for j in range(size):
        for i in range(size):
            if px[i, j] > thr:
                k = j*size + i
                data[k >> 3] |= (0x80 >> (k & 7))
    return bytes(data)

def show(label, bm, size):
    print("--- %s (%dx%d) ---" % (label, size, size))
    for ln in art_of(bm, size):
        print("   " + ln)

# ───────────────────────── 主流程 ─────────────────────────
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('char')
    ap.add_argument('--size', choices=['12', '16', 'both'], default='both')
    ap.add_argument('--font', default=FONT_PATH)
    ap.add_argument('--index', type=int, default=FONT_INDEX)
    ap.add_argument('--thr', type=int, default=FONT_THR)
    ap.add_argument('--dx', type=int, default=0)
    ap.add_argument('--dy', type=int, default=0)
    ap.add_argument('--art', help='用手画的字符画文件覆盖（行数=字号）')
    ap.add_argument('--export', help='把当前点阵导出成字符画模板到该文件')
    ap.add_argument('--show-only', action='store_true')
    a = ap.parse_args()

    if len(a.char) != 1:
        sys.exit("请只传入一个汉字")
    cp = ord(a.char)

    src = open(CPP, encoding='utf-8').read()
    uni, bw12, t12, bw16, t16 = parse(src)
    print("字库共 %d 字；目标 %s U+%04X" % (len(uni), a.char, cp))

    new_char = cp not in uni
    if new_char:
        print("!! 字库未收录，将新增该字（索引会插入到升序位置）")
        idx = 0
        while idx < len(uni) and uni[idx] < cp:
            idx += 1
        uni.insert(idx, cp)
        t12.insert(idx, bytes(bw12))
        t16.insert(idx, bytes(bw16))
    else:
        idx = uni.index(cp)

    cur12, cur16 = t12[idx], t16[idx]

    # 导出模板
    if a.export:
        with open(a.export, 'w', encoding='utf-8') as f:
            if a.size in ('12', 'both'):
                f.write("# %s 12x12：改完后用 --size 12 --art %s 回写\n" % (a.char, os.path.basename(a.export)))
                f.write("\n".join(art_of(cur12, 12)) + "\n")
            if a.size in ('16', 'both'):
                f.write("# %s 16x16\n" % a.char)
                f.write("\n".join(art_of(cur16, 16)) + "\n")
        print("已导出模板: %s" % a.export)
        show("当前 12x12", cur12, 12)
        show("当前 16x16", cur16, 16)
        return

    # 计算新位图
    if a.art:
        lines = [l.rstrip("\n") for l in open(a.art, encoding='utf-8') if not l.startswith('#')]
        lines = [l for l in lines if l.strip() != ""]
        if a.size == '12':
            n12 = bm_of(lines, 12)
            n16 = cur16
        elif a.size == '16':
            n12 = cur12
            n16 = bm_of(lines, 16)
        else:
            n12, n16 = bm_of(lines[:12], 12), bm_of(lines[12:28], 16)
    else:
        n12 = render(a.char, 12, a.font, a.index, a.thr, a.dx, a.dy) if a.size in ('12', 'both') else cur12
        n16 = render(a.char, 16, a.font, a.index, a.thr, a.dx, a.dy) if a.size in ('16', 'both') else cur16

    show("改后 12x12", n12, 12)
    show("改后 16x16", n16, 16)

    if a.show_only:
        print("(--show-only，未写回)")
        return

    t12[idx], t16[idx] = n12, n16
    if len(t12) != len(uni):
        sys.exit("内部错误：表长度不一致")
    shutil.copy(CPP, CPP + ".bak")
    open(CPP, 'w', encoding='utf-8').write(emit(uni, bw12, t12, bw16, t16))
    print("已写回 %s（备份 %s.bak）" % (CPP, os.path.basename(CPP)))

    # 新增字时同步 zh_font.h 的 ZH_FONT_COUNT，避免两处不一致
    if new_char:
        H = os.path.join(SKETCH, 'zh_font.h')
        h = open(H, encoding='utf-8').read()
        h2 = re.sub(r'#define ZH_FONT_COUNT\s+\d+', '#define ZH_FONT_COUNT %d' % len(uni), h)
        if h2 != h:
            shutil.copy(H, H + ".bak")
            open(H, 'w', encoding='utf-8').write(h2)
            print("已同步 ZH_FONT_COUNT=%d 到 zh_font.h" % len(uni))
    print("→ 重新编译烧录即可生效。")

if __name__ == '__main__':
    main()
