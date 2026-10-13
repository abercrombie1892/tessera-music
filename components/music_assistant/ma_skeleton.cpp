#include "ma_skeleton.h"
#include "ma_theme.h"
#include "ma_widgets.h"

#include <algorithm>

namespace esphome {
namespace music_assistant {
namespace ma_skeleton {

using namespace theme;
using widgets::make_box;
using widgets::make_label;

static constexpr lv_opa_t OPA_7 = 18, OPA_8 = 20, OPA_11 = 28, OPA_17 = 43;
static constexpr uint32_t WHITE = 0xFFFFFF;
// glyphs of ma_dashboard.yaml
static const char *const GLYPH_DOWN = "\U000F0140";
static const char *const GLYPH_SPEAKER = "\U000F04C3";
static const char *const GLYPH_GRID = "\U000F11D9";
static const char *const GLYPH_LIBRARY = "\U000F0411";  // playlist-play, in the icon font's glyphs
static const char *const GLYPH_SHUFFLE = "\U000F049F";
static const char *const GLYPH_PREV = "\U000F04AE";
static const char *const GLYPH_NEXT = "\U000F04AD";
static const char *const GLYPH_REPEAT_OFF = "\U000F0457";

// An obj of the YAML: a box with a fill and, for the glass ones, a 1 px border.
static lv_obj_t *obj(lv_obj_t *parent, int x, int y, int w, int h, int radius, uint32_t color, lv_opa_t opa,
                     bool border) {
  lv_obj_t *o = make_box(parent, x, y, w, h, radius);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);  // the YAML's clickable: false; the keys say so themselves
  lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
  lv_obj_set_style_bg_opa(o, opa, 0);
  if (border) {
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(WHITE), 0);
    lv_obj_set_style_border_opa(o, OPA_11, 0);
  }
  return o;
}

static lv_obj_t *glass(lv_obj_t *parent, int x, int y, int w, int h, lv_opa_t opa = OPA_7) {
  lv_obj_t *o = obj(parent, x, y, w, h, std::min(w, h) / 2, WHITE, opa, true);
  lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
  return o;
}

// A label with `long_mode: dot`, a width and a height of its own (the YAML's labels with height:).
static lv_obj_t *line(lv_obj_t *parent, const lv_font_t *font, uint32_t color, int x, int y, int w, int h) {
  lv_obj_t *l = make_label(parent, font, color, x, y, w);
  lv_obj_set_height(l, std::max<int>(h, lv_font_get_line_height(font)));  // the larger small fonts keep their descenders
  return l;
}

static lv_obj_t *icon(lv_obj_t *parent, const lv_font_t *font, const char *glyph, uint32_t color) {
  return widgets::make_icon(parent, font, glyph, color);
}

static lv_obj_t *picture(lv_obj_t *parent, int x, int y, int side, int radius) {
  lv_obj_t *img = lv_image_create(parent);
  lv_obj_set_pos(img, x, y);
  lv_obj_set_size(img, side, side);
  lv_obj_set_style_radius(img, radius, 0);
  lv_obj_set_style_clip_corner(img, true, 0);
  lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
  return img;
}

static lv_obj_t *screen(lv_obj_t *dashboard, bool hidden) {
  lv_obj_t *s = make_box(dashboard, 0, 0, 720, 720, 0);
  lv_obj_remove_flag(s, LV_OBJ_FLAG_CLICKABLE);
  if (hidden) lv_obj_add_flag(s, LV_OBJ_FLAG_HIDDEN);
  return s;
}

// Topbar: back · player pill · clock · rooms
static void build_topbar(lv_obj_t *s, const Fonts &f, ma_ui::Handles &h) {
  h.back_btn = glass(s, 20, 20, 52, 52);
  icon(h.back_btn, f.icon34, GLYPH_BACK, COL_TEXT);

  h.player_pill = glass(s, 86, 20, 330, 52, OPA_8);
  lv_obj_t *dot = obj(h.player_pill, 7, 7, 36, 36, 18, COL_ACCENT, LV_OPA_COVER, false);
  icon(dot, f.icon24, GLYPH_SPEAKER, COL_ON_ACCENT);
  h.player_name = make_label(h.player_pill, f.text20, COL_TEXT, 0, 0, 212);
  lv_obj_align(h.player_name, LV_ALIGN_LEFT_MID, 54, 0);
  lv_label_set_text(h.player_name, "…");
  h.group_badge = obj(h.player_pill, 230, 12, 34, 28, 14, COL_TEXT, LV_OPA_COVER, false);
  lv_obj_add_flag(h.group_badge, LV_OBJ_FLAG_HIDDEN);
  h.group_badge_lbl = icon(h.group_badge, f.text14, "+2", COL_ON_ACCENT);
  lv_obj_t *chevron = make_label(h.player_pill, f.icon24, COL_MUTED, 0, 0);
  lv_label_set_text(chevron, GLYPH_DOWN);
  lv_obj_align(chevron, LV_ALIGN_LEFT_MID, 292, 0);

  // the clock gives the library key room: it ends at 570 instead of 630
  h.clock_lbl = make_label(s, f.text30, COL_TEXT, 430, 27, 140);
  lv_obj_set_style_text_align(h.clock_lbl, LV_TEXT_ALIGN_RIGHT, 0);
  lv_label_set_text(h.clock_lbl, "--:--");

  h.library_btn = glass(s, 588, 20, 52, 52);
  icon(h.library_btn, f.icon28, GLYPH_LIBRARY, COL_TEXT);
  h.grid_btn = glass(s, 648, 20, 52, 52);
  icon(h.grid_btn, f.icon28, GLYPH_GRID, COL_TEXT);
}

// Cover (320 px, left) and the right column: status · title · artist · album · favourite
static void build_now(lv_obj_t *s, const Fonts &f, ma_ui::Handles &h) {
  h.cover_ph = obj(s, 20, 92, 320, 320, 24, WHITE, OPA_7, true);
  icon(h.cover_ph, f.icon48, GLYPH_MUSIC, COL_MUTED);
  h.cover_img = picture(s, 20, 92, 320, 24);

  h.status_dot = obj(s, 364, 100, 10, 10, 5, COL_PLAYING, LV_OPA_COVER, false);
  h.status_lbl = line(s, f.text14, COL_MUTED, 382, 95, 256, 18);
  lv_obj_set_style_text_letter_space(h.status_lbl, 2, 0);
  lv_label_set_text(h.status_lbl, "SPIELT");

  h.fav_btn = glass(s, 652, 88, 48, 48);
  h.fav_icon = icon(h.fav_btn, f.icon24, GLYPH_HEART_OUTLINE, COL_GRAY);

  h.title = line(s, f.text34, COL_TEXT, 364, 132, 336, 46);
  lv_label_set_text(h.title, "—");
  h.artist = line(s, f.text20, COL_TEXT2, 364, 184, 336, 26);
  h.album = line(s, f.text17, COL_DIM, 364, 214, 336, 22);
}

// Als Nächstes: 2 tiles; the whole card opens screen 3
static void build_next_up(lv_obj_t *s, const Fonts &f, ma_ui::Handles &h) {
  h.queue_btn = obj(s, 364, 262, 336, 150, 20, WHITE, OPA_7, true);
  lv_obj_add_flag(h.queue_btn, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_opa(h.queue_btn, OPA_17, LV_STATE_PRESSED);
  lv_obj_t *head = make_label(h.queue_btn, f.text14, COL_MUTED, 16, 12);
  lv_obj_set_style_text_letter_space(head, 2, 0);
  lv_label_set_text(head, "ALS NÄCHSTES");
  h.next_count = line(h.queue_btn, f.text14, COL_GRAY, 150, 12, 170, 18);
  lv_obj_set_style_text_align(h.next_count, LV_TEXT_ALIGN_RIGHT, 0);
  for (int i = 0; i < 2; i++) {
    h.tiles[i] = make_box(h.queue_btn, 10, 40 + i * 54, 316, 50, 0);
    lv_obj_remove_flag(h.tiles[i], LV_OBJ_FLAG_CLICKABLE);  // a tap anywhere on the card opens the list
    h.tile_img[i] = picture(h.tiles[i], 6, 3, 44, 10);
    h.tile_title[i] = line(h.tiles[i], f.text17, COL_TEXT, 60, 4, 250, 22);
    h.tile_artist[i] = line(h.tiles[i], f.text14, COL_DIM, 60, 27, 250, 18);
  }
}

// Progress (waveform drawn behind it by C++) + times, the transport row, the volume pill
static void build_controls(lv_obj_t *s, const Fonts &f, ma_ui::Handles &h) {
  h.progress = lv_slider_create(s);
  lv_obj_set_pos(h.progress, 20, 444);
  lv_obj_set_size(h.progress, 680, 24);
  h.time_cur = make_label(s, f.text14, COL_DIM, 20, 482);
  lv_label_set_text(h.time_cur, "0:00");
  h.time_rem = make_label(s, f.text14, COL_DIM, 20, 482, 680);
  lv_obj_set_style_text_align(h.time_rem, LV_TEXT_ALIGN_RIGHT, 0);
  lv_label_set_text(h.time_rem, "−0:00");

  h.shuffle_btn = make_box(s, 152, 534, 56, 56, 28);
  lv_obj_add_flag(h.shuffle_btn, LV_OBJ_FLAG_CLICKABLE);
  h.shuffle_icon = icon(h.shuffle_btn, f.icon28, GLYPH_SHUFFLE, COL_MUTED);
  h.prev_btn = glass(s, 230, 528, 68, 68);
  icon(h.prev_btn, f.icon34, GLYPH_PREV, COL_TEXT);
  h.play_btn = obj(s, 314, 516, 92, 92, 46, COL_ACCENT, LV_OPA_COVER, false);
  lv_obj_add_flag(h.play_btn, LV_OBJ_FLAG_CLICKABLE);
  h.play_icon = icon(h.play_btn, f.icon48, GLYPH_PLAY, COL_ON_ACCENT);
  h.next_btn = glass(s, 422, 528, 68, 68);
  icon(h.next_btn, f.icon34, GLYPH_NEXT, COL_TEXT);
  h.repeat_btn = make_box(s, 512, 534, 56, 56, 28);
  lv_obj_add_flag(h.repeat_btn, LV_OBJ_FLAG_CLICKABLE);
  h.repeat_icon = icon(h.repeat_btn, f.icon28, GLYPH_REPEAT_OFF, COL_MUTED);

  lv_obj_t *pill = obj(s, 20, 632, 680, 64, 32, WHITE, OPA_7, true);
  h.mute_btn = obj(pill, 10, 10, 44, 44, 22, WHITE, OPA_8, false);
  lv_obj_add_flag(h.mute_btn, LV_OBJ_FLAG_CLICKABLE);
  h.mute_icon = icon(h.mute_btn, f.icon28, GLYPH_VOL_HIGH, COL_TEXT);
  h.vol = lv_slider_create(pill);
  lv_obj_set_pos(h.vol, 70, 22);
  lv_obj_set_size(h.vol, 520, 20);
  h.vol_pct = make_label(pill, f.text17, COL_TEXT, 0, 0, 64);
  lv_obj_set_style_text_align(h.vol_pct, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_align(h.vol_pct, LV_ALIGN_RIGHT_MID, -20, 0);
  lv_label_set_text(h.vol_pct, "—");
}

ma_ui::Handles build(lv_obj_t *parent, const Fonts &f, lv_obj_t *clock) {
  ma_ui::Handles h{};
  h.dashboard = obj(parent, 0, 0, 720, 720, 0, COL_BG, LV_OPA_COVER, false);
  lv_obj_add_flag(h.dashboard, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(h.dashboard, LV_OBJ_FLAG_HIDDEN);
  h.glow_img = lv_image_create(h.dashboard);
  lv_obj_set_pos(h.glow_img, 0, 0);
  lv_obj_set_size(h.glow_img, 720, 720);
  lv_obj_add_flag(h.glow_img, LV_OBJ_FLAG_HIDDEN);

  h.screen1 = screen(h.dashboard, false);
  build_topbar(h.screen1, f, h);
  build_now(h.screen1, f, h);
  build_next_up(h.screen1, f, h);
  build_controls(h.screen1, f, h);
  h.screen2 = screen(h.dashboard, true);
  h.screen3 = screen(h.dashboard, true);

  h.clock_src = clock;
  h.title_font_small = f.text26;
  h.fonts.text14 = f.text14;
  h.fonts.text17 = f.text17;
  h.fonts.text20 = f.text20;
  h.fonts.text26 = f.text26;
  h.fonts.text30 = f.text30;
  h.fonts.icon24 = f.icon24;
  h.fonts.icon28 = f.icon28;
  h.fonts.icon34 = f.icon34;

  // The cover pipeline of ma_dashboard.yaml: 1 cover (320), 2 next-up tiles (44), 4 room covers (48), 4 queue
  // thumbnails (44).
  h.cover_dl = new artwork_image::ArtworkImage(320);
  for (auto &dl : h.tile_dl) dl = nullptr;
  for (int i = 0; i < 2; i++) h.tile_dl[i] = new artwork_image::ArtworkImage(44);
  for (auto &dl : h.card_dl) dl = new artwork_image::ArtworkImage(48);
  for (int i = 0; i < ma_queue::THUMB_SLOTS; i++) h.row_dl[i] = i < 4 ? new artwork_image::ArtworkImage(44) : nullptr;
  return h;
}

}  // namespace ma_skeleton
}  // namespace music_assistant
}  // namespace esphome
