#!/usr/bin/env python3
"""Проверка, что все символы строк игры есть в атласе шрифта.

Раньше отсутствующий глиф молча превращался в «?» — так, например,
кнопка магазина показывала «Buy ? 100 Candies». Скрипт сверяет каждый
символ из строковых литералов game/*.ds со списком в ttf_font.c
(латиница 32..126, кириллица А-я, Ё/ё плюс явный список add_utf8).
"""
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def atlas_chars():
    src = open(os.path.join(ROOT, 'ttf_font.c'), encoding='utf-8').read()
    block = re.search(r'if \(!add_utf8\(f,\n(.*?)\)\) \{', src, re.S)
    if not block:
        print('cannot find the add_utf8 list in ttf_font.c', file=sys.stderr)
        sys.exit(2)
    chars = set(chr(c) for c in range(32, 127))
    chars |= set(chr(c) for c in range(0x0410, 0x0450))
    chars |= {'\u0401', '\u0451'}
    for piece in re.findall(r'"([^"]*)"', block.group(1)):
        chars |= set(piece)
    return chars


def main():
    covered = atlas_chars()
    missing = {}
    for path in sorted(glob.glob(os.path.join(ROOT, 'game', '*.ds'))):
        text = open(path, encoding='utf-8').read()
        for literal in re.findall(r'"((?:[^"\\]|\\.)*)"', text):
            for ch in literal:
                if ch not in covered:
                    missing.setdefault(ch, set()).add(os.path.basename(path))
    if missing:
        for ch, files in missing.items():
            print(f"missing glyph {ch!r} U+{ord(ch):04X} used in {', '.join(sorted(files))}")
        print(f"{len(missing)} character(s) would be drawn as '?'", file=sys.stderr)
        return 1
    print(f"font atlas covers every character of game/*.ds ({len(covered)} glyphs listed)")
    return 0


if __name__ == '__main__':
    sys.exit(main())
