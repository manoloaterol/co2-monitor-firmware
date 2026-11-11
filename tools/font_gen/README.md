# font_gen

Converts fonts to the `font_t` format of `components/display/fonts.c`:

- `gen_font.py` rasterises a TTF. It works well from about 13 px up.
- `bdf2font.py` copies a BDF bitmap font pixel by pixel. It is meant for small
  sizes, where thresholding the antialiasing breaks the strokes.

## Format

A flat `uint16_t` array. Each row of each glyph takes `ceil(width / 16)`
words; bit 15 of the first word is the leftmost pixel. Glyphs are contiguous
from `first` (ASCII 32) to `last`, so the driver finds a row with:

```c
((ch - first) * height + row) * words_per_row
```

With `width <= 16` there is one word per row. The array is declared in
`fonts.c` with the `FONT()` macro, which checks at compile time that its size
matches `first` and `last`.

## Usage

```sh
python3 -m venv .venv && .venv/bin/pip install pillow

# download the font
curl -L -o Goldman-Bold.ttf \
  https://github.com/google/fonts/raw/main/ofl/goldman/Goldman-Bold.ttf

# preview as ASCII art before generating
.venv/bin/python gen_font.py Goldman-Bold.ttf X 32 30 --preview "1398"

# dump the C data
.venv/bin/python gen_font.py Goldman-Bold.ttf s_goldman_32x30 32 30 > goldman.inc
```

Paste the array into `fonts.c`, together with its `FONT(...)` line, and
declare the `font_t` in `include/font.h`.

The two numbers are the cell width and height (32x30 by default). The script
finds the size in px that makes the digits fill the available height, and
condenses horizontally just enough for the widest digit to fit the cell, so all
digits are tabular.

For the Regular weight, point it at `Goldman-Regular.ttf`.

## Choosing the cell width

Keep the width a multiple of 16: a 33 px cell takes 3 words per row, the same
as a 48 px one, wasting flash. For Goldman at 30 px high, the natural
proportion of the digits asks for 32 px, which fits exactly in 2 words; hence
the 32x30 cell.

Constraints:

- The range must be contiguous: the driver computes the index by subtracting
  `first`, with no lookup table. The scripts always start at 32.
- `width` and `height` are `uint8_t`, so the maximum is 255x255.

## Font licences

Goldman and Russo One are under the SIL Open Font License 1.1 (see
`Goldman-OFL.txt` and `RussoOne-OFL.txt`).

## BDF fonts

`bdf2font.py` does no scaling: the cell is the font's `FONTBOUNDINGBOX`. The
project's 6x9 is `6x9` from X11 misc-fixed, public domain:

```sh
curl -L -o 6x9.bdf \
  https://gitlab.freedesktop.org/xorg/font/misc-misc/-/raw/master/6x9.bdf
python3 bdf2font.py 6x9.bdf X 126 --preview "abc1234.dirty"
python3 bdf2font.py 6x9.bdf s_fixed_6x9 126 > fixed.inc
```

It does not need Pillow.
