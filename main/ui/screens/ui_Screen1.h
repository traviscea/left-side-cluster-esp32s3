// Left gauge: STI-style fuel + coolant needles with digital oil pressure and
// oil temp in the middle. Matches the center tach (black face, red-orange
// backlit marks, Jost font).
// LVGL version: 8.4

#ifndef UI_SCREEN1_H
#define UI_SCREEN1_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------- Look & feel (edit these to taste) ----------------
#define UI_COOLANT_MIN_F       100   // "C" end of the coolant scale
#define UI_COOLANT_Q1_F        180   // temperature at the 1/4 mark
#define UI_COOLANT_MID_F       215   // temperature at the middle mark
#define UI_COOLANT_MAX_F       260   // "H" end of the coolant scale
#define UI_COOLANT_HATCH_POS  87.5f  // dotted hot zone starts at this mark (87.5 = the 7/8 mark)
#define UI_COOLANT_COLD_F      150   // coolant number is blue below this...
#define UI_COOLANT_HOT_F       205   // ...white in between, red above this

#define UI_FUEL_LOW_PCT         20   // fuel number turns red below this
#define UI_FUEL_LAMP_PCT        15   // orange low-fuel lamp (under the E) lights below this

#define UI_OIL_PRESS_LOW_PSI    25   // oil pressure is red below this (low = danger)...
#define UI_OIL_PRESS_HIGH_PSI  125   // ...blue above this (high = cold, thick oil), white between
#define UI_OIL_TEMP_COLD_F     150   // oil temp is blue below this...
#define UI_OIL_TEMP_HOT_F      220   // ...white in between, red above this

#define UI_COLOR_NEEDLE    0xFF2A12
#define UI_COLOR_VALUE     0xE6F0FF  // normal numbers (ice white, like the speedo)
#define UI_COLOR_COLD      0x4FB4FF  // numbers: cold
#define UI_COLOR_HOT       0xFF2020  // numbers: hot / low / danger
#define UI_COLOR_FUEL_LAMP 0xFF8A00  // low-fuel warning lamp (orange)
#define UI_COLOR_STATUS    0xFFC400  // restart message (yellow)

// ---------------- Needle damping (stock-like, calm needles) ----------------
// The hub numbers and oil readouts always show the live value; only the
// needles (and the low-fuel lamp) are smoothed.
#define UI_NEEDLE_TICK_MS       40   // how often the needles are updated

#define UI_FUEL_SMOOTH_S        20   // fuel averaging time: ignores sloshing
#define UI_FUEL_DEADBAND_PCT     2   // needle only moves once the average drifts this far
#define UI_FUEL_GLIDE_PCT_S      5   // how fast it glides when it does move (%/s)
#define UI_FUEL_JUMP_PCT        15   // bigger change than this (key-on, refuel) catches up quickly

#define UI_COOLANT_SMOOTH_S      4
#define UI_COOLANT_DEADBAND_F    2
#define UI_COOLANT_GLIDE_F_S    15
#define UI_COOLANT_JUMP_F       40

// ---------------- Startup (like the stock cluster at key-on) ----------------
// Screen dark until the needles appear and sweep to full scale and back to the
// live values, then the face fades in. Times are from when the screen is built.
#define UI_SWEEP_ENABLE      1     // 0 = no sweep
#define UI_SWEEP_DELAY_MS    1200  // needles appear and start sweeping (same as the center, after its STI badge)
#define UI_BACKLIGHT_ON_MS   300   // backlight on at this point of the center's startup (with its STI badge)
#define UI_SWEEP_WAIT_MS     5000  // wait this long for the center's timing; if none comes, sweep on our own
#define UI_SWEEP_UP_MS       800   // rest -> full scale
#define UI_SWEEP_HOLD_MS     250   // pause at full scale
#define UI_SWEEP_DOWN_MS     900   // full scale -> live value
#define UI_SWEEP_FADE_MS     400   // then the face fades in over this long


// ---------------- Face color themes ----------------
// The face (ticks, numerals, labels, icons) follows the current theme. Needles,
// readouts, warnings and the red dotted zones keep their own colors.
// The theme list must be identical on all three screens: the center sends the
// theme number to the side gauges.
typedef struct { const char *name; uint32_t dial; uint32_t dim; } ui_theme_t;
#define UI_THEME_COUNT 6
extern const ui_theme_t ui_themes[UI_THEME_COUNT];

uint32_t ui_theme_dial(void);              // current face color
uint32_t ui_theme_dim(void);               // current secondary color (captions)
uint8_t  ui_get_theme(void);
// Switch theme. Call from the LVGL thread (or with the LVGL lock held). Cheap
// to call every frame: it only redraws when the theme actually changes.
void     ui_set_theme(uint8_t index);

#define UI_COLOR_DIAL      (ui_theme_dial())
#define UI_COLOR_DIAL_DIM  (ui_theme_dim())
#define UI_COLOR_REDLINE   0xFF2A0A  // dotted red zones stay red on every theme

// 1 = pre-render the static dial into one image at boot (smoother needles).
#define UI_DIAL_USE_SNAPSHOT 1

// SCREEN: ui_Screen1
extern void ui_Screen1_screen_init(void);
extern void ui_Screen1_screen_destroy(void);
extern lv_obj_t *ui_Screen1;

// Update the gauges. Each one skips work when the value hasn't changed.
void ui_left_set_fuel(int percent);        // 0..100
void ui_left_set_coolant(int deg_f);
void ui_left_set_oil_pressure(int psi);
void ui_left_set_oil_temp(int deg_f);

// Called once when it's time to turn the backlight on (a set point in the
// startup, the same on all three screens). Register before ui_init().
void ui_set_backlight_cb(void (*cb)(void));

// Line up the startup with the center cluster's: center_ms is how far into its
// startup the center is right now (from its packets). Call on the LVGL thread.
// If the center is already past its startup, this one finishes straight away.
void ui_sweep_sync(uint32_t center_ms);

// Show a short status line at the bottom of the screen, then remove it after
// duration_ms (used to report an unexpected restart).
void ui_show_status(const char *text, uint32_t duration_ms);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif
