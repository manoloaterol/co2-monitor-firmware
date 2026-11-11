#!/usr/bin/env python3
"""Converts a fixed-width BDF bitmap font to the font format of the display
component, without any rescaling: every pixel is copied as drawn.

gen_font.py rasterizes outline fonts, which works down to about 13 px; below
that the thresholded antialiasing leaves broken strokes. Bitmap fonts drawn
for a small size do not have that problem.

Target format: same as gen_font.py (flat uint16_t array, ceil(width / 16)
words per glyph row, bit 15 of the first word = leftmost pixel, glyphs
contiguous from FIRST).

Usage:  bdf2font.py <font.bdf> <ArrayName> [last] [--preview TEXT]

  last    code of the last character (126 = '~' by default)
"""
import sys

FIRST = 32
LAST = 126


def parse(path):
    """Returns (cell_w, cell_h, ascent, {code: (bbx, rows)})."""
    glyphs = {}
    cell_w = cell_h = ascent = None
    code = bbx = rows = None
    with open(path, encoding="latin-1") as f:
        for line in f:
            parts = line.split()
            if not parts:
                continue
            key = parts[0]
            if key == "FONTBOUNDINGBOX":
                cell_w, cell_h = int(parts[1]), int(parts[2])
            elif key == "FONT_ASCENT":
                ascent = int(parts[1])
            elif key == "ENCODING":
                code = int(parts[1])
            elif key == "BBX":
                bbx = tuple(int(p) for p in parts[1:5])
            elif key == "BITMAP":
                rows = []
            elif key == "ENDCHAR":
                glyphs[code] = (bbx, rows)
                rows = None
            elif rows is not None:
                rows.append(int(key, 16) << (4 * (8 - len(key))))   # left-align in 32 bits
    return cell_w, cell_h, ascent, glyphs


def to_words(glyph, cell_w, cell_h, ascent):
    """Places the glyph box in the cell using its BBX offsets and baseline."""
    wpr = (cell_w + 15) // 16
    out = [0] * (cell_h * wpr)
    if glyph is None:
        return out
    (w, h, xoff, yoff), rows = glyph
    top = ascent - (h + yoff)                 # cell row of the glyph box's first row
    for r, bits in enumerate(rows):
        y = top + r
        if not 0 <= y < cell_h:
            continue
        for c in range(w):
            x = xoff + c
            if 0 <= x < cell_w and bits & (0x80000000 >> c):
                out[y * wpr + (x >> 4)] |= 0x8000 >> (x & 15)
    return out


def main():
    path, name = sys.argv[1], sys.argv[2]
    rest = [a for a in sys.argv[3:] if not a.startswith("--")]
    last = int(rest[0]) if rest else LAST
    cell_w, cell_h, ascent, glyphs = parse(path)
    wpr = (cell_w + 15) // 16
    words = {c: to_words(glyphs.get(c), cell_w, cell_h, ascent) for c in range(FIRST, last + 1)}
    sys.stderr.write(f"{path.split('/')[-1]}: cell {cell_w}x{cell_h}, ASCII {FIRST}..{last}, "
                     f"{(last - FIRST + 1) * cell_h * wpr * 2} bytes\n")

    if "--preview" in sys.argv:
        text = sys.argv[sys.argv.index("--preview") + 1]
        for y in range(cell_h):
            line = ""
            for ch in text:
                row = words[ord(ch)][y * wpr:(y + 1) * wpr]
                line += "".join("#" if (row[x >> 4] << (x & 15)) & 0x8000 else "."
                                for x in range(cell_w))
            sys.stderr.write(line + "\n")
        return

    out = [f"static const uint16_t {name}[] = {{", "", f"// ASCII {FIRST}..{last}", ""]
    for c in range(FIRST, last + 1):
        # a backslash at the end of a // comment would swallow the next line
        label = "sp" if c == 32 else (r"'\\'" if c == 92 else chr(c))
        out.append(", ".join(f"0x{w:04X}" for w in words[c]) + f", //\t{label}")
    print("\n".join(out + ["", "};"]))


if __name__ == "__main__":
    main()
