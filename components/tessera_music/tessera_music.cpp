#include "tessera_music.h"
#include <algorithm>
#include <new>
#include <string>
#include "esp_heap_caps.h"
#include "player_card.h"

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

// Now playing: the cover, the title, the artist and how far the track is, for one room or whatever plays.
class NowTile : public tessera::Tile {
 public:
  explicit NowTile(TesseraMusic *plugin) : plugin_(plugin) {}

  void create(const tessera::TileContext &c) override {
    width_ = c.width;
    height_ = c.height;
    tile_ = c.tile;
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
    ui::set_color(artist_, playing ? theme::MUTED : theme::SUBTLE);

    plugin_->cover().request(now.image_url, side_);
    paint_cover_();
  }

  void on_theme() override {
    ui::set_color(title_, theme::INK);
    ui::set_color(artist_, theme::MUTED);
    ui::set_color(elapsed_, theme::SUBTLE);
    ui::set_color(total_, theme::SUBTLE);
    ui::set_color(note_, theme::MUTED);
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

  TesseraMusic *plugin_;
  std::string player_;
  bool show_title_{true}, side_by_side_{false};
  Font title_font_{Font::HEADLINE};
  int width_{0}, height_{0}, side_{0}, text_w_{0}, tile_{-1};
  uint32_t cover_revision_{0};
  lv_obj_t *art_{}, *note_{}, *image_{}, *title_{}, *artist_{}, *track_{}, *fill_{}, *elapsed_{}, *total_{};
};

void TesseraMusic::setup() {
  // The small covers of the card's lists live in PSRAM, beside their pixels.
  void *room = heap_caps_calloc(MAX_ROWS, sizeof(CoverLoader), MALLOC_CAP_SPIRAM);
  if (room != nullptr) {
    thumbs_ = static_cast<CoverLoader *>(room);
    for (int i = 0; i < MAX_ROWS; ++i) new (&thumbs_[i]) CoverLoader();
  }
  add_tile("now_playing", [this]() { return new NowTile(this); });
  add_card("player", [this]() { return new PlayerCard(this); }, true);
}

CoverLoader &TesseraMusic::thumb(int row) {
  static CoverLoader none;
  return thumbs_ != nullptr && row >= 0 && row < MAX_ROWS ? thumbs_[row] : none;
}

void TesseraMusic::follow_room(const std::string &name) {
  if (!card_open_ && ma_ != nullptr) ma_->pin_player(name);
}

void TesseraMusic::card_closed() {
  card_open_ = false;
  card_cover_.release();
  if (thumbs_ != nullptr)
    for (int i = 0; i < MAX_ROWS; ++i) thumbs_[i].release();
  if (ma_ != nullptr) ma_->release_lists();
}

void TesseraMusic::loop() {
  const bool active = seen_ms_ != 0 && millis() - seen_ms_ < ACTIVE_MS;
  if (ma_ != nullptr) ma_->set_ui_active(active);
  if (!active) return;
  card_cover_.poll();
  cover_.poll();
  if (!card_open_ || thumbs_ == nullptr) return;
  for (int i = 0; i < MAX_ROWS; ++i) thumbs_[i].poll();
}

}  // namespace esphome::tessera_music
