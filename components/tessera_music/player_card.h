#pragma once
#include <string>
#include <utility>
#include <vector>
#include "tessera_music.h"

namespace esphome::tessera_music {

// The player, over the whole glass: what plays with its cover and every key, the rooms with their volume and groups,
// the queue, and the library (playlists, radio, favourites). A tap on the Now playing tile opens it.
class PlayerCard : public tessera::Card {
 public:
  explicit PlayerCard(TesseraMusic *plugin) : plugin_(plugin) {}
  ~PlayerCard() override;
  void open(const tessera::CardContext &c) override;
  void on_tick(uint32_t epoch) override;
  void on_theme() override;
  bool on_back() override;

 protected:
  enum View : uint8_t { NOW, ROOMS, QUEUE, LIBRARY, VIEWS };
  static constexpr int SHELVES = 3;
  static constexpr int MAX_ROOMS = 16;

  struct Row {
    lv_obj_t *root{}, *art{}, *image{}, *note{}, *title{}, *sub{}, *side{};
    uint32_t cover_revision{0};
  };
  struct Room {
    lv_obj_t *root{}, *name{}, *sub{}, *play{}, *play_icon{}, *group{}, *group_icon{}, *volume{};
  };

  static void on_event_(lv_event_t *e);
  void handle_(int code, lv_event_code_t event, lv_obj_t *target);
  void tap_(int code);
  void slide_(int code, lv_obj_t *slider, bool released);
  void seek_();
  void show_(View view);
  void choose_room_(int index);
  void toggle_member_(int index);

  lv_obj_t *fill_(lv_obj_t *parent, theme::Role role);
  lv_obj_t *text_(lv_obj_t *parent, tessera::Font font, theme::Role role);
  lv_obj_t *key_(lv_obj_t *parent, int code, theme::Role fill, int side, uint32_t icon, tessera::Font font);
  lv_obj_t *slider_(lv_obj_t *parent, int code);
  lv_obj_t *list_box_(lv_obj_t *parent);
  void clickable_(lv_obj_t *obj, int code, bool feedback = true);

  void build_tabs_(lv_obj_t *parent, int width, int height);
  void build_now_(int width, int height);
  void build_now_top_(int width, int top_h);
  void build_now_keys_(int width, int y);
  void build_volume_(int width, int y, int height);
  void build_rooms_(int width, int height);
  void build_lists_(int width, int height);
  void build_row_(int index, int width);

  void refresh_();
  void paint_tabs_();
  void paint_now_();
  void paint_now_keys_(const music_assistant::MAPlayer *player);
  void paint_cover_();
  void paint_rooms_();
  void paint_list_();
  void paint_row_(int index, const std::string &title, const std::string &sub, const std::string &side,
                  const std::string &image, bool current);
  void paint_more_(bool more, bool loading, int rows);

  TesseraMusic *plugin_;
  View view_{NOW};
  int shelf_{0};
  int width_{0}, cover_side_{0}, thumb_side_{0}, row_step_{0}, bar_w_{0};
  uint32_t cover_revision_{0}, list_revision_{0}, hold_volume_until_{0}, last_volume_ms_{0};
  int list_view_{-1}, list_shelf_{-1};
  lv_obj_t *dragging_{nullptr};
  std::vector<std::pair<lv_obj_t *, theme::Role>> fills_, inks_;

  lv_obj_t *tabs_[VIEWS]{}, *tab_text_[VIEWS]{}, *views_[VIEWS]{};
  lv_obj_t *art_{}, *image_{}, *note_{}, *room_{}, *title_{}, *artist_{}, *album_{}, *favorite_{};
  lv_obj_t *track_{}, *fill_bar_{}, *seek_zone_{}, *elapsed_{}, *total_{};
  lv_obj_t *shuffle_{}, *prev_{}, *play_{}, *next_{}, *repeat_{};
  lv_obj_t *mute_{}, *volume_{}, *percent_{};
  lv_obj_t *shelves_[SHELVES]{}, *shelf_text_[SHELVES]{}, *list_{}, *more_{}, *more_text_{}, *empty_{};
  lv_obj_t *rooms_list_{};
  Row rows_[TesseraMusic::MAX_ROWS];
  Room rooms_[MAX_ROOMS];
};

}  // namespace esphome::tessera_music
