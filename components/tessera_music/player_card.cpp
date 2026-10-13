#include "player_card.h"
#include <algorithm>
#include <cstdio>

namespace esphome::tessera_music {

using tessera::Font;
using music_assistant::MALibrary;
using music_assistant::MAPlayer;
namespace ui = tessera::ui;

// What a tap or a slider means: a code in the object's user data.
enum Code : int {
  TAB = 1,  // + the view
  PREV = 10, PLAY, NEXT, SHUFFLE, REPEAT, FAVORITE, MUTE, VOLUME, SEEK,
  SHELF = 30,  // + the shelf
  MORE = 40,
  ROW = 100,  // + the row
  ROOM = 200, ROOM_PLAY = 300, ROOM_GROUP = 400, ROOM_VOLUME = 500,  // + the room
};

static constexpr uint32_t ICON_MUSIC = 0xF075A, ICON_PLAY = 0xF040A, ICON_PAUSE = 0xF03E4, ICON_NEXT = 0xF04AD,
                          ICON_PREV = 0xF04AE, ICON_SHUFFLE = 0xF049F, ICON_REPEAT = 0xF0456, ICON_REPEAT_ONE = 0xF0458,
                          ICON_VOLUME = 0xF057E, ICON_MUTED = 0xF0581, ICON_HEART = 0xF02D1, ICON_SPEAKER = 0xF04C3,
                          ICON_GROUP = 0xF0D38;
static constexpr int QUEUE_PAGE = 20;
static constexpr uint32_t VOLUME_EVERY_MS = 250, VOLUME_HOLD_MS = 2000;
static const char *const TAB_KEYS[] = {"tab_now", "tab_rooms", "tab_queue", "tab_library"};
static const char *const SHELF_KEYS[] = {"shelf_playlists", "shelf_radio", "shelf_favorites"};
static const MALibrary SHELF_KINDS[] = {MALibrary::PLAYLISTS, MALibrary::RADIOS, MALibrary::FAVORITES};

static std::string clock_of(uint32_t seconds) {
  char text[16];
  if (seconds >= 3600)
    snprintf(text, sizeof(text), "%u:%02u:%02u", (unsigned) (seconds / 3600), (unsigned) (seconds / 60 % 60),
             (unsigned) (seconds % 60));
  else
    snprintf(text, sizeof(text), "%u:%02u", (unsigned) (seconds / 60), (unsigned) (seconds % 60));
  return text;
}

static void hide(lv_obj_t *obj, bool hidden) {
  if (hidden == lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return;
  if (hidden)
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
  else
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static void set_fill(lv_obj_t *obj, theme::Role role) {
  const lv_color_t color = ui::color(role);
  if (!lv_color_eq(lv_obj_get_style_bg_color(obj, LV_PART_MAIN), color)) lv_obj_set_style_bg_color(obj, color, 0);
}

static lv_obj_t *bare(lv_obj_t *parent) {
  lv_obj_t *obj = lv_obj_create(parent);
  lv_obj_remove_style_all(obj);
  lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
  return obj;
}

static const MAPlayer *leader_of(music_assistant::MusicAssistant *ma, const MAPlayer *player) {
  if (player == nullptr || player->synced_to.empty()) return player;
  const MAPlayer *leader = ma->player(ma->player_index(player->synced_to));
  return leader != nullptr ? leader : player;
}

static bool member_of(const MAPlayer *leader, const std::string &id) {
  return leader != nullptr && std::find(leader->group_members.begin(), leader->group_members.end(), id) !=
                                  leader->group_members.end();
}

// ---- making

lv_obj_t *PlayerCard::fill_(lv_obj_t *parent, theme::Role role) {
  lv_obj_t *obj = ui::block(parent, role);
  lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
  fills_.push_back({obj, role});
  return obj;
}

lv_obj_t *PlayerCard::text_(lv_obj_t *parent, Font font, theme::Role role) {
  lv_obj_t *label = ui::label(parent, font, role);
  inks_.push_back({label, role});
  return label;
}

void PlayerCard::clickable_(lv_obj_t *obj, int code, bool feedback) {
  lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_user_data(obj, reinterpret_cast<void *>(static_cast<intptr_t>(code)));
  lv_obj_add_event_cb(obj, on_event_, LV_EVENT_CLICKED, this);
  if (feedback) lv_obj_set_style_bg_opa(obj, LV_OPA_60, LV_STATE_PRESSED);
}

lv_obj_t *PlayerCard::key_(lv_obj_t *parent, int code, theme::Role fill, int side, uint32_t icon, Font font) {
  lv_obj_t *key = fill_(parent, fill);
  lv_obj_set_size(key, side, side);
  lv_obj_set_style_radius(key, LV_RADIUS_CIRCLE, 0);
  clickable_(key, code);
  lv_obj_t *glyph = text_(key, font, theme::INK);
  ui::set_text(glyph, ui::icon(icon));
  lv_obj_center(glyph);
  return key;
}

static lv_obj_t *glyph_of(lv_obj_t *key) { return lv_obj_get_child(key, 0); }

lv_obj_t *PlayerCard::slider_(lv_obj_t *parent, int code) {
  lv_obj_t *slider = lv_slider_create(parent);
  lv_slider_set_range(slider, 0, 100);
  lv_obj_set_style_bg_color(slider, ui::color(theme::TRACK), LV_PART_MAIN);
  lv_obj_set_style_bg_color(slider, ui::color(theme::ACCENT), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(slider, ui::color(theme::SLIDER_KNOB), LV_PART_KNOB);
  lv_obj_set_style_pad_all(slider, ui::px(4), LV_PART_KNOB);
  lv_obj_set_user_data(slider, reinterpret_cast<void *>(static_cast<intptr_t>(code)));
  lv_obj_add_event_cb(slider, on_event_, LV_EVENT_VALUE_CHANGED, this);
  lv_obj_add_event_cb(slider, on_event_, LV_EVENT_RELEASED, this);
  lv_obj_set_ext_click_area(slider, ui::px(12));
  return slider;
}

lv_obj_t *PlayerCard::list_box_(lv_obj_t *parent) {
  lv_obj_t *list = bare(parent);
  lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
  return list;
}


void PlayerCard::open(const tessera::CardContext &c) {
  plugin_->card_opened();
  width_ = c.width;
  const int tab_h = ui::mm(8), body_y = tab_h + ui::px(14), body_h = c.height - body_y;
  build_tabs_(c.parent, c.width, tab_h);
  for (int v = NOW; v <= QUEUE; ++v) {
    views_[v] = bare(c.parent);
    lv_obj_set_pos(views_[v], 0, body_y);
    lv_obj_set_size(views_[v], c.width, body_h);
  }
  views_[LIBRARY] = views_[QUEUE];
  build_now_(c.width, body_h);
  build_rooms_(c.width, body_h);
  build_lists_(c.width, body_h);
  show_(NOW);
}

PlayerCard::~PlayerCard() { plugin_->card_closed(); }

void PlayerCard::build_tabs_(lv_obj_t *parent, int width, int height) {
  const int gap = ui::px(8), w = (width - (VIEWS - 1) * gap) / VIEWS;
  for (int v = 0; v < VIEWS; ++v) {
    tabs_[v] = fill_(parent, theme::TRACK);
    lv_obj_set_pos(tabs_[v], v * (w + gap), 0);
    lv_obj_set_size(tabs_[v], w, height);
    lv_obj_set_style_radius(tabs_[v], height / 2, 0);
    clickable_(tabs_[v], TAB + v);
    tab_text_[v] = text_(tabs_[v], Font::BODY_LARGE, theme::INK);
    lv_obj_set_size(tab_text_[v], w - ui::px(12), ui::line_height(Font::BODY_LARGE));
    lv_obj_set_style_text_align(tab_text_[v], LV_TEXT_ALIGN_CENTER, 0);
    ui::set_text(tab_text_[v], plugin_->text(TAB_KEYS[v]));
    lv_obj_center(tab_text_[v]);
  }
}

// ---- Now playing: the cover and the words on top, the bar, the keys and the volume along the bottom.

void PlayerCard::build_now_(int width, int height) {
  lv_obj_t *view = views_[NOW];
  const int gap = ui::px(16), play = ui::mm(12), volume_h = ui::mm(8);
  const int bar_row_h = ui::px(12) + ui::line_height(Font::BODY);
  const int volume_y = height - volume_h, keys_y = volume_y - gap - play, bar_y = keys_y - gap - bar_row_h;
  build_now_top_(width, bar_y - gap);

  const int row_w = std::min(width, ui::mm(130)), row_x = (width - row_w) / 2;
  bar_w_ = row_w;
  track_ = fill_(view, theme::TRACK);
  lv_obj_set_pos(track_, row_x, bar_y);
  lv_obj_set_size(track_, row_w, ui::px(6));
  fill_bar_ = fill_(track_, theme::ACCENT);
  lv_obj_set_size(fill_bar_, 0, ui::px(6));
  seek_zone_ = bare(view);
  lv_obj_set_pos(seek_zone_, row_x, bar_y - ui::mm(3));
  lv_obj_set_size(seek_zone_, row_w, ui::mm(6) + ui::px(6));
  clickable_(seek_zone_, SEEK, false);
  const int times_y = bar_y + ui::px(12), body_h = ui::line_height(Font::BODY);
  elapsed_ = text_(view, Font::BODY, theme::SUBTLE);
  lv_obj_set_pos(elapsed_, row_x, times_y);
  lv_obj_set_size(elapsed_, row_w / 2, body_h);
  total_ = text_(view, Font::BODY, theme::SUBTLE);
  lv_obj_set_pos(total_, row_x + row_w / 2, times_y);
  lv_obj_set_size(total_, row_w - row_w / 2, body_h);
  lv_obj_set_style_text_align(total_, LV_TEXT_ALIGN_RIGHT, 0);

  build_now_keys_(width, keys_y);
  build_volume_(width, volume_y, volume_h);
}

void PlayerCard::build_now_top_(int width, int top_h) {
  lv_obj_t *view = views_[NOW];
  cover_side_ = std::max(ui::mm(10), std::min(top_h, width * 45 / 100));
  art_ = fill_(view, theme::TRACK);
  lv_obj_set_size(art_, cover_side_, cover_side_);
  lv_obj_set_pos(art_, 0, std::max(0, (top_h - cover_side_) / 2));
  lv_obj_set_style_radius(art_, ui::px(16), 0);
  lv_obj_set_style_clip_corner(art_, true, 0);
  note_ = text_(art_, Font::ICON, theme::MUTED);
  ui::set_text(note_, ui::icon(ICON_MUSIC));
  lv_obj_center(note_);
  image_ = lv_image_create(art_);
  lv_obj_set_size(image_, cover_side_, cover_side_);
  lv_image_set_inner_align(image_, LV_IMAGE_ALIGN_CENTER);
  lv_obj_center(image_);
  lv_obj_add_flag(image_, LV_OBJ_FLAG_HIDDEN);

  const int x = cover_side_ + ui::px(24), w = width - x, favorite = ui::mm(8);
  const int room_h = ui::line_height(Font::BODY_LARGE), title_h = ui::line_height(Font::HEADLINE),
            artist_h = ui::line_height(Font::BODY_LARGE), album_h = ui::line_height(Font::BODY);
  int y = std::max(0, (top_h - (room_h + ui::px(8) + title_h + artist_h + album_h + ui::px(14) + favorite)) / 2);
  room_ = text_(view, Font::BODY_LARGE, theme::ACCENT);
  lv_obj_set_pos(room_, x, y);
  lv_obj_set_size(room_, w, room_h);
  clickable_(room_, TAB + static_cast<int>(ROOMS), false);
  y += room_h + ui::px(8);
  title_ = text_(view, Font::HEADLINE, theme::INK);
  lv_obj_set_pos(title_, x, y);
  lv_obj_set_size(title_, w, title_h);
  y += title_h;
  artist_ = text_(view, Font::BODY_LARGE, theme::MUTED);
  lv_obj_set_pos(artist_, x, y);
  lv_obj_set_size(artist_, w, artist_h);
  y += artist_h;
  album_ = text_(view, Font::BODY, theme::SUBTLE);
  lv_obj_set_pos(album_, x, y);
  lv_obj_set_size(album_, w, album_h);
  y += album_h + ui::px(14);
  favorite_ = key_(view, FAVORITE, theme::TRACK, favorite, ICON_HEART, Font::ICON_SMALL);
  lv_obj_set_pos(favorite_, x, y);
}

void PlayerCard::build_now_keys_(int width, int y) {
  lv_obj_t *view = views_[NOW];
  const int small = ui::mm(8), key = ui::mm(10), play = ui::mm(12), gap = ui::px(28);
  int x = (width - (2 * small + 2 * key + play + 4 * gap)) / 2;
  auto place = [&](lv_obj_t *obj, int side) {
    lv_obj_set_pos(obj, x, y + (play - side) / 2);
    x += side + gap;
    return obj;
  };
  shuffle_ = place(key_(view, SHUFFLE, theme::PAGE, small, ICON_SHUFFLE, Font::ICON_SMALL), small);
  prev_ = place(key_(view, PREV, theme::TRACK, key, ICON_PREV, Font::ICON), key);
  play_ = place(key_(view, PLAY, theme::ACCENT, play, ICON_PLAY, Font::ICON), play);
  next_ = place(key_(view, NEXT, theme::TRACK, key, ICON_NEXT, Font::ICON), key);
  repeat_ = place(key_(view, REPEAT, theme::PAGE, small, ICON_REPEAT, Font::ICON_SMALL), small);
  ui::set_color(glyph_of(play_), theme::ON_ACCENT);
  inks_.push_back({glyph_of(play_), theme::ON_ACCENT});
}

void PlayerCard::build_volume_(int width, int y, int height) {
  lv_obj_t *view = views_[NOW];
  const int row_w = std::min(width, ui::mm(130)), x = (width - row_w) / 2;
  const int percent_w = ui::text_width("100 %", Font::BODY) + ui::px(8), slider_h = ui::px(14);
  mute_ = key_(view, MUTE, theme::TRACK, height, ICON_VOLUME, Font::ICON_SMALL);
  lv_obj_set_pos(mute_, x, y);
  volume_ = slider_(view, VOLUME);
  const int slider_x = x + height + ui::px(24);
  lv_obj_set_pos(volume_, slider_x, y + (height - slider_h) / 2);
  lv_obj_set_size(volume_, x + row_w - percent_w - ui::px(16) - slider_x, slider_h);
  percent_ = text_(view, Font::BODY, theme::MUTED);
  lv_obj_set_pos(percent_, x + row_w - percent_w, y + (height - ui::line_height(Font::BODY)) / 2);
  lv_obj_set_size(percent_, percent_w, ui::line_height(Font::BODY));
  lv_obj_set_style_text_align(percent_, LV_TEXT_ALIGN_RIGHT, 0);
}

// ---- Rooms: one row each, with what plays, play or pause, joining the group, and the volume.

void PlayerCard::build_rooms_(int width, int height) {
  rooms_list_ = list_box_(views_[ROOMS]);
  lv_obj_set_size(rooms_list_, width, height);
  const int pad = ui::px(14), key = ui::mm(9), slider_h = ui::px(14);
  const int name_h = ui::line_height(Font::TITLE), sub_h = ui::line_height(Font::BODY);
  const int head_h = std::max(name_h + sub_h, key), row_h = pad + head_h + ui::px(18) + slider_h + ui::px(6) + pad;
  const int words_w = width - 2 * pad - 2 * key - ui::px(28);
  for (int i = 0; i < MAX_ROOMS; ++i) {
    Room &r = rooms_[i];
    r.root = fill_(rooms_list_, theme::CARD);
    lv_obj_set_pos(r.root, 0, i * (row_h + ui::px(8)));
    lv_obj_set_size(r.root, width, row_h);
    clickable_(r.root, ROOM + i);
    r.name = text_(r.root, Font::TITLE, theme::INK);
    lv_obj_set_pos(r.name, pad, pad + (head_h - name_h - sub_h) / 2);
    lv_obj_set_size(r.name, words_w, name_h);
    r.sub = text_(r.root, Font::BODY, theme::MUTED);
    lv_obj_set_pos(r.sub, pad, pad + (head_h - name_h - sub_h) / 2 + name_h);
    lv_obj_set_size(r.sub, words_w, sub_h);
    r.play = key_(r.root, ROOM_PLAY + i, theme::TRACK, key, ICON_PLAY, Font::ICON_SMALL);
    lv_obj_set_pos(r.play, width - pad - key, pad + (head_h - key) / 2);
    r.play_icon = glyph_of(r.play);
    r.group = key_(r.root, ROOM_GROUP + i, theme::TRACK, key, ICON_GROUP, Font::ICON_SMALL);
    lv_obj_set_pos(r.group, width - pad - 2 * key - ui::px(14), pad + (head_h - key) / 2);
    r.group_icon = glyph_of(r.group);
    r.volume = slider_(r.root, ROOM_VOLUME + i);
    lv_obj_set_pos(r.volume, pad + ui::px(10), pad + head_h + ui::px(18));
    lv_obj_set_size(r.volume, width - 2 * pad - ui::px(20), slider_h);
    lv_obj_add_flag(r.root, LV_OBJ_FLAG_HIDDEN);
  }
}

// ---- The queue and the library share one list; the library has its shelves above it.

void PlayerCard::build_lists_(int width, int height) {
  lv_obj_t *view = views_[QUEUE];
  const int shelf_h = ui::mm(7), gap = ui::px(8), shelf_w = (width - (SHELVES - 1) * gap) / SHELVES;
  for (int s = 0; s < SHELVES; ++s) {
    shelves_[s] = fill_(view, theme::TRACK);
    lv_obj_set_pos(shelves_[s], s * (shelf_w + gap), 0);
    lv_obj_set_size(shelves_[s], shelf_w, shelf_h);
    lv_obj_set_style_radius(shelves_[s], shelf_h / 2, 0);
    clickable_(shelves_[s], SHELF + s);
    shelf_text_[s] = text_(shelves_[s], Font::BODY, theme::INK);
    lv_obj_set_size(shelf_text_[s], shelf_w - ui::px(12), ui::line_height(Font::BODY));
    lv_obj_set_style_text_align(shelf_text_[s], LV_TEXT_ALIGN_CENTER, 0);
    ui::set_text(shelf_text_[s], plugin_->text(SHELF_KEYS[s]));
    lv_obj_center(shelf_text_[s]);
  }
  list_ = list_box_(view);
  lv_obj_set_size(list_, width, height);
  for (int i = 0; i < TesseraMusic::MAX_ROWS; ++i) build_row_(i, width);
  more_ = fill_(list_, theme::TRACK);
  lv_obj_set_size(more_, width, ui::mm(8));
  clickable_(more_, MORE);
  more_text_ = text_(more_, Font::BODY_LARGE, theme::INK);
  lv_obj_set_size(more_text_, width - ui::px(24), ui::line_height(Font::BODY_LARGE));
  lv_obj_set_style_text_align(more_text_, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_center(more_text_);
  empty_ = text_(view, Font::BODY_LARGE, theme::MUTED);
  lv_obj_set_size(empty_, width, ui::line_height(Font::BODY_LARGE));
  lv_obj_set_style_text_align(empty_, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(empty_, LV_ALIGN_CENTER, 0, 0);
}

void PlayerCard::build_row_(int index, int width) {
  Row &r = rows_[index];
  const int title_h = ui::line_height(Font::BODY_LARGE), sub_h = ui::line_height(Font::BODY);
  const int row_h = std::max(ui::mm(10), title_h + sub_h + ui::px(16)), inset = ui::px(6);
  thumb_side_ = row_h - 2 * inset;
  row_step_ = row_h + ui::px(8);
  r.root = fill_(list_, theme::CARD);
  lv_obj_set_pos(r.root, 0, index * row_step_);
  lv_obj_set_size(r.root, width, row_h);
  clickable_(r.root, ROW + index);
  r.art = fill_(r.root, theme::TRACK);
  lv_obj_set_pos(r.art, inset, inset);
  lv_obj_set_size(r.art, thumb_side_, thumb_side_);
  lv_obj_set_style_clip_corner(r.art, true, 0);
  r.note = text_(r.art, Font::ICON_SMALL, theme::MUTED);
  ui::set_text(r.note, ui::icon(ICON_MUSIC));
  lv_obj_center(r.note);
  r.image = lv_image_create(r.art);
  lv_obj_set_size(r.image, thumb_side_, thumb_side_);
  lv_image_set_inner_align(r.image, LV_IMAGE_ALIGN_CENTER);
  lv_obj_center(r.image);
  lv_obj_add_flag(r.image, LV_OBJ_FLAG_HIDDEN);
  const int side_w = ui::text_width("00:00", Font::BODY) + ui::px(16);
  const int x = thumb_side_ + inset + ui::px(16), w = width - x - side_w - ui::px(8), y = (row_h - title_h - sub_h) / 2;
  r.title = text_(r.root, Font::BODY_LARGE, theme::INK);
  lv_obj_set_pos(r.title, x, y);
  lv_obj_set_size(r.title, w, title_h);
  r.sub = text_(r.root, Font::BODY, theme::MUTED);
  lv_obj_set_pos(r.sub, x, y + title_h);
  lv_obj_set_size(r.sub, w, sub_h);
  r.side = text_(r.root, Font::BODY, theme::SUBTLE);
  lv_obj_set_pos(r.side, width - side_w, (row_h - sub_h) / 2);
  lv_obj_set_size(r.side, side_w - ui::px(14), sub_h);
  lv_obj_set_style_text_align(r.side, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_add_flag(r.root, LV_OBJ_FLAG_HIDDEN);
}

// ---- taps and sliders

void PlayerCard::on_event_(lv_event_t *e) {
  auto *card = static_cast<PlayerCard *>(lv_event_get_user_data(e));
  auto *target = static_cast<lv_obj_t *>(lv_event_get_current_target(e));
  const int code = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(target)));
  card->handle_(code, lv_event_get_code(e), target);
}

void PlayerCard::handle_(int code, lv_event_code_t event, lv_obj_t *target) {
  plugin_->seen();
  if (code == VOLUME || code >= ROOM_VOLUME)
    slide_(code, target, event == LV_EVENT_RELEASED);
  else if (event == LV_EVENT_CLICKED)
    tap_(code);
  refresh_();
}

void PlayerCard::tap_(int code) {
  if (code >= TAB && code < TAB + static_cast<int>(VIEWS)) return show_(static_cast<View>(code - TAB));
  auto *ma = plugin_->ma();
  if (ma == nullptr || !ma->is_connected()) return;
  if (code >= SHELF && code < SHELF + SHELVES) {
    shelf_ = code - SHELF;
    ma->request_library(SHELF_KINDS[shelf_]);
    list_view_ = -1;
    return;
  }
  if (code >= ROOM_GROUP) return toggle_member_(code - ROOM_GROUP);
  if (code >= ROOM_PLAY) {
    if (const MAPlayer *p = ma->player(code - ROOM_PLAY)) ma->cmd_play_pause(p->player_id);
    return;
  }
  if (code >= ROOM) return choose_room_(code - ROOM);
  if (code >= ROW) {
    const int row = code - ROW;
    if (view_ == QUEUE) {
      const auto &queue = ma->queue();
      if (row > 0 && row < (int) queue.size()) ma->cmd_play_item(queue[row].queue_item_id);
      return;
    }
    const auto &library = ma->library();
    if (row >= (int) library.size()) return;
    ma->cmd_play_media(library[row].uri, library[row].media_type == "track" ? "play" : "replace");
    return show_(NOW);
  }
  const auto &now = ma->now_playing();
  const MAPlayer *player = ma->player(ma->selected_player());
  switch (code) {
    case PREV: return ma->cmd_previous();
    case PLAY: return ma->cmd_play_pause();
    case NEXT: return ma->cmd_next();
    case SHUFFLE: return ma->cmd_shuffle(!now.shuffle);
    case REPEAT: return ma->cmd_repeat(now.repeat == "off" ? "all" : now.repeat == "all" ? "one" : "off");
    case FAVORITE: return ma->cmd_toggle_favorite();
    case MUTE:
      if (player != nullptr) ma->cmd_mute(!player->muted);
      return;
    case SEEK: return seek_();
    case MORE:
      if (view_ == QUEUE)
        ma->set_queue_limit(std::min(ma->queue_limit() + QUEUE_PAGE, (int) music_assistant::MusicAssistant::MAX_QUEUE_ROWS));
      else if (!ma->library_loading())
        ma->request_library(SHELF_KINDS[shelf_], true);
      return;
    default: return;
  }
}

void PlayerCard::slide_(int code, lv_obj_t *slider, bool released) {
  auto *ma = plugin_->ma();
  dragging_ = released ? nullptr : slider;
  hold_volume_until_ = millis() + VOLUME_HOLD_MS;
  const int value = lv_slider_get_value(slider);
  if (code == VOLUME) ui::set_text(percent_, std::to_string(value) + " %");
  if (ma == nullptr || !ma->is_connected()) return;
  if (!released && millis() - last_volume_ms_ < VOLUME_EVERY_MS) return;
  last_volume_ms_ = millis();
  if (code == VOLUME) return ma->cmd_set_volume(value);
  if (const MAPlayer *p = ma->player(code - ROOM_VOLUME)) ma->cmd_set_volume(p->player_id, value);
}

void PlayerCard::seek_() {
  auto *ma = plugin_->ma();
  const uint32_t duration = ma->now_playing().duration_s;
  lv_indev_t *indev = lv_indev_active();
  if (duration == 0 || indev == nullptr) return;
  lv_point_t point;
  lv_indev_get_point(indev, &point);
  lv_area_t area;
  lv_obj_get_coords(track_, &area);
  const int w = std::max(1, (int) lv_area_get_width(&area));
  const int at = std::clamp((int) point.x - (int) area.x1, 0, w);
  ma->cmd_seek((uint32_t) ((uint64_t) duration * at / w));
}

void PlayerCard::show_(View view) {
  view_ = view;
  for (int v = 0; v < VIEWS; ++v) hide(views_[v], true);
  hide(views_[view], false);
  list_view_ = -1;
  if (view == QUEUE || view == LIBRARY) {
    const bool library = view == LIBRARY;
    for (lv_obj_t *shelf : shelves_) hide(shelf, !library);
    const int top = library ? ui::mm(7) + ui::px(12) : 0;
    lv_obj_set_pos(list_, 0, top);
    lv_obj_set_height(list_, lv_obj_get_height(views_[QUEUE]) - top);
    lv_obj_scroll_to_y(list_, 0, LV_ANIM_OFF);
    auto *ma = plugin_->ma();
    if (library && ma != nullptr && ma->is_connected() && !ma->library_loading() &&
        (ma->library().empty() || ma->library_kind() != SHELF_KINDS[shelf_]))
      ma->request_library(SHELF_KINDS[shelf_]);
  }
  if (view == ROOMS) lv_obj_scroll_to_y(rooms_list_, 0, LV_ANIM_OFF);
  refresh_();
}

void PlayerCard::choose_room_(int index) {
  auto *ma = plugin_->ma();
  const MAPlayer *p = ma->player(index);
  if (p == nullptr) return;
  ma->select_player_user(index);
  ma->pin_player(p->display_name);
  show_(NOW);
}

void PlayerCard::toggle_member_(int index) {
  auto *ma = plugin_->ma();
  const MAPlayer *leader = leader_of(ma, ma->player(ma->selected_player()));
  const MAPlayer *p = ma->player(index);
  if (leader == nullptr || p == nullptr || p->player_id == leader->player_id) return;
  const std::string leader_id = leader->player_id, id = p->player_id;
  if (member_of(leader, id))
    ma->cmd_set_members(leader_id, {}, {id});
  else
    ma->cmd_set_members(leader_id, {id}, {});
}

bool PlayerCard::on_back() {
  if (view_ == NOW) return false;
  show_(NOW);
  return true;
}

void PlayerCard::on_tick(uint32_t) {
  plugin_->seen();
  refresh_();
}

void PlayerCard::on_theme() {
  for (auto &[obj, role] : fills_) lv_obj_set_style_bg_color(obj, ui::color(role), 0);
  for (auto &[label, role] : inks_) lv_obj_set_style_text_color(label, ui::color(role), 0);
  lv_obj_set_style_bg_color(volume_, ui::color(theme::TRACK), LV_PART_MAIN);
  lv_obj_set_style_bg_color(volume_, ui::color(theme::ACCENT), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(volume_, ui::color(theme::SLIDER_KNOB), LV_PART_KNOB);
  for (Room &r : rooms_) {
    lv_obj_set_style_bg_color(r.volume, ui::color(theme::TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_color(r.volume, ui::color(theme::ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(r.volume, ui::color(theme::SLIDER_KNOB), LV_PART_KNOB);
  }
  list_view_ = -1;
  refresh_();
}

// ---- painting

void PlayerCard::refresh_() {
  paint_tabs_();
  switch (view_) {
    case NOW: return paint_now_();
    case ROOMS: return paint_rooms_();
    default: return paint_list_();
  }
}

void PlayerCard::paint_tabs_() {
  for (int v = 0; v < VIEWS; ++v) {
    const bool on = v == view_;
    set_fill(tabs_[v], on ? theme::ACCENT : theme::TRACK);
    ui::set_color(tab_text_[v], on ? theme::ON_ACCENT : theme::INK);
  }
  for (int s = 0; s < SHELVES; ++s) {
    const bool on = s == shelf_;
    set_fill(shelves_[s], on ? theme::INK : theme::TRACK);
    ui::set_color(shelf_text_[s], on ? theme::PAGE : theme::INK);
  }
}

void PlayerCard::paint_now_() {
  auto *ma = plugin_->ma();
  if (ma == nullptr || !ma->is_connected()) {
    ui::set_text(title_, plugin_->text("connecting"));
    return;
  }
  const auto &now = ma->now_playing();
  const MAPlayer *player = ma->player(ma->selected_player());
  const MAPlayer *leader = leader_of(ma, player);
  std::string room = player != nullptr ? player->display_name : "";
  if (leader != nullptr && leader->group_members.size() > 1)
    room = leader->display_name + " +" + std::to_string(leader->group_members.size() - 1);
  ui::set_text(room_, room);
  ui::set_text(title_, now.title.empty() ? plugin_->text("nothing") : now.title);
  ui::set_text(artist_, now.artist);
  ui::set_text(album_, now.album);

  const uint32_t duration = now.duration_s, elapsed = std::min((uint32_t) ma->elapsed_now(), duration);
  ui::set_text(elapsed_, duration ? clock_of(elapsed) : "");
  ui::set_text(total_, duration ? clock_of(duration) : "");
  const int filled = duration ? (int) ((int64_t) bar_w_ * elapsed / duration) : 0;
  if (lv_obj_get_width(fill_bar_) != filled) lv_obj_set_width(fill_bar_, filled);

  paint_now_keys_(player);
  if (player != nullptr && player->volume_level >= 0 && dragging_ != volume_ &&
      (int32_t) (millis() - hold_volume_until_) >= 0) {
    if (lv_slider_get_value(volume_) != player->volume_level)
      lv_slider_set_value(volume_, player->volume_level, LV_ANIM_OFF);
    ui::set_text(percent_, std::to_string(player->volume_level) + " %");
  }
  paint_cover_();
}

void PlayerCard::paint_now_keys_(const MAPlayer *player) {
  const auto &now = plugin_->ma()->now_playing();
  const bool playing = player != nullptr && player->playback_state == "playing";
  ui::set_text(glyph_of(play_), ui::icon(playing ? ICON_PAUSE : ICON_PLAY));
  ui::set_color(glyph_of(shuffle_), now.shuffle ? theme::ACCENT : theme::SUBTLE);
  ui::set_text(glyph_of(repeat_), ui::icon(now.repeat == "one" ? ICON_REPEAT_ONE : ICON_REPEAT));
  ui::set_color(glyph_of(repeat_), now.repeat == "off" ? theme::SUBTLE : theme::ACCENT);
  ui::set_color(glyph_of(favorite_), now.favorite ? theme::ACCENT : theme::SUBTLE);
  const bool muted = player != nullptr && player->muted;
  ui::set_text(glyph_of(mute_), ui::icon(muted ? ICON_MUTED : ICON_VOLUME));
}

void PlayerCard::paint_cover_() {
  CoverLoader &cover = plugin_->card_cover();
  cover.request(plugin_->ma()->now_playing().image_url, cover_side_);
  if (cover.revision() == cover_revision_) return;
  cover_revision_ = cover.revision();
  const lv_image_dsc_t *picture = cover.picture();
  if (picture != nullptr) lv_image_set_src(image_, picture);
  hide(image_, picture == nullptr);
  hide(note_, picture != nullptr);
}

void PlayerCard::paint_rooms_() {
  auto *ma = plugin_->ma();
  const bool online = ma != nullptr && ma->is_connected();
  const int count = online ? std::min((int) ma->player_count(), MAX_ROOMS) : 0;
  const int selected = online ? ma->selected_player() : -1;
  const MAPlayer *leader = online ? leader_of(ma, ma->player(selected)) : nullptr;
  for (int i = 0; i < MAX_ROOMS; ++i) {
    Room &r = rooms_[i];
    hide(r.root, i >= count);
    if (i >= count) continue;
    const MAPlayer *p = ma->player(i);
    ui::set_text(r.name, p->display_name);
    ui::set_color(r.name, i == selected ? theme::ACCENT : theme::INK);
    const std::string words = p->title.empty() ? "" : p->artist.empty() ? p->title : p->title + " · " + p->artist;
    ui::set_text(r.sub, words.empty() ? plugin_->text("nothing") : words);
    ui::set_text(r.play_icon, ui::icon(p->playback_state == "playing" ? ICON_PAUSE : ICON_PLAY));
    const bool lead = leader != nullptr && p->player_id == leader->player_id;
    hide(r.group, leader == nullptr || lead);
    ui::set_color(r.group_icon, member_of(leader, p->player_id) ? theme::ACCENT : theme::SUBTLE);
    if (p->volume_level >= 0 && dragging_ != r.volume && (int32_t) (millis() - hold_volume_until_) >= 0 &&
        lv_slider_get_value(r.volume) != p->volume_level)
      lv_slider_set_value(r.volume, p->volume_level, LV_ANIM_OFF);
  }
}

void PlayerCard::paint_list_() {
  auto *ma = plugin_->ma();
  const bool online = ma != nullptr && ma->is_connected(), queue = view_ == QUEUE;
  const bool fresh = online && (ma->revision() != list_revision_ || list_view_ != view_ || list_shelf_ != shelf_);
  int rows = 0;
  if (online && queue) rows = std::min((int) ma->queue().size(), (int) TesseraMusic::MAX_ROWS);
  if (online && !queue) rows = std::min((int) ma->library().size(), (int) TesseraMusic::MAX_ROWS);
  if (fresh) {
    list_revision_ = ma->revision();
    list_view_ = view_;
    list_shelf_ = shelf_;
    for (int i = 0; i < TesseraMusic::MAX_ROWS; ++i) {
      if (i >= rows) {
        hide(rows_[i].root, true);
        continue;
      }
      if (queue) {
        const auto &item = ma->queue()[i];
        paint_row_(i, item.title, item.artist, item.duration_s ? clock_of(item.duration_s) : "", item.image_url, i == 0);
      } else {
        const auto &item = ma->library()[i];
        paint_row_(i, item.name, item.subtitle, "", item.image_url, false);
      }
    }
    const int left = ma->queue_total_items() - ma->queue_current_index();
    const bool more = queue ? rows < left && ma->queue_limit() < music_assistant::MusicAssistant::MAX_QUEUE_ROWS
                            : ma->library_has_more() && rows < TesseraMusic::MAX_ROWS;
    paint_more_(more, !queue && ma->library_loading(), rows);
  }
  if (!online) paint_more_(false, false, 0);
  for (int i = 0; i < rows; ++i) {
    Row &r = rows_[i];
    CoverLoader &thumb = plugin_->thumb(i);
    if (thumb.revision() == r.cover_revision) continue;
    r.cover_revision = thumb.revision();
    const lv_image_dsc_t *picture = thumb.picture();
    if (picture != nullptr) lv_image_set_src(r.image, picture);
    hide(r.image, picture == nullptr);
    hide(r.note, picture != nullptr);
  }
}

void PlayerCard::paint_row_(int index, const std::string &title, const std::string &sub, const std::string &side,
                            const std::string &image, bool current) {
  Row &r = rows_[index];
  hide(r.root, false);
  ui::set_text(r.title, title);
  ui::set_color(r.title, current ? theme::ACCENT : theme::INK);
  ui::set_text(r.sub, sub);
  ui::set_text(r.side, side);
  plugin_->thumb(index).request(image, thumb_side_, false);
}

void PlayerCard::paint_more_(bool more, bool loading, int rows) {
  hide(more_, !more && !loading);
  lv_obj_set_y(more_, rows * row_step_);
  ui::set_text(more_text_, plugin_->text(loading ? "loading" : "more"));
  hide(empty_, rows > 0);
  if (rows > 0) return;
  auto *ma = plugin_->ma();
  const bool online = ma != nullptr && ma->is_connected();
  const char *key = !online ? "connecting" : loading ? "loading" : view_ == QUEUE ? "queue_empty" : "list_empty";
  ui::set_text(empty_, plugin_->text(key));
}

}  // namespace esphome::tessera_music
