#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "screens.h"

#include "app_config.h"
#include "chart.h"
#include "fw_info.h"
#include "co2_color.h"
#include "sensor_data.h"

#include "display.h"
#include "logo_mat.h"

//---- text --------------------------------------------------------------------
// Every word the screens paint, per language. Upper case and plain ASCII: the
// bitmap fonts carry nothing else. "TEMP" and "HUM" and the boot screen labels
// read the same in both languages and stay out of the table.
typedef struct {
    const char *setup_title;
    const char *setup_join;
    const char *setup_open;
    const char *fault_title;
    const char *fault_hint;
    const char *enter_setup;
    const char *leave_setup;
    const char *restarting;
} screen_text_t;

static const screen_text_t s_text[APP_LANG_COUNT] = {
    [APP_LANG_EN] = {
        .setup_title = "SETUP",
        .setup_join  = "JOIN THE NETWORK",
        .setup_open  = "AND OPEN A BROWSER",
        .fault_title = "NO READINGS",
        .fault_hint  = "CHECK THE SENSOR",
        .enter_setup = "SETUP MODE",
        .leave_setup = "BACK TO NETWORK",
        .restarting  = "RESTARTING...",
    },
    [APP_LANG_ES] = {
        .setup_title = "CONFIGURAR",
        .setup_join  = "CONECTA A LA RED",
        .setup_open  = "Y ABRE EL NAVEGADOR",
        .fault_title = "SIN LECTURAS",
        .fault_hint  = "REVISA EL SENSOR",
        .enter_setup = "CONFIGURACION",
        .leave_setup = "VOLVER A LA RED",
        .restarting  = "REINICIANDO...",
    },
};

static const screen_text_t *s_tx = &s_text[APP_LANG_EN];

void screens_set_language(uint8_t lang)
{
    s_tx = &s_text[lang < APP_LANG_COUNT ? lang : APP_LANG_EN];
}

#define TX(field) (s_tx->field)

//---- boot screen -------------------------------------------------------------
// Logo and name in the upper half, versions in a label / value table below.
// The table is centered as a block, not line by line, so the values line up.
#define LOGO_Y        34
#define LOGO_NAME_Y  100
#define VER_Y        142   // first row of the table
#define VER_ROW       20
#define VER_GAP       12   // between the label and the value columns
#define VER_LABEL    RGB565(0x70, 0x70, 0x70)
#define VER_VALUE    RGB565(0xC8, 0xC8, 0xC8)
#define VER_BAD      RGB565(0xE4, 0x70, 0x7A)
#define VER_VALUE_MAX 10   // characters; longer values are cut

static void draw_version_row(int16_t x0, int16_t xv, int16_t y,
                             const char *label, const char *value, uint16_t color)
{
    char buf[VER_VALUE_MAX + 1];

    snprintf(buf, sizeof(buf), "%s", label);
    lcd_text(x0, y, VER_LABEL, &font_goldman_13x13, buf);
    snprintf(buf, sizeof(buf), "%s", value);
    lcd_text(xv, y, color, &font_goldman_13x13, buf);
}

