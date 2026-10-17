#include "tessera_music.h"
#include <algorithm>
#include <string>
#include "esphome/components/music_assistant/ma_artwork.h"
#include "esphome/components/music_assistant/ma_skeleton.h"
#include <vector>
#include "esp_heap_caps.h"

// private LVGL header (misc/cache/instance/lv_image_cache.h), not in lvgl.h
extern "C" void lv_image_cache_drop(const void *src);

namespace esphome::tessera_music {

using tessera::Font;
namespace ui = tessera::ui;

static constexpr uint32_t ACTIVE_MS = 3000;
static constexpr uint32_t ICON_PLAY = 0xF040A, ICON_PAUSE = 0xF03E4, ICON_NEXT = 0xF04AD, ICON_PREV = 0xF04AE;
// In the plugin's own MDI font (playlist-play, speaker, volume-high), not Tessera's icon set.
static constexpr const char *GLYPH_QUEUE = "\U000F0411", *GLYPH_SPEAKER = "\U000F04C3", *GLYPH_VOLUME = "\U000F057E";
static constexpr uint32_t VOLUME_OPEN_MS = 5000;

static std::string clock_of(uint32_t seconds) {
  char text[16];
  if (seconds >= 3600)
    snprintf(text, sizeof(text), "%u:%02u:%02u", (unsigned) (seconds / 3600), (unsigned) (seconds / 60 % 60),
             (unsigned) (seconds % 60));
  else
    snprintf(text, sizeof(text), "%u:%02u", (unsigned) (seconds / 60), (unsigned) (seconds % 60));
  return text;
}

// A cover's colour as a lit ground for white words (the dashboard's glow_color): saturated a little more, and never
// brighter than `peak`, less for a grey cover.
static lv_color_t lit(uint32_t tone, float peak) {
  float r = (tone >> 16) & 0xFF, g = (tone >> 8) & 0xFF, b = tone & 0xFF;
  const float hi = std::max({r, g, b, 1.0f}), lo = std::min({r, g, b}), avg = (r + g + b) / 3;
  r = std::max(0.0f, avg + (r - avg) * 1.2f), g = std::max(0.0f, avg + (g - avg) * 1.2f),
  b = std::max(0.0f, avg + (b - avg) * 1.2f);
  peak *= 0.4f + 0.6f * std::min(1.0f, (hi - lo) / hi * 2.0f);
  const float scale = peak / std::max({r, g, b, 1.0f});
  return lv_color_make((uint8_t) (r * scale), (uint8_t) (g * scale), (uint8_t) (b * scale));
}

// The ground of a tile in the cover's colours: a soft diagonal blend from `top` (top left) to `foot` (bottom right),
// Floyd-Steinberg dithered into RGB565 as the dashboard's glow is, so it shows no bands.
static void paint_ground(uint16_t *pixels, int w, int h, lv_color_t top, lv_color_t foot) {
  std::vector<float> rows(6 * (w + 2), 0.0f);
  float *cur = rows.data(), *next = cur + 3 * (w + 2);
  const float tr = top.red, tg = top.green, tb = top.blue, fr = foot.red, fg = foot.green, fb = foot.blue;
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      const float t = std::min(1.0f, 0.8f * y / std::max(1, h - 1) + 0.2f * x / std::max(1, w - 1));
      const float want[3] = {tr + (fr - tr) * t, tg + (fg - tg) * t, tb + (fb - tb) * t};
      const int levels[3] = {31, 63, 31};
      int q[3];
      for (int c = 0; c < 3; c++) {
        const int i = (x + 1) * 3 + c;
        const float v = std::clamp(want[c] + cur[i], 0.0f, 255.0f);
        q[c] = (int) (v * levels[c] / 255.0f + 0.5f);
        const float e = v - q[c] * 255.0f / levels[c];
        cur[i + 3] += e * 7 / 16;
        next[i - 3] += e * 3 / 16;
        next[i] += e * 5 / 16;
        next[i + 3] += e / 16;
      }
      pixels[y * w + x] = (uint16_t) ((q[0] << 11) | (q[1] << 5) | q[2]);
    }
    std::swap(cur, next);
    std::fill(next, next + 3 * (w + 2), 0.0f);
  }
}

