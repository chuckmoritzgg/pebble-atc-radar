// main.c
// LOTSE v2.3 — river bg, runway refactor, BT fix, emoji heart, WX fix+city, bigger hands/fonts


#include <pebble.h>


#define MESSAGE_KEY_TEMPERATURE     0
#define MESSAGE_KEY_CONDITIONS      1
#define MESSAGE_KEY_REQUEST_WEATHER 2
#define MESSAGE_KEY_CITY            3


#define PERSIST_KEY_SWEEP_MODE  10
#define PERSIST_KEY_TARGET_TTL  11
#define PERSIST_KEY_STEPS_GOAL  12


typedef enum {
  SWEEP_ALWAYS = 0,
  SWEEP_BACKLIGHT = 1,
  SWEEP_NEVER = 2
} SweepMode;


#if defined(PBL_ROUND)
#define SCR_W 260
#define SCR_H 260
#define CX 130
#define CY 120
#define NM_PX 58
#define RADAR_RADIUS 118
#else
#define SCR_W 200
#define SCR_H 228
#define CX 100
#define CY 110
#define NM_PX 46
#define RADAR_RADIUS 94
#endif


#define MAX_TARGETS 8
#define DEFAULT_TARGET_TTL 7
#define DEG_ANG(d) ((TRIG_MAX_ANGLE * (d)) / 360)


typedef enum { ALPHA_100, ALPHA_75, ALPHA_50, ALPHA_25, ALPHA_12 } AlphaLevel;

// Forward declaration — tick_handler is defined later but referenced by update_tick_subscription()
static void tick_handler(struct tm *t, TimeUnits changed);

static const char *MONTH_ABBR[12] = {
  "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"
};


static GPoint polar(int cx, int cy, int r, int cwdeg) {
  int32_t a = DEG_ANG(cwdeg);
  return GPoint(cx + r * sin_lookup(a) / TRIG_MAX_RATIO,
                cy - r * cos_lookup(a) / TRIG_MAX_RATIO);
}


static void draw_line_alpha(GContext *ctx, GPoint a, GPoint b, GColor col, AlphaLevel alpha) {
  if (alpha == ALPHA_100) {
    graphics_context_set_stroke_color(ctx, col);
    graphics_context_set_stroke_width(ctx, 1);
    graphics_draw_line(ctx, a, b);
    return;
  }
  int keep, period;
  switch(alpha) {
    case ALPHA_75: keep = 3; period = 4; break;
    case ALPHA_50: keep = 1; period = 2; break;
    case ALPHA_25: keep = 1; period = 4; break;
    case ALPHA_12: keep = 1; period = 8; break;
    default:       keep = 1; period = 1; break;
  }
  int dx = b.x - a.x, dy = b.y - a.y;
  int steps = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
  if (steps == 0) return;
  graphics_context_set_stroke_color(ctx, col);
  graphics_context_set_stroke_width(ctx, 1);
  for (int i = 0; i <= steps; i++) {
    if (i % period < keep) {
      GPoint p = GPoint(a.x + dx * i / steps, a.y + dy * i / steps);
      graphics_draw_pixel(ctx, p);
    }
  }
}


static void draw_dashed_bold_line(GContext *ctx, GPoint a, GPoint b, GColor col, int width, int on_px, int off_px) {
  int dx = b.x - a.x, dy = b.y - a.y;
  int steps = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
  if (steps == 0) return;
  int period = on_px + off_px;
  if (period <= 0) period = 1;
  for (int w = -(width / 2); w <= width / 2; w++) {
    for (int i = 0; i <= steps; i++) {
      if ((i % period) >= on_px) continue;
      int x = a.x + dx * i / steps;
      int y = a.y + dy * i / steps;
      if (abs(dx) > abs(dy)) y += w;
      else x += w;
      graphics_context_set_stroke_color(ctx, col);
      graphics_draw_pixel(ctx, GPoint(x, y));
    }
  }
}


static void fill_rect_alpha(GContext *ctx, GRect r, GColor col, AlphaLevel alpha) {
  if (alpha == ALPHA_100) {
    graphics_context_set_fill_color(ctx, col);
    graphics_fill_rect(ctx, r, 0, GCornerNone);
    return;
  }
  int keep, period;
  switch(alpha) {
    case ALPHA_75: keep = 3; period = 4; break;
    case ALPHA_50: keep = 1; period = 2; break;
    case ALPHA_25: keep = 1; period = 4; break;
    case ALPHA_12: keep = 1; period = 8; break;
    default:       keep = 1; period = 1; break;
  }
  graphics_context_set_stroke_color(ctx, col);
  graphics_context_set_stroke_width(ctx, 1);
  for (int y = r.origin.y; y < r.origin.y + r.size.h; y++) {
    for (int x = r.origin.x; x < r.origin.x + r.size.w; x++) {
      if (((x + y) % period) < keep) graphics_draw_pixel(ctx, GPoint(x, y));
    }
  }
}


typedef struct {
  GPoint pos;
  int    ttl;
  char   call[8];
} GhostTarget;


