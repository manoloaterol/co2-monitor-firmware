#include "display.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// References are to the NV3030B datasheet V0.6 (docs/NV3030B_datasheet_V0_6.pdf).

#define LCD_HOST SPI2_HOST

// The minimum write cycle (TSCLCW, section 10.3.1) is 15.4 ns, about 65 MHz. The
// SPI clock is derived from the 80 MHz APB, so 40 MHz is the fastest setting
// within spec.
#define LCD_SPI_HZ (40 * 1000 * 1000)

// The controller is sized for 240x320 and this panel shows 280 of the 320
// lines. In landscape those lines are the columns, and the visible ones start
// at column 20.
#define LCD_X_OFFSET 20

// The frame buffer lives in PSRAM, which the SPI DMA cannot read. The SPI
// driver copies each transaction into a temporary internal buffer, and with
// Wi-Fi up the largest free internal block is about 55 KB. Sending the
// 131 KB frame in chunks keeps each copy small.
#define LCD_CHUNK 16384

// Commands from section 7.1 (standard set).
#define CMD_SLPOUT 0x11
#define CMD_INVON  0x21
#define CMD_DISPON 0x29
#define CMD_CASET  0x2A
#define CMD_RASET  0x2B
#define CMD_RAMWR  0x2C
#define CMD_MADCTL 0x36
#define CMD_COLMOD 0x3A

// MADCTL (7.1.25): MY and MV together turn the portrait panel into landscape
// with the image the right way up for how it sits in the case. The BGR bit is
// left at 0 (RGB).
#define MADCTL_LANDSCAPE 0xA0

// COLMOD (7.1.29): 16 bits per pixel on both the RGB and the system interface.
#define COLMOD_RGB565 0x55

// Pixels are stored already in the order the panel expects them, high byte
// first, so lcd_flush() can send the buffer as it is.
static uint16_t s_fb[LCD_W * LCD_H] EXT_RAM_BSS_ATTR;

static spi_device_handle_t s_spi;

//==============================================================================
// Controller setup
//==============================================================================

typedef struct {
    uint8_t cmd;
    uint8_t len;
    uint8_t data[8];
} lcd_cmd_t;

// Analog, timing and gamma setup, written through the private registers
// (7.2). The datasheet gives what each register does but not the values a
// given panel needs. These come from the initialization sequence in
// Sang.IoT.NV3030B (https://github.com/sangyuxiaowu/NV3030B), written for a
// Waveshare NV3030B module:
//
//   Copyright [sangsq] (sang93@qq.com)
//   Licensed under the Apache License, Version 2.0. A copy is in
//   LICENSE-NV3030B-init.txt, next to this file.
//
// Changes: rewritten as a C table, with the commands that follow it (pixel
// format, orientation, inversion) set for this panel; the tearing effect
// output is not enabled because its pin is not wired.
static const lcd_cmd_t s_init[] = {
    { 0xFD, 2, { 0x06, 0x08 } },                                // enter private registers (7.2.44)
    { 0x61, 2, { 0x07, 0x04 } },                                // dvdd
    { 0x62, 3, { 0x00, 0x44, 0x45 } },                          // bias
    { 0x63, 4, { 0x41, 0x07, 0x12, 0x12 } },                    // vgl
    { 0x64, 1, { 0x37 } },                                      // vgh
    { 0x65, 3, { 0x09, 0x10, 0x21 } },                          // vsp pump
    { 0x66, 3, { 0x09, 0x10, 0x21 } },                          // vsn pump
    { 0x67, 2, { 0x21, 0x40 } },                                // pump clock
    { 0x68, 4, { 0x90, 0x4C, 0x50, 0x70 } },                    // gamma reference, vcom
    { 0xB1, 3, { 0x0F, 0x02, 0x01 } },                          // frame rate
    { 0xB4, 1, { 0x01 } },                                      // inversion: 1 dot
    { 0xB5, 4, { 0x02, 0x02, 0x0A, 0x14 } },                    // porches
    { 0xB6, 5, { 0x04, 0x01, 0x9F, 0x00, 0x02 } },              // display function
    { 0xDF, 1, { 0x11 } },                                      // gamma select
    { 0xE2, 6, { 0x03, 0x00, 0x00, 0x30, 0x33, 0x3F } },        // gamma positive 3
    { 0xE5, 6, { 0x3F, 0x33, 0x30, 0x00, 0x00, 0x03 } },        // gamma negative 3
    { 0xE1, 2, { 0x05, 0x67 } },                                // gamma positive 2
    { 0xE4, 2, { 0x67, 0x06 } },                                // gamma negative 2
    { 0xE0, 8, { 0x05, 0x06, 0x0A, 0x0C, 0x0B, 0x0B, 0x13, 0x19 } },  // gamma positive 1
    { 0xE3, 8, { 0x18, 0x13, 0x0D, 0x09, 0x0B, 0x0B, 0x05, 0x06 } },  // gamma negative 1
    { 0xE6, 2, { 0x00, 0xFF } },                                // source control 1
    { 0xE7, 6, { 0x01, 0x04, 0x03, 0x03, 0x00, 0x12 } },        // source control 2
    { 0xE8, 3, { 0x00, 0x70, 0x00 } },                          // source control 3
    { 0xEC, 1, { 0x52 } },                                      // gate timing
    { 0xF1, 3, { 0x01, 0x01, 0x02 } },                          // tearing effect
    { 0xF6, 4, { 0x01, 0x30, 0x00, 0x00 } },                    // interface control (7.2.41)
    { 0xFD, 2, { 0xFA, 0xFC } },                                // leave private registers
    { CMD_COLMOD, 1, { COLMOD_RGB565 } },
    { CMD_MADCTL, 1, { MADCTL_LANDSCAPE } },
    { CMD_INVON,  0, { 0 } },                                   // IPS panels need inversion on
};