void screens_logo(const char *sensor_fw)
{
    const char *label = "CO2 Monitor";
    int16_t cx = LCD_W / 2;

    lcd_clear();

    lcd_image((LCD_W - LOGO_MAT_W) / 2, LOGO_Y, LOGO_MAT_W, LOGO_MAT_H, img_logo_mat);

    lcd_text_aligned(cx, LOGO_NAME_Y, RGB565(0xF2, 0xD0, 0x2E), &font_goldman_16x16,
                     LCD_ALIGN_CENTER, label);

    // "1.3.0-dev+abc1234.dirty" does not fit the table: the version goes in
    // its row and the build metadata, when there is any, on a small line below.
    char ver[32];
    snprintf(ver, sizeof(ver), "%s", fw_info_version());
    char *meta = strchr(ver, '+');
    if (meta != NULL) {
        *meta++ = '\0';
    }

    const char *sensor = sensor_fw == NULL   ? "..."
                       : sensor_fw[0] == '\0' ? "ERROR"
                                              : sensor_fw;

    int16_t cw = font_goldman_13x13.width;
    int16_t wl = 5 * cw;   // "SCD30", the longest label
    size_t  nv = strlen(ver);
    if (strlen(fw_info_hw_revision()) > nv) nv = strlen(fw_info_hw_revision());
    if (strlen(sensor) > nv)                nv = strlen(sensor);
    if (nv > VER_VALUE_MAX)                 nv = VER_VALUE_MAX;
    int16_t x0 = cx - (int16_t)(wl + VER_GAP + (int16_t)nv * cw) / 2;
    int16_t xv = x0 + wl + VER_GAP;

    draw_version_row(x0, xv, VER_Y,               "FW",    ver, VER_VALUE);
    draw_version_row(x0, xv, VER_Y + VER_ROW,     "HW",    fw_info_hw_revision(), VER_VALUE);
    draw_version_row(x0, xv, VER_Y + 2 * VER_ROW, "SCD30", sensor,
                     sensor_fw != NULL && sensor_fw[0] == '\0' ? VER_BAD : VER_VALUE);

    if (meta != NULL) {
        lcd_text_aligned(cx, VER_Y + 3 * VER_ROW + 2, VER_LABEL, &font_fixed_6x9,
                         LCD_ALIGN_CENTER, meta);
    }

    lcd_flush();
}



//==============================================================================
// "CO2 <value> ppm" header on a single line, centered on cx.
//
// The value uses font_goldman_32x30 and the labels the same typeface at 16x16,
// sitting on the same baseline as the number.
//==============================================================================
#define CO2_HDR_GAP    8                          // gap between label and number
#define CO2_HDR_DIGITS 4                          // slot reserved for the number
#define CO2_HDR_LABEL  RGB565(0x90, 0x90, 0x90)   // label color

// No reading (ppm 0) is grey, never the green of a low value.
static uint16_t ppm_color(uint16_t ppm)
{
    return ppm != 0 ? co2_color(ppm) : CO2_HDR_LABEL;
}

static void draw_co2_header(int16_t cx, int16_t y, uint16_t ppm)
{
    char num[8];
    const char *left  = "CO2";
    const char *right = "ppm";

    if (ppm != 0) {
        snprintf(num, sizeof(num), "%u", (unsigned)ppm);
    } else {
        snprintf(num, sizeof(num), "---");
    }

    uint16_t wl   = lcd_text_width(&font_goldman_16x16, left);
    uint16_t wr   = lcd_text_width(&font_goldman_16x16, right);
    uint16_t wn   = lcd_text_width(&font_goldman_32x30, num);
    uint16_t slot = font_goldman_32x30.width * CO2_HDR_DIGITS;

    // the number always gets a slot of CO2_HDR_DIGITS digits, so the labels
    // do not move even when the value changes length
    int16_t x0 = cx - (int16_t)(wl + CO2_HDR_GAP + slot + CO2_HDR_GAP + wr) / 2;

    // both fonts were generated with the same margin below the baseline
    // (2 rows), so matching their bottom edges leaves them sitting on it
    int16_t ly = y + font_goldman_32x30.height - font_goldman_16x16.height;

    lcd_text(x0, ly, CO2_HDR_LABEL, &font_goldman_16x16, left);

    // the number is centered inside its slot
    int16_t pad = (int16_t)(slot - wn) / 2;
    if (pad < 0) {
        pad = 0;
    }
    lcd_text(x0 + wl + CO2_HDR_GAP + pad, y, ppm_color(ppm), &font_goldman_32x30, num);

    lcd_text(x0 + wl + CO2_HDR_GAP + slot + CO2_HDR_GAP, ly, CO2_HDR_LABEL, &font_goldman_16x16,
             right);
}
//==============================================================================


//==============================================================================
// Temperature and humidity row, in two columns with their label on top.
//
// The label uses font_goldman_13x13 and the value font_goldman_20x20, so the
// hierarchy against the CO2 value (32x30) reads 32 / 20 / 13.
//
// The degree sign does not exist in ASCII, so the Goldman fonts carry it in the
// circumflex slot: writing "23.4^C" paints "23.4<degree>C".
//==============================================================================
#define ENV_LABEL_Y   86    // line of the TEMP / HUM label
#define ENV_VALUE_Y  104    // line of the value
#define ENV_TEMP_CX   62    // center of the temperature column
#define ENV_HUM_CX   218    // center of the humidity column
#define ENV_LABEL    RGB565(0x70, 0x70, 0x70)
#define ENV_UNIT     RGB565(0x90, 0x90, 0x90)
#define ENV_VALUE    RGB565(0xC8, 0xC8, 0xC8)