static Window           *s_window;
static Layer            *s_bg_layer, *s_rwy_layer, *s_sweep_layer, *s_blip_layer, *s_hud_layer;
static struct tm         s_now;
static int               s_steps = 0;
static int               s_steps_goal = 10000;
static int               s_battery = 0;
static bool              s_charging = false;
static bool              s_bt_ok = true;
static int               s_hr = 0;
static int               s_temp_c = -999;
static char              s_conditions[16] = "WX --";
static char              s_city[24]       = "";
static GhostTarget       s_targets[MAX_TARGETS];
static int               s_prev_sweep = -1;
static int               s_target_ttl = DEFAULT_TARGET_TTL;
static SweepMode         s_sweep_mode = SWEEP_BACKLIGHT;
static bool              s_backlight_force_sweep = false;
// Tracks which tick unit is currently subscribed to avoid redundant re-subscriptions
static TimeUnits         s_tick_unit = 0;


static Window           *s_menu_window;
static SimpleMenuLayer  *s_menu_layer;
static SimpleMenuSection s_menu_sections[1];
static SimpleMenuItem    s_menu_items[7];
static char              s_sweep_subtitle[16];
static char              s_ttl_subtitle[16];
static char              s_goal_subtitle[16];


static const struct { int az; float r_frac; const char *call; } SPAWN_TABLE[] = {
  {  18, 0.56f, "DLH1A" }, {  62, 0.72f, "EZY3B" }, { 108, 0.50f, "RYR7C" }, { 156, 0.64f, "AFL2D" },
  { 205, 0.58f, "TUI5E" }, { 246, 0.69f, "WZZ9F" }, { 292, 0.43f, "CLX6H" }, { 334, 0.61f, "HLX4G" }
};
#define N_SPAWNS ((int)(sizeof(SPAWN_TABLE)/sizeof(SPAWN_TABLE[0])))


static GPoint geo(float lat, float lon) {
  float km_per_unit = 2.3f * 1.852f;
  float dy = (lat - 51.2895f) * 111.0f / km_per_unit;
  float dx = (lon - 6.7668f)  *  68.0f / km_per_unit;
  return GPoint((int)(CX + dx * NM_PX), (int)(CY - dy * NM_PX));
}


static bool sweep_visible(void) {
  if (s_sweep_mode == SWEEP_ALWAYS)    return true;
  if (s_sweep_mode == SWEEP_BACKLIGHT) return s_backlight_force_sweep;
  return false;
}


// ── Tick subscription helper ───────────────────────────────────────────────
// Subscribe to SECOND_UNIT when the sweep is active (needs per-second redraws),
// or MINUTE_UNIT otherwise (saves battery).  Re-subscribing is a no-op when
// the required unit has not changed.
static void update_tick_subscription(void) {
  TimeUnits needed = sweep_visible() ? SECOND_UNIT : MINUTE_UNIT;
  if (needed == s_tick_unit) return;
  tick_timer_service_subscribe(needed, tick_handler);
  s_tick_unit = needed;
}


static void save_settings(void) {
  persist_write_int(PERSIST_KEY_SWEEP_MODE, (int)s_sweep_mode);
  persist_write_int(PERSIST_KEY_TARGET_TTL, s_target_ttl);
  persist_write_int(PERSIST_KEY_STEPS_GOAL, s_steps_goal);
}


static void load_settings(void) {
  if (persist_exists(PERSIST_KEY_SWEEP_MODE)) s_sweep_mode = (SweepMode)persist_read_int(PERSIST_KEY_SWEEP_MODE);
  if (persist_exists(PERSIST_KEY_TARGET_TTL)) s_target_ttl = persist_read_int(PERSIST_KEY_TARGET_TTL);
  if (persist_exists(PERSIST_KEY_STEPS_GOAL)) s_steps_goal = persist_read_int(PERSIST_KEY_STEPS_GOAL);
  if (s_target_ttl < 4) s_target_ttl = DEFAULT_TARGET_TTL;
  if (s_steps_goal <= 0) s_steps_goal = 10000;
}


static void draw_battery_icon(GContext *ctx, GRect r, int pct, bool charging) {
  graphics_context_set_stroke_color(ctx, GColorBlack);
  graphics_draw_rect(ctx, r);
  graphics_draw_line(ctx, GPoint(r.origin.x + r.size.w,     r.origin.y + 2),
                          GPoint(r.origin.x + r.size.w,     r.origin.y + r.size.h - 3));
  graphics_draw_line(ctx, GPoint(r.origin.x + r.size.w + 1, r.origin.y + 3),
                          GPoint(r.origin.x + r.size.w + 1, r.origin.y + r.size.h - 4));
  int fill = (pct * (r.size.w - 2)) / 100;
  if (fill < 0) fill = 0;
  if (fill > r.size.w - 2) fill = r.size.w - 2;
  graphics_context_set_fill_color(ctx, pct > 20 ? GColorJaegerGreen : GColorRed);
  graphics_fill_rect(ctx, GRect(r.origin.x + 1, r.origin.y + 1, fill, r.size.h - 2), 0, GCornerNone);
  if (charging) {
    GPoint bolt[] = {
      GPoint(r.origin.x + r.size.w / 2,     r.origin.y + 1),
      GPoint(r.origin.x + r.size.w / 2 - 2, r.origin.y + r.size.h / 2),
      GPoint(r.origin.x + r.size.w / 2 + 1, r.origin.y + r.size.h / 2),
      GPoint(r.origin.x + r.size.w / 2 - 1, r.origin.y + r.size.h - 1)
    };
    graphics_context_set_stroke_color(ctx, GColorBlack);
    graphics_draw_line(ctx, bolt[0], bolt[1]);
    graphics_draw_line(ctx, bolt[1], bolt[2]);
    graphics_draw_line(ctx, bolt[2], bolt[3]);
  }
}


