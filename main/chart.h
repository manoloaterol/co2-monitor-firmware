#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "font.h"

//==============================================================================
// Generic bar chart.
//
// Each chart is a chart_t instance with its own configuration, bar buffer and
// state, so several can coexist.
//
// Each bar is the average of avg_n readings. Bars live in a ring buffer
// supplied by whoever declares the chart: once full, every new bar pushes out
// the oldest one (FIFO).
//
// Vertical scale: the baseline is always the minimum of the visible bars; the
// top depends on .scale (see chart_scale_t).
//==============================================================================

// returns the color of a bar from its value
typedef uint16_t (*chart_color_fn)(uint32_t value);

typedef enum {
    CHART_SCALE_HIST,     // top = all-time maximum since boot: stable scale,
                          // but one spike flattens the chart forever
    CHART_SCALE_WINDOW,   // top = maximum of the visible bars: autoscaling,
                          // the full height is always used
} chart_scale_t;

typedef enum {
    CHART_BASE_WINDOW,    // baseline = minimum of the visible bars: maximum
                          // contrast, but it jumps as the window slides
    CHART_BASE_FIXED,     // baseline = .base_fixed, e.g. the sensor minimum:
                          // stable and with physical meaning
} chart_base_t;

typedef enum {
    CHART_LABELS_NONE,    // no labels: the bars take the full width
    CHART_LABELS_SIDE,    // left column with the top of the scale at the top
                          // and the baseline at the bottom, each at its height
} chart_labels_t;

#define CHART_LABEL_GAP 3   // gap between the label column and the bars

typedef struct {
    // --- configuration ---
    int16_t  x, y, w, h;      // area the chart occupies
    uint16_t *vals;           // bar buffer (supplied by whoever declares it)
    uint16_t bars;            // how many bars fit in vals[]
    uint16_t avg_n;           // readings averaged into each bar
    uint8_t  gap;             // spacing between bars
    uint8_t  last_gap;        // extra spacing before the most recent bar
    uint8_t  min_bar;         // minimum height, so the lowest bar stays visible
    chart_scale_t  scale;
    chart_base_t   base;      // where the bar starts from
    uint16_t       base_fixed;// only used with CHART_BASE_FIXED
    chart_color_fn color;

    // --- scale labels (only with CHART_LABELS_SIDE) ---
    chart_labels_t labels;
    const font_t  *label_font;
    uint16_t       label_color;
    uint8_t        label_digits;  // reserved digits; sets the column width

    // --- internal state, do not touch by hand ---
    uint16_t count;           // valid bars in the buffer
    uint16_t head;            // index of the oldest bar
    uint16_t max_hist;        // all-time maximum since boot
    uint32_t acc_sum;         // accumulator for the bar in progress
    uint16_t acc_n;
} chart_t;

// accumulates a reading; every avg_n readings closes a bar, and then it
// returns true
bool chart_add_reading(chart_t *c, uint16_t v);

// clears the chart and its all-time maximum
void chart_reset(chart_t *c);

// moves the chart vertically, so each screen fits it into the space it
// leaves
void chart_place(chart_t *c, int16_t y, int16_t h);

void chart_draw(const chart_t *c);