// paints "<value><unit>" centered on cx, with the unit in a dimmer color
static void draw_env_value(int16_t cx, const char *value, const char *unit)
{
    uint16_t wv = lcd_text_width(&font_goldman_20x20, value);
    uint16_t wu = lcd_text_width(&font_goldman_20x20, unit);
    int16_t  x  = cx - (int16_t)(wv + wu) / 2;

    lcd_text(x, ENV_VALUE_Y, ENV_VALUE, &font_goldman_20x20, value);
    lcd_text(x + wv, ENV_VALUE_Y, ENV_UNIT, &font_goldman_20x20, unit);
}

static void draw_env_row(float temperature, float humidity)
{
    char buf[12];
    const char *lbl_t = "TEMP";
    const char *lbl_h = "HUM";
    const char *unit_t = "^C";   // '^' is the slot where the degree glyph lives
    const char *unit_h = "%";

    lcd_text(ENV_TEMP_CX - lcd_text_width(&font_goldman_13x13, lbl_t) / 2, ENV_LABEL_Y,
             ENV_LABEL, &font_goldman_13x13, lbl_t);
    lcd_text(ENV_HUM_CX - lcd_text_width(&font_goldman_13x13, lbl_h) / 2, ENV_LABEL_Y,
             ENV_LABEL, &font_goldman_13x13, lbl_h);

    // A NaN means there is no reading to show.
    if (isfinite(temperature)) {
        snprintf(buf, sizeof(buf), "%.1f", temperature);
    } else {
        snprintf(buf, sizeof(buf), "--");
    }
    draw_env_value(ENV_TEMP_CX, buf, unit_t);

    if (isfinite(humidity)) {
        snprintf(buf, sizeof(buf), "%.0f", humidity);
    } else {
        snprintf(buf, sizeof(buf), "--");
    }
    draw_env_value(ENV_HUM_CX, buf, unit_h);
}
//==============================================================================


//---- charts of this screen ---------------------------------------------------
#define SCD30_CO2_MIN 400   // lower bound of the SCD30 range (400..10000 ppm)

// Sized for the largest count allowed; screens_chart_setup() picks the one in
// use.
static uint16_t co2_chart_vals[APP_CHART_BARS_MAX];

static chart_t co2_chart = {
    // x=9 and w=263 are not arbitrary: they leave bw = 263 - 63 = 200, which
    // divides evenly among every bar count in APP_CHART_BARS (5 px cells with
    // the default 40), with no remainder to make bars of differing widths. It
    // also centers the chart on x=140, same as the header.
    .x = 9, .y = 152, .w = 263, .h = 68,
    .vals       = co2_chart_vals,
    .bars       = 40,   // overwritten by screens_chart_setup()
    .avg_n      = 10,
    .gap        = 0,   // bars touching: the chart reads as solid
    .last_gap   = 0,   // px taken off the bar before the newest one, to set it apart
    .min_bar    = 2,
    .scale      = CHART_SCALE_WINDOW,
    .base       = CHART_BASE_WINDOW,   // baseline follows the minimum of the visible bars
    .base_fixed = SCD30_CO2_MIN,
    .color      = co2_color,

    .labels       = CHART_LABELS_SIDE,
    .label_font   = &font_goldman_reg_15x17,
    .label_color  = RGB565(0x90, 0x90, 0x90),
    .label_digits = 4,
};
//------------------------------------------------------------------------------


//==============================================================================
// Screens.
//
// Each screen is a function that paints the frame buffer from the latest
// reading. To add a new one, write it and put it in screens[]; the button walks
// the table in order and wraps around.
//
// The chart is a single shared instance (the history has to be), so each screen
// moves it before painting according to the space it leaves.
//==============================================================================
typedef void (*screen_draw_fn)(const sensor_data_t *d);

