// Left gauge, styled after the stock GD STI fuel/coolant gauge:
//   * fuel needle on the left (E bottom, F top), coolant needle on the right
//     (C bottom, H top, stippled hot zone), red-orange backlit marks
//   * the fuel % and coolant temp are shown inside the needle hubs
//   * oil pressure (top) and oil temp (bottom) as digital readouts
// Designed for the 480x480 round display.
//
// The two scales sit on arcs that follow the round screen edge, while each
// needle pivots from its own hub. Every tick is placed where its needle
// direction meets that arc, so the needle always points exactly at it.
//
// The static dial is drawn once and flattened into one image with
// lv_snapshot at boot, so a needle move only re-blits a slice of it.

#include "../ui.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define UI_BASE_RES 480
#define K           ((float)LV_HOR_RES / UI_BASE_RES)   // scale for 480 -> actual
#define UI_SCALE(x) ((lv_coord_t)lroundf((x) * K))

// ---- Geometry (in 480px space, origin at the screen center, y down) ----
#define PIVOT_X     78.0f    // needle hubs at (-78, 0) and (+78, 0)
#define SCALE_R     228.0f   // outer edge of both scales, concentric with the screen
#define HALF_SPAN   40.0f    // each scale covers 180+-40 deg (fuel) / 0+-40 deg (coolant)
#define HUB_D       88

LV_FONT_DECLARE(ui_font_letters_36);
LV_FONT_DECLARE(ui_font_value_66);
LV_FONT_DECLARE(ui_font_caption_20);
LV_FONT_DECLARE(ui_font_hub_32);
LV_IMG_DECLARE(ui_icon_fuel);
LV_IMG_DECLARE(ui_icon_coolant);

lv_obj_t *ui_Screen1 = NULL;

typedef struct {
    float pivot_x;                  // -PIVOT_X (fuel) or +PIVOT_X (coolant)
    lv_obj_t *needle, *glow;        // lv_line objects
    lv_point_t pts[2], glow_pts[2];
} gauge_t;

static gauge_t fuel_g = { .pivot_x = -PIVOT_X };
static gauge_t cool_g = { .pivot_x =  PIVOT_X };

static lv_obj_t *fuel_lamp;
static lv_obj_t *oilp_value, *oilt_value;
static lv_obj_t *fuel_hub_value, *cool_hub_value;
static lv_img_dsc_t *dial_snapshot = NULL;

static int last_fuel = -1, last_cool = -1000, last_oilp = -1000, last_oilt = -1000;

// Damped needle state: target = latest value, avg = smoothed value,
// shown = where the needle actually is (glides toward avg).
typedef struct {
    float target, avg, shown, drawn_angle;
    float avg_mark;                 // avg one second ago, to tell when it has settled
    float mark_age;
    bool seeded, moving;
} damp_t;
static damp_t fuel_d, cool_d;
static lv_timer_t *needle_timer = NULL;
static bool fuel_lamp_on = false;

// ---------------------------------------------------------------- geometry

static const float D2R = 3.14159265f / 180.0f;

// Screen angle (deg, clockwise from 3 o'clock) of a value 0..100 on each scale
static float fuel_angle(float v) { return 180.0f - HALF_SPAN + v * (2 * HALF_SPAN) / 100.0f; } // E low-left -> F up-left
static float cool_angle(float p) { return HALF_SPAN - p * (2 * HALF_SPAN) / 100.0f; }          // C low-right -> H up-right

// Point on the scale arc (480px space)
static void arc_point(float ang_deg, float r, float *x, float *y)
{
    *x = r * cosf(ang_deg * D2R);
    *y = r * sinf(ang_deg * D2R);
}

// ---------------------------------------------------------------- helpers

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *txt)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

static void place(lv_obj_t *o, float x, float y)
{
    lv_obj_align(o, LV_ALIGN_CENTER, UI_SCALE(x), UI_SCALE(y));
}

// ---------------------------------------------------------------- static dial drawing