#if !defined(PBL_ROUND)
static void draw_steps_icon(GContext *ctx, int x, int y, GColor col) {
  graphics_context_set_fill_color(ctx, col);
  graphics_fill_circle(ctx, GPoint(x + 3, y + 3), 2);
  graphics_fill_rect(ctx, GRect(x + 2, y + 4, 4, 6), 1, GCornersAll);
  graphics_fill_circle(ctx, GPoint(x + 10, y + 2), 2);
  graphics_fill_rect(ctx, GRect(x + 9, y + 3, 4, 5), 1, GCornersAll);
}
#endif


// ── Bluetooth icon — correct ♦ fork shape ─────────────────────────────────
static void draw_bt_icon(GContext *ctx, int x, int y, GColor col) {
  graphics_context_set_stroke_color(ctx, col);
  graphics_context_set_stroke_width(ctx, 1);
  graphics_draw_line(ctx, GPoint(x + 4, y),      GPoint(x + 4, y + 10));
  graphics_draw_line(ctx, GPoint(x + 4, y),      GPoint(x + 8, y + 3));
  graphics_draw_line(ctx, GPoint(x + 8, y + 3),  GPoint(x + 4, y + 5));
  graphics_draw_line(ctx, GPoint(x + 4, y + 5),  GPoint(x + 8, y + 7));
  graphics_draw_line(ctx, GPoint(x + 8, y + 7),  GPoint(x + 4, y + 10));
  graphics_draw_line(ctx, GPoint(x + 4, y),      GPoint(x + 1, y + 3));
  graphics_draw_line(ctx, GPoint(x + 4, y + 10), GPoint(x + 1, y + 7));
}


// ── bg_layer: river FIRST, underneath everything ───────────────────────────
static void bg_layer_draw(Layer *layer, GContext *ctx) {
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, layer_get_bounds(layer), 0, GCornerNone);


  // River Rhine — drawn first, sits under all labels/runways/radar
  static const float rhine[][2] = {
    {51.172f, 6.708f}, {51.188f, 6.700f}, {51.205f, 6.690f}, {51.224f, 6.676f}, {51.242f, 6.662f},
    {51.259f, 6.649f}, {51.274f, 6.641f}, {51.288f, 6.639f}, {51.301f, 6.642f}, {51.315f, 6.651f},
    {51.329f, 6.667f}, {51.342f, 6.686f}, {51.355f, 6.709f}
  };
  int nr = (int)(sizeof(rhine)/sizeof(rhine[0]));
  graphics_context_set_stroke_color(ctx, GColorTiffanyBlue);
  graphics_context_set_stroke_width(ctx, 3);
  for (int i = 0; i < nr - 1; i++) {
    GPoint p1 = geo(rhine[i][0],   rhine[i][1]);
    GPoint p2 = geo(rhine[i+1][0], rhine[i+1][1]);
    p1.x += 20; p2.x += 20;
    graphics_draw_line(ctx, p1, p2);
  }
  graphics_context_set_stroke_width(ctx, 1);


  // VOR symbol
  GPoint vor = geo(51.2897f, 6.7663f);
  graphics_context_set_stroke_color(ctx, GColorChromeYellow);
  graphics_context_set_stroke_width(ctx, 1);
  for (int t = 0; t < 2; t++) {
    for (int v = 0; v < 3; v++) {
      GPoint p1 = polar(vor.x, vor.y, 4, t * 60 + v * 120);
      GPoint p2 = polar(vor.x, vor.y, 4, t * 60 + (v + 1) * 120);
      graphics_draw_line(ctx, p1, p2);
    }
  }


  // EDDL label — shifted to very top
  graphics_context_set_text_color(ctx, GColorChromeYellow);
  graphics_draw_text(ctx, "EDDL",
                     fonts_get_system_font(FONT_KEY_BITHAM_30_BLACK),
                     GRect(0, -2, SCR_W, 24),
                     GTextOverflowModeFill, GTextAlignmentCenter, NULL);


  // Time + date (date 3 px lower than original)
  char ltime[6], ldate[16];
  snprintf(ltime, sizeof(ltime), "%02d:%02d", s_now.tm_hour, s_now.tm_min);
  int m = s_now.tm_mon;
  if (m < 0) m = 0;
  if (m > 11) m = 11;
  snprintf(ldate, sizeof(ldate), "%d. %s", s_now.tm_mday, MONTH_ABBR[m]);


  graphics_context_set_text_color(ctx, GColorBlack);
