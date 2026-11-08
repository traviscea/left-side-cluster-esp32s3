// Left gauge: STI-style fuel + coolant needles with digital oil pressure and
// oil temp in the middle. Matches the center tach (black face, red-orange
// backlit marks, Jost font).
// LVGL version: 8.4

#ifndef UI_SCREEN1_H
#define UI_SCREEN1_H

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
#define UI_COOLANT_HOT_F       212   // ...white in between, red above this

#define UI_FUEL_LOW_PCT         20   // fuel number turns red below this
#define UI_FUEL_LAMP_PCT        15   // orange low-fuel lamp (under the E) lights below this

#define UI_OIL_PRESS_LOW_PSI    25   // oil pressure is red below this (low = danger)...
#define UI_OIL_PRESS_HIGH_PSI  125   // ...blue above this (high = cold, thick oil), white between
#define UI_OIL_TEMP_COLD_F     150   // oil temp is blue below this...
#define UI_OIL_TEMP_HOT_F      220   // ...white in between, red above this

#define UI_COLOR_DIAL      0xFF3D1F  // backlit red-orange (same as center tach)
#define UI_COLOR_DIAL_DIM  0xB82A14  // captions / units
#define UI_COLOR_NEEDLE    0xFF2A12
#define UI_COLOR_VALUE     0xE6F0FF  // normal numbers (ice white, like the speedo)
#define UI_COLOR_COLD      0x4FB4FF  // numbers: cold
#define UI_COLOR_HOT       0xFF2020  // numbers: hot / low / danger
#define UI_COLOR_FUEL_LAMP 0xFF8A00  // low-fuel warning lamp (orange)

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

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif
