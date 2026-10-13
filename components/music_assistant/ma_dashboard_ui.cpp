#include "ma_dashboard_ui.h"
#include "ma_theme.h"
#include "ma_widgets.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <esp_heap_caps.h>

#include <algorithm>
#include <cmath>


// private LVGL header (misc/cache/instance/lv_image_cache.h), not in lvgl.h
extern "C" void lv_image_cache_drop(const void *src);

namespace esphome {
namespace music_assistant {
namespace ma_ui {

static const char *const TAG = "ma_ui";

using namespace theme;

static constexpr uint32_t IDLE_CLOSE_MS = 120000;    // Frank's tuning (23.09.): 2 min without touch
static constexpr uint32_t VOL_SEND_THROTTLE_MS = 250;

// glyphs (present in the ma_icon_* fonts)
static const char *GLYPH_REPEAT_OFF = "\U000F0457";
static const char *GLYPH_REPEAT_ALL = "\U000F0456";
static const char *GLYPH_REPEAT_ONE = "\U000F0458";

static MusicAssistant *s_client = nullptr;
static Handles H;
static std::function<void()> s_hold_screensaver, s_return_to_screensaver, s_clear_wake_guard;
static bool s_bound = false;

// change-detected label cache: set text only when it really changes
static void set_lbl(lv_obj_t *lbl, const char *text) {
  if (lbl == nullptr) return;
  const char *cur = lv_label_get_text(lbl);
  if (cur == nullptr || strcmp(cur, text) != 0) lv_label_set_text(lbl, text);
}

// Title stays on one line and is never clipped: the YAML font (52 px) when it
// fits, the smaller fallback otherwise (re-centred in the line box), "…" last.
static const lv_font_t *s_title_font_large = nullptr;
static int32_t s_title_y = 0, s_title_h = 0;

static void set_title(const char *text) {
  if (H.title == nullptr) return;
  set_lbl(H.title, text);
  const lv_font_t *font = s_title_font_large;
  if (font == nullptr) return;
  if (H.title_font_small != nullptr) {
    lv_point_t size;
    lv_text_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    if (size.x > lv_obj_get_style_width(H.title, LV_PART_MAIN)) font = H.title_font_small;
  }
  if (lv_obj_get_style_text_font(H.title, LV_PART_MAIN) == font) return;
  lv_obj_set_style_text_font(H.title, font, LV_PART_MAIN);
  lv_obj_set_y(H.title, s_title_y + (s_title_h - lv_font_get_line_height(font)) / 2);
}

static std::string fmt_mmss(uint32_t s) {
  char b[10];
  snprintf(b, sizeof(b), "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
  return std::string(b);
}

// Style setters invalidate even when the value does not change — compare
// first (these run on every data revision).
static void set_color(lv_obj_t *obj, uint32_t color) {
  if ((lv_color_to_u32(lv_obj_get_style_text_color(obj, LV_PART_MAIN)) & 0xFFFFFF) == color) return;
  lv_obj_set_style_text_color(obj, lv_color_hex(color), LV_PART_MAIN);
}

static void set_bg_color(lv_obj_t *obj, uint32_t color) {
  if ((lv_color_to_u32(lv_obj_get_style_bg_color(obj, LV_PART_MAIN)) & 0xFFFFFF) == color) return;
  lv_obj_set_style_bg_color(obj, lv_color_hex(color), LV_PART_MAIN);
}

static void set_bg_opa(lv_obj_t *obj, lv_opa_t opa) {
  if (lv_obj_get_style_bg_opa(obj, LV_PART_MAIN) == opa) return;
  lv_obj_set_style_bg_opa(obj, opa, LV_PART_MAIN);
}

// Re-point an image at a downloader's descriptor. The pointer stays the same
// across downloads, so clear first to make LVGL re-read and redraw it once.
static void bind_image(lv_obj_t *img, artwork_image::ArtworkImage *dl) {
  lv_image_set_src(img, nullptr);
  lv_image_set_src(img, dl->get_lv_image_dsc());
}

// ── event handlers ──────────────────────────────────────────────────────────

static void guard_and(std::function<void()> action) {
  if (s_clear_wake_guard) s_clear_wake_guard();
  action();
}

static void on_transport(lv_event_t *e) {
  auto which = (int)(intptr_t) lv_event_get_user_data(e);
  switch (which) {
  case 0: guard_and([]() { s_client->cmd_play_pause(); }); break;
  case 1: guard_and([]() { s_client->cmd_previous(); }); break;
  case 2: guard_and([]() { s_client->cmd_next(); }); break;
  case 3:  // shuffle toggle
    guard_and([]() { s_client->cmd_shuffle(!s_client->now_playing().shuffle); });
    break;
  case 4: {  // repeat cycle off -> all -> one -> off
    guard_and([]() {
      const std::string &r = s_client->now_playing().repeat;
      s_client->cmd_repeat(r == "off" ? "all" : (r == "all" ? "one" : "off"));
    });
    break;
  }
  case 6:  // favourite toggle
    guard_and([]() { s_client->cmd_toggle_favorite(); });
    break;
  case 5:  // mute toggle
    guard_and([]() {
      const auto *p = s_client->player(s_client->selected_player());
      if (p != nullptr) s_client->cmd_mute(!p->muted);
    });
    break;
  default: break;
  }
}

static void on_tile(lv_event_t *e) {
  auto idx = (int)(intptr_t) lv_event_get_user_data(e);
  guard_and([idx]() {
    // tile i shows queue row i + 1 (row 0 is the playing track), see update_tiles_
    const auto &q = s_client->queue();
    const int row = idx + 1;
    if (row < (int) q.size()) s_client->cmd_play_item(q[row].queue_item_id);
  });
}

static void on_close(lv_event_t *) { close(); }

// Idle timer fed straight from the touch input device: every press and every
// drag step counts, independent of LVGL's display-wide inactivity bookkeeping
// (which did not reliably reset while the dashboard was in use).
static uint32_t s_last_touch_ms = 0;

static void on_any_touch(lv_event_t *) { s_last_touch_ms = millis(); }

static void watch_touch_input() {
  for (lv_indev_t *indev = lv_indev_get_next(nullptr); indev != nullptr; indev = lv_indev_get_next(indev)) {
    if (lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) continue;
    lv_indev_add_event_cb(indev, on_any_touch, LV_EVENT_PRESSED, nullptr);
    lv_indev_add_event_cb(indev, on_any_touch, LV_EVENT_PRESSING, nullptr);
  }
}

// ── screen switching (HIDDEN flags, one container per screen) ──────────────

static int s_screen = 1;

static void show_screen(int screen, int tab = 0) {
  s_screen = screen;
  if (H.screen1 != nullptr) (screen == 1 ? lv_obj_clear_flag : lv_obj_add_flag)(H.screen1, LV_OBJ_FLAG_HIDDEN);
  if (H.screen2 != nullptr) (screen == 2 ? lv_obj_clear_flag : lv_obj_add_flag)(H.screen2, LV_OBJ_FLAG_HIDDEN);
  if (H.screen3 != nullptr) (screen == 3 ? lv_obj_clear_flag : lv_obj_add_flag)(H.screen3, LV_OBJ_FLAG_HIDDEN);
  const char *clock = H.clock_src != nullptr ? lv_label_get_text(H.clock_src) : nullptr;
  if (screen == 2) ma_rooms::tick(clock);
  if (screen == 3) {
    ma_queue::shown(tab);
    ma_queue::tick(clock);
  }
}

static void on_open_rooms(lv_event_t *) {
  guard_and([]() { show_screen(2); });
}

static void on_open_queue(lv_event_t *) {
  guard_and([]() { show_screen(3); });
}

static void on_open_library(lv_event_t *) {
  guard_and([]() { show_screen(3, 1); });
}

static bool s_progress_dragging = false;
static void on_progress_pressed(lv_event_t *) { s_progress_dragging = true; }
static void on_progress_released(lv_event_t *) {
  s_progress_dragging = false;
  const uint32_t dur = s_client->now_playing().duration_s;
  if (dur == 0 || H.progress == nullptr) return;
  const int32_t v = lv_slider_get_value(H.progress);
  guard_and([v, dur]() { s_client->cmd_seek((uint32_t)((uint64_t) v * dur / 1000)); });
}

static bool s_vol_dragging = false;
static uint32_t s_last_vol_send_ms = 0;
static void on_vol_changed(lv_event_t *) {
  s_vol_dragging = true;
  if (H.vol == nullptr) return;
  const int32_t pct = lv_slider_get_value(H.vol);
  char b[8];
  snprintf(b, sizeof(b), "%d%%", (int) pct);
  set_lbl(H.vol_pct, b);
  if (millis() - s_last_vol_send_ms >= VOL_SEND_THROTTLE_MS) {
    s_last_vol_send_ms = millis();
    s_client->cmd_set_volume((int) pct);
  }
}
static void on_vol_released(lv_event_t *) {
  if (H.vol == nullptr) { s_vol_dragging = false; return; }
  const int32_t pct = lv_slider_get_value(H.vol);
  guard_and([pct]() { s_client->cmd_set_volume((int) pct); });
  s_vol_dragging = false;
}

// ── styling (once, in bind) ─────────────────────────────────────────────────

static void style_progress(lv_obj_t *s) {
  lv_obj_set_style_bg_opa(s, 36 /* 14% */, LV_PART_MAIN);
  lv_obj_set_style_bg_color(s, lv_color_hex(COL_TEXT), LV_PART_MAIN);
  lv_obj_set_style_bg_color(s, lv_color_hex(COL_TEXT), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(s, lv_color_hex(COL_TEXT), LV_PART_KNOB);
  lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_KNOB);
  lv_obj_set_style_pad_all(s, 8, LV_PART_KNOB);      // 8px track + pad => ~24 knob
  lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, LV_PART_KNOB);
  lv_obj_set_style_pad_ver(s, 8, LV_PART_MAIN);
}

static void style_volume(lv_obj_t *s) {
  lv_obj_set_style_bg_opa(s, 36 /* 14% */, LV_PART_MAIN);
  lv_obj_set_style_bg_color(s, lv_color_hex(COL_TEXT), LV_PART_MAIN);
  lv_obj_set_style_bg_color(s, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(s, lv_color_hex(COL_TEXT), LV_PART_KNOB);
  lv_obj_set_style_pad_all(s, 6, LV_PART_KNOB);
  lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, LV_PART_KNOB);
  lv_obj_set_style_pad_ver(s, 4, LV_PART_MAIN);
}

// ── ambient background (soft glow in the cover's colors) ────────────────────
// LVGL has no blur, so the decoded cover only serves as a color source:
// two vivid colors (upper-left / lower-right half) become separable Gaussian
// glows painted once per track into a full-screen RGB565 buffer in PSRAM.
// Floyd-Steinberg dithering keeps RGB565 from banding. Painting runs in
// ~6 ms slices from an LVGL timer into a back buffer (PSRAM contention with
// the display made a one-shot paint block the loop for seconds), and the
// finished frame is swapped in whole.

// Glow size follows the dashboard container (1280x800 panel, 720x720 P4-86).
static int GLOW_W = 0, GLOW_H = 0;
static constexpr uint32_t GLOW_SLICE_US = 6000;
static uint16_t *s_glow_buf[2] = {nullptr, nullptr};
static int s_glow_front = 0;
static lv_image_dsc_t s_glow_dsc{};

struct Rgb {
  float r, g, b;
};

struct Glow {
  Rgb color;
  float cx, cy, sigma_x, sigma_y, strength;
};

static Rgb rgb565_to_rgb(uint16_t p) {
  return {float((p >> 11) & 0x1F) * (255.0f / 31), float((p >> 5) & 0x3F) * (255.0f / 63),
          float(p & 0x1F) * (255.0f / 31)};
}

static Rgb from_hex(uint32_t hex) {
  return {float((hex >> 16) & 0xFF), float((hex >> 8) & 0xFF), float(hex & 0xFF)};
}

static Rgb mix(Rgb a, Rgb b, float t) {
  return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

// Saturation x brightness weighted mean of one diagonal half: vivid pixels
// define the glow, grey and black areas barely count.
// Samples a ~64x64 grid, so the already decoded 480 px cover serves as the
// color source (no second download/decode just for the glow).
static Rgb vivid_mean(const lv_image_dsc_t *d, bool upper_left) {
  const int w = d->header.w, h = d->header.h;
  const int stride = d->header.stride != 0 ? d->header.stride / 2 : w;
  const int step = std::max(1, std::min(w, h) / 64);
  const auto *px = reinterpret_cast<const uint16_t *>(d->data);
  float r = 0, g = 0, b = 0, total = 0;
  for (int y = 0; y < h; y += step) {
    for (int x = 0; x < w; x += step) {
      if ((x * h + y * w < w * h) != upper_left) continue;
      const Rgb c = rgb565_to_rgb(px[y * stride + x]);
      const float hi = std::max({c.r, c.g, c.b}), lo = std::min({c.r, c.g, c.b});
      const float weight = (hi > 0 ? (hi - lo) / 255.0f : 0) + 0.02f;
      r += c.r * weight, g += c.g * weight, b += c.b * weight, total += weight;
    }
  }
  return {r / total, g / total, b / total};
}

// Peak brightness follows saturation: colorful covers glow at full strength,
// grey ones stay a dim tint instead of a bright grey haze — and nothing ever
// washes out the white text on top.
static Rgb glow_color(Rgb c, float peak) {
  const float hi = std::max({c.r, c.g, c.b, 1.0f}), lo = std::min({c.r, c.g, c.b});
  const float saturation = (hi - lo) / hi;
  const float avg = (c.r + c.g + c.b) / 3;
  c = {std::max(0.0f, avg + (c.r - avg) * 1.2f), std::max(0.0f, avg + (c.g - avg) * 1.2f),
       std::max(0.0f, avg + (c.b - avg) * 1.2f)};
  peak *= 0.4f + 0.6f * std::min(1.0f, saturation * 2.0f);
  const float scale = peak / std::max({c.r, c.g, c.b, 1.0f});
  return {c.r * scale, c.g * scale, c.b * scale};
}

static void fill_gaussian(float *axis, int n, float center, float sigma, float strength) {
  for (int i = 0; i < n; i++) {
    const float d = (i - center) / sigma;
    axis[i] = strength * std::exp(-0.5f * d * d);
  }
}

// Floyd-Steinberg error diffusion: unlike an ordered (Bayer) matrix it leaves
// no periodic grid that shows up as stripes under the translucent pills.
// Integer fixed point (channel values in 1/16 steps of 8-bit).
static constexpr int FP = 16, FP_MAX = 255 * FP;
static int ERR_LEN = 0;  // (GLOW_W + 2) * 3, set with the buffers

struct ErrorRows {
  int16_t *cur, *next;
};

static int quantize(int value, int levels, int *error) {
  const int v = std::clamp(value, 0, FP_MAX);
  const int q = (v * levels + FP_MAX / 2) / FP_MAX;
  *error = v - (q * FP_MAX + levels / 2) / levels;
  return q;
}

static void diffuse(ErrorRows &err, int i, int e) {
  const int right = e * 7 / 16, below_left = e * 3 / 16, below = e * 5 / 16;
  err.cur[i + 3] += right;
  err.next[i - 3] += below_left;
  err.next[i] += below;
  err.next[i + 3] += e - right - below_left - below;
}

static uint16_t diffused_rgb565(ErrorRows &err, Rgb c, int x) {
  const int i = (x + 1) * 3;  // one padding pixel each side
  int er, eg, eb;
  const int r = quantize(int(c.r * FP) + err.cur[i], 31, &er);
  const int g = quantize(int(c.g * FP) + err.cur[i + 1], 63, &eg);
  const int b = quantize(int(c.b * FP) + err.cur[i + 2], 31, &eb);
  diffuse(err, i, er);
  diffuse(err, i + 1, eg);
  diffuse(err, i + 2, eb);
  return uint16_t((r << 11) | (g << 5) | b);
}

// One slice-painted frame in progress. Axes and error rows are allocated
// once in PSRAM together with the two frame buffers.
struct GlowJob {
  bool active = false;
  int row = 0;
  std::string url;
  Rgb base{}, upper{}, lower{};
  float *ax = nullptr, *ay = nullptr, *bx = nullptr, *by = nullptr;
  ErrorRows err{nullptr, nullptr};
  uint32_t started_ms = 0, busy_us = 0;
};
static GlowJob s_job;
static lv_timer_t *s_glow_timer = nullptr;
static std::string s_glow_rendered_url;
static void on_glow_frame_ready();

template<typename T> static T *psram_alloc(size_t count) {
  return static_cast<T *>(heap_caps_calloc(count, sizeof(T), MALLOC_CAP_SPIRAM));
}

static bool ensure_glow_buffers() {
  if (s_glow_buf[0] != nullptr) return true;
  if (H.dashboard == nullptr) return false;
  GLOW_W = lv_obj_get_style_width(H.dashboard, LV_PART_MAIN);
  GLOW_H = lv_obj_get_style_height(H.dashboard, LV_PART_MAIN);
  ERR_LEN = (GLOW_W + 2) * 3;
  s_glow_buf[0] = psram_alloc<uint16_t>(GLOW_W * GLOW_H);
  s_glow_buf[1] = psram_alloc<uint16_t>(GLOW_W * GLOW_H);
  float *axes = psram_alloc<float>(2 * (GLOW_W + GLOW_H));
  s_job.err = {psram_alloc<int16_t>(ERR_LEN), psram_alloc<int16_t>(ERR_LEN)};
  if (s_glow_buf[0] == nullptr || s_glow_buf[1] == nullptr || axes == nullptr ||
      s_job.err.cur == nullptr || s_job.err.next == nullptr) {
    for (void *p : {(void *) s_glow_buf[0], (void *) s_glow_buf[1], (void *) axes,
                    (void *) s_job.err.cur, (void *) s_job.err.next})
      heap_caps_free(p);
    s_glow_buf[0] = s_glow_buf[1] = nullptr;
    ESP_LOGW(TAG, "glow: no PSRAM for the %dx%d buffers", GLOW_W, GLOW_H);
    return false;
  }
  s_job.ax = axes;
  s_job.ay = s_job.ax + GLOW_W;
  s_job.bx = s_job.ay + GLOW_H;
  s_job.by = s_job.bx + GLOW_W;
  s_glow_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
  s_glow_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
  s_glow_dsc.header.w = GLOW_W;
  s_glow_dsc.header.h = GLOW_H;
  s_glow_dsc.header.stride = GLOW_W * sizeof(uint16_t);
  s_glow_dsc.data_size = GLOW_W * GLOW_H * sizeof(uint16_t);
  return true;
}

static void paint_glow_row(int y) {
  uint16_t *row = s_glow_buf[1 - s_glow_front] + y * GLOW_W;
  const float ay = s_job.ay[y], by = s_job.by[y];
  for (int x = 0; x < GLOW_W; x++) {
    Rgb c = mix(s_job.base, s_job.upper, s_job.ax[x] * ay);
    c = mix(c, s_job.lower, s_job.bx[x] * by);
    row[x] = diffused_rgb565(s_job.err, c, x);
  }
  std::swap(s_job.err.cur, s_job.err.next);
  std::fill(s_job.err.next, s_job.err.next + ERR_LEN, int16_t(0));
}

static void finish_glow_job() {
  s_job.active = false;
  lv_timer_pause(s_glow_timer);
  s_glow_front = 1 - s_glow_front;
  s_glow_dsc.data = reinterpret_cast<const uint8_t *>(s_glow_buf[s_glow_front]);
  s_glow_rendered_url = s_job.url;
  ESP_LOGI(TAG, "glow ready: busy %ums over %ums (upper %02X%02X%02X, lower %02X%02X%02X)",
           (unsigned)(s_job.busy_us / 1000), (unsigned)(millis() - s_job.started_ms),
           int(s_job.upper.r), int(s_job.upper.g), int(s_job.upper.b), int(s_job.lower.r),
           int(s_job.lower.g), int(s_job.lower.b));
  on_glow_frame_ready();
}

static void glow_timer_cb(lv_timer_t *) {
  if (!s_job.active) {
    lv_timer_pause(s_glow_timer);
    return;
  }
  const uint32_t start = micros();
  do {
    paint_glow_row(s_job.row++);
  } while (s_job.row < GLOW_H && micros() - start < GLOW_SLICE_US);
  s_job.busy_us += micros() - start;
  if (s_job.row == GLOW_H) finish_glow_job();
}

// (Re)starts painting for a new track; a job still running for an older
// track simply starts over with the new colors.
static bool start_glow_job(const lv_image_dsc_t *thumb, const std::string &url) {
  if (thumb == nullptr || thumb->data == nullptr || thumb->header.cf != LV_COLOR_FORMAT_RGB565 ||
      thumb->header.w < 2 || thumb->header.h < 2 || !ensure_glow_buffers())
    return false;
  s_job.upper = glow_color(vivid_mean(thumb, true), 150);
  s_job.lower = glow_color(vivid_mean(thumb, false), 140);
  s_job.base = mix(from_hex(COL_BG), mix(s_job.upper, s_job.lower, 0.5f), 0.10f);
  // main glow sits behind the cover (center 288,344 on the 1280x800 panel),
  // the second one bleeds in from the lower-right corner — same composition
  // as the design mockup, scaled to the screen
  const float sx = GLOW_W / 1280.0f, sy = GLOW_H / 800.0f;
  fill_gaussian(s_job.ax, GLOW_W, 300 * sx, 520 * sx, 0.62f);
  fill_gaussian(s_job.ay, GLOW_H, 320 * sy, 400 * sy, 1.0f);
  fill_gaussian(s_job.bx, GLOW_W, 1260 * sx, 600 * sx, 0.48f);
  fill_gaussian(s_job.by, GLOW_H, 820 * sy, 400 * sy, 1.0f);
  std::fill(s_job.err.cur, s_job.err.cur + ERR_LEN, int16_t(0));
  std::fill(s_job.err.next, s_job.err.next + ERR_LEN, int16_t(0));
  s_job.url = url;
  s_job.row = 0;
  s_job.busy_us = 0;
  s_job.started_ms = millis();
  s_job.active = true;
  if (s_glow_timer == nullptr) s_glow_timer = lv_timer_create(glow_timer_cb, 1, nullptr);
  lv_timer_resume(s_glow_timer);
  return true;
}

// ── data-driven updates ─────────────────────────────────────────────────────

static int32_t s_player_name_w = 0;  // YAML width; the "+N" badge takes 40 px of it
static std::string s_cover_url, s_tile_url[4];
static bool s_tile_src_bound[4] = {false, false, false, false};
static int s_tile_count = 0;  // "Als Nächstes" tiles the device layout has (4 panel, 2 P4-86)

static void on_glow_frame_ready() {
  if (H.glow_img == nullptr) return;
  lv_image_cache_drop(&s_glow_dsc);
  lv_image_set_src(H.glow_img, nullptr);
  lv_image_set_src(H.glow_img, &s_glow_dsc);
  lv_obj_invalidate(H.glow_img);
}

// Paint once per track; the previous glow stays up while the next cover
// downloads and paints, so track changes never flash the flat background.
static void update_glow_(bool have) {
  if (H.glow_img == nullptr) return;
  const bool cover_ready = have && widgets::image_ready(H.cover_dl, s_cover_url);
  const bool pending = s_glow_rendered_url != s_cover_url &&
                       !(s_job.active && s_job.url == s_cover_url);
  if (cover_ready && pending) start_glow_job(H.cover_dl->get_lv_image_dsc(), s_cover_url);
  const bool show = have && !s_glow_rendered_url.empty();
  if (show == lv_obj_has_flag(H.glow_img, LV_OBJ_FLAG_HIDDEN))
    (show ? lv_obj_clear_flag : lv_obj_add_flag)(H.glow_img, LV_OBJ_FLAG_HIDDEN);
}

static void update_cover_() {
  const std::string url = ma_cover_url(s_client->now_playing().image_url);
  const bool have = !url.empty();
  if (have && url != s_cover_url && H.cover_dl != nullptr && widgets::download_slot()) {
    s_cover_url = url;
    H.cover_dl->request_update_url(url);
    ESP_LOGD(TAG, "cover download: %.60s", url.c_str());
  }
  // cover visible only when the buffer for the REQUESTED url is decoded and
  // no request is in flight — has_image() alone is already true for the 1x1
  // boot placeholder, which painted the column black (glass round 2)
  const bool cover_ready = have && widgets::image_ready(H.cover_dl, s_cover_url);
  // Bind the DECODED descriptor once per URL — the YAML src: binding keeps
  // pointing at the boot-time 1x1 descriptor forever (fork's media grid does
  // the same after download: button_grid_media.h)
  static std::string s_cover_bound_url;
  if (cover_ready && s_cover_bound_url != s_cover_url && H.cover_img != nullptr) {
    bind_image(H.cover_img, H.cover_dl);
    s_cover_bound_url = s_cover_url;
  }
  static bool s_cover_shown = false;
  if (H.cover_img != nullptr) {
    const bool vis = !lv_obj_has_flag(H.cover_img, LV_OBJ_FLAG_HIDDEN);
    if (vis != cover_ready) {
      (cover_ready ? lv_obj_clear_flag : lv_obj_add_flag)(H.cover_img, LV_OBJ_FLAG_HIDDEN);
      if (cover_ready != s_cover_shown) {
        ESP_LOGI(TAG, "cover %s (http %d)", cover_ready ? "READY" : "pending",
                 H.cover_dl != nullptr ? H.cover_dl->get_last_http_status() : 0);
        s_cover_shown = cover_ready;
      }
    }
  }
  if (H.cover_ph != nullptr) {
    const bool vis = !lv_obj_has_flag(H.cover_ph, LV_OBJ_FLAG_HIDDEN);
    if (vis == cover_ready)
      (cover_ready ? lv_obj_add_flag : lv_obj_clear_flag)(H.cover_ph, LV_OBJ_FLAG_HIDDEN);
  }
  update_glow_(have);
}

// On a track change the row shifts by one: tiles 1-3 want images that the
// neighbouring downloaders already hold. Swap downloaders instead of
// re-downloading — one new decode per track change instead of four (every
// burst of HW-JPEG/PPA work competes with the display refresh on the PSRAM
// bus and showed up as flicker).
static void reuse_tile_downloads_(const std::vector<MAQueueItem> &q) {
  for (int i = 0; i < s_tile_count; i++) {
    const int row = i + 1;
    if (row >= (int) q.size() || q[row].image_url.empty() || s_tile_url[i] == q[row].image_url) continue;
    for (int j = 0; j < s_tile_count; j++) {
      if (j == i || s_tile_url[j] != q[row].image_url) continue;
      std::swap(H.tile_dl[i], H.tile_dl[j]);
      std::swap(s_tile_url[i], s_tile_url[j]);
      s_tile_src_bound[i] = s_tile_src_bound[j] = false;  // re-point both images
      break;
    }
  }
}

static void update_tiles_() {
  const auto &q = s_client->queue();
  uint32_t remaining = s_client->queue_total_items() > s_client->queue_current_index()
                           ? s_client->queue_total_items() - s_client->queue_current_index() - 1 : 0;
  uint32_t rem_seconds = 0;
  for (const auto &item : q) rem_seconds += item.duration_s;
  char cnt[48];
  snprintf(cnt, sizeof(cnt), "%u Titel · %u:%02u h", (unsigned) remaining,
           (unsigned)(rem_seconds / 3600), (unsigned)((rem_seconds % 3600) / 60));
  set_lbl(H.next_count, cnt);

  // "Als Nächstes" = the tracks AFTER the current one: the loaded queue
  // window starts at current_index, so row 0 is the playing track — skip it.
  reuse_tile_downloads_(q);
  for (int i = 0; i < s_tile_count; i++) {
    const int row = i + 1;
    const bool have = row < (int) q.size();
    if (H.tiles[i] != nullptr) {
      const bool vis = !lv_obj_has_flag(H.tiles[i], LV_OBJ_FLAG_HIDDEN);
      if (vis != have) (have ? lv_obj_clear_flag : lv_obj_add_flag)(H.tiles[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (!have) continue;
    set_lbl(H.tile_title[i], q[row].title.c_str());
    set_lbl(H.tile_artist[i], q[row].artist.c_str());
    const bool thumb_ready = have && widgets::image_ready(H.tile_dl[i], s_tile_url[i]);
    if (H.tile_img[i] != nullptr) {
      const bool vis = !lv_obj_has_flag(H.tile_img[i], LV_OBJ_FLAG_HIDDEN);
      if (vis != thumb_ready)
        (thumb_ready ? lv_obj_clear_flag : lv_obj_add_flag)(H.tile_img[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (thumb_ready && !s_tile_src_bound[i] && H.tile_img[i] != nullptr) {
      bind_image(H.tile_img[i], H.tile_dl[i]);
      s_tile_src_bound[i] = true;
    }
    if (!q[row].image_url.empty() && q[row].image_url != s_tile_url[i] && H.tile_dl[i] != nullptr &&
        widgets::download_slot()) {
      s_tile_url[i] = q[row].image_url;
      s_tile_src_bound[i] = false;
      H.tile_dl[i]->request_update_url(q[row].image_url);
    }
  }
}

static void update_static_() {
  const bool connected = s_client->is_connected();
  const auto *p = s_client->player(s_client->selected_player());
  const auto &np = s_client->now_playing();
  const bool playing = p != nullptr && p->playback_state == "playing";
  const bool paused = p != nullptr && p->playback_state == "paused";

  if (!connected) {
    set_lbl(H.status_lbl, "VERBINDE …");
    set_title("Music Assistant nicht erreichbar");
    set_lbl(H.artist, "— verbinde —");
    set_lbl(H.album, "");
    set_bg_color(H.status_dot, COL_GRAY);
    set_bg_opa(H.play_btn, LV_OPA_40);
    return;
  }
  set_bg_opa(H.play_btn, LV_OPA_COVER);

  // status line + dot
  std::string where = p != nullptr ? p->display_name : "";
  for (char &c : where) c = toupper(c);
  set_lbl(H.status_lbl, playing ? ("SPIELT IN " + where).c_str()
                                : (paused ? ("PAUSIERT IN " + where).c_str() : "BEREIT"));
  set_bg_color(H.status_dot, playing ? COL_PLAYING : COL_GRAY);

  // now playing
  const bool nothing = np.title.empty();
  set_title(nothing ? "Nichts läuft" : np.title.c_str());
  set_lbl(H.artist, nothing ? "" : np.artist.c_str());
  set_lbl(H.album, nothing ? "" : np.album.c_str());

  // player pill
  set_lbl(H.player_name, p != nullptr ? p->display_name.c_str() : "—");
  const size_t members = p != nullptr ? p->group_members.size() : 0;
  const bool badge = members > 1;
  const bool badge_vis = H.group_badge == nullptr || !lv_obj_has_flag(H.group_badge, LV_OBJ_FLAG_HIDDEN);
  if (badge_vis != badge && H.group_badge != nullptr)
      (badge ? lv_obj_clear_flag : lv_obj_add_flag)(H.group_badge, LV_OBJ_FLAG_HIDDEN);
  // the name gives way to the "+N" badge
  if (H.player_name != nullptr) lv_obj_set_width(H.player_name, badge ? s_player_name_w - 40 : s_player_name_w);
  if (badge) {
    char b[8];
    snprintf(b, sizeof(b), "+%u", (unsigned)(members - 1));
    set_lbl(H.group_badge_lbl, b);
  }

  // transport icon states
  set_lbl(H.play_icon, playing ? GLYPH_PAUSE : GLYPH_PLAY);
  set_color(H.shuffle_icon, np.shuffle ? COL_ACCENT : COL_MUTED);
  // favourite: filled accent heart on the accent "selected" style; not a
  // favourite: grey outline on plain glass
  if (H.fav_icon != nullptr) {
    set_lbl(H.fav_icon, np.favorite ? GLYPH_HEART : GLYPH_HEART_OUTLINE);
    set_color(H.fav_icon, np.favorite ? COL_ACCENT : COL_GRAY);
  }
  if (H.fav_btn != nullptr) {
    if (np.favorite)
      widgets::style_selected(H.fav_btn);
    else
      widgets::style_glass(H.fav_btn);
  }
  set_lbl(H.repeat_icon, np.repeat == "one" ? GLYPH_REPEAT_ONE
                                            : (np.repeat == "all" ? GLYPH_REPEAT_ALL : GLYPH_REPEAT_OFF));
  set_color(H.repeat_icon, np.repeat != "off" ? COL_ACCENT : COL_MUTED);

  // mute + volume
  set_lbl(H.mute_icon, (p != nullptr && p->muted) ? GLYPH_VOL_OFF : GLYPH_VOL_HIGH);
  if (!s_vol_dragging && p != nullptr && p->volume_level >= 0 && H.vol != nullptr) {
    lv_slider_set_value(H.vol, p->volume_level, LV_ANIM_OFF);
    char b[8];
    snprintf(b, sizeof(b), "%d%%", p->volume_level);
    set_lbl(H.vol_pct, b);
  }

  update_cover_();
  update_tiles_();
}

// ── waveform progress ───────────────────────────────────────────────────────
// With an MA audio analysis the slider turns invisible (it stays the touch
// target for seeking) and the RMS waveform is drawn behind it: played bars
// bright, the rest dim. Redrawn only when the played/unplayed split moves.
static lv_obj_t *s_wave = nullptr;
static int s_wave_played = -1;
static bool s_wave_shown = false;
static constexpr int WAVE_HEIGHT = 40;

static bool have_waveform() { return (int) s_client->now_playing().waveform.size() == WAVE_BINS; }

static void on_wave_draw(lv_event_t *e) {
  if (!have_waveform()) return;
  const auto &wave = s_client->now_playing().waveform;
  lv_area_t area;
  lv_obj_get_coords(s_wave, &area);
  const int height = lv_area_get_height(&area), mid = area.y1 + height / 2;
  const float step = (float) lv_area_get_width(&area) / WAVE_BINS;
  const int bar_w = std::max(1, (int) (step * 0.6f));
  lv_draw_rect_dsc_t played, rest;
  lv_draw_rect_dsc_init(&played);
  lv_draw_rect_dsc_init(&rest);
  played.bg_color = rest.bg_color = lv_color_hex(COL_TEXT);
  played.bg_opa = LV_OPA_COVER;
  rest.bg_opa = 90;
  played.radius = rest.radius = 1;
  lv_layer_t *layer = lv_event_get_layer(e);
  for (int b = 0; b < WAVE_BINS; b++) {
    const int half = std::max(1, wave[b] * height / 255 / 2);
    const int x = area.x1 + (int) (b * step);
    const lv_area_t bar = {x, mid - half, x + bar_w - 1, mid + half};
    lv_draw_rect(layer, b < s_wave_played ? &played : &rest, &bar);
  }
}

static void create_wave_() {
  if (H.progress == nullptr) return;
  s_wave = lv_obj_create(lv_obj_get_parent(H.progress));
  lv_obj_remove_style_all(s_wave);
  lv_obj_remove_flag(s_wave, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(s_wave, LV_OBJ_FLAG_SCROLLABLE);
  const int32_t centre = lv_obj_get_style_y(H.progress, LV_PART_MAIN) + lv_obj_get_style_height(H.progress, LV_PART_MAIN) / 2;
  lv_obj_set_pos(s_wave, lv_obj_get_style_x(H.progress, LV_PART_MAIN), centre - WAVE_HEIGHT / 2);
  lv_obj_set_size(s_wave, lv_obj_get_style_width(H.progress, LV_PART_MAIN), WAVE_HEIGHT);
  lv_obj_move_to_index(s_wave, lv_obj_get_index(H.progress));  // behind the slider
  lv_obj_add_event_cb(s_wave, on_wave_draw, LV_EVENT_DRAW_MAIN, nullptr);
  lv_obj_add_flag(s_wave, LV_OBJ_FLAG_HIDDEN);
}

// Waveform on/off: the slider keeps handling touch, only its look changes.
static void show_wave_(bool on) {
  if (s_wave == nullptr || on == s_wave_shown) return;
  s_wave_shown = on;
  (on ? lv_obj_remove_flag : lv_obj_add_flag)(s_wave, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_style_bg_opa(H.progress, on ? LV_OPA_TRANSP : 36, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(H.progress, on ? LV_OPA_TRANSP : LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(H.progress, on ? LV_OPA_TRANSP : LV_OPA_COVER, LV_PART_KNOB);
}

static void update_wave_(float fraction, bool data_changed) {
  show_wave_(have_waveform());
  if (!s_wave_shown) return;
  const int played = (int) (std::max(0.0f, std::min(1.0f, fraction)) * WAVE_BINS);
  if (played == s_wave_played && !data_changed) return;
  s_wave_played = played;
  lv_obj_invalidate(s_wave);
}

static void update_progress_() {
  const auto &np = s_client->now_playing();
  const float el = s_client->elapsed_now();
  set_lbl(H.time_cur, fmt_mmss((uint32_t) el).c_str());
  if (np.duration_s > 0) {
    const uint32_t rem = np.duration_s > (uint32_t) el ? np.duration_s - (uint32_t) el : 0;
    set_lbl(H.time_rem, ("−" + fmt_mmss(rem)).c_str());
    if (!s_progress_dragging && H.progress != nullptr)
      lv_slider_set_value(H.progress, (int32_t)(el * 1000.0f / np.duration_s), LV_ANIM_OFF);
    // while dragging the waveform follows the finger
    const float fraction = s_progress_dragging && H.progress != nullptr
                               ? lv_slider_get_value(H.progress) / 1000.0f
                               : el / np.duration_s;
    static const uint8_t *s_wave_data = nullptr;
    const bool data_changed = np.waveform.empty() ? s_wave_data != nullptr : s_wave_data != np.waveform.data();
    s_wave_data = np.waveform.empty() ? nullptr : np.waveform.data();
    update_wave_(fraction, data_changed);
  } else {
    set_lbl(H.time_rem, "");
    show_wave_(false);
  }
}

// ── public API ──────────────────────────────────────────────────────────────

// The default theme pads every obj, so a 28 px icon label overflows the small
// icon buttons and LVGL paints thin grey scrollbars at their right and bottom
// edges. Nothing on the dashboard scrolls — disable it for the whole tree
// (a future scrolling list must opt back in explicitly).
static void disable_scrolling(lv_obj_t *obj) {
  lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
  lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
  const uint32_t n = lv_obj_get_child_count(obj);
  for (uint32_t i = 0; i < n; i++) disable_scrolling(lv_obj_get_child(obj, i));
}

void bind(MusicAssistant *client, const Handles &h,
          std::function<void()> hold_screensaver,
          std::function<void()> return_to_screensaver,
          std::function<void()> clear_wake_guard) {
  s_client = client;
  H = h;
  s_hold_screensaver = std::move(hold_screensaver);
  s_return_to_screensaver = std::move(return_to_screensaver);
  s_clear_wake_guard = std::move(clear_wake_guard);
  s_bound = s_client != nullptr && H.dashboard != nullptr;

  if (H.glow_img != nullptr) lv_obj_set_style_opa(H.glow_img, LV_OPA_COVER, LV_PART_MAIN);
  if (H.dashboard != nullptr) disable_scrolling(H.dashboard);
  if (H.player_name != nullptr) s_player_name_w = lv_obj_get_style_width(H.player_name, LV_PART_MAIN);
  while (s_tile_count < 4 && H.tiles[s_tile_count] != nullptr) s_tile_count++;
  if (H.title != nullptr) {
    s_title_font_large = lv_obj_get_style_text_font(H.title, LV_PART_MAIN);
    s_title_y = lv_obj_get_style_y(H.title, LV_PART_MAIN);
    s_title_h = lv_obj_get_style_height(H.title, LV_PART_MAIN);
  }
  style_progress(H.progress);
  create_wave_();
  style_volume(H.vol);
  lv_slider_set_range(H.progress, 0, 1000);
  lv_slider_set_range(H.vol, 0, 100);

  auto click = [](lv_obj_t *obj, lv_event_cb_t cb, void *ud) {
    if (obj != nullptr) lv_obj_add_event_cb(obj, cb, LV_EVENT_CLICKED, ud);
  };
  click(H.back_btn, on_close, nullptr);
  click(H.play_btn, on_transport, (void *)(intptr_t) 0);
  click(H.prev_btn, on_transport, (void *)(intptr_t) 1);
  click(H.next_btn, on_transport, (void *)(intptr_t) 2);
  click(H.shuffle_btn, on_transport, (void *)(intptr_t) 3);
  click(H.repeat_btn, on_transport, (void *)(intptr_t) 4);
  click(H.mute_btn, on_transport, (void *)(intptr_t) 5);
  click(H.fav_btn, on_transport, (void *)(intptr_t) 6);
  for (int i = 0; i < 4; i++) click(H.tiles[i], on_tile, (void *)(intptr_t) i);
  lv_obj_add_event_cb(H.progress, on_progress_pressed, LV_EVENT_PRESSED, nullptr);
  lv_obj_add_event_cb(
      H.progress, [](lv_event_t *) { if (s_progress_dragging) update_wave_(lv_slider_get_value(H.progress) / 1000.0f, false); },
      LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(H.progress, on_progress_released, LV_EVENT_RELEASED, nullptr);
  lv_obj_add_event_cb(H.vol, on_vol_changed, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(H.vol, on_vol_released, LV_EVENT_RELEASED, nullptr);
  watch_touch_input();
  click(H.player_pill, on_open_rooms, nullptr);
  click(H.grid_btn, on_open_rooms, nullptr);
  click(H.queue_btn, on_open_queue, nullptr);
  click(H.library_btn, on_open_library, nullptr);

  // square 720x720 screen: rooms as a list, queue without the cover column
  const bool compact = lv_obj_get_style_width(H.dashboard, LV_PART_MAIN) < 1000;
  ma_rooms::build(H.screen2, s_client, H.fonts, H.card_dl, compact,
                  {[]() {
                     show_screen(1);
                     update_static_();
                     update_progress_();
                   },
                   []() {
                     if (s_clear_wake_guard) s_clear_wake_guard();
                   }});
  ma_queue::build(H.screen3, s_client, H.fonts, H.cover_dl, H.row_dl, compact,
                  {[]() {
                     show_screen(1);
                     update_static_();
                     update_progress_();
                   },
                   []() {
                     if (s_clear_wake_guard) s_clear_wake_guard();
                   }});

  ESP_LOGI(TAG, "bound (dashboard UI)");
}

bool bound() { return s_bound; }

void open() {
  if (!s_bound) return;
  lv_obj_clear_flag(H.dashboard, LV_OBJ_FLAG_HIDDEN);
  s_client->set_ui_active(true);  // fast polling + queue data from now on
  s_last_touch_ms = millis();
  show_screen(1);
  // populate immediately — otherwise labels/cover wait for the next data
  // revision (up to a poll interval of stale-looking emptiness)
  update_static_();
  update_progress_();
}

void close() {
  if (!s_bound) return;
  lv_obj_add_flag(H.dashboard, LV_OBJ_FLAG_HIDDEN);
  s_client->set_ui_active(false);
  // give the internal RAM back to WiFi/lwIP while the screensaver runs
  ma_queue::release();
  s_client->release_lists();
  if (s_return_to_screensaver) s_return_to_screensaver();
}

void refresh() {
  if (!s_bound) return;
  const bool is_open = !lv_obj_has_flag(H.dashboard, LV_OBJ_FLAG_HIDDEN);
  if (is_open) {
    if (s_hold_screensaver) s_hold_screensaver();
    if (millis() - s_last_touch_ms >= IDLE_CLOSE_MS) {
      ESP_LOGI(TAG, "no touch for %.0fs: returning to screensaver", IDLE_CLOSE_MS / 1000.0f);
      close();
      return;
    }
  }

  // closed: no UI work, no downloads, slow MA polling — open() catches up
  s_client->set_ui_active(is_open);
  if (!is_open) return;

  static uint32_t last_rev = 0;
  static uint32_t last_tick_ms = 0;
  const uint32_t rev = s_client->revision();
  if (rev != last_rev) {
    last_rev = rev;
    update_static_();
    update_progress_();
    if (is_open && s_screen == 2) ma_rooms::refresh();
    if (is_open && s_screen == 3) ma_queue::refresh();
    last_tick_ms = millis();
    return;
  }
  if (millis() - last_tick_ms >= 1000) {
    last_tick_ms = millis();
    if (is_open) {
      update_progress_();
      // downloads deferred by the shared pacing (widgets::download_slot)
      // must not wait for the next data revision
      if (s_screen == 1) {
        update_cover_();
        update_tiles_();
      }
      if (H.clock_src != nullptr && H.clock_lbl != nullptr)
        set_lbl(H.clock_lbl, lv_label_get_text(H.clock_src));
      const char *clock = H.clock_src != nullptr ? lv_label_get_text(H.clock_src) : nullptr;
      if (s_screen == 2) ma_rooms::tick(clock);
      if (s_screen == 3) ma_queue::tick(clock);
    }
  }
}

}  // namespace ma_ui
}  // namespace music_assistant
}  // namespace esphome