#if defined(PBL_ROUND)
  graphics_draw_text(ctx, ltime,
                     fonts_get_system_font(FONT_KEY_LECO_28_LIGHT_NUMBERS),
                     GRect(0, 34, SCR_W, 28),
                     GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
  graphics_context_set_text_color(ctx, GColorDarkGreen);
  graphics_draw_text(ctx, ldate,
                     fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                     GRect(0, 67, SCR_W, 20),
                     GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
#else
  graphics_draw_text(ctx, ltime,
                     fonts_get_system_font(FONT_KEY_LECO_28_LIGHT_NUMBERS),
                     GRect(0, 24, SCR_W, 28),
                     GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
  graphics_context_set_text_color(ctx, GColorDarkGreen);
  graphics_draw_text(ctx, ldate,
                     fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                     GRect(0, 57, SCR_W, 20),
                     GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
#endif
}


// ── Two independent runways, each with its own center line ─────────────────
static void rwy_layer_draw(Layer *layer, GContext *ctx) {
  static const struct { float mlat, mlon; float half_nm; } rwys[2] = {
    { 51.2871f, 6.7701f, 0.75f },
    { 51.2906f, 6.7639f, 0.75f }
  };
  int32_t perp_sin  = sin_lookup(DEG_ANG(141));
  int32_t perp_cos  = cos_lookup(DEG_ANG(141));
  int     rwy_hw    = 5;
  int     ext_solid = (int)(0.50f * NM_PX);
  int     ext_dash  = (int)(1.20f * NM_PX);


  for (int i = 0; i < 2; i++) {
    GPoint mid  = geo(rwys[i].mlat, rwys[i].mlon);
    int    half = (int)(rwys[i].half_nm * NM_PX);
    GPoint t05  = polar(mid.x, mid.y, half, 231);
    GPoint t23  = polar(mid.x, mid.y, half, 51);


    for (int side = -1; side <= 1; side += 2) {
      GPoint a = GPoint(t05.x + side * rwy_hw * perp_sin / TRIG_MAX_RATIO,
                        t05.y - side * rwy_hw * perp_cos / TRIG_MAX_RATIO);
      GPoint b = GPoint(t23.x + side * rwy_hw * perp_sin / TRIG_MAX_RATIO,
                        t23.y - side * rwy_hw * perp_cos / TRIG_MAX_RATIO);
      draw_line_alpha(ctx, a, b, GColorDarkGreen, ALPHA_75);
    }


    GPoint a1 = GPoint(t05.x - rwy_hw * perp_sin / TRIG_MAX_RATIO,
                       t05.y + rwy_hw * perp_cos / TRIG_MAX_RATIO);
    GPoint a2 = GPoint(t05.x + rwy_hw * perp_sin / TRIG_MAX_RATIO,
                       t05.y - rwy_hw * perp_cos / TRIG_MAX_RATIO);
    GPoint b1 = GPoint(t23.x - rwy_hw * perp_sin / TRIG_MAX_RATIO,
                       t23.y + rwy_hw * perp_cos / TRIG_MAX_RATIO);
    GPoint b2 = GPoint(t23.x + rwy_hw * perp_sin / TRIG_MAX_RATIO,
                       t23.y - rwy_hw * perp_cos / TRIG_MAX_RATIO);
    draw_line_alpha(ctx, a1, a2, GColorBlack, ALPHA_100);
    draw_line_alpha(ctx, b1, b2, GColorBlack, ALPHA_100);


    // Center line — belongs exclusively to this runway
    for (int s = -half + 4; s < half - 4; s += 8) {
      draw_line_alpha(ctx,
        polar(mid.x, mid.y, s,     51),
        polar(mid.x, mid.y, s + 5, 51),
        GColorDarkGreen, ALPHA_75);
    }


    for (int dir = 0; dir < 2; dir++) {
      int    hdg     = (dir == 0) ? 231 : 51;
      GPoint thr     = (dir == 0) ? t05 : t23;
      GPoint ext_end = polar(mid.x, mid.y, half + ext_solid, hdg);
      draw_line_alpha(ctx, thr, ext_end, GColorBlack, ALPHA_50);
      for (int s = half + ext_solid + 3; s < half + ext_solid + ext_dash; s += 10) {
        draw_line_alpha(ctx,
          polar(mid.x, mid.y, s,     hdg),
          polar(mid.x, mid.y, s + 6, hdg),
          GColorDarkGreen, ALPHA_50);
      }
    }
  }
}


static void sweep_layer_draw(Layer *layer, GContext *ctx) {
  int sweep_deg = s_now.tm_sec * 6;
  int r_max     = 2 * NM_PX;


  for (int nm = 1; nm <= 2; nm++) {
    int r = nm * NM_PX;
    for (int d = 0; d < 360; d += 2) {
      draw_line_alpha(ctx, polar(CX, CY, r, d),
                           polar(CX, CY, r, d + 1),
                           GColorBlack, ALPHA_50);
    }
  }
  for (int d = 0; d < 360; d += 5) {
    int tick_len = (d % 30 == 0) ? 6 : 3;
    draw_line_alpha(ctx,
      polar(CX, CY, r_max - tick_len, d),
      polar(CX, CY, r_max, d),
      GColorBlack,
      d % 30 == 0 ? ALPHA_100 : ALPHA_50);
  }


  bool sweep_on = sweep_visible();
#if defined(PBL_ROUND)
  graphics_context_set_stroke_color(ctx, GColorBlack);
  graphics_context_set_stroke_width(ctx, 1);
  graphics_draw_circle(ctx, GPoint(CX, CY), r_max);
#endif


  if (sweep_on) {
    static const AlphaLevel trail_alpha[] = { ALPHA_75, ALPHA_50, ALPHA_50, ALPHA_25, ALPHA_12 };
    for (int lvl = 4; lvl >= 0; lvl--) {
      int trail_deg = sweep_deg - (lvl + 1) * 6;
      if (trail_deg < 0) trail_deg += 360;
      for (int d = trail_deg; d < trail_deg + 6; d += 2) {
        draw_line_alpha(ctx, GPoint(CX, CY),
                             polar(CX, CY, r_max, d % 360),
                             GColorJaegerGreen, trail_alpha[lvl]);
      }
    }
    graphics_context_set_stroke_color(ctx, GColorJaegerGreen);
    graphics_context_set_stroke_width(ctx, 1);
    graphics_draw_line(ctx, GPoint(CX, CY), polar(CX, CY, r_max, sweep_deg));
  }


  if (sweep_on) {
    for (int s = 0; s < N_SPAWNS; s++) {
      int  saz     = SPAWN_TABLE[s].az;
      bool crossed = false;
      if (s_prev_sweep >= 0) {
        int prev = s_prev_sweep, curr = sweep_deg;
        crossed = (curr >= prev) ? (saz >= prev && saz < curr)
                                 : (saz >= prev || saz < curr);
      }
      if (crossed) {
        int slot = -1;
        for (int t = 0; t < MAX_TARGETS; t++) {
          if (s_targets[t].ttl == 0) { slot = t; break; }
        }
        if (slot < 0) slot = 0;
        int rp = (int)(SPAWN_TABLE[s].r_frac * r_max);
        s_targets[slot].pos = polar(CX, CY, rp, saz);
        s_targets[slot].ttl = s_target_ttl;
        strncpy(s_targets[slot].call, SPAWN_TABLE[s].call, sizeof(s_targets[slot].call) - 1);
        s_targets[slot].call[sizeof(s_targets[slot].call) - 1] = '\0';
      }
    }
    s_prev_sweep = sweep_deg;
  } else {
    s_prev_sweep = -1;
  }


  GFont f9 = fonts_get_system_font(FONT_KEY_GOTHIC_09);
  for (int t = 0; t < MAX_TARGETS; t++) {
    if (s_targets[t].ttl == 0) continue;
    int        fade_idx = (s_target_ttl - s_targets[t].ttl);
    AlphaLevel al =
      (fade_idx <= 1) ? ALPHA_100 :
      (fade_idx <= 3) ? ALPHA_75  :
      (fade_idx <= 5) ? ALPHA_50  : ALPHA_25;
    GPoint p = s_targets[t].pos;
    fill_rect_alpha(ctx, GRect(p.x - 2, p.y - 2, 5, 5), GColorJaegerGreen, al);
    if (al == ALPHA_100 || al == ALPHA_75) {
      graphics_context_set_text_color(ctx, GColorBlack);
      graphics_draw_text(ctx, s_targets[t].call, f9,
                         GRect(p.x + 5, p.y - 5, 30, 10),
                         GTextOverflowModeFill, GTextAlignmentLeft, NULL);
    }
    s_targets[t].ttl--;
  }
}


// ── blip_layer: 18×18 squares (+7px), velocity lines stroke_width 1 (less bold) ──
static void blip_layer_draw(Layer *layer, GContext *ctx) {
  int hour_12   = s_now.tm_hour % 12;
  int min       = s_now.tm_min;
  int hr_ang    = hour_12 * 30 + min / 2;
  int min_ang   = min * 6;
  int r_hr      = (int)(0.34f * 2 * NM_PX);
  int r_min     = (int)(0.86f * 2 * NM_PX);
  int v_len_hr  = (int)(0.18f * 2 * NM_PX);
  int v_len_min = (int)(0.24f * 2 * NM_PX);


  GPoint hr_pt   = polar(CX, CY, r_hr,  hr_ang);
  GPoint min_pt  = polar(CX, CY, r_min, min_ang);
  GPoint hr_vec  = polar(hr_pt.x,  hr_pt.y,  v_len_hr,  hr_ang);
  GPoint min_vec = polar(min_pt.x, min_pt.y, v_len_min, min_ang);


  draw_dashed_bold_line(ctx, GPoint(CX, CY), hr_pt,  GColorDarkCandyAppleRed, 2, 6, 3);
  draw_dashed_bold_line(ctx, GPoint(CX, CY), min_pt, GColorOxfordBlue,        2, 6, 3);


  graphics_context_set_stroke_color(ctx, GColorDarkCandyAppleRed);
  graphics_context_set_stroke_width(ctx, 2);
  graphics_draw_line(ctx, hr_pt, hr_vec);


  graphics_context_set_stroke_color(ctx, GColorOxfordBlue);
  graphics_context_set_stroke_width(ctx, 2);
  graphics_draw_line(ctx, min_pt, min_vec);


  graphics_context_set_fill_color(ctx, GColorDarkCandyAppleRed);
  graphics_fill_rect(ctx, GRect(hr_pt.x - 5, hr_pt.y - 5, 11, 11), 0, GCornerNone);
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, GRect(hr_pt.x - 1, hr_pt.y - 1, 3, 3), 0, GCornerNone);


  graphics_context_set_fill_color(ctx, GColorOxfordBlue);
  graphics_fill_rect(ctx, GRect(min_pt.x - 5, min_pt.y - 5, 11, 11), 0, GCornerNone);
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, GRect(min_pt.x - 1, min_pt.y - 1, 3, 3), 0, GCornerNone);


  GFont f9 = fonts_get_system_font(FONT_KEY_GOTHIC_09);
  char hcall[8], mcall[8];
  snprintf(hcall, sizeof(hcall), "DLH%02d", s_now.tm_hour % 100);
  snprintf(mcall, sizeof(mcall), "KLM%02d", s_now.tm_min  % 100);
  GPoint htxt = polar(CX, CY, r_hr  + 24, hr_ang);
  GPoint mtxt = polar(CX, CY, r_min - 14, min_ang);
  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, hcall, f9,
                     GRect(htxt.x - 18, htxt.y - 5, 36, 10),
                     GTextOverflowModeFill, GTextAlignmentCenter, NULL);
  graphics_draw_text(ctx, mcall, f9,
                     GRect(mtxt.x - 18, mtxt.y - 5, 36, 10),
                     GTextOverflowModeFill, GTextAlignmentCenter, NULL);


  graphics_context_set_fill_color(ctx, GColorJaegerGreen);
  graphics_fill_circle(ctx, GPoint(CX, CY), 4);
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_circle(ctx, GPoint(CX, CY), 2);
}


static void draw_corner_status(GContext *ctx) {
  GFont f9 = fonts_get_system_font(FONT_KEY_GOTHIC_09);
  draw_battery_icon(ctx, GRect(6, 6, 16, 8), s_battery, s_charging);
  draw_bt_icon(ctx, 6, 18, s_bt_ok ? GColorDarkGreen : GColorRed);
#if defined(PBL_ROUND)
  if (!s_bt_ok) {
    graphics_context_set_text_color(ctx, GColorRed);
    graphics_draw_text(ctx, "BT", f9, GRect(6, 30, 24, 12),
                       GTextOverflowModeFill, GTextAlignmentLeft, NULL);
  }
#endif
}


// ── hud_layer: WX above UTC strip, emoji ❤, bigger step/HR font ───────────
static void hud_layer_draw(Layer *layer, GContext *ctx) {
  GFont f9  = fonts_get_system_font(FONT_KEY_GOTHIC_09);
  GFont f18 = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
  draw_corner_status(ctx);


  time_t now_utc = time(NULL);
  struct tm *utc_tm = gmtime(&now_utc);
  char utime[6];
  if (utc_tm) snprintf(utime, sizeof(utime), "%02d:%02d", utc_tm->tm_hour, utc_tm->tm_min);
  else        snprintf(utime, sizeof(utime), "--:--");


  char stepbuf[12];
  if (s_steps >= 1000)
    snprintf(stepbuf, sizeof(stepbuf), "%d.%dk", s_steps / 1000, (s_steps % 1000) / 100);
  else
    snprintf(stepbuf, sizeof(stepbuf), "%d", s_steps);


  char hrbuf[8];
  if (s_hr > 0) snprintf(hrbuf, sizeof(hrbuf), "%d", s_hr);
  else          snprintf(hrbuf, sizeof(hrbuf), "--");


  char wxbuf[24];
  if (s_temp_c > -999)
    snprintf(wxbuf, sizeof(wxbuf), "%dC %s", s_temp_c, s_conditions);
  else
    snprintf(wxbuf, sizeof(wxbuf), "WX --");


#if defined(PBL_ROUND)
  // STEPS (top-right)
  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, "STEPS", f9,
                     GRect(SCR_W - 78, 6, 72, 12),
                     GTextOverflowModeFill, GTextAlignmentRight, NULL);
  graphics_draw_text(ctx, stepbuf, f18,
                     GRect(SCR_W - 78, 18, 72, 22),
                     GTextOverflowModeFill, GTextAlignmentRight, NULL);


  // ❤ emoji + HR value
  graphics_context_set_text_color(ctx, GColorDarkCandyAppleRed);
  graphics_draw_text(ctx, "\u2764", f18,
                     GRect(SCR_W - 24, 44, 22, 22),
                     GTextOverflowModeFill, GTextAlignmentLeft, NULL);
  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, hrbuf, f18,
                     GRect(SCR_W - 78, 44, 50, 22),
                     GTextOverflowModeFill, GTextAlignmentRight, NULL);


  // WX — fully above the UTC strip (UTC strip top = SCR_H-26 = 234)
  // WX value (22px) ends at SCR_H-26-2=232, label above that
  graphics_context_set_text_color(ctx, GColorDarkGreen);
  graphics_draw_text(ctx,
                     s_city[0] != '\0' ? s_city : "WX",
                     f9,
                     GRect(SCR_W - 92, SCR_H - 60, 90, 12),
                     GTextOverflowModeFill, GTextAlignmentRight, NULL);
  graphics_draw_text(ctx, wxbuf, f18,
                     GRect(SCR_W - 92, SCR_H - 48, 90, 22),
                     GTextOverflowModeFill, GTextAlignmentRight, NULL);


  // UTC strip — drawn last so it cleanly overlays the radar edge
  int y_utc = SCR_H - 26;
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, GRect(CX - 60, y_utc, 120, 26), 0, GCornerNone);
  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, utime,
                     fonts_get_system_font(FONT_KEY_LECO_20_BOLD_NUMBERS),
                     GRect(CX - 58, y_utc + 1, 70, 18),
                     GTextOverflowModeFill, GTextAlignmentLeft, NULL);
  graphics_context_set_text_color(ctx, GColorDarkGreen);
  graphics_draw_text(ctx, "UTC", f9,
                     GRect(CX + 12, y_utc + 3, 24, 10),
                     GTextOverflowModeFill, GTextAlignmentLeft, NULL);


