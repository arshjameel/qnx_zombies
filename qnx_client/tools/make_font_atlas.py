#!/usr/bin/env python3
"""
Usage:
    python3 make_font_atlas.py /path/to/PublicPixel.ttf ../src/font_atlas.h

Requires Pillow (pip install Pillow --break-system-packages).
"""
import sys
from PIL import Image, ImageDraw, ImageFont

CELL = 16          # px per glyph cell
COLS = 16
FIRST_CHAR = 32    
LAST_CHAR = 126    


def main():
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <font.ttf> <output.h>")
        sys.exit(1)
    font_path, out_path = sys.argv[1], sys.argv[2]

    num_chars = LAST_CHAR - FIRST_CHAR + 1
    rows = (num_chars + COLS - 1) // COLS
    atlas_w, atlas_h = CELL * COLS, CELL * rows

    atlas = Image.new("L", (atlas_w, atlas_h), 0)
    draw = ImageDraw.Draw(atlas)
    font = ImageFont.truetype(font_path, CELL)

    for i in range(num_chars):
        ch = chr(FIRST_CHAR + i)
        x, y = (i % COLS) * CELL, (i // COLS) * CELL
        draw.text((x, y), ch, font=font, fill=255)

    data = list(atlas.getdata())

    with open(out_path, "w") as f:
        f.write("/*\n")
        f.write(" * font_atlas.h -- Public Pixel font (GGBotNet, CC0 1.0 Universal,\n")
        f.write(" * https://github.com/ggbotnet/fonts-cc0), rasterized to a single-\n")
        f.write(" * channel (alpha-only) bitmap atlas at build time. Regenerate with\n")
        f.write(" * tools/make_font_atlas.py \n")
        f.write(" *\n")
        f.write(f" * Layout: {COLS} columns, {CELL}x{CELL} px per cell, monospaced\n")
        f.write(f" * (every glyph has an identical {CELL}px advance width in the source\n")
        f.write(" * font). Glyph for ASCII code C is at cell index\n")
        f.write(" * (C - FONT_ATLAS_FIRST_CHAR).\n")
        f.write(" */\n\n")
        f.write("#ifndef FONT_ATLAS_H\n#define FONT_ATLAS_H\n\n")
        f.write(f"#define FONT_ATLAS_W {atlas_w}\n")
        f.write(f"#define FONT_ATLAS_H_PX {atlas_h}\n")
        f.write(f"#define FONT_ATLAS_CELL {CELL}\n")
        f.write(f"#define FONT_ATLAS_COLS {COLS}\n")
        f.write(f"#define FONT_ATLAS_FIRST_CHAR {FIRST_CHAR}\n")
        f.write(f"#define FONT_ATLAS_LAST_CHAR {LAST_CHAR}\n\n")
        f.write(f"static const unsigned char font_atlas_pixels[{len(data)}] = {{\n")
        for i in range(0, len(data), 20):
            f.write("    " + ",".join(str(v) for v in data[i:i + 20]) + ",\n")
        f.write("};\n\n")
        f.write("#endif /* FONT_ATLAS_H */\n")

    print(f"Wrote {out_path}: {atlas_w}x{atlas_h} atlas, {COLS}x{rows} grid, "
          f"chars {FIRST_CHAR}-{LAST_CHAR} ({num_chars} glyphs)")


if __name__ == "__main__":
    main()
