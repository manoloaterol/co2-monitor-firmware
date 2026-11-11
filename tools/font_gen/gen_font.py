#!/usr/bin/env python3
"""Converts a TTF to the font_t format of components/display/fonts.c.

Target format: flat uint16_t array. Each row of each glyph takes
ceil(FontWidth / 16) words, bit 15 of the first word = leftmost pixel.
Glyphs are contiguous from ASCII 32, so the driver finds a row with

    ((ch - 32) * FontHeight + row) * words_per_row

For FontWidth <= 16 that is one word per row, which is the original format.

Usage:  gen_font.py <font.ttf> <ArrayName> [width] [height] [last] [--preview TEXT]

  last    ASCII code of the last character (57 = '9' by default, 122 = 'z').
          The range always starts at 32 and must be contiguous.

  --sub X=Y  draws character Y in the slot of character X. Used to fit
             symbols outside the ASCII range (the driver does not handle
             Unicode) by giving up a character that is not used. Repeatable.
             Example:  --sub "^=°"   puts the degree sign in the circumflex slot
"""
import sys
from PIL import Image, ImageDraw, ImageFont

FIRST = 32                    # the driver indexes (ch - 32): the range starts here
LAST  = 57                    # last character included; '9' by default
THRESHOLD = 110               # binarization threshold on the antialiased render
GAP = 1                       # blank columns to the right of the digits
SUBS = {}                     # ASCII slot -> character drawn in its place
DIGITS = "0123456789"

BASE_X, BASE_Y = 60, 150
CANVAS = (400, 260)


def rasterize(font, ch):
    img = Image.new("L", CANVAS, 0)
    ImageDraw.Draw(img).text((BASE_X, BASE_Y), ch, font=font, fill=255, anchor="ls")
    return img


def ink_bbox(img):
    return img.point(lambda p: 255 if p >= THRESHOLD else 0).getbbox()


def squeeze(img, xscale):
    """Squeezes the whole canvas in X; rows (and with them the baseline) are untouched.
    The horizontal position does not matter because each glyph is recentered later."""
    if xscale >= 1.0:
        return img
    return img.resize((max(1, round(CANVAS[0] * xscale)), CANVAS[1]), Image.LANCZOS)


def pick_size(path, target_h):
    """Largest size whose digit height does not exceed target_h."""
    best = None
    for size in range(8, 200):
        bb = ink_bbox(rasterize(ImageFont.truetype(path, size), "0"))
        if bb is None:
            continue
        if bb[3] - bb[1] <= target_h:
            best = size
        else:
            break
    return best


def to_words(img, cell_w, cell_h, dx, dy):
    """Rasterizes the cell into rows of ceil(cell_w/16) 16-bit words."""
    wpr = (cell_w + 15) // 16
    px = img.load()
    out = []
    for y in range(cell_h):
        sy = y - dy
        row = [0] * wpr
        if 0 <= sy < CANVAS[1]:
            for x in range(cell_w):
                sx = x - dx
                if 0 <= sx < img.width and px[sx, sy] >= THRESHOLD:
                    row[x >> 4] |= 0x8000 >> (x & 15)
        out.extend(row)
    return out


def build(path, cell_w, cell_h):
    # cell margins: 2 rows on top and whatever is left below for the descenders
    digit_top = 2
    digit_bot = cell_h - 3
    size = pick_size(path, digit_bot - digit_top + 1)
    font = ImageFont.truetype(path, size)

    # horizontal scale shared by the whole set: set by the widest digit, so
    # the digits stay tabular and keep the font's proportions
    widest = max(ink_bbox(rasterize(font, d))[2] - ink_bbox(rasterize(font, d))[0] for d in DIGITS)
    xscale = min(1.0, (cell_w - GAP) / widest)

    bb0 = ink_bbox(rasterize(font, "0"))
    dy = digit_top - bb0[1]                       # aligns the top of the '0' with digit_top

    glyphs = {}
    wpr = (cell_w + 15) // 16
    for code in range(FIRST, LAST + 1):
        ch = SUBS.get(code, chr(code))
        if ch == " ":
            glyphs[code] = [0] * (cell_h * wpr)
            continue
        img = squeeze(rasterize(font, ch), xscale)
        bb = ink_bbox(img)
        if bb is None:
            glyphs[code] = [0] * (cell_h * wpr)
            continue
        # symbols that still overflow (# $ % &) are condensed a bit more
        if bb[2] - bb[0] > cell_w:
            img = squeeze(rasterize(font, ch), xscale * cell_w / (bb[2] - bb[0]))
            bb = ink_bbox(img)
        glyphs[code] = to_words(img, cell_w, cell_h, (cell_w - (bb[2] - bb[0])) // 2 - bb[0], dy)
    return size, xscale, glyphs


def preview(glyphs, text, cell_w, cell_h):
    wpr = (cell_w + 15) // 16
    lines = [""] * cell_h
    for ch in text:
        words = glyphs[ord(ch)]
        for y in range(cell_h):
            row = words[y * wpr:(y + 1) * wpr]
            lines[y] += "".join("#" if (row[x >> 4] << (x & 15)) & 0x8000 else "." for x in range(cell_w))
    return "\n".join(lines)


def emit(glyphs, name):
    out = [f"static const uint16_t {name}[] = {{", "",
           f"// ASCII {FIRST}..{LAST}", ""]
    for code in range(FIRST, LAST + 1):
        ch = SUBS.get(code, chr(code))
        # careful: a backslash at the end of a // comment continues it onto the
        # next line and would swallow the data row, so it is quoted
        if code in SUBS:
            label = f"{ch}  (in the '{chr(code)}' slot)"
        else:
            label = "sp" if ch == " " else (r"'\\'" if ch == "\\" else ch)
        out.append(", ".join(f"0x{w:04X}" for w in glyphs[code]) + f", //\t{label}")
    return "\n".join(out + ["", "};"])


if __name__ == "__main__":
    ttf, name = sys.argv[1], sys.argv[2]
    rest = [a for a in sys.argv[3:] if not a.startswith("--")]
    cell_w = int(rest[0]) if len(rest) > 0 else 32
    cell_h = int(rest[1]) if len(rest) > 1 else 30
    if len(rest) > 2:
        globals()["LAST"] = int(rest[2])
    for i, a in enumerate(sys.argv):
        if a == "--sub":
            hueco, _, nuevo = sys.argv[i + 1].partition("=")
            SUBS[ord(hueco)] = nuevo
    size, xscale, glyphs = build(ttf, cell_w, cell_h)
    sys.stderr.write(f"{ttf.split('/')[-1]}: cell {cell_w}x{cell_h}, size {size}px, "
                     f"condensed x{xscale:.3f}, {(cell_w + 15) // 16} words/row, "
                     f"ASCII {FIRST}..{LAST}, "
                     f"{(LAST - FIRST + 1) * cell_h * ((cell_w + 15) // 16) * 2} bytes\n")
    if "--preview" in sys.argv:
        sys.stderr.write(preview(glyphs, sys.argv[sys.argv.index("--preview") + 1], cell_w, cell_h) + "\n")
    else:
        print(emit(glyphs, name))
