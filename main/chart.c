#include <stdio.h>
#include <stdint.h>

#include "chart.h"

#include "display.h"

// puts an already averaged bar into the ring buffer
static void chart_push(chart_t *c, uint16_t v)
{
    if (c->count < c->bars) {
        c->vals[(c->head + c->count) % c->bars] = v;
        c->count++;
    } else {
        c->vals[c->head] = v;                      // overwrites the oldest one
        c->head = (c->head + 1) % c->bars;
    }

    if (v > c->max_hist) {
        c->max_hist = v;
    }
}

// accumulates a reading; every avg_n readings closes a bar
bool chart_add_reading(chart_t *c, uint16_t v)
{
    c->acc_sum += v;
    c->acc_n++;

    if (c->acc_n < c->avg_n) {
        return false;
    }
    chart_push(c, (uint16_t)(c->acc_sum / c->acc_n));
    c->acc_sum = 0;
    c->acc_n   = 0;
    return true;
}

// clears the chart and its all-time maximum
void chart_reset(chart_t *c)
{
    c->count = c->head = c->max_hist = 0;
    c->acc_sum = 0;
    c->acc_n   = 0;
}

void chart_draw(const chart_t *c)
{
    if (c->count == 0) {
        return;   // the first bar has not been closed yet
    }

    uint16_t wmin = 0xFFFF;   // minimum and maximum of the visible window
    uint16_t wmax = 0;
    for (uint16_t i = 0; i < c->count; i++) {
        uint16_t v = c->vals[(c->head + i) % c->bars];
        if (v < wmin) wmin = v;
        if (v > wmax) wmax = v;
    }

    uint16_t vmin = (c->base  == CHART_BASE_FIXED) ? c->base_fixed : wmin;
    uint16_t vmax = (c->scale == CHART_SCALE_HIST) ? c->max_hist   : wmax;

    // if the range is flat we avoid dividing by zero
    uint32_t span = (vmax > vmin) ? (uint32_t)(vmax - vmin) : 1;

    // the label column, if any, is taken out of the bar area
    int16_t colw = 0;
    if (c->labels == CHART_LABELS_SIDE && c->label_font != NULL) {
        colw = (int16_t)c->label_font->width * (int16_t)c->label_digits
             + CHART_LABEL_GAP;
        if (colw > c->w / 2) {
            colw = c->w / 2;   // never eats more than half the chart
        }
    }

    int16_t bx = c->x + colw;   // origin and width of the bar area
    int16_t bw = c->w - colw;

    // Constant bar width. If it were derived from the difference between two
    // proportional edges, the remainder of the division would make some bars
    // come out 1 px wider than others, which is very noticeable with bars of
    // 3-4 px. By fixing it, the leftover falls into the gaps, which are
    // background and draw no attention.
    //
    // If bw is a multiple of bars nothing is left over. With the current
    // geometry (bw = 200, bars = 40) the cells come out at exactly 5 px.
    int16_t barw = (int16_t)(bw / (int16_t)c->bars) - (int16_t)c->gap;
    if (barw < 1) {
        barw = 1;
    }

    for (uint16_t i = 0; i < c->count; i++) {
        uint16_t v = c->vals[(c->head + i) % c->bars];

        // with a fixed baseline a value can fall below it (reading out of the
        // sensor range); we clamp it so the subtraction does not underflow
        uint16_t vb = (v > vmin) ? v : vmin;

        uint32_t hh = ((uint32_t)(vb - vmin) * (uint32_t)c->h) / span;
        if (hh < c->min_bar)          hh = c->min_bar;
        if (hh > (uint32_t)c->h)      hh = (uint32_t)c->h;

        // Bars are aligned to the RIGHT: the most recent one always takes the
        // last slot. While the buffer fills up, aligning to the left would make
        // the block grow rightwards, and once full the behaviour would switch
        // abruptly to sliding leftwards because of the FIFO. This way the
        // motion is the same from the very first reading: new data enters from
        // the right and the history runs off to the left.
        uint16_t slot = c->bars - c->count + i;

        // the left edge is proportional, so the block spans bw
        int16_t x1 = bx + (int16_t)(((uint32_t)slot * bw) / c->bars);

        // The second to last bar is narrowed, not the last one: that way the
        // gap appears to the left of the current reading without moving it.
        // With gap = 0 this is what detaches the present reading from the
        // solid history.
        int16_t bwidth = barw;
        if (c->last_gap && i + 2 == c->count) {
            bwidth -= (int16_t)c->last_gap;
            if (bwidth < 1) {
                bwidth = 1;
            }
        }

        int16_t x2 = x1 + bwidth - 1;

        int16_t y = c->y + c->h - (int16_t)hh;

        // the oldest bar sits on the left and the most recent on the right
        lcd_fill_rect(x1, y, x2, c->y + c->h - 1, c->color(v));
    }

    // labels: flush against the right edge of the column and at the height
    // they represent, the top of the scale above and the baseline below
    if (colw > 0) {
        char lbl[12];
        uint16_t lx = (uint16_t)(c->x + colw - CHART_LABEL_GAP);
        uint8_t  fh = c->label_font->height;

        snprintf(lbl, sizeof(lbl), "%u", (unsigned)vmax);
        lcd_text_aligned(lx, c->y, c->label_color, c->label_font, LCD_ALIGN_RIGHT, lbl);

        if (c->h >= 2 * fh) {   // if both do not fit, only the top is drawn
            snprintf(lbl, sizeof(lbl), "%u", (unsigned)vmin);
            lcd_text_aligned(lx, c->y + c->h - fh, c->label_color, c->label_font,
                             LCD_ALIGN_RIGHT, lbl);
        }
    }
}

// moves the chart vertically; each screen fits it into the space it leaves
void chart_place(chart_t *c, int16_t y, int16_t h)
{
    c->y = y;
    c->h = h;
}
