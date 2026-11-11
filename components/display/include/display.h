#pragma once

// Driver for the 1.83" 240x280 IPS panel with an NV3030B controller, used in
// landscape: 280 wide and 240 high.
//
// Every drawing call writes to a frame buffer in PSRAM. Nothing reaches the
// panel until lcd_flush() sends the whole frame. Coordinates may fall outside
// the screen: whatever lands off screen is clipped.

#include <stdint.h>

#include "font.h"

#define LCD_W 280
#define LCD_H 240

// Wiring. The backlight is dimmed by backlight.c through LEDC; this driver
// only holds it off until then.
#define LCD_PIN_MOSI 16
#define LCD_PIN_CLK  15
#define LCD_PIN_CS   14
#define LCD_PIN_DC   13
#define LCD_PIN_RST  17
#define LCD_PIN_BL   21

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | (((b) & 0xF8) >> 3)))

#define LCD_BLACK 0x0000
#define LCD_WHITE 0xFFFF

typedef enum {
    LCD_ALIGN_LEFT,     // x is the left edge of the text
    LCD_ALIGN_CENTER,   // x is the center of the text
    LCD_ALIGN_RIGHT,    // x is the right edge of the text
} lcd_align_t;

// Sets up SPI and the controller and leaves the panel showing black, with
// the backlight off.
void lcd_init(void);

// Sends the frame buffer to the panel.
void lcd_flush(void);

// Paints the whole frame buffer black.
void lcd_clear(void);

// Filled rectangle; both corners are inclusive and may come in any order.
void lcd_fill_rect(int16_t x1, int16_t y1, int16_t x2, int16_t y2, uint16_t color);

// Rectangle outline, 1 px wide, with rounded corners of radius r.
void lcd_round_rect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color);

// Copies a w x h RGB565 image, stored in the CPU's byte order.
void lcd_image(int16_t x, int16_t y, int16_t w, int16_t h, const uint16_t *pixels);

// Draws text starting at (x, y), its top left corner. Only the glyph pixels
// are painted; the background is left as it was.
void lcd_text(int16_t x, int16_t y, uint16_t color, const font_t *font, const char *s);

// Same as lcd_text(), with x taken as the left edge, center or right edge.
// Text that would start left of the screen is moved to start at x = 0.
void lcd_text_aligned(int16_t x, int16_t y, uint16_t color, const font_t *font,
                      lcd_align_t align, const char *s);

// Width of the text in pixels.
uint16_t lcd_text_width(const font_t *font, const char *s);