#else
  // STEPS (top-right)
  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, "STEPS", f9,
                     GRect(SCR_W - 78, 6, 72, 12),
                     GTextOverflowModeFill, GTextAlignmentRight, NULL);
  graphics_draw_text(ctx, stepbuf, f18,
                     GRect(SCR_W - 78, 18, 72, 22),
                     GTextOverflowModeFill, GTextAlignmentRight, NULL);


  // ❤ emoji + HR value
  graphics_context_set_text_color(ctx, GColorDarkCandyAppleRed);
  graphics_draw_text(ctx, "\u2764", f18,
                     GRect(SCR_W - 22, 42, 20, 22),
                     GTextOverflowModeFill, GTextAlignmentLeft, NULL);
  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, hrbuf, f18,
                     GRect(SCR_W - 78, 42, 52, 22),
                     GTextOverflowModeFill, GTextAlignmentRight, NULL);


  // UTC strip — drawn last
  int py = SCR_H - 24;
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, GRect(0, py, SCR_W, 24), 0, GCornerNone);
  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, utime,
                     fonts_get_system_font(FONT_KEY_LECO_20_BOLD_NUMBERS),
                     GRect(4, py + 2, 70, 18),
                     GTextOverflowModeFill, GTextAlignmentLeft, NULL);
  graphics_context_set_text_color(ctx, GColorDarkGreen);
  graphics_draw_text(ctx, "UTC", f9,
                     GRect(78, py + 3, 40, 10),
                     GTextOverflowModeFill, GTextAlignmentLeft, NULL);


  // WX — fully above the UTC strip (UTC strip top = SCR_H-24 = 204)
  // WX value (22px) must end before y=204: place at y=180, label at y=168
  graphics_context_set_text_color(ctx, GColorDarkGreen);
  graphics_draw_text(ctx,
                     s_city[0] != '\0' ? s_city : "WX",
                     f9,
                     GRect(SCR_W - 92, SCR_H - 32, 90, 12),
                     GTextOverflowModeFill, GTextAlignmentRight, NULL);
  graphics_draw_text(ctx, wxbuf, f18,
                     GRect(SCR_W - 92, SCR_H - 20, 90, 22),
                     GTextOverflowModeFill, GTextAlignmentRight, NULL);
