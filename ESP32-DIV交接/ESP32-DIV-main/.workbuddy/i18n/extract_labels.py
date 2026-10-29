# -*- coding: utf-8 -*-
"""抽取"通过函数参数传入的 UI 标签字面量"（上次漏掉的一批），并列出尚未翻译的。

覆盖：
  setTouchNavLabels(...) 的所有参数
  FeatureUI::drawButtonRect(..., "label", ...) 的 label 参数
  footerToast("...") / showFeatureUnavailable(..., "...") / drawPanelFrame(..., "...")
  setTouchNavLabel("...") 之类的其它标签函数
"""
import re, glob, os, json, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FILES = sorted(glob.glob(os.path.join(ROOT, 'ESP32-DIV', '*.cpp')) +
               glob.glob(os.path.join(ROOT, 'ESP32-DIV', '*.ino')) +
               glob.glob(os.path.join(ROOT, 'ESP32-DIV', '*.h')))

# 已知翻译表（生成器里的）
ZH_KEYS = set()
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
txt = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'gen_all.py'), encoding='utf-8').read()
m = re.search(r'^ZH = \{(.*?)^\}', txt, re.S | re.M)
for k in re.findall(r'"((?:[^"\\]|\\.)*)"\s*:', m.group(1)):
    ZH_KEYS.add(k.encode().decode('unicode_escape'))

FUNCS = ['setTouchNavLabels', 'drawButtonRect', 'footerToast', 'showFeatureUnavailable',
         'drawPanelFrame', 'setTouchNavLabel', 'drawNavButton', 'drawButton',
         'showToast', 'drawToast', 'featureUnavailable', 'drawHeader', 'drawStatusLine']

def split_args(s):
    """按顶层逗号切分实参，忽略括号/引号内的逗号"""
    out, depth, cur, q = [], 0, '', None
    for ch in s:
        if q:
            cur += ch
            if ch == q:
                q = None
            continue
        if ch in '"\'':
            q = ch; cur += ch; continue
        if ch in '([':
            depth += 1
        elif ch in ')]':
            depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur); cur = ''; continue
        cur += ch
    out.append(cur)
    return out

hits = {}          # literal -> set(callers)
for f in FILES:
    src = open(f, encoding='utf-8', errors='ignore').read()
    for fn in FUNCS:
        for call in re.finditer(r'\b' + fn + r'\s*\(', src):
            # 抓取配对括号内的实参
            i = src.index('(', call.end() - 1)
            depth, j = 0, i
            while j < len(src):
                if src[j] == '(':
                    depth += 1
                elif src[j] == ')':
                    depth -= 1
                    if depth == 0:
                        break
                j += 1
            body = src[i + 1:j]
            # 去掉嵌套的单行注释
            for arg in split_args(body):
                for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', arg):
                    if not lit.strip():
                        continue
                    hits.setdefault(lit, set()).add(fn)

missing = sorted(k for k in hits if k not in ZH_KEYS and re.search(r'[A-Za-z]', k))
print("捕获到标签字面量 %d 个，其中未翻译 %d 个\n" % (len(hits), len(missing)))
print("=== 未翻译（建议补进翻译表）===")
for k in missing:
    print("  %-24s  <- %s" % (k, ",".join(sorted(hits[k]))))

out = os.path.join(ROOT, '.workbuddy', 'i18n', 'missing_labels.txt')
with open(out, 'w', encoding='utf-8') as fh:
    fh.write("# 未翻译的标签字面量（来自函数参数）\n")
    for k in missing:
        fh.write("%s\n" % k)
print("\n已写出: %s" % out)