typedef struct {
    const char     *name;
    screen_draw_fn  draw;
} screen_t;

// CO2 only. Without the temperature and humidity row there is
// room to spare for a taller chart.
static void screen_co2(const sensor_data_t *d)
{
    // vertical budget: 30 of margin, 30 of header, 40 of air, 110 of chart
    // and 30 of margin = 240, with equal top and bottom margins
    chart_place(&co2_chart, 100, 110);
    draw_co2_header(140, 30, d->ppm);
    chart_draw(&co2_chart);
}

// temperature and humidity as well
static void screen_co2_env(const sensor_data_t *d)
{
    // the header sits at y=30 as in screen_co2, so it does not jump when moving
    // between screens with the joystick. The bottom margin is 30 as well, so
    // the chart baseline ends up at the same height in both
    chart_place(&co2_chart, 146, 64);
    draw_co2_header(140, 30, d->ppm);
    draw_env_row(d->temperature, d->humidity);
    chart_draw(&co2_chart);
}

//---- giant value screen ------------------------------------------------------
// The frame is painted in the color of the level: from a distance the state
// reads even when the figure itself cannot.
//
// It uses an inset because the display has rounded corners (R5 mm, about
// 37 px): a frame flush against the edge would be clipped by the bezel. With
// inset 8 and radius 30 the frame arc stays ~6 px inside the bezel arc.
#define BIG_FRAME_INSET   8
#define BIG_FRAME_RADIUS 30
#define BIG_FRAME_THICK   4
#define BIG_LABEL_GAP_UP 34   // distance from the top label to the number
#define BIG_LABEL_GAP_DN 18   // and from the bottom one

static void screen_co2_big(const sensor_data_t *d)
{
    char num[8];
    const char *left  = "CO2";
    const char *right = "ppm";

    uint16_t col = ppm_color(d->ppm);
    int16_t  cx  = LCD_W / 2;

    if (d->ppm != 0) {
        snprintf(num, sizeof(num), "%u", (unsigned)d->ppm);
    } else {
        snprintf(num, sizeof(num), "---");
    }

    for (uint8_t k = 0; k < BIG_FRAME_THICK; k++) {
        lcd_round_rect(BIG_FRAME_INSET + k, BIG_FRAME_INSET + k,
                       LCD_W - 2 * (BIG_FRAME_INSET + k), LCD_H - 2 * (BIG_FRAME_INSET + k),
                       BIG_FRAME_RADIUS - k, col);
    }

    int16_t y = (LCD_H - font_russo_58x90.height) / 2;

    lcd_text_aligned(cx, y, col, &font_russo_58x90, LCD_ALIGN_CENTER, num);

    lcd_text_aligned(cx, y - BIG_LABEL_GAP_UP, CO2_HDR_LABEL, &font_goldman_16x16,
                     LCD_ALIGN_CENTER, left);

    lcd_text_aligned(cx, y + font_russo_58x90.height + BIG_LABEL_GAP_DN, CO2_HDR_LABEL,
                     &font_goldman_16x16, LCD_ALIGN_CENTER, right);
}
//------------------------------------------------------------------------------

static const screen_t screens[] = {
    { "Big CO2",           screen_co2_big },   // default
    { "CO2",               screen_co2     },
    { "CO2 + environment", screen_co2_env },
};

#define SCREEN_COUNT (sizeof(screens) / sizeof(screens[0]))


//---- setup screen ------------------------------------------------------------
// Shown while the device runs its provisioning access point. Without it the
// user has no way of knowing which network to join, and the whole captive
// portal is unreachable.

static const char *s_setup_ssid;

void screens_set_setup(const char *ap_ssid)
{
    s_setup_ssid = ap_ssid;
}

