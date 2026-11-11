#include <stdint.h>
#include <stddef.h>

#include "co2_color.h"

#include "display.h"

//==============================================================================
// CO2 color ramp. The control points come from the indoor air quality
// thresholds; see docs/co2-color-scale.md.
//==============================================================================
typedef struct {
    uint16_t ppm;
    uint8_t  r, g, b;
} co2_stop_t;

static const co2_stop_t co2_scale[] = {
    {    0, 0x76, 0xB0, 0x43 },   // green
    {  800, 0x76, 0xB0, 0x43 },   // end of green: IDA 1 and 2 fall inside
    { 1000, 0xF2, 0xD0, 0x2E },   // full yellow at the most quoted threshold
    { 1400, 0xF2, 0xD0, 0x2E },   // end of the yellow plateau
    { 1700, 0xE0, 0x7B, 0x26 },   // orange
    { 2000, 0xE0, 0x7B, 0x26 },   // end of "elevated" per the UBA
    { 2500, 0x9E, 0x2B, 0x25 },   // "unacceptable": full red
    { 5000, 0x9E, 0x2B, 0x25 },   // stays red from here on
};

#define CO2_SCALE_N (sizeof(co2_scale) / sizeof(co2_scale[0]))

// linear interpolation between two channels; int32_t to allow a negative slope
static uint8_t lerp8(uint8_t a, uint8_t b, uint32_t t, uint32_t span)
{
    return (uint8_t)((int32_t)a + ((int32_t)b - (int32_t)a) * (int32_t)t / (int32_t)span);
}

uint16_t co2_color(uint32_t ppm)
{
    const co2_stop_t *first = &co2_scale[0];
    const co2_stop_t *last  = &co2_scale[CO2_SCALE_N - 1];

    if (ppm <= first->ppm) {
        return RGB565(first->r, first->g, first->b);
    }

    for (size_t i = 1; i < CO2_SCALE_N; i++) {
        const co2_stop_t *a = &co2_scale[i - 1];
        const co2_stop_t *b = &co2_scale[i];

        if (ppm <= b->ppm) {
            uint32_t span = (uint32_t)b->ppm - (uint32_t)a->ppm;
            uint32_t t    = ppm - (uint32_t)a->ppm;
            return RGB565(lerp8(a->r, b->r, t, span),
                          lerp8(a->g, b->g, t, span),
                          lerp8(a->b, b->b, t, span));
        }
    }

    // above the last control point it saturates
    return RGB565(last->r, last->g, last->b);
}
//==============================================================================
