#pragma once

#include <stdint.h>

// Monospaced 1-bit font. Each glyph row takes (width + 15) / 16 words, and
// bit 15 of the first word is the leftmost pixel. Glyphs are stored one after
// another, from `first` to `last` with no gaps. Characters outside that range
// draw nothing, but they still take their cell's width.
typedef struct {
    uint8_t         width;
    uint8_t         height;
    uint8_t         first;
    uint8_t         last;
    const uint16_t *data;
} font_t;

// Generated with tools/font_gen/, see the comments in fonts.c.
extern const font_t font_goldman_13x13;       // Goldman Bold, ASCII 32..122
extern const font_t font_goldman_16x16;       // Goldman Bold, ASCII 32..122
extern const font_t font_goldman_20x20;       // Goldman Bold, ASCII 32..122
extern const font_t font_goldman_32x30;       // Goldman Bold, ASCII 32..57
extern const font_t font_goldman_reg_15x17;   // Goldman Regular, ASCII 32..122
extern const font_t font_russo_58x90;         // Russo One, ASCII 32..57
extern const font_t font_fixed_6x9;           // X11 misc-fixed 6x9, ASCII 32..126