// Runs before each transaction and sets D/C from the transaction's user field:
// 0 for a command, 1 for data.
static void IRAM_ATTR lcd_pre_transfer(spi_transaction_t *t)
{
    gpio_set_level(LCD_PIN_DC, (int)t->user);
}

static void lcd_send(bool is_data, const void *buf, size_t len, uint32_t flags)
{
    spi_transaction_t t = {
        .flags     = flags,
        .length    = len * 8,
        .tx_buffer = buf,
        .user      = (void *)(is_data ? 1 : 0),
    };
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));
}

static void lcd_cmd(uint8_t cmd, const uint8_t *data, size_t len)
{
    lcd_send(false, &cmd, 1, 0);
    if (len > 0) {
        lcd_send(true, data, len, 0);
    }
}

static void lcd_window(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    uint8_t col[4] = { x1 >> 8, x1 & 0xFF, x2 >> 8, x2 & 0xFF };
    uint8_t row[4] = { y1 >> 8, y1 & 0xFF, y2 >> 8, y2 & 0xFF };

    lcd_cmd(CMD_CASET, col, sizeof(col));
    lcd_cmd(CMD_RASET, row, sizeof(row));
}

void lcd_init(void)
{
    // The backlight stays off until backlight.c takes the pin over, so the
    // reset and the power-up contents are never seen.
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << LCD_PIN_DC) | (1ULL << LCD_PIN_RST) | (1ULL << LCD_PIN_BL),
        .mode         = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_set_level(LCD_PIN_BL, 0);

    spi_bus_config_t bus = {
        .mosi_io_num     = LCD_PIN_MOSI,
        .miso_io_num     = -1,
        .sclk_io_num     = LCD_PIN_CLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = LCD_CHUNK,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev = {
        .clock_speed_hz = LCD_SPI_HZ,
        .mode           = 0,
        .spics_io_num   = LCD_PIN_CS,
        .queue_size     = 1,
        .pre_cb         = lcd_pre_transfer,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(LCD_HOST, &dev, &s_spi));

    // Hardware reset (8.12): the low pulse needs at least 0.1 ms. After the
    // rising edge the controller loads its defaults within 5 ms, and blanks
    // the panel for up to 120 ms.
    gpio_set_level(LCD_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(LCD_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    for (size_t i = 0; i < sizeof(s_init) / sizeof(s_init[0]); i++) {
        lcd_cmd(s_init[i].cmd, s_init[i].data, s_init[i].len);
    }

    // Sleep out needs 5 ms before the next command (7.1.11). The longer wait
    // lets the charge pumps settle before the panel is switched on.
    lcd_cmd(CMD_SLPOUT, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120));

    // Frame memory powers up with random contents: clear it before the panel
    // shows anything.
    lcd_clear();
    lcd_flush();
    lcd_cmd(CMD_DISPON, NULL, 0);
}

void lcd_flush(void)
{
    lcd_window(LCD_X_OFFSET, 0, LCD_X_OFFSET + LCD_W - 1, LCD_H - 1);
    lcd_cmd(CMD_RAMWR, NULL, 0);

    // CS has to stay low between chunks: each time it goes high the
    // controller would start the window again.
    ESP_ERROR_CHECK(spi_device_acquire_bus(s_spi, portMAX_DELAY));

    const uint8_t *p    = (const uint8_t *)s_fb;
    size_t         left = sizeof(s_fb);

    while (left > 0) {
        size_t n = left > LCD_CHUNK ? LCD_CHUNK : left;
        left -= n;
        lcd_send(true, p, n, left > 0 ? SPI_TRANS_CS_KEEP_ACTIVE : 0);
        p += n;
    }

    spi_device_release_bus(s_spi);
}

//==============================================================================
// Drawing
//==============================================================================

static inline void put(int16_t x, int16_t y, uint16_t color)
{
    if (x >= 0 && x < LCD_W && y >= 0 && y < LCD_H) {
        s_fb[y * LCD_W + x] = __builtin_bswap16(color);
    }
}

void lcd_clear(void)
{
    memset(s_fb, 0, sizeof(s_fb));
}

void lcd_fill_rect(int16_t x1, int16_t y1, int16_t x2, int16_t y2, uint16_t color)
{
    if (x1 > x2) { int16_t t = x1; x1 = x2; x2 = t; }
    if (y1 > y2) { int16_t t = y1; y1 = y2; y2 = t; }

    if (x1 < 0)         x1 = 0;
    if (y1 < 0)         y1 = 0;
    if (x2 > LCD_W - 1) x2 = LCD_W - 1;
    if (y2 > LCD_H - 1) y2 = LCD_H - 1;

    uint16_t c = __builtin_bswap16(color);

    for (int16_t y = y1; y <= y2; y++) {
        uint16_t *row = &s_fb[y * LCD_W];
        for (int16_t x = x1; x <= x2; x++) {
            row[x] = c;
        }
    }
}

void lcd_round_rect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color)
{
    if (w <= 0 || h <= 0) {
        return;
    }

    int16_t r_max = (w < h ? w : h) / 2;
    if (r > r_max) r = r_max;
    if (r < 0)     r = 0;

    int16_t x2 = x + w - 1;
    int16_t y2 = y + h - 1;

    // straight edges, between the corners
    for (int16_t i = x + r; i <= x2 - r; i++) {
        put(i, y, color);
        put(i, y2, color);
    }
    for (int16_t i = y + r; i <= y2 - r; i++) {
        put(x, i, color);
        put(x2, i, color);
    }

    // Corners: midpoint circle. Each step gives one point per octant, and each
    // corner takes the two octants that face it.
    int16_t cl = x + r, cr = x2 - r;    // centers, left and right
    int16_t ct = y + r, cb = y2 - r;    // centers, top and bottom
    int16_t px = 0, py = r;
    int16_t f  = 1 - r;

    while (px < py) {
        if (f >= 0) {
            py--;
            f -= 2 * py;
        }
        px++;
        f += 2 * px + 1;

        put(cl - py, ct - px, color);  put(cl - px, ct - py, color);
        put(cr + px, ct - py, color);  put(cr + py, ct - px, color);
        put(cr + px, cb + py, color);  put(cr + py, cb + px, color);
        put(cl - py, cb + px, color);  put(cl - px, cb + py, color);
    }
}