typedef struct {
    lv_draw_ctx_t *ctx;
    float cx, cy;                   // screen center in absolute px
} dctx_t;

static lv_point_t to_px(const dctx_t *d, float x, float y)
{
    lv_point_t p = { (lv_coord_t)lroundf(d->cx + x * K), (lv_coord_t)lroundf(d->cy + y * K) };
    return p;
}

// Tick at a screen angle, pointing from the arc toward that gauge's hub
static void draw_tick(const dctx_t *d, float pivot_x, float ang, float len, float width)
{
    float ox, oy;
    arc_point(ang, SCALE_R, &ox, &oy);
    float dx = pivot_x - ox, dy = -oy;
    float n = sqrtf(dx * dx + dy * dy);
    lv_point_t a = to_px(d, ox, oy);
    lv_point_t b = to_px(d, ox + dx / n * len, oy + dy / n * len);

    lv_draw_line_dsc_t ld;
    lv_draw_line_dsc_init(&ld);
    ld.color = lv_color_hex(UI_COLOR_DIAL);
    ld.width = UI_SCALE(width) < 1 ? 1 : UI_SCALE(width);
    lv_draw_line(d->ctx, &ld, &a, &b);
}

static void draw_scale(const dctx_t *d, float pivot_x, float (*angle_of)(float))
{
    // Continuous thin line along the screen edge
    float a0 = angle_of(0), a1 = angle_of(100);
    float lo = a0 < a1 ? a0 : a1, hi = a0 < a1 ? a1 : a0;
    lv_draw_arc_dsc_t ad;
    lv_draw_arc_dsc_init(&ad);
    ad.color = lv_color_hex(UI_COLOR_DIAL);
    ad.width = UI_SCALE(3);
    lv_point_t c = { (lv_coord_t)lroundf(d->cx), (lv_coord_t)lroundf(d->cy) };
    int16_t s = (int16_t)lroundf(lo), e = (int16_t)lroundf(hi);
    if (s < 0) s += 360;
    if (e < 0) e += 360;
    lv_draw_arc(d->ctx, &ad, &c, UI_SCALE(SCALE_R), (uint16_t)s, (uint16_t)e);

    // Eighths (short), quarters (medium), ends (long, heavy)
    for (int i = 0; i <= 8; i++) {
        float ang = angle_of(i * 12.5f);
        if (i == 0 || i == 8)      draw_tick(d, pivot_x, ang, 30, 6);
        else if (i % 2 == 0)       draw_tick(d, pivot_x, ang, 20, 4);
        else                       draw_tick(d, pivot_x, ang, 12, 3);
    }
}

// Angle of a point as seen from a hub (deg)
static float hub_angle(float pivot_x, float x, float y)
{
    return atan2f(y, x - pivot_x) / D2R;
}

