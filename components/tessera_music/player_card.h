#pragma once
#include <string>
#include "tessera_music.h"

namespace esphome::tessera_music {

// The player over the whole glass, in the look of the espcontrol MA dashboard: a dark ground lit by the cover's
// colour, glass keys, the orange play key, the track's waveform as its progress. Now playing first; the room pill and
// the grid key open the rooms, the "next up" box the queue and the library. Back goes to Now playing, then closes.
class PlayerCard : public tessera::Card {
 public:
  explicit PlayerCard(TesseraMusic *plugin) : plugin_(plugin) {}
  ~PlayerCard() override;
  void open(const tessera::CardContext &c) override;
  void on_tick(uint32_t epoch) override;
  bool on_back() override;

 protected:
  enum View : uint8_t { NOW, ROOMS, LISTS, VIEWS };
  static constexpr int SHELVES = 4;  // the queue, playlists, radio, favourites
  static constexpr int MAX_ROOMS = 16;
  static constexpr int NEXT_ROWS = 2;

  struct Row {
    lv_obj_t *root{}, *art{}, *image{}, *note{}, *title{}, *sub{}, *side{};
    uint32_t cover_revision{0};
  };
  struct Room {
    lv_obj_t *root{}, *name{}, *sub{}, *play{}, *group{}, *volume{};
  };

  int s_(int base) const { return base * scale_ / 1000; }
  static void on_event_(lv_event_t *e);
  void handle_(int code, lv_event_code_t event, lv_obj_t *target);
  void tap_(int code);
  void tap_list_row_(int row);
  void slide_(int code, lv_obj_t *slider, bool released);
  void seek_();
  void show_(View view);
  void choose_shelf_(int shelf);
  void choose_room_(int index);
  void toggle_member_(int index);

  lv_obj_t *box_(lv_obj_t *parent, int x, int y, int w, int h, int radius);
  lv_obj_t *glass_(lv_obj_t *parent, int x, int y, int w, int h, int radius, int code);
  lv_obj_t *words_(lv_obj_t *parent, tessera::Font font, uint32_t color, int x, int y, int w);
  lv_obj_t *glyph_(lv_obj_t *parent, uint32_t icon, tessera::Font font, uint32_t color);
  lv_obj_t *slider_(lv_obj_t *parent, int code);
  void clickable_(lv_obj_t *obj, int code);

  void build_now_(int width, int height);
  void build_pill_(lv_obj_t *view, int width);
  void build_column_(lv_obj_t *view, int x, int y, int w, int h);
  void build_next_up_(lv_obj_t *view, int x, int y, int w, int h);
  void build_progress_(lv_obj_t *view, int width, int y);
  void build_keys_(lv_obj_t *view, int width, int y);
  void build_volume_(lv_obj_t *view, int width, int y);
  void build_rooms_(int width, int height);
  void build_room_(int index, int width, int y, int h);
  void build_lists_(int width, int height);
  void build_row_(int index, int width);

  void refresh_();
  void paint_glow_();
  void paint_now_();
  void paint_now_keys_(const music_assistant::MAPlayer *player);
  void paint_wave_(uint32_t elapsed, uint32_t duration);
  void paint_next_up_();
  void paint_cover_();
  void paint_rooms_();
  void paint_list_();
  void paint_row_(int index, const std::string &title, const std::string &sub, const std::string &side,
                  const std::string &image, bool current);
  void paint_more_(bool more, bool loading, int rows);
  void paint_thumbs_(int rows, int first_thumb);

  TesseraMusic *plugin_;
  View view_{NOW};
  int shelf_{0};
  int scale_{1000}, cover_side_{0}, thumb_side_{0}, row_step_{0}, wave_w_{0}, wave_h_{0}, next_thumb_{0};
  uint32_t cover_revision_{0}, list_revision_{0}, glow_{1}, hold_volume_until_{0}, last_volume_ms_{0};
  int list_view_{-1}, list_shelf_{-1}, wave_drawn_{-1}, wave_played_{-1};
  std::string wave_uri_;
  lv_obj_t *dragging_{nullptr};
  uint8_t *wave_pixels_{nullptr};

  lv_obj_t *ground_{}, *views_[VIEWS]{};
  lv_obj_t *pill_{}, *pill_dot_{}, *room_{}, *badge_{}, *badge_text_{}, *grid_{}, *favorite_{};
  lv_obj_t *art_{}, *image_{}, *note_{}, *status_dot_{}, *status_{}, *title_{}, *artist_{}, *album_{};
  lv_obj_t *next_box_{}, *next_count_{}, *next_rows_[NEXT_ROWS]{}, *next_art_[NEXT_ROWS]{}, *next_image_[NEXT_ROWS]{},
      *next_title_[NEXT_ROWS]{}, *next_artist_[NEXT_ROWS]{};
  uint32_t next_revision_[NEXT_ROWS]{};
  lv_obj_t *wave_{}, *track_{}, *fill_bar_{}, *seek_zone_{}, *elapsed_{}, *remaining_{};
  lv_obj_t *shuffle_{}, *prev_{}, *play_{}, *next_{}, *repeat_{};
  lv_obj_t *mute_{}, *volume_{}, *percent_{};
  lv_obj_t *segments_{}, *shelves_[SHELVES]{}, *shelf_text_[SHELVES]{}, *list_{}, *more_{}, *more_text_{}, *empty_{};
  lv_obj_t *rooms_list_{};
  Row rows_[TesseraMusic::MAX_ROWS];
  Room rooms_[MAX_ROOMS];
};

}  // namespace esphome::tessera_music
