#!/usr/bin/env python3
"""
Regenerates src/font_atlas.h from the Public Pixel TTF.

Public Pixel is CC0 1.0 Universal (GGBotNet,
https://github.com/ggbotnet/fonts-cc0) -- public domain, no attribution
required, safe to redistribute embedded in this project.

This script is NOT part of the qnx_client build itself -- the build
only ever compiles the already-generated src/font_atlas.h, matching
this whole renderer's "no asset loaded at runtime" approach (same
reasoning as QNX's own gles2-maze sample embedding its texture as a
C header via `xxd -i` rather than loading a .tga file on the target).
Run this only when you want to regenerate the atlas (e.g. a different
font, a different character range, a different cell size).

Usage:
    python3 make_font_atlas.py /path/to/PublicPixel.ttf ../src/font_atlas.h

Requires Pillow (pip install Pillow --break-system-packages).
"""
import sys
from PIL import Image, ImageDraw, ImageFont

CELL = 16          # px per glyph cell -- confirmed via font.getlength() that
                   # every printable ASCII character in Public Pixel has an
                   # identical 16px advance width at this render size, so a
                   # fixed-size grid needs no per-glyph width table at all.
COLS = 16
FIRST_CHAR = 32    # space
LAST_CHAR = 126    # ~ -- full printable ASCII


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
        f.write(" * tools/make_font_atlas.py -- this header IS the asset, same pattern\n")
        f.write(" * as QNX's gles2-maze sample embedding brick_wall.tga as brick_wall.h\n")
        f.write(" * via xxd -i, just single-channel and pre-decoded instead of a raw\n")
        f.write(" * TGA file (so no runtime image-format parser is needed at all).\n")
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