// Staggered dot band just inside the arc, between two scale angles (hot zone).
// Dots are laid out in short columns that point at the hub, exactly like the
// tick marks. A column is either fully in or fully out, so both ends of the
// band are straight lines parallel to the end ticks, and the outer dots follow
// the arc.
static void draw_stipple(const dctx_t *d, float pivot_x, float ang_from, float ang_to)
{
    if (ang_from > ang_to) { float t = ang_from; ang_from = ang_to; ang_to = t; }

    // Band edges as seen from the hub (that's how the ticks are angled)
    float x0, y0, x1, y1;
    arc_point(ang_from, SCALE_R, &x0, &y0);
    arc_point(ang_to, SCALE_R, &x1, &y1);
    float h0 = hub_angle(pivot_x, x0, y0), h1 = hub_angle(pivot_x, x1, y1);
    float hlo = h0 < h1 ? h0 : h1, hhi = h0 < h1 ? h1 : h0;

    lv_draw_rect_dsc_t dot;
    lv_draw_rect_dsc_init(&dot);
    dot.radius = LV_RADIUS_CIRCLE;
    dot.bg_color = lv_color_hex(UI_COLOR_DIAL);
    dot.bg_opa = LV_OPA_COVER;
    lv_coord_t ds = UI_SCALE(3) < 2 ? 2 : UI_SCALE(3);

    const float col_gap = 2.5f;               // spacing between columns along the arc (px)
    const float edge_gap = 4.5f;              // keep dots clear of the end ticks (px)
    static const float depth_even[] = { 8, 18 };   // distance in from the arc (px)
    static const float depth_odd[]  = { 13, 23 };  // offset rows -> staggered pattern

    float step = col_gap / SCALE_R / D2R;

    // First pass: find the last column that's inside the band
    int last_col = -1, col = 0;
    for (float ang = ang_from - 6; ang <= ang_to + 6; ang += step, col++) {
        float ox, oy;
        arc_point(ang, SCALE_R, &ox, &oy);
        float n = sqrtf((pivot_x - ox) * (pivot_x - ox) + oy * oy);
        float h = hub_angle(pivot_x, ox, oy);
        float margin = edge_gap / n / D2R;
        if (h >= hlo + margin && h <= hhi - margin) last_col = col;
    }

    col = 0;
    for (float ang = ang_from - 6; ang <= ang_to + 6; ang += step, col++) {
        float ox, oy;
        arc_point(ang, SCALE_R, &ox, &oy);
        float dx = pivot_x - ox, dy = -oy;
        float n = sqrtf(dx * dx + dy * dy);
        float h = hub_angle(pivot_x, ox, oy);
        float margin = edge_gap / n / D2R;
        // The final column's deepest dot would stick out below the band, so
        // it's moved one column further, into the empty spot next to the arc.
        bool extra = (col == last_col + 1) && (last_col & 1);
        if (!extra && (h < hlo + margin || h > hhi - margin)) continue;

        static const float depth_last_odd[] = { 13 };
        static const float depth_extra[] = { 8 };
        const float *depths = depth_even;
        int count = 2;
        if (extra)                      { depths = depth_extra;    count = 1; }
        else if ((col & 1) && col == last_col) { depths = depth_last_odd; count = 1; }
        else if (col & 1)               { depths = depth_odd; }
        for (int k = 0; k < count; k++) {
            lv_point_t p = to_px(d, ox + dx / n * depths[k], oy + dy / n * depths[k]);
            lv_area_t a = { p.x - ds / 2, p.y - ds / 2, p.x - ds / 2 + ds - 1, p.y - ds / 2 + ds - 1 };
            lv_draw_rect(d->ctx, &dot, &a);
        }
    }
}

// Coolant temperature -> scale position 0..100 (C..H), calibrated so the
// 1/4 mark is UI_COOLANT_Q1_F and the middle mark is UI_COOLANT_MID_F.
// Straight lines between: C -> 1/4 -> middle -> H.
static float cool_pos(float deg_f)
{
    float p;
    if (deg_f <= UI_COOLANT_Q1_F)
        p = (deg_f - UI_COOLANT_MIN_F) * 25.0f / (UI_COOLANT_Q1_F - UI_COOLANT_MIN_F);
    else if (deg_f <= UI_COOLANT_MID_F)
        p = 25.0f + (deg_f - UI_COOLANT_Q1_F) * 25.0f / (UI_COOLANT_MID_F - UI_COOLANT_Q1_F);
    else
        p = 50.0f + (deg_f - UI_COOLANT_MID_F) * 50.0f / (UI_COOLANT_MAX_F - UI_COOLANT_MID_F);
    return p < 0 ? 0 : p > 100 ? 100 : p;
}

static void dial_draw_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    dctx_t d = {
        .ctx = lv_event_get_draw_ctx(e),
        .cx = a.x1 + lv_area_get_width(&a) / 2.0f,
        .cy = a.y1 + lv_area_get_height(&a) / 2.0f,
    };

    float hatch_p = UI_COOLANT_HATCH_POS;
    draw_stipple(&d, PIVOT_X, cool_angle(hatch_p), cool_angle(100));

    draw_scale(&d, -PIVOT_X, fuel_angle);
    draw_scale(&d,  PIVOT_X, cool_angle);
}

