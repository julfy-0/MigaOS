#!/usr/bin/env python3
"""Генерирует drivers/video/fbcon/font8x16.h (ASCII 32..126, 8x16) из системного TTF."""
import sys
from PIL import Image, ImageDraw, ImageFont

TTF = sys.argv[1] if len(sys.argv) > 1 else "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
OUT = sys.argv[2] if len(sys.argv) > 2 else "drivers/video/fbcon/font8x16.h"
font = ImageFont.truetype(TTF, 13)

rows = []
for code in range(32, 127):
    im = Image.new("1", (8, 16), 0)
    d = ImageDraw.Draw(im)
    d.fontmode = "1"
    d.text((0, 1), chr(code), font=font, fill=1)
    glyph = []
    for y in range(16):
        b = 0
        for x in range(8):
            if im.getpixel((x, y)):
                b |= 0x80 >> x
        glyph.append(b)
    rows.append((code, glyph))

with open(OUT, "w") as f:
    f.write("/* Автогенерация: tools/genfont.py. Не править руками. */\n")
    f.write("#pragma once\n#include <stdint.h>\n\n")
    f.write("#define FONT_W 8\n#define FONT_H 16\n#define FONT_FIRST 32\n#define FONT_LAST 126\n\n")
    f.write("static const uint8_t font8x16[95][16] = {\n")
    for code, g in rows:
        f.write("    {" + ",".join(f"0x{b:02X}" for b in g) + "}, /* " +
                (chr(code) if chr(code) not in "\\*/" else "?") + " */\n")
    f.write("};\n")
print("ok", OUT)
