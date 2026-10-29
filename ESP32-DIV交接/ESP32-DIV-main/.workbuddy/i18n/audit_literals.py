# -*- coding: utf-8 -*-
"""全量审计：列出所有"未进翻译表"的字符串字面量，按文件分组。
比 extract_labels.py 更彻底——不再依赖函数名白名单（那种做法会因为
jdDrawPanelFrame 之类的命名空间前缀而漏掉，\b 在 "jdDrawPanelFrame" 里
的 'd' 前不成立）。这里直接扫全部字面量，再按规则过滤掉显然不是 UI 的。

用法：python audit_literals.py [--full]
"""
import re, glob, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
SKETCH = os.path.join(ROOT, 'ESP32-DIV')

# 已翻译的键（从生成器里取，含尾随空格处理）
txt = open(os.path.join(HERE, 'gen_all.py'), encoding='utf-8').read()
m = re.search(r'^ZH = \{(.*?)^\}', txt, re.S | re.M)
KNOWN = set(re.findall(r'"((?:[^"\\]|\\.)*)"\s*:', m.group(1)))
KNOWN = {k.encode().decode('unicode_escape') for k in KNOWN}

# 不是 UI 的：格式串 / 路径 / 键名 / 数值单位 / 明显的数据表
SKIP_PAT = [
    r'%', r'^/', r'^\.', r'^[0-9]', r'^#', r'^_', r'^0x',
    r'^[a-z_]+$',                      # 全小写单词（多为 key / 协议标识）
    r'^[\W_]+$',                       # 纯符号
]

FILES = sorted(glob.glob(os.path.join(SKETCH, '*.cpp')) +
               glob.glob(os.path.join(SKETCH, '*.ino')) +
               glob.glob(os.path.join(SKETCH, '*.h')))

per_file = {}
for f in FILES:
    base = os.path.basename(f)
    if base.startswith(('i18n_table', 'zh_font')):
        continue
    src = open(f, encoding='utf-8', errors='ignore').read()
    # 去掉注释，避免把注释里的字面量算进来
    src = re.sub(r'/\*.*?\*/', '', src, flags=re.S)
    src = re.sub(r'//[^\n]*', '', src)
    found = {}
    for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', src):
        if not lit or len(lit) < 2:
            continue
        if not re.search(r'[A-Za-z\u4e00-\u9fff]', lit):
            continue
        if lit in KNOWN:
            continue
        if any(re.search(p, lit) for p in SKIP_PAT):
            continue
        found[lit] = found.get(lit, 0) + 1
    if found:
        per_file[base] = found

total = sum(len(v) for v in per_file.values())
print("未翻译字面量合计 %d 个\n" % total)
for base in sorted(per_file, key=lambda b: -len(per_file[b])):
    items = sorted(per_file[base].items(), key=lambda kv: -kv[1])
    print("=== %s (%d) ===" % (base, len(items)))
    for k, c in items:
        print("   [%2d] %s" % (c, k))
    print()

out = os.path.join(HERE, 'audit_literals.txt')
with open(out, 'w', encoding='utf-8') as fh:
    for base in sorted(per_file):
        fh.write("=== %s ===\n" % base)
        for k in sorted(per_file[base]):
            fh.write("%s\n" % k)
        fh.write("\n")
print("已写出: %s" % out)