static lv_obj_t *build_static_dial(lv_obj_t *parent)
{
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_HOR_RES, LV_VER_RES);
    lv_obj_center(root);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x0B0B0B), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(root, dial_draw_cb, LV_EVENT_DRAW_MAIN_END, NULL);

    // F / E / H / C, just inside the scale ends
    float x, y;
    const float lr = 188, inset = 8;
    arc_point(fuel_angle(100) - inset, lr, &x, &y);
    place(make_label(root, &ui_font_letters_36, UI_COLOR_DIAL, "F"), x, y);
    arc_point(fuel_angle(0) + inset, lr, &x, &y);
    place(make_label(root, &ui_font_letters_36, UI_COLOR_DIAL, "E"), x, y);
    arc_point(cool_angle(100) + inset, lr, &x, &y);
    place(make_label(root, &ui_font_letters_36, UI_COLOR_DIAL, "H"), x, y);
    arc_point(cool_angle(0) - inset, lr, &x, &y);
    place(make_label(root, &ui_font_letters_36, UI_COLOR_DIAL, "C"), x, y);

    // Captions sit on the hub side of each oil readout
    place(make_label(root, &ui_font_caption_20, UI_COLOR_DIAL_DIM, "OIL PRESS (psi)"), 0, -124);
    place(make_label(root, &ui_font_caption_20, UI_COLOR_DIAL_DIM, "OIL TEMP (\xC2\xB0" "F)"), 0, 124);

    return root;
}

// ---------------------------------------------------------------- live parts

static lv_obj_t *make_icon(lv_obj_t *parent, const lv_img_dsc_t *src, float x, float y)
{
    lv_obj_t *img = lv_img_create(parent);
    lv_img_set_src(img, src);
    lv_obj_set_style_img_recolor(img, lv_color_hex(UI_COLOR_DIAL), 0);
    lv_obj_set_style_img_recolor_opa(img, LV_OPA_COVER, 0);
    if (LV_HOR_RES != UI_BASE_RES) lv_img_set_zoom(img, (uint16_t)(256 * K));
    place(img, x, y);
    return img;
}

static void make_readout(lv_obj_t *parent, float y, lv_obj_t **value_out)
{
    *value_out = make_label(parent, &ui_font_value_66, UI_COLOR_VALUE, "--");
    place(*value_out, 0, y);
}

