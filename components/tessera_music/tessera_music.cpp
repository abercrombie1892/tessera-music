#include "tessera_music.h"
#include <algorithm>
#include <string>
#include "esphome/components/music_assistant/ma_artwork.h"
#include "esphome/components/music_assistant/ma_skeleton.h"

namespace esphome::tessera_music {

using tessera::Font;
namespace ui = tessera::ui;

static constexpr uint32_t ACTIVE_MS = 3000;

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

static void text_color(lv_obj_t *label, uint32_t color) {
  if (!lv_color_eq(lv_obj_get_style_text_color(label, LV_PART_MAIN), lv_color_hex(color)))
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
}

// Now playing: the cover, the title, the artist and how far the track is, for one room or whatever plays.
class NowTile : public tessera::Tile {
 public:
  explicit NowTile(TesseraMusic *plugin) : plugin_(plugin) {}

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

    const int text_h = (show_title_ ? ui::line_height(title_font_) : 0) + ui::line_height(Font::BODY_LARGE) +
                       ui::px(14) + ui::line_height(Font::BODY);
    side_by_side_ = width_ >= height_ * 13 / 10;
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
    if (tint_) paint_tint_();
  }

  void on_tap() override { tessera::open_card(plugin_->plugin_id(), "player", "", tile_); }

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
    const lv_color_t top = lit(tone_, 140), foot = lit(tone_, 70);
    if (!lv_color_eq(lv_obj_get_style_bg_color(card_, LV_PART_MAIN), top)) lv_obj_set_style_bg_color(card_, top, 0);
    if (!lv_color_eq(lv_obj_get_style_bg_grad_color(card_, LV_PART_MAIN), foot))
      lv_obj_set_style_bg_grad_color(card_, foot, 0);
    if (lv_obj_get_style_bg_grad_dir(card_, LV_PART_MAIN) != LV_GRAD_DIR_VER)
      lv_obj_set_style_bg_grad_dir(card_, LV_GRAD_DIR_VER, 0);
    text_color(title_, 0xF4F1EC);
    text_color(artist_, 0xE6E1D9);
    text_color(elapsed_, 0xD9D4CC);
    text_color(total_, 0xD9D4CC);
    if (lv_obj_get_style_bg_opa(track_, LV_PART_MAIN) != 70) {
      lv_obj_set_style_bg_color(track_, lv_color_hex(0xFFFFFF), 0);
      lv_obj_set_style_bg_opa(track_, 70, 0);
      lv_obj_set_style_bg_color(fill_, lv_color_hex(0xFFFFFF), 0);
    }
  }

  TesseraMusic *plugin_;
  std::string player_;
  lv_obj_t *card_{};
  bool tint_{false};
  uint32_t tone_{0};
  bool show_title_{true}, side_by_side_{false};
  Font title_font_{Font::HEADLINE};
  int width_{0}, height_{0}, side_{0}, text_w_{0}, tile_{-1};
  uint32_t cover_revision_{0};
  lv_obj_t *art_{}, *note_{}, *image_{}, *title_{}, *artist_{}, *track_{}, *fill_{}, *elapsed_{}, *total_{};
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
      fonts_[8]->get_lv_font(), fonts_[9]->get_lv_font()};
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
  music_assistant::ma_ui::open();
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