void lcd_image(int16_t x, int16_t y, int16_t w, int16_t h, const uint16_t *pixels)
{
    for (int16_t j = 0; j < h; j++) {
        for (int16_t i = 0; i < w; i++) {
            put(x + i, y + j, *pixels++);
        }
    }
}

//==============================================================================
// Text
//==============================================================================

void lcd_text(int16_t x, int16_t y, uint16_t color, const font_t *font, const char *s)
{
    const int16_t wpr   = (font->width + 15) / 16;
    const size_t  glyph = (size_t)font->height * wpr;

    for (; *s != '\0'; s++, x += font->width) {
        uint8_t ch = (uint8_t)*s;

        if (ch < font->first || ch > font->last) {
            continue;
        }
        if (x >= LCD_W || x + font->width <= 0) {
            continue;
        }

        const uint16_t *g = font->data + (ch - font->first) * glyph;

        for (int16_t row = 0; row < font->height; row++, g += wpr) {
            for (int16_t col = 0; col < font->width; col++) {
                if (g[col >> 4] & (0x8000 >> (col & 15))) {
                    put(x + col, y + row, color);
                }
            }
        }
    }
}

uint16_t lcd_text_width(const font_t *font, const char *s)
{
    return (uint16_t)(strlen(s) * font->width);
}

void lcd_text_aligned(int16_t x, int16_t y, uint16_t color, const font_t *font,
                      lcd_align_t align, const char *s)
{
    int16_t w = (int16_t)lcd_text_width(font, s);

    if (align == LCD_ALIGN_CENTER) {
        x -= w / 2;
    } else if (align == LCD_ALIGN_RIGHT) {
        x -= w;
    }
    if (x < 0) {
        x = 0;
    }

    lcd_text(x, y, color, font, s);
}