static void screen_setup(const sensor_data_t *d)
{
    (void)d;

    int16_t cx = LCD_W / 2;
    int16_t y  = 46;

    lcd_text_aligned(cx, y, CO2_HDR_LABEL, &font_goldman_16x16, LCD_ALIGN_CENTER,
                     TX(setup_title));

    y += 52;
    lcd_text_aligned(cx, y, LCD_WHITE, &font_goldman_13x13, LCD_ALIGN_CENTER, TX(setup_join));

    y += 26;
    lcd_text_aligned(cx, y, RGB565(0xF2, 0xD0, 0x2E), &font_goldman_reg_15x17, LCD_ALIGN_CENTER,
                     s_setup_ssid != NULL ? s_setup_ssid : "?");

    y += 50;
    lcd_text_aligned(cx, y, CO2_HDR_LABEL, &font_goldman_13x13, LCD_ALIGN_CENTER,
                     TX(setup_open));

    y += 22;
    lcd_text_aligned(cx, y, CO2_HDR_LABEL, &font_goldman_13x13, LCD_ALIGN_CENTER,
                     "192.168.4.1");
}

//---- restart notice ------------------------------------------------------------
// Painted straight away, outside the normal redraw, because the restart follows
// at once: without it a long press looked like nothing happened, or like a
// crash, for the seconds the boot takes to reach the next screen.
void screens_show_restart(bool entering_setup)
{
    int16_t cx = LCD_W / 2;

    lcd_clear();
    lcd_text_aligned(cx, 96, RGB565(0xF2, 0xD0, 0x2E), &font_goldman_16x16, LCD_ALIGN_CENTER,
                     entering_setup ? TX(enter_setup) : TX(leave_setup));
    lcd_text_aligned(cx, 128, CO2_HDR_LABEL, &font_goldman_13x13, LCD_ALIGN_CENTER,
                     TX(restarting));
    lcd_flush();
}

//---- public interface --------------------------------------------------------

uint8_t screens_count(void)
{
    return (uint8_t)SCREEN_COUNT + (s_setup_ssid != NULL ? 1 : 0);
}

const char *screens_name(uint8_t idx)
{
    if (s_setup_ssid != NULL) {
        if (idx == 0) {
            return "Setup";
        }
        idx--;
    }
    return screens[idx % SCREEN_COUNT].name;
}

//---- sensor warning ----------------------------------------------------------
// A box over the lower half, where every screen has its chart or the bottom of
// the big figure. It covers the history rather than erasing it: the chart is
// still there once the sensor comes back. As wide as the chart with its scale
// labels (co2_chart.x and .w), so no stray digit shows at the sides.
#define FAULT_Y  128
#define FAULT_H   62
#define FAULT_BG RGB565(0x9E, 0x2B, 0x25)

static bool s_sensor_fault;

void screens_set_sensor_fault(bool fault)
{
    s_sensor_fault = fault;
}

static void draw_sensor_fault(void)
{
    int16_t cx = LCD_W / 2;

    lcd_fill_rect(co2_chart.x, FAULT_Y, co2_chart.x + co2_chart.w - 1, FAULT_Y + FAULT_H - 1,
                  FAULT_BG);
    lcd_text_aligned(cx, FAULT_Y + 12, LCD_WHITE, &font_goldman_16x16, LCD_ALIGN_CENTER,
                     TX(fault_title));
    lcd_text_aligned(cx, FAULT_Y + 36, LCD_WHITE, &font_goldman_13x13, LCD_ALIGN_CENTER,
                     TX(fault_hint));
}

void screens_draw(uint8_t idx, const sensor_data_t *d)
{
    // The setup screen goes first so it is what shows at boot, which is the
    // only moment it matters; the readings stay one click away.
    if (s_setup_ssid != NULL) {
        if (idx == 0) {
            screen_setup(d);
            return;
        }
        idx--;
    }
    screens[idx % SCREEN_COUNT].draw(d);

    if (s_sensor_fault) {
        draw_sensor_fault();
    }
}

void screens_chart_setup(uint16_t bars, uint16_t avg_n)
{
    if (bars == 0 || bars > APP_CHART_BARS_MAX) {
        bars = APP_CHART_BARS_MAX;   // app_config never passes this; stay in bounds
    }
    co2_chart.bars  = bars;
    co2_chart.avg_n = avg_n != 0 ? avg_n : 1;
    chart_reset(&co2_chart);
}

// The chart is unique and so must its history be, so it is not exposed: the
// loop only feeds it readings.
bool screens_add_reading(uint16_t ppm)
{
    return chart_add_reading(&co2_chart, ppm);
}
//==============================================================================