static void text_color(lv_obj_t *label, uint32_t color) {
  if (!lv_color_eq(lv_obj_get_style_text_color(label, LV_PART_MAIN), lv_color_hex(color)))
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
}

// Now playing: the cover, the title, the artist and how far the track is, for one room or whatever plays.
class NowTile : public tessera::Tile {
 public:
  explicit NowTile(TesseraMusic *plugin) : plugin_(plugin) {}
  ~NowTile() override {
    if (ground_px_ == nullptr) return;
    if (card_ != nullptr && lv_obj_is_valid(card_)) lv_obj_set_style_bg_image_src(card_, nullptr, 0);
    lv_image_cache_drop(&ground_);
    heap_caps_free(ground_px_);
  }

  void create(const tessera::TileContext &c) override {
    width_ = c.width;
    height_ = c.height;
    tile_ = c.tile;
    card_ = lv_obj_get_parent(c.parent);
    tint_ = std::string(c.options["background"] | "theme") == "cover";
    player_ = c.options["player"] | "";
    const std::string title = c.options["title"] | "large";
    show_title_ = title != "off";
    title_font_ = title == "normal" ? Font::TITLE : Font::HEADLINE;
    keys_ = std::string(c.options["keys"] | "show") == "show";
    side_by_side_ = width_ >= height_ * 13 / 10;
    // The bar (queue, room, volume) only where it fits: a small tile stays as it was.
    bar_ = std::string(c.options["bar"] | "show") == "show" && plugin_->icon_font(false) != nullptr;
    int text_h = layout_();
    if (bar_ && !bar_fits_(text_h)) {
      bar_ = false;
      text_h = layout_();
    }
    side_ = side_by_side_ ? height_ : std::max(0, std::min(width_, height_ - text_h - ui::px(10)));

    art_ = ui::block(c.parent, theme::TRACK);
    lv_obj_set_size(art_, side_, side_);
    lv_obj_set_pos(art_, side_by_side_ ? 0 : (width_ - side_) / 2, 0);
    note_ = ui::label(art_, Font::ICON, theme::MUTED);
    ui::set_text(note_, ui::icon(0xF075A));  // music-note
    lv_obj_center(note_);
    image_ = lv_image_create(art_);
    lv_obj_set_size(image_, side_, side_);
    lv_image_set_inner_align(image_, LV_IMAGE_ALIGN_CENTER);
    lv_obj_center(image_);
    lv_obj_add_flag(image_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_radius(art_, ui::px(12), 0);
    lv_obj_set_style_clip_corner(art_, true, 0);

    const int x = side_by_side_ ? side_ + ui::px(16) : 0;
    const int text_w = width_ - x;
    int y = side_by_side_ ? std::max(0, (height_ - text_h) / 2) : side_ + ui::px(10);
    title_ = ui::label(c.parent, title_font_, theme::INK);
    lv_obj_set_pos(title_, x, y);
    lv_obj_set_size(title_, text_w, show_title_ ? ui::line_height(title_font_) : 1);
    if (!show_title_) lv_obj_add_flag(title_, LV_OBJ_FLAG_HIDDEN);
    y += show_title_ ? ui::line_height(title_font_) : 0;
    artist_ = ui::label(c.parent, Font::BODY_LARGE, theme::MUTED);
    lv_obj_set_pos(artist_, x, y);
    lv_obj_set_size(artist_, text_w, ui::line_height(Font::BODY_LARGE));
    y += ui::line_height(Font::BODY_LARGE) + ui::px(8);
    track_ = ui::block(c.parent, theme::TRACK);
    lv_obj_set_pos(track_, x, y);
    lv_obj_set_size(track_, text_w, ui::px(6));
    fill_ = ui::block(track_, theme::ACCENT);
    lv_obj_set_pos(fill_, 0, 0);
    lv_obj_set_size(fill_, 0, ui::px(6));
    y += ui::px(6) + ui::px(6);
    elapsed_ = ui::label(c.parent, Font::BODY, theme::SUBTLE);
    lv_obj_set_pos(elapsed_, x, y);
    lv_obj_set_size(elapsed_, text_w / 2, ui::line_height(Font::BODY));
    total_ = ui::label(c.parent, Font::BODY, theme::SUBTLE);
    lv_obj_set_pos(total_, x + text_w / 2, y);
    lv_obj_set_size(total_, text_w - text_w / 2, ui::line_height(Font::BODY));
    lv_obj_set_style_text_align(total_, LV_TEXT_ALIGN_RIGHT, 0);
    text_w_ = text_w;
    lv_obj_add_flag(art_, LV_OBJ_FLAG_CLICKABLE);  // only the cover opens the dashboard
    lv_obj_set_user_data(art_, reinterpret_cast<void *>(static_cast<intptr_t>(KEY_COVER)));
    lv_obj_add_event_cb(art_, on_key_, LV_EVENT_CLICKED, this);
    y += ui::line_height(Font::BODY) + ui::px(10);
    if (keys_) build_keys_(c.parent, x, y, text_w);
    if (keys_) y += key_h_ + ui::px(10);
    if (bar_) build_bar_(c.parent, x, y, text_w);
  }

  void on_tick(uint32_t) override {
    plugin_->seen();
    auto *ma = plugin_->ma();
    if (ma == nullptr || !ma->is_connected()) {
      ui::set_text(title_, plugin_->text("connecting"));
      ui::set_text(artist_, "");
      return;
    }
    plugin_->follow_room(player_);
    const auto &now = ma->now_playing();
    const auto *player = ma->player(ma->selected_player());
    const bool playing = player != nullptr && player->playback_state == "playing";
    ui::set_text(title_, now.title.empty() ? (player ? player->display_name : plugin_->text("nothing")) : now.title);
    ui::set_text(artist_, now.artist);
    const uint32_t elapsed = (uint32_t) ma->elapsed_now();
    const uint32_t duration = now.duration_s;
    ui::set_text(elapsed_, duration ? clock_of(std::min(elapsed, duration)) : "");
    ui::set_text(total_, duration ? clock_of(duration) : "");
    const int filled = duration ? (int) ((int64_t) text_w_ * std::min(elapsed, duration) / duration) : 0;
    if (lv_obj_get_width(fill_) != filled) lv_obj_set_width(fill_, filled);
    if (!tint_) ui::set_color(artist_, playing ? theme::MUTED : theme::SUBTLE);
    if (keys_) ui::set_text(glyph_of(play_), ui::icon(playing ? ICON_PAUSE : ICON_PLAY));
    if (bar_) tick_bar_(player);

    plugin_->cover().request(now.image_url, side_);
    paint_cover_();
    if (tint_) paint_tint_();
  }

  void on_theme() override {
    ui::set_color(title_, theme::INK);
    ui::set_color(artist_, theme::MUTED);
    ui::set_color(elapsed_, theme::SUBTLE);
    ui::set_color(total_, theme::SUBTLE);
    ui::set_color(note_, theme::MUTED);
    if (bar_) theme_bar_();
    if (tint_) paint_tint_();
  }

  // The cover and the keys take their own taps; the rest of the card does nothing, so a stray touch changes nothing.
  void on_tap() override {}

 protected:
  void paint_cover_() {
    CoverLoader &cover = plugin_->cover();
    if (cover.revision() == cover_revision_) return;
    cover_revision_ = cover.revision();
    const lv_image_dsc_t *picture = cover.picture();
    if (picture == nullptr) {
      lv_obj_add_flag(image_, LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(note_, LV_OBJ_FLAG_HIDDEN);
      return;
    }
    lv_image_set_src(image_, picture);
    lv_obj_clear_flag(image_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(note_, LV_OBJ_FLAG_HIDDEN);
  }

  // The card in the cover's colours, top lighter than the foot, with light words. Tessera colours its cards again when
  // it draws them: every tick puts the cover's colours back (only when they differ).
  void paint_tint_() {
    const uint32_t tone = plugin_->cover().tone();
    if (tone != 0) tone_ = tone;
    if (tone_ == 0) return;
    paint_ground_();
    text_color(title_, 0xF4F1EC);
    text_color(artist_, 0xE6E1D9);
    text_color(elapsed_, 0xD9D4CC);
    text_color(total_, 0xD9D4CC);
    if (lv_obj_get_style_bg_opa(track_, LV_PART_MAIN) != 70) {
      lv_obj_set_style_bg_color(track_, lv_color_hex(0xFFFFFF), 0);
      lv_obj_set_style_bg_opa(track_, 70, 0);
      lv_obj_set_style_bg_color(fill_, lv_color_hex(0xFFFFFF), 0);
      for (lv_obj_t *key : {prev_, play_, next_, queue_, pill_, volume_}) {
        if (key == nullptr) continue;
        lv_obj_set_style_bg_color(key, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(key, key == play_ ? LV_OPA_COVER : 50, 0);
        text_color(glyph_of(key), key == play_ ? 0x1A1208 : 0xFFFFFF);
      }
      if (room_ != nullptr) text_color(room_, 0xFFFFFF);
      if (slider_ != nullptr) {
        lv_obj_set_style_bg_color(slider_, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(slider_, 70, LV_PART_MAIN);
        lv_obj_set_style_bg_color(slider_, lv_color_hex(0xFFFFFF), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(slider_, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
      }
    }
  }

  enum Key : int { KEY_PREV = 1, KEY_PLAY, KEY_NEXT, KEY_COVER, KEY_QUEUE, KEY_ROOMS, KEY_VOLUME };

  // The words and keys under the cover, their height; the keys take what the card has left, a finger's size at most.
  int layout_() {
    int text_h = (show_title_ ? ui::line_height(title_font_) : 0) + ui::line_height(Font::BODY_LARGE) + ui::px(14) +
                 ui::line_height(Font::BODY);
    bar_h_ = bar_ ? ui::mm(7) : 0;
    if (bar_) text_h += bar_h_ + ui::px(10);
    key_h_ = keys_ ? std::max(ui::mm(6), std::min(ui::mm(9), height_ - text_h - ui::px(10))) : 0;
    if (keys_) text_h += key_h_ + ui::px(10);
    return text_h;
  }

  // Beside the cover the words must fit the height; under it the cover keeps a third of the tile at least.
  bool bar_fits_(int text_h) const {
    const int text_w = side_by_side_ ? width_ - height_ - ui::px(16) : width_;
    if (text_w < 2 * bar_h_ + ui::mm(20)) return false;
    return side_by_side_ ? text_h <= height_ : height_ - text_h - ui::px(10) >= height_ / 3;
  }

  // As Sonos has it: the queue on the left, the room as a pill in the middle (the rooms screen), the volume on the
  // right, which turns the row into a slider for a few seconds.
  void build_bar_(lv_obj_t *parent, int x, int y, int w) {
    const lv_font_t *icons = plugin_->icon_font(bar_h_ >= ui::mm(8));
    queue_ = bar_key_(parent, KEY_QUEUE, GLYPH_QUEUE, icons, x, y, bar_h_);
    volume_ = bar_key_(parent, KEY_VOLUME, GLYPH_VOLUME, icons, x + w - bar_h_, y, bar_h_);
    pill_x_ = x + bar_h_ + ui::px(12);
    pill_max_ = w - 2 * (bar_h_ + ui::px(12));
    pill_ = bar_key_(parent, KEY_ROOMS, GLYPH_SPEAKER, icons, pill_x_, y, bar_h_);
    lv_obj_set_style_radius(pill_, bar_h_ / 2, 0);
    lv_obj_align(glyph_of(pill_), LV_ALIGN_LEFT_MID, ui::px(14), 0);
    room_x_ = ui::px(14) + lv_font_get_line_height(icons) + ui::px(8);
    room_ = ui::label(pill_, Font::BODY_LARGE, theme::INK);
    lv_obj_align(room_, LV_ALIGN_LEFT_MID, room_x_, 0);

    slider_ = lv_slider_create(parent);
    lv_obj_set_pos(slider_, pill_x_, y + bar_h_ / 2 - ui::px(5));
    lv_obj_set_size(slider_, x + w - bar_h_ - ui::px(12) - pill_x_ - ui::px(8), ui::px(10));
    lv_slider_set_range(slider_, 0, 100);
    lv_obj_set_style_pad_all(slider_, ui::px(6), LV_PART_KNOB);
    lv_obj_set_style_radius(slider_, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(slider_, 0, LV_PART_KNOB);
    lv_obj_set_ext_click_area(slider_, bar_h_ / 2);
    lv_obj_add_event_cb(slider_, on_slider_, LV_EVENT_VALUE_CHANGED, this);
    lv_obj_add_event_cb(slider_, on_slider_, LV_EVENT_RELEASED, this);
    lv_obj_add_flag(slider_, LV_OBJ_FLAG_HIDDEN);
    theme_bar_();
  }

  lv_obj_t *bar_key_(lv_obj_t *parent, int code, const char *glyph, const lv_font_t *font, int x, int y, int side) {
    lv_obj_t *key = key_(parent, code, ICON_PLAY, x, y, side, theme::TRACK);
    lv_obj_set_style_text_font(glyph_of(key), font, 0);
    lv_label_set_text(glyph_of(key), glyph);
    lv_obj_center(glyph_of(key));
    return key;
  }

  void theme_bar_() {
    for (lv_obj_t *key : {queue_, pill_, volume_}) ui::set_color(glyph_of(key), theme::INK);
    ui::set_color(room_, theme::INK);
    lv_obj_set_style_bg_color(slider_, ui::color(theme::TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider_, ui::color(theme::ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider_, ui::color(theme::INK), LV_PART_KNOB);
  }

  void tick_bar_(const music_assistant::MAPlayer *player) {
    const std::string room = player != nullptr ? player->display_name : "";
    if (room != room_text_) {
      room_text_ = room;
      ui::set_text(room_, room);
      // The pill as wide as its words, never wider than the room between the keys.
      const int text_w = ui::text_width(room, Font::BODY_LARGE) + ui::px(4);
      const int pill_w = std::min(pill_max_, std::max(bar_h_, room_x_ + text_w + ui::px(18)));
      lv_obj_set_width(room_, std::max(0, pill_w - room_x_ - ui::px(18)));
      lv_obj_set_width(pill_, pill_w);
      lv_obj_set_x(pill_, pill_x_ + (pill_max_ - pill_w) / 2);
    }
    if (!volume_open_) return;
    const bool dragging = lv_obj_has_state(slider_, LV_STATE_PRESSED);
    if (!dragging && player != nullptr && player->volume_level >= 0 &&
        lv_slider_get_value(slider_) != player->volume_level && millis() - volume_ms_ > 1500)
      lv_slider_set_value(slider_, player->volume_level, LV_ANIM_OFF);
    if (!dragging && millis() - volume_ms_ > VOLUME_OPEN_MS) show_volume_(false);
  }

  void show_volume_(bool open) {
    volume_open_ = open;
    volume_ms_ = millis();
    for (lv_obj_t *obj : {queue_, pill_}) (open ? lv_obj_add_flag : lv_obj_clear_flag)(obj, LV_OBJ_FLAG_HIDDEN);
    (open ? lv_obj_clear_flag : lv_obj_add_flag)(slider_, LV_OBJ_FLAG_HIDDEN);
    if (!open) return;
    auto *ma = plugin_->ma();
    const auto *player = ma != nullptr ? ma->player(ma->selected_player()) : nullptr;
    if (player != nullptr && player->volume_level >= 0) lv_slider_set_value(slider_, player->volume_level, LV_ANIM_OFF);
  }

  static void on_slider_(lv_event_t *e) {
    auto *tile = static_cast<NowTile *>(lv_event_get_user_data(e));
    tile->volume_ms_ = millis();
    if (lv_event_get_code(e) != LV_EVENT_RELEASED) return;
    auto *ma = tile->plugin_->ma();
    if (ma != nullptr && ma->is_connected()) ma->cmd_set_volume(lv_slider_get_value(tile->slider_));
  }

  // Previous, play or pause, next: round keys under the words, play the largest, in the middle of the column.
  void build_keys_(lv_obj_t *parent, int x, int y, int w) {
    const int small = key_h_ * 4 / 5, gap = std::max(ui::px(10), std::min(ui::px(28), (w - key_h_ - 2 * small) / 4));
    int at = x + (w - (key_h_ + 2 * small + 2 * gap)) / 2;
    prev_ = key_(parent, KEY_PREV, ICON_PREV, at, y + (key_h_ - small) / 2, small, theme::TRACK);
    at += small + gap;
    play_ = key_(parent, KEY_PLAY, ICON_PLAY, at, y, key_h_, theme::ACCENT);
    ui::set_color(glyph_of(play_), theme::ON_ACCENT);
    at += key_h_ + gap;
    next_ = key_(parent, KEY_NEXT, ICON_NEXT, at, y + (key_h_ - small) / 2, small, theme::TRACK);
  }

  lv_obj_t *key_(lv_obj_t *parent, int code, uint32_t icon, int x, int y, int side, theme::Role fill) {
    lv_obj_t *key = ui::block(parent, fill);
    lv_obj_set_pos(key, x, y);
    lv_obj_set_size(key, side, side);
    lv_obj_set_style_radius(key, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_flag(key, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(key, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_set_user_data(key, reinterpret_cast<void *>(static_cast<intptr_t>(code)));
    lv_obj_add_event_cb(key, on_key_, LV_EVENT_CLICKED, this);
    lv_obj_t *glyph = ui::label(key, side >= ui::mm(8) ? Font::ICON : Font::ICON_SMALL, theme::INK);
    ui::set_text(glyph, ui::icon(icon));
    lv_obj_center(glyph);
    return key;
  }

  static lv_obj_t *glyph_of(lv_obj_t *key) { return lv_obj_get_child(key, 0); }

  static void on_key_(lv_event_t *e) {
    auto *tile = static_cast<NowTile *>(lv_event_get_user_data(e));
    auto *target = static_cast<lv_obj_t *>(lv_event_get_current_target(e));
    tile->press_(static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(target))));
  }

  void press_(int code) {
    if (code == KEY_VOLUME) {
      show_volume_(!volume_open_);
      return;
    }
    if (code == KEY_COVER || code == KEY_QUEUE || code == KEY_ROOMS) {
      plugin_->open_dashboard_at(code == KEY_QUEUE ? 3 : code == KEY_ROOMS ? 2 : 1);
      tessera::open_card(plugin_->plugin_id(), "player", "", tile_);
      return;
    }
    auto *ma = plugin_->ma();
    if (ma == nullptr || !ma->is_connected()) return;
    if (code == KEY_PREV) ma->cmd_previous();
    if (code == KEY_PLAY) ma->cmd_play_pause();
    if (code == KEY_NEXT) ma->cmd_next();
  }

  // The dithered ground as the card's background picture, drawn again for a new colour or size. Tessera colours its
  // cards again when it draws them: every tick puts the picture back (only when it is not there).
  void paint_ground_() {
    const int w = lv_obj_get_width(card_), h = lv_obj_get_height(card_);
    if (w <= 0 || h <= 0) return;
    if (tone_ != ground_tone_ || w != ground_.header.w || h != ground_.header.h) {
      const size_t bytes = (size_t) w * h * 2;
      if (bytes > ground_cap_) {
        lv_obj_set_style_bg_image_src(card_, nullptr, 0);
        heap_caps_free(ground_px_);
        ground_px_ = static_cast<uint8_t *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM));
        ground_cap_ = ground_px_ == nullptr ? 0 : bytes;
        if (ground_px_ == nullptr) return;
      }
      paint_ground(reinterpret_cast<uint16_t *>(ground_px_), w, h, lit(tone_, 140), lit(tone_, 60));
      lv_image_cache_drop(&ground_);
      ground_.header.magic = LV_IMAGE_HEADER_MAGIC;
      ground_.header.cf = LV_COLOR_FORMAT_RGB565;
      ground_.header.w = w;
      ground_.header.h = h;
      ground_.header.stride = w * 2;
      ground_.data = ground_px_;
      ground_.data_size = bytes;
      ground_tone_ = tone_;
      lv_obj_set_style_bg_image_src(card_, nullptr, 0);
      lv_obj_invalidate(card_);
    }
    if (lv_obj_get_style_bg_image_src(card_, LV_PART_MAIN) != &ground_)
      lv_obj_set_style_bg_image_src(card_, &ground_, 0);
    if (lv_obj_get_style_bg_grad_dir(card_, LV_PART_MAIN) != LV_GRAD_DIR_NONE)
      lv_obj_set_style_bg_grad_dir(card_, LV_GRAD_DIR_NONE, 0);
  }

  TesseraMusic *plugin_;
  std::string player_;
  lv_obj_t *card_{};
  bool tint_{false};
  uint32_t tone_{0}, ground_tone_{0};
  lv_image_dsc_t ground_{};
  uint8_t *ground_px_{nullptr};
  size_t ground_cap_{0};
  bool show_title_{true}, side_by_side_{false};
  Font title_font_{Font::HEADLINE};
  int width_{0}, height_{0}, side_{0}, text_w_{0}, tile_{-1};
  uint32_t cover_revision_{0};
  lv_obj_t *art_{}, *note_{}, *image_{}, *title_{}, *artist_{}, *track_{}, *fill_{}, *elapsed_{}, *total_{};
  lv_obj_t *prev_{}, *play_{}, *next_{};
  bool keys_{true};
  int key_h_{0};
  lv_obj_t *queue_{}, *pill_{}, *volume_{}, *room_{}, *slider_{};
  bool bar_{true}, volume_open_{false};
  int bar_h_{0}, pill_x_{0}, pill_max_{0}, room_x_{0};
  uint32_t volume_ms_{0};
  std::string room_text_;
};

// The card a tap on the tile opens. It draws nothing itself: the dashboard lies over the whole glass, and the card
// is what Tessera closes on standby, an alert or Back to page 1, as it closes its own.
class DashboardCard : public tessera::Card {
 public:
  explicit DashboardCard(TesseraMusic *plugin) : plugin_(plugin) {}
  ~DashboardCard() override {
    plugin_->dashboard_closed();
    music_assistant::ma_ui::close();
  }
  void open(const tessera::CardContext &) override { plugin_->open_dashboard(); }
  void on_tick(uint32_t) override { plugin_->seen(); }

 protected:
  TesseraMusic *plugin_;
};

void TesseraMusic::setup() {
  add_tile("now_playing", [this]() { return new NowTile(this); });
  add_card("player", [this]() { return new DashboardCard(this); }, true);
}

void TesseraMusic::follow_room(const std::string &name) {
  if (!dashboard_open_ && ma_ != nullptr) ma_->pin_player(name);
}

bool TesseraMusic::build_dashboard_() {
  if (built_) return true;
  for (font::Font *font : fonts_)
    if (font == nullptr || ma_ == nullptr) return false;
  // The time for the dashboard's clock, as espcontrol's display_time label was.
  clock_ = lv_label_create(lv_layer_top());
  lv_obj_add_flag(clock_, LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text(clock_, "--:--");
  const music_assistant::ma_skeleton::Fonts fonts{
      fonts_[0]->get_lv_font(), fonts_[1]->get_lv_font(), fonts_[2]->get_lv_font(), fonts_[3]->get_lv_font(),
      fonts_[4]->get_lv_font(), fonts_[5]->get_lv_font(), fonts_[6]->get_lv_font(), fonts_[7]->get_lv_font(),
      fonts_[8]->get_lv_font(), fonts_[9]->get_lv_font(), fonts_[10]->get_lv_font()};
  const auto handles = music_assistant::ma_skeleton::build(lv_layer_top(), fonts, clock_);
  music_assistant::ma_ui::bind(
      ma_, handles, []() {},
      // the dashboard closed itself (its back key, two minutes untouched): the card goes with it
      [this]() {
        dashboard_open_ = false;
        tessera::close_card();
      },
      []() {});
  built_ = true;
  return true;
}

void TesseraMusic::open_dashboard() {
  if (!build_dashboard_()) {
    tessera::close_card();
    return;
  }
  // The rooms screen chooses the room now: the tile's room stays chosen for a while, then the dashboard follows
  // what plays, as it always did.
  const int room = ma_->selected_player();
  ma_->pin_player("");
  if (room >= 0 && room < (int) ma_->player_count()) ma_->select_player_user(room);
  dashboard_open_ = true;
  seen();
  music_assistant::ma_ui::open(start_screen_);
  start_screen_ = 1;
}

void TesseraMusic::loop() {
  const bool active = seen_ms_ != 0 && millis() - seen_ms_ < ACTIVE_MS;
  if (ma_ != nullptr && !dashboard_open_) ma_->set_ui_active(active);
  if (active) cover_.poll();
  if (!dashboard_open_) return;
  artwork_image::ArtworkImage::poll_all();
  if (millis() - clock_ms_ >= 1000) {
    clock_ms_ = millis();
    const uint32_t now = tessera::epoch();
    if (now != 0) lv_label_set_text(clock_, tessera::clock_text(now).c_str());
  }
  music_assistant::ma_ui::refresh();
}

}  // namespace esphome::tessera_music