#endif
}


static void request_weather(void) {
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) == APP_MSG_OK) {
    dict_write_uint8(iter, MESSAGE_KEY_REQUEST_WEATHER, 1);
    app_message_outbox_send();
  }
}


static void tick_handler(struct tm *t, TimeUnits changed) {
  s_now = *t;
  layer_mark_dirty(s_sweep_layer);
  layer_mark_dirty(s_blip_layer);
  if (changed & MINUTE_UNIT) {
    layer_mark_dirty(s_bg_layer);
    layer_mark_dirty(s_rwy_layer);
    layer_mark_dirty(s_hud_layer);
    if (t->tm_min % 30 == 0) request_weather();
  }
}


static void battery_handler(BatteryChargeState state) {
  s_battery  = state.charge_percent;
  s_charging = state.is_charging;
  layer_mark_dirty(s_hud_layer);
}


static void bt_handler(bool connected) {
  s_bt_ok = connected;
  if (!connected) vibes_short_pulse();
  layer_mark_dirty(s_hud_layer);
}


#if defined(PBL_HEALTH)
static void health_handler(HealthEventType event, void *ctx) {
  time_t start = time_start_of_today();
  time_t end   = time(NULL);
  if (event == HealthEventMovementUpdate || event == HealthEventSignificantUpdate) {
    if (health_service_metric_accessible(HealthMetricStepCount, start, end) & HealthServiceAccessibilityMaskAvailable) {
      s_steps = (int)health_service_sum_today(HealthMetricStepCount);
      layer_mark_dirty(s_hud_layer);
    }
  }
  if (event == HealthEventHeartRateUpdate) {
    if (health_service_metric_accessible(HealthMetricHeartRateBPM, end, end) & HealthServiceAccessibilityMaskAvailable) {
      HealthValue v = health_service_peek_current_value(HealthMetricHeartRateBPM);
      if (v > 0) s_hr = (int)v;
      layer_mark_dirty(s_hud_layer);
    }
  }
}
#endif


static void inbox_handler(DictionaryIterator *iter, void *ctx) {
  Tuple *t;
  if ((t = dict_find(iter, MESSAGE_KEY_TEMPERATURE))) s_temp_c = (int)t->value->int32;
  if ((t = dict_find(iter, MESSAGE_KEY_CONDITIONS))) {
    strncpy(s_conditions, t->value->cstring, sizeof(s_conditions) - 1);
    s_conditions[sizeof(s_conditions) - 1] = '\0';
  }
  if ((t = dict_find(iter, MESSAGE_KEY_CITY))) {
    strncpy(s_city, t->value->cstring, sizeof(s_city) - 1);
    s_city[sizeof(s_city) - 1] = '\0';
  }
  layer_mark_dirty(s_hud_layer);
}


static void menu_reload(void) {
  snprintf(s_sweep_subtitle, sizeof(s_sweep_subtitle), "%s",
           s_sweep_mode == SWEEP_ALWAYS ? "Always" :
           (s_sweep_mode == SWEEP_BACKLIGHT ? "Backlight" : "Never"));
  snprintf(s_ttl_subtitle,  sizeof(s_ttl_subtitle),  "%d sec", s_target_ttl);
  snprintf(s_goal_subtitle, sizeof(s_goal_subtitle), "%d",     s_steps_goal);
  s_menu_items[0] = (SimpleMenuItem){ .title = "Sweep mode",                .subtitle = s_sweep_subtitle };
  s_menu_items[1] = (SimpleMenuItem){ .title = "Sweep always",              .callback = NULL };
  s_menu_items[2] = (SimpleMenuItem){ .title = "Sweep on backlight",        .callback = NULL };
  s_menu_items[3] = (SimpleMenuItem){ .title = "Sweep never",               .callback = NULL };
  s_menu_items[4] = (SimpleMenuItem){ .title = "Target fade",               .subtitle = s_ttl_subtitle };
  s_menu_items[5] = (SimpleMenuItem){ .title = "Steps goal",                .subtitle = s_goal_subtitle };
  s_menu_items[6] = (SimpleMenuItem){ .title = "Select with up/down/select", .subtitle = "Hold back for exit" };
}


