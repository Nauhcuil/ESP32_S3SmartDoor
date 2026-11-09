#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 ui_strings.h 提取所有字符串字面量中出现过的字符。

用法（在工程根目录）：
    python main/ui/gen_font_chars.py

生成：
    main/ui/font_chars.txt    —— 去重后的全部字符（含 ASCII），供人工核对
    main/ui/font_symbols.txt  —— 仅非 ASCII 字符，供 lv_font_conv --symbols 使用

注意：修改 ui_strings.h 后必须重新运行本脚本并重新生成字库。
"""

import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(ROOT, "main", "ui", "ui_strings.h")
OUT_ALL = os.path.join(ROOT, "main", "ui", "font_chars.txt")
OUT_SYMBOLS = os.path.join(ROOT, "main", "ui", "font_symbols.txt")


def main() -> None:
    with open(SRC, "r", encoding="utf-8") as f:
        text = f.read()

    # 去掉注释，避免把注释里的中文算进字库
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)

    # 提取 C 字符串字面量
    literals = re.findall(r'"((?:[^"\\]|\\.)*)"', text)

    chars = set()
    for lit in literals:
        for ch in lit:
            # 跳过转义序列的引导字符与格式占位符中的 ASCII（ASCII 由 --range 覆盖）
            if ch == "\\":
                continue
            chars.add(ch)

    # 仅做展示/转换所需的非 ASCII 字符（中文、中文标点、° 等）
    non_ascii = sorted((c for c in chars if ord(c) > 0x7F), key=ord)
    all_chars = sorted(chars, key=ord)

    with open(OUT_ALL, "w", encoding="utf-8", newline="\n") as f:
        f.write("".join(all_chars))
    with open(OUT_SYMBOLS, "w", encoding="utf-8", newline="\n") as f:
        f.write("".join(non_ascii))

    print("字符串字面量数量: %d" % len(literals))
    print("去重字符总数: %d（其中非 ASCII: %d）" % (len(all_chars), len(non_ascii)))
    print("非 ASCII 字符:")
    print("".join(non_ascii))
    print()
    print("已写入:")
    print("  " + OUT_ALL)
    print("  " + OUT_SYMBOLS)


if __name__ == "__main__":
    main()
