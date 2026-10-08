#!/usr/bin/env python3
"""Генерирует сглаженные шрифты VLW (формат M5GFX/TFT_eSPI) с кириллицей в fonts.h."""
import struct, os
from PIL import ImageFont

DIR = "/usr/share/fonts/truetype/dejavu/"
TEXT = ([chr(c) for c in range(0x21, 0x7F)] + ["°", "Ё", "ё", "—", "«", "»", "·"]
        + [chr(c) for c in range(0x410, 0x450)])
DIGITS = list("+-−0123456789°.,")

FONTS = [  # имя, файл, размер, набор символов
    ("FontR18", "DejaVuSans.ttf", 18, TEXT),
    ("FontR20", "DejaVuSans.ttf", 20, TEXT),
    ("FontR24", "DejaVuSans.ttf", 24, TEXT),
    ("FontR28", "DejaVuSans.ttf", 28, TEXT),
    ("FontB28", "DejaVuSans-Bold.ttf", 28, TEXT),
    ("FontB36", "DejaVuSans-Bold.ttf", 36, TEXT),
    ("FontBig", "DejaVuSans-Bold.ttf", 110, DIGITS),
]


def make_vlw(path, size, chars):
    font = ImageFont.truetype(path, size)
    ascent, descent = font.getmetrics()
    glyphs = []
    for ch in sorted(set(chars), key=ord):
        mask, (ox, oy) = font.getmask2(ch, mode="L", anchor="ls")
        w, h = mask.size
        adv = int(round(font.getlength(ch)))
        bmp = bytes(mask) if w and h else b""
        glyphs.append((ord(ch), h, w, adv, -oy, ox, bmp))
    out = struct.pack(">6i", len(glyphs), 11, size, 0, ascent, descent)
    for u, h, w, adv, dy, dx, _ in glyphs:
        out += struct.pack(">7i", u, h, w, adv, dy, dx, 0)
    for g in glyphs:
        out += g[6]
    return out


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    lines = ["// Сгенерировано tools/make_fonts.py — не редактировать вручную.",
             "#pragma once", "#include <stdint.h>", ""]
    for name, file, size, chars in FONTS:
        data = make_vlw(DIR + file, size, chars)
        lines.append(f"// {file} {size}px, {len(data)} байт")
        lines.append(f"static const uint8_t {name}[] = {{")
        for i in range(0, len(data), 32):
            lines.append("  " + ",".join(str(b) for b in data[i:i + 32]) + ",")
        lines.append("};\n")
        print(name, len(data))
    with open(os.path.join(here, "..", "fonts.h"), "w") as f:
        f.write("\n".join(lines))


main()