static void menu_select_callback(int index, void *ctx) {
  switch(index) {
    case 1: s_sweep_mode = SWEEP_ALWAYS;    break;
    case 2: s_sweep_mode = SWEEP_BACKLIGHT; break;
    case 3: s_sweep_mode = SWEEP_NEVER;     break;
    case 4:
      s_target_ttl = (s_target_ttl == 4) ? 7 : ((s_target_ttl == 7) ? 10 : 4);
      break;
    case 5:
      s_steps_goal = (s_steps_goal == 8000) ? 10000 : ((s_steps_goal == 10000) ? 15000 : 8000);
      break;
    default: return;
  }
  save_settings();
  menu_reload();
  if (s_menu_window && s_menu_layer)
    layer_mark_dirty(simple_menu_layer_get_layer(s_menu_layer));
  // Sweep mode may have changed — update tick subscription accordingly
  update_tick_subscription();
  layer_mark_dirty(s_sweep_layer);
  layer_mark_dirty(s_hud_layer);
}


static void menu_window_load(Window *window) {
  menu_reload();
  for (int i = 1; i <= 5; i++) s_menu_items[i].callback = menu_select_callback;
  Layer *window_layer = window_get_root_layer(window);
  GRect  bounds       = layer_get_bounds(window_layer);
  s_menu_sections[0]  = (SimpleMenuSection){ .num_items = 7, .items = s_menu_items };
  s_menu_layer        = simple_menu_layer_create(bounds, window, s_menu_sections, 1, NULL);
  layer_add_child(window_layer, simple_menu_layer_get_layer(s_menu_layer));
}


static void menu_window_unload(Window *window) {
  if (s_menu_layer) simple_menu_layer_destroy(s_menu_layer);
  s_menu_layer = NULL;
}


static void click_select_handler(ClickRecognizerRef recognizer, void *context) {
  if (!s_menu_window) {
    s_menu_window = window_create();
    window_set_window_handlers(s_menu_window, (WindowHandlers){
      .load   = menu_window_load,
      .unload = menu_window_unload,
    });
  }
  window_stack_push(s_menu_window, true);
}


static void click_up_handler(ClickRecognizerRef recognizer, void *context) {
  s_backlight_force_sweep = true;
  light_enable_interaction();
  // Backlight just turned on — may need to upgrade to SECOND_UNIT
  update_tick_subscription();
  layer_mark_dirty(s_sweep_layer);
}


static void click_down_handler(ClickRecognizerRef recognizer, void *context) {
  request_weather();
}


static void click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, click_select_handler);
  window_single_click_subscribe(BUTTON_ID_UP,     click_up_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN,   click_down_handler);
}


static void window_load(Window *win) {
  Layer *root = window_get_root_layer(win);
  GRect  b    = layer_get_bounds(root);
  window_set_background_color(win, GColorWhite);


  s_bg_layer    = layer_create(b);
  s_rwy_layer   = layer_create(b);
  s_sweep_layer = layer_create(b);
  s_blip_layer  = layer_create(b);
  s_hud_layer   = layer_create(b);


  layer_set_update_proc(s_bg_layer,    bg_layer_draw);
  layer_set_update_proc(s_rwy_layer,   rwy_layer_draw);
  layer_set_update_proc(s_sweep_layer, sweep_layer_draw);
  layer_set_update_proc(s_blip_layer,  blip_layer_draw);
  layer_set_update_proc(s_hud_layer,   hud_layer_draw);


  layer_add_child(root, s_bg_layer);
  layer_add_child(root, s_rwy_layer);
  layer_add_child(root, s_sweep_layer);
  layer_add_child(root, s_blip_layer);
  layer_add_child(root, s_hud_layer);


  time_t      now = time(NULL);
  struct tm  *t   = localtime(&now);
  if (t) s_now = *t;


  BatteryChargeState bat = battery_state_service_peek();
  s_battery  = bat.charge_percent;
  s_charging = bat.is_charging;
  s_bt_ok    = connection_service_peek_pebble_app_connection();
}


static void window_unload(Window *win) {
  layer_destroy(s_hud_layer);
  layer_destroy(s_blip_layer);
  layer_destroy(s_sweep_layer);
  layer_destroy(s_rwy_layer);
  layer_destroy(s_bg_layer);
}


static void init(void) {
  load_settings();
  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers){
    .load   = window_load,
    .unload = window_unload,
  });
  window_set_click_config_provider(s_window, click_config_provider);
  window_stack_push(s_window, true);


  // Subscribe at MINUTE_UNIT by default; update_tick_subscription() will
  // upgrade to SECOND_UNIT automatically if sweep starts immediately.
  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
  s_tick_unit = MINUTE_UNIT;
  update_tick_subscription();

  battery_state_service_subscribe(battery_handler);
  connection_service_subscribe((ConnectionHandlers){
    .pebble_app_connection_handler = bt_handler
  });
#if defined(PBL_HEALTH)
  health_service_events_subscribe(health_handler, NULL);
#endif
  app_message_register_inbox_received(inbox_handler);
  app_message_open(256, 128);
  request_weather();
}


static void deinit(void) {
  if (s_menu_window) window_destroy(s_menu_window);
  window_destroy(s_window);
}


int main(void) {
  init();
  app_event_loop();
  deinit();
}