static lv_obj_t *make_line(lv_obj_t *parent, lv_coord_t width, lv_opa_t opa)
{
    lv_obj_t *l = lv_line_create(parent);
    lv_obj_set_style_line_width(l, width, 0);
    lv_obj_set_style_line_color(l, lv_color_hex(UI_COLOR_NEEDLE), 0);
    lv_obj_set_style_line_opa(l, opa, 0);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

// Move a line object so it only covers the needle's own bounding box; keeps
// redraws small instead of invalidating the whole screen.
static void set_line(lv_obj_t *l, lv_point_t *pts, lv_coord_t x0, lv_coord_t y0, lv_coord_t x1, lv_coord_t y1)
{
    lv_coord_t minx = LV_MIN(x0, x1), miny = LV_MIN(y0, y1);
    lv_obj_invalidate(l);
    lv_obj_set_pos(l, minx, miny);
    pts[0].x = x0 - minx; pts[0].y = y0 - miny;
    pts[1].x = x1 - minx; pts[1].y = y1 - miny;
    lv_line_set_points(l, pts, 2);
}

// Point the needle at a screen angle on the scale arc
static void needle_to(gauge_t *g, float ang)
{
    float ox, oy;
    arc_point(ang, SCALE_R, &ox, &oy);
    float dx = ox - g->pivot_x, dy = oy;
    float n = sqrtf(dx * dx + dy * dy);
    float len = n - 10;                       // stop just inside the arc line
    float cx = LV_HOR_RES / 2.0f, cy = LV_VER_RES / 2.0f;
    lv_coord_t x0 = (lv_coord_t)lroundf(cx + g->pivot_x * K);
    lv_coord_t y0 = (lv_coord_t)lroundf(cy);
    lv_coord_t x1 = (lv_coord_t)lroundf(cx + (g->pivot_x + dx / n * len) * K);
    lv_coord_t y1 = (lv_coord_t)lroundf(cy + (dy / n * len) * K);
    set_line(g->glow, g->glow_pts, x0, y0, x1, y1);
    set_line(g->needle, g->pts, x0, y0, x1, y1);
}

static void fuel_lamp_set(bool on)
{
    lv_color_t lit = lv_color_hex(UI_COLOR_FUEL_LAMP);
    lv_obj_set_style_bg_color(fuel_lamp, on ? lit : lv_color_hex(0x161616), 0);
    lv_obj_set_style_border_color(fuel_lamp, on ? lit : lv_color_hex(0x262626), 0);
    lv_obj_set_style_outline_opa(fuel_lamp, on ? LV_OPA_40 : LV_OPA_TRANSP, 0);
}

static lv_obj_t *make_hub(lv_obj_t *parent, float x, lv_obj_t **value_out)
{
    lv_obj_t *h = lv_obj_create(parent);
    lv_obj_remove_style_all(h);
    lv_obj_set_size(h, UI_SCALE(HUB_D), UI_SCALE(HUB_D));
    place(h, x, 0);
    lv_obj_set_style_radius(h, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(h, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(h, lv_color_hex(0x202123), 0);
    lv_obj_set_style_bg_grad_color(h, lv_color_hex(0x0A0A0B), 0);
    lv_obj_set_style_bg_grad_dir(h, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(h, UI_SCALE(3), 0);
    lv_obj_set_style_border_color(h, lv_color_hex(0x3C3E41), 0);
    lv_obj_clear_flag(h, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    *value_out = make_label(h, &ui_font_hub_32, UI_COLOR_DIAL, "--");
    lv_obj_center(*value_out);
    return h;
}

// ---------------------------------------------------------------- screen

// ---------------------------------------------------------------- needle damping

// One step of: average the input, then glide the needle toward the average
// only once it has drifted past the deadband. Returns true if 'shown' changed.
static bool damp_step(damp_t *d, float dt, float smooth_s, float deadband, float glide_per_s, float jump)
{
    if (!d->seeded) return false;

    // Big change (key-on, refuel, real event): catch up fast instead of
    // slowly averaging toward it
    float tau = fabsf(d->target - d->avg) > jump ? 0.5f : smooth_s;
    d->avg += (d->target - d->avg) * (dt / (tau + dt));

    float err = d->avg - d->shown;
    if (!d->moving && fabsf(err) >= deadband) d->moving = true;
    if (!d->moving) return false;

    float max_step = glide_per_s * dt;
    if (fabsf(err) > jump) max_step *= 8;     // large catch-up glides faster
    if (err > max_step) err = max_step;
    if (err < -max_step) err = -max_step;
    d->shown += err;
    // Keep following until the average itself has stopped drifting (moved less
    // than a tenth of the deadband in the last second), so the needle doesn't
    // come to rest short of the real value -- but noise alone won't keep it moving
    d->mark_age += dt;
    bool settled = false;
    if (d->mark_age >= 1.0f) {
        settled = fabsf(d->avg - d->avg_mark) < deadband * 0.1f;
        d->avg_mark = d->avg;
        d->mark_age = 0;
    }
    if (settled && fabsf(d->avg - d->shown) < 0.05f) d->moving = false;
    return true;
}

static void seed(damp_t *d, float v)
{
    d->target = v;
    if (!d->seeded) {                         // first real value: no slow crawl from zero
        d->avg = d->shown = d->avg_mark = v;
        d->seeded = true;
        d->moving = true;
    }
}

static void needle_timer_cb(lv_timer_t *t)
{
    (void)t;
    const float dt = UI_NEEDLE_TICK_MS / 1000.0f;

    damp_step(&fuel_d, dt, UI_FUEL_SMOOTH_S, UI_FUEL_DEADBAND_PCT, UI_FUEL_GLIDE_PCT_S, UI_FUEL_JUMP_PCT);
    damp_step(&cool_d, dt, UI_COOLANT_SMOOTH_S, UI_COOLANT_DEADBAND_F, UI_COOLANT_GLIDE_F_S, UI_COOLANT_JUMP_F);

    // Only redraw a needle when it has actually moved a visible amount
    if (fuel_d.seeded) {
        float a = fuel_angle(fuel_d.shown);
        if (fabsf(a - fuel_d.drawn_angle) >= 0.2f) { needle_to(&fuel_g, a); fuel_d.drawn_angle = a; }

        // Low-fuel lamp follows the smoothed level, with a little hysteresis,
        // so sloshing near the threshold doesn't make it flicker
        bool on = fuel_lamp_on ? fuel_d.avg < UI_FUEL_LAMP_PCT + 1.0f : fuel_d.avg < UI_FUEL_LAMP_PCT;
        if (on != fuel_lamp_on) { fuel_lamp_on = on; fuel_lamp_set(on); }
    }
    if (cool_d.seeded) {
        float a = cool_angle(cool_pos(cool_d.shown));
        if (fabsf(a - cool_d.drawn_angle) >= 0.2f) { needle_to(&cool_g, a); cool_d.drawn_angle = a; }
    }
}

void ui_Screen1_screen_init(void)
{
    ui_Screen1 = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_Screen1, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui_Screen1, lv_color_hex(0x0B0B0B), 0);
    lv_obj_set_style_bg_opa(ui_Screen1, LV_OPA_COVER, 0);

    lv_obj_t *dial = build_static_dial(ui_Screen1);
    lv_obj_update_layout(ui_Screen1);
#if LV_USE_SNAPSHOT && UI_DIAL_USE_SNAPSHOT
    dial_snapshot = lv_snapshot_take(dial, LV_IMG_CF_TRUE_COLOR);
    if (dial_snapshot) {
        lv_obj_del(dial);
        lv_obj_t *img = lv_img_create(ui_Screen1);
        lv_img_set_src(img, dial_snapshot);
        lv_obj_center(img);
        lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
    }
#endif

    // Icons (always dial red)
    float x, y;
    arc_point(fuel_angle(50), 178, &x, &y);
    make_icon(ui_Screen1, &ui_icon_fuel, x, y);
    arc_point(cool_angle(50), 178, &x, &y);
    make_icon(ui_Screen1, &ui_icon_coolant, x, y);

    // Low-fuel lamp just below the E, like the stock warning light
    fuel_lamp = lv_obj_create(ui_Screen1);
    lv_obj_remove_style_all(fuel_lamp);
    lv_obj_set_size(fuel_lamp, UI_SCALE(26), UI_SCALE(26));
    lv_obj_set_style_radius(fuel_lamp, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(fuel_lamp, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(fuel_lamp, UI_SCALE(2), 0);
    lv_obj_set_style_outline_width(fuel_lamp, UI_SCALE(6), 0);
    lv_obj_set_style_outline_pad(fuel_lamp, 0, 0);
    lv_obj_set_style_outline_color(fuel_lamp, lv_color_hex(UI_COLOR_FUEL_LAMP), 0);
    lv_obj_clear_flag(fuel_lamp, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    place(fuel_lamp, -140, 150);
    fuel_lamp_set(false);

    // Oil readouts, out toward the top and bottom edges
    make_readout(ui_Screen1, -170, &oilp_value);
    make_readout(ui_Screen1, 170, &oilt_value);

    // Needles (soft glow under each), then hubs on top
    fuel_g.glow = make_line(ui_Screen1, UI_SCALE(14), LV_OPA_30);
    fuel_g.needle = make_line(ui_Screen1, UI_SCALE(6), LV_OPA_COVER);
    cool_g.glow = make_line(ui_Screen1, UI_SCALE(14), LV_OPA_30);
    cool_g.needle = make_line(ui_Screen1, UI_SCALE(6), LV_OPA_COVER);
    make_hub(ui_Screen1, -PIVOT_X, &fuel_hub_value);
    make_hub(ui_Screen1, PIVOT_X, &cool_hub_value);

    last_fuel = -1; last_cool = -1000; last_oilp = -1000; last_oilt = -1000;
    // Park both needles at the bottom until the first real value arrives
    memset(&fuel_d, 0, sizeof(fuel_d));
    memset(&cool_d, 0, sizeof(cool_d));
    fuel_d.drawn_angle = cool_d.drawn_angle = -1000;
    needle_to(&fuel_g, fuel_angle(0));
    needle_to(&cool_g, cool_angle(0));
    needle_timer = lv_timer_create(needle_timer_cb, UI_NEEDLE_TICK_MS, NULL);
}

void ui_Screen1_screen_destroy(void)
{
    if (needle_timer) lv_timer_del(needle_timer);
    needle_timer = NULL;
    if (ui_Screen1) lv_obj_del(ui_Screen1);
#if LV_USE_SNAPSHOT
    if (dial_snapshot) lv_snapshot_free(dial_snapshot);
#endif
    dial_snapshot = NULL;
    ui_Screen1 = NULL;
    fuel_g.needle = fuel_g.glow = cool_g.needle = cool_g.glow = NULL;
    oilp_value = oilt_value = fuel_hub_value = cool_hub_value = NULL;
    fuel_lamp = NULL;
    fuel_lamp_on = false;
}

// ---------------------------------------------------------------- setters

static void set_text_int(lv_obj_t *label, int v)
{
    char buf[8];
    lv_snprintf(buf, sizeof(buf), "%d", v);
    if (strcmp(lv_label_get_text(label), buf) != 0) lv_label_set_text(label, buf);
}

static void set_color(lv_obj_t *label, uint32_t color)
{
    lv_color_t c = lv_color_hex(color);
    if (lv_obj_get_style_text_color(label, 0).full != c.full) lv_obj_set_style_text_color(label, c, 0);
}

// Blue below cold, red above hot, white in between
static uint32_t band_color(int v, int cold, int hot)
{
    return v < cold ? UI_COLOR_COLD : v > hot ? UI_COLOR_HOT : UI_COLOR_VALUE;
}

void ui_left_set_fuel(int percent)
{
    if (!fuel_g.needle) return;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    if (percent == last_fuel) return;
    last_fuel = percent;
    seed(&fuel_d, (float)percent);            // needle is damped in needle_timer_cb
    set_text_int(fuel_hub_value, percent);    // number stays live
    set_color(fuel_hub_value, percent < UI_FUEL_LOW_PCT ? UI_COLOR_HOT : UI_COLOR_VALUE);
}

void ui_left_set_coolant(int deg_f)
{
    if (!cool_g.needle || deg_f == last_cool) return;
    last_cool = deg_f;
    seed(&cool_d, (float)deg_f);              // needle is damped in needle_timer_cb
    set_text_int(cool_hub_value, deg_f);      // number stays live
    set_color(cool_hub_value, band_color(deg_f, UI_COOLANT_COLD_F, UI_COOLANT_HOT_F));
}

void ui_left_set_oil_pressure(int psi)
{
    if (!oilp_value || psi == last_oilp) return;
    last_oilp = psi;
    set_text_int(oilp_value, psi);
    set_color(oilp_value, psi < UI_OIL_PRESS_LOW_PSI  ? UI_COLOR_HOT :
                          psi > UI_OIL_PRESS_HIGH_PSI ? UI_COLOR_COLD : UI_COLOR_VALUE);
}

void ui_left_set_oil_temp(int deg_f)
{
    if (!oilt_value || deg_f == last_oilt) return;
    last_oilt = deg_f;
    set_text_int(oilt_value, deg_f);
    set_color(oilt_value, band_color(deg_f, UI_OIL_TEMP_COLD_F, UI_OIL_TEMP_HOT_F));
}
