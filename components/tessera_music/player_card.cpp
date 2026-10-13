#include "player_card.h"
#include <algorithm>
#include <cstdio>
#include "esp_heap_caps.h"

namespace esphome::tessera_music {

using tessera::Font;
using music_assistant::MALibrary;
using music_assistant::MAPlayer;
namespace ui = tessera::ui;

// The espcontrol MA dashboard's colours: this card keeps its own dark look, whatever the screen's theme.
static constexpr uint32_t BG = 0x0F1012, TEXT = 0xF4F1EC, TEXT2 = 0xE6E1D9, MUTED = 0xB3AEA6, DIM = 0xA9A49C,
                          ACCENT = 0xE8945A, ON_ACCENT = 0x1A1208, PLAYING = 0x7EE2A8;
static constexpr lv_opa_t GLASS = 18, GLASS_BORDER = 28, GLASS_PRESSED = 44, SELECTED = 36;

// What a tap or a slider means: a code in the object's user data.
enum Code : int {
  PILL = 1, GRID, NEXT_UP, PREV, PLAY, NEXT, SHUFFLE, REPEAT, FAVORITE, MUTE, VOLUME, SEEK, MORE,
  SHELF = 30,  // + the shelf
  ROW = 100,   // + the row
  ROOM = 200, ROOM_PLAY = 300, ROOM_GROUP = 400, ROOM_VOLUME = 500,  // + the room
};

static constexpr uint32_t ICON_MUSIC = 0xF075A, ICON_PLAY = 0xF040A, ICON_PAUSE = 0xF03E4, ICON_NEXT = 0xF04AD,
                          ICON_PREV = 0xF04AE, ICON_SHUFFLE = 0xF049F, ICON_REPEAT = 0xF0456, ICON_REPEAT_ONE = 0xF0458,
                          ICON_VOLUME = 0xF057E, ICON_MUTED = 0xF0581, ICON_HEART = 0xF02D1, ICON_SPEAKER = 0xF04C3,
                          ICON_GROUP = 0xF0D38, ICON_DOWN = 0xF0140;
static constexpr int QUEUE_PAGE = 20;
static constexpr uint32_t VOLUME_EVERY_MS = 250, VOLUME_HOLD_MS = 2000;
static const char *const SHELF_KEYS[] = {"tab_queue", "shelf_playlists", "shelf_radio", "shelf_favorites"};
static const MALibrary SHELF_KINDS[] = {MALibrary::PLAYLISTS, MALibrary::PLAYLISTS, MALibrary::RADIOS,
                                        MALibrary::FAVORITES};
// The dashboard's layout, drawn for 680 x 600 and scaled to the card.
static constexpr int BASE_W = 680, BASE_H = 600;

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

static bool same(lv_color_t current, uint32_t color) { return (lv_color_to_u32(current) & 0xFFFFFF) == color; }

// Style setters redraw even when nothing changes, and the card repaints every second: only what differs.
static void fill(lv_obj_t *obj, uint32_t color, lv_opa_t opa) {
  if (!same(lv_obj_get_style_bg_color(obj, LV_PART_MAIN), color))
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), LV_PART_MAIN);
  if (lv_obj_get_style_bg_opa(obj, LV_PART_MAIN) != opa) lv_obj_set_style_bg_opa(obj, opa, LV_PART_MAIN);
}

static void border(lv_obj_t *obj, int width, uint32_t color, lv_opa_t opa) {
  if (lv_obj_get_style_border_width(obj, LV_PART_MAIN) != width) lv_obj_set_style_border_width(obj, width, 0);
  if (!same(lv_obj_get_style_border_color(obj, LV_PART_MAIN), color))
    lv_obj_set_style_border_color(obj, lv_color_hex(color), 0);
  if (lv_obj_get_style_border_opa(obj, LV_PART_MAIN) != opa) lv_obj_set_style_border_opa(obj, opa, 0);
}

static void glass(lv_obj_t *obj) {
  fill(obj, TEXT, GLASS);
  border(obj, 1, TEXT, GLASS_BORDER);
}

static void selected(lv_obj_t *obj) {
  fill(obj, ACCENT, SELECTED);
  border(obj, 2, ACCENT, LV_OPA_COVER);
}

static void ink(lv_obj_t *label, uint32_t color) {
  if (!same(lv_obj_get_style_text_color(label, LV_PART_MAIN), color))
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
}

static lv_obj_t *glyph_of(lv_obj_t *key) { return lv_obj_get_child(key, 0); }

static const MAPlayer *leader_of(music_assistant::MusicAssistant *ma, const MAPlayer *player) {
  if (player == nullptr || player->synced_to.empty()) return player;
  const MAPlayer *leader = ma->player(ma->player_index(player->synced_to));
  return leader != nullptr ? leader : player;
}

static bool member_of(const MAPlayer *leader, const std::string &id) {
  return leader != nullptr && std::find(leader->group_members.begin(), leader->group_members.end(), id) !=
                                  leader->group_members.end();
}

static std::string upper(std::string text) {
  for (char &c : text)
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  return text;
}

// ---- making

lv_obj_t *PlayerCard::box_(lv_obj_t *parent, int x, int y, int w, int h, int radius) {
  lv_obj_t *obj = lv_obj_create(parent);
  lv_obj_remove_style_all(obj);
  lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_pos(obj, x, y);
  lv_obj_set_size(obj, w, h);
  lv_obj_set_style_radius(obj, radius, 0);
  return obj;
}

lv_obj_t *PlayerCard::glass_(lv_obj_t *parent, int x, int y, int w, int h, int radius, int code) {
  lv_obj_t *obj = box_(parent, x, y, w, h, radius);
  glass(obj);
  if (code != 0) clickable_(obj, code);
  return obj;
}

void PlayerCard::clickable_(lv_obj_t *obj, int code) {
  lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_user_data(obj, reinterpret_cast<void *>(static_cast<intptr_t>(code)));
  lv_obj_add_event_cb(obj, on_event_, LV_EVENT_CLICKED, this);
  lv_obj_set_style_bg_opa(obj, GLASS_PRESSED, LV_STATE_PRESSED);
}

lv_obj_t *PlayerCard::words_(lv_obj_t *parent, Font font, uint32_t color, int x, int y, int w) {
  lv_obj_t *label = ui::label(parent, font, theme::INK);
  ink(label, color);
  lv_obj_set_pos(label, x, y);
  lv_obj_set_size(label, std::max(1, w), ui::line_height(font));
  return label;
}

lv_obj_t *PlayerCard::glyph_(lv_obj_t *parent, uint32_t icon, Font font, uint32_t color) {
  lv_obj_t *label = ui::label(parent, font, theme::INK);
  ink(label, color);
  ui::set_text(label, ui::icon(icon));
  lv_obj_center(label);
  return label;
}

lv_obj_t *PlayerCard::slider_(lv_obj_t *parent, int code) {
  lv_obj_t *slider = lv_slider_create(parent);
  lv_slider_set_range(slider, 0, 100);
  lv_obj_set_style_bg_color(slider, lv_color_hex(TEXT), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(slider, SELECTED, LV_PART_MAIN);
  lv_obj_set_style_bg_color(slider, lv_color_hex(ACCENT), LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(slider, lv_color_hex(TEXT), LV_PART_KNOB);
  lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_KNOB);
  lv_obj_set_style_pad_all(slider, s_(6), LV_PART_KNOB);
  lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
  lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
  lv_obj_set_user_data(slider, reinterpret_cast<void *>(static_cast<intptr_t>(code)));
  lv_obj_add_event_cb(slider, on_event_, LV_EVENT_VALUE_CHANGED, this);
  lv_obj_add_event_cb(slider, on_event_, LV_EVENT_RELEASED, this);
  lv_obj_set_ext_click_area(slider, s_(18));
  return slider;
}

void PlayerCard::open(const tessera::CardContext &c) {
  plugin_->card_opened();
  scale_ = std::min(c.width * 1000 / BASE_W, c.height * 1000 / BASE_H);
  // The ground: the card's whole glass, top bar too, dark and lit from above by the cover's colour.
  ground_ = lv_obj_get_parent(c.parent);
  lv_obj_set_style_bg_opa(ground_, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(ground_, lv_color_hex(BG), 0);
  lv_obj_set_style_bg_grad_color(ground_, lv_color_hex(BG), 0);
  lv_obj_set_style_bg_grad_dir(ground_, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_bg_main_stop(ground_, 0, 0);
  lv_obj_set_style_bg_grad_stop(ground_, 220, 0);
  const int width = std::min(c.width, s_(BASE_W)), x = (c.width - width) / 2;
  for (int v = 0; v < VIEWS; ++v) views_[v] = box_(c.parent, x, 0, width, c.height, 0);
  build_now_(width, c.height);
  build_rooms_(width, c.height);
  build_lists_(width, c.height);
  show_(NOW);
}

PlayerCard::~PlayerCard() {
  heap_caps_free(wave_pixels_);
  plugin_->card_closed();
}

// ---- Now playing

void PlayerCard::build_now_(int width, int height) {
  lv_obj_t *view = views_[NOW];
  build_pill_(view, width);
  const int top = s_(68), volume_y = height - s_(64), keys_y = volume_y - s_(16) - s_(92);
  const int times_h = ui::line_height(Font::BODY), wave_y = keys_y - s_(6) - times_h - s_(4) - s_(56);
  cover_side_ = std::max(s_(120), std::min(wave_y - s_(16) - top, width * 47 / 100));
  art_ = box_(view, 0, top, cover_side_, cover_side_, s_(24));
  glass(art_);
  lv_obj_set_style_clip_corner(art_, true, 0);
  note_ = glyph_(art_, ICON_MUSIC, Font::ICON, MUTED);
  image_ = lv_image_create(art_);
  lv_obj_set_size(image_, cover_side_, cover_side_);
  lv_image_set_inner_align(image_, LV_IMAGE_ALIGN_CENTER);
  lv_obj_center(image_);
  lv_obj_add_flag(image_, LV_OBJ_FLAG_HIDDEN);
  const int column_x = cover_side_ + s_(24);
  build_column_(view, column_x, top, width - column_x, cover_side_);
  build_progress_(view, width, wave_y);
  build_keys_(view, width, keys_y);
  build_volume_(view, width, volume_y);
}

// The room pill (the room, its group, a tap for the rooms), the favourite and the grid key.
void PlayerCard::build_pill_(lv_obj_t *view, int width) {
  const int h = s_(52), w = std::min(s_(330), width - 2 * h - s_(24));
  pill_ = glass_(view, 0, 0, w, h, h / 2, PILL);
  pill_dot_ = box_(pill_, s_(7), s_(7), h - s_(14), h - s_(14), LV_RADIUS_CIRCLE);
  fill(pill_dot_, ACCENT, LV_OPA_COVER);
  glyph_(pill_dot_, ICON_SPEAKER, Font::ICON_SMALL, ON_ACCENT);
  const int name_x = h + s_(4), chevron_w = s_(36), badge_w = s_(40);
  room_ = words_(pill_, Font::BODY_LARGE, TEXT, name_x, (h - ui::line_height(Font::BODY_LARGE)) / 2,
                 w - name_x - chevron_w - badge_w - s_(8));
  badge_ = box_(pill_, w - chevron_w - badge_w - s_(4), (h - s_(28)) / 2, badge_w, s_(28), LV_RADIUS_CIRCLE);
  fill(badge_, TEXT, LV_OPA_COVER);
  badge_text_ = words_(badge_, Font::BODY, ON_ACCENT, 0, (s_(28) - ui::line_height(Font::BODY)) / 2, badge_w);
  lv_obj_set_style_text_align(badge_text_, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_t *chevron = box_(pill_, w - chevron_w - s_(6), 0, chevron_w, h, 0);
  glyph_(chevron, ICON_DOWN, Font::ICON_SMALL, MUTED);
  grid_ = glass_(view, width - h, 0, h, h, LV_RADIUS_CIRCLE, GRID);
  glyph_(grid_, ICON_GROUP, Font::ICON_SMALL, TEXT);
  favorite_ = glass_(view, width - 2 * h - s_(12), 0, h, h, LV_RADIUS_CIRCLE, FAVORITE);
  glyph_(favorite_, ICON_HEART, Font::ICON_SMALL, MUTED);
}

// Beside the cover: what plays where, the title, the artist, the album, and what comes next.
void PlayerCard::build_column_(lv_obj_t *view, int x, int y, int w, int h) {
  const int status_h = ui::line_height(Font::BODY);
  status_dot_ = box_(view, x, y + (status_h - s_(10)) / 2, s_(10), s_(10), LV_RADIUS_CIRCLE);
  fill(status_dot_, PLAYING, LV_OPA_COVER);
  status_ = words_(view, Font::BODY, MUTED, x + s_(18), y, w - s_(18));
  int at = y + status_h + s_(6);
  title_ = words_(view, Font::HEADLINE, TEXT, x, at, w);
  at += ui::line_height(Font::HEADLINE);
  artist_ = words_(view, Font::BODY_LARGE, TEXT2, x, at, w);
  at += ui::line_height(Font::BODY_LARGE);
  album_ = words_(view, Font::BODY, MUTED, x, at, w);
  at += ui::line_height(Font::BODY) + s_(12);
  build_next_up_(view, x, at, w, y + h - at);
}

void PlayerCard::build_next_up_(lv_obj_t *view, int x, int y, int w, int h) {
  next_box_ = glass_(view, x, y, w, std::max(s_(40), h), s_(20), NEXT_UP);
  const int pad = s_(14), head_h = ui::line_height(Font::BODY);
  lv_obj_t *head = words_(next_box_, Font::BODY, MUTED, pad, s_(10), w / 2);
  ui::set_text(head, upper(plugin_->text("next_up")));
  next_count_ = words_(next_box_, Font::BODY, DIM, w / 2, s_(10), w / 2 - pad);
  lv_obj_set_style_text_align(next_count_, LV_TEXT_ALIGN_RIGHT, 0);
  const int row_h = s_(50), first = s_(10) + head_h + s_(6);
  next_thumb_ = s_(44);
  for (int i = 0; i < NEXT_ROWS; ++i) {
    next_rows_[i] = box_(next_box_, s_(6), first + i * (row_h + s_(4)), w - s_(12), row_h, 0);
    next_art_[i] = box_(next_rows_[i], s_(6), (row_h - next_thumb_) / 2, next_thumb_, next_thumb_, s_(10));
    glass(next_art_[i]);
    lv_obj_set_style_clip_corner(next_art_[i], true, 0);
    next_image_[i] = lv_image_create(next_art_[i]);
    lv_obj_set_size(next_image_[i], next_thumb_, next_thumb_);
    lv_image_set_inner_align(next_image_[i], LV_IMAGE_ALIGN_CENTER);
    lv_obj_center(next_image_[i]);
    lv_obj_add_flag(next_image_[i], LV_OBJ_FLAG_HIDDEN);
    const int tx = next_thumb_ + s_(16), tw = w - s_(12) - tx - s_(8);
    const int th = ui::line_height(Font::BODY_LARGE), sh = ui::line_height(Font::BODY);
    next_title_[i] = words_(next_rows_[i], Font::BODY_LARGE, TEXT, tx, (row_h - th - sh) / 2, tw);
    next_artist_[i] = words_(next_rows_[i], Font::BODY, MUTED, tx, (row_h - th - sh) / 2 + th, tw);
    hide(next_rows_[i], first + (i + 1) * (row_h + s_(4)) > h);
  }
}

// The track's waveform as its progress (a bar while there is none), the time played and the time left.
void PlayerCard::build_progress_(lv_obj_t *view, int width, int y) {
  wave_w_ = width;
  wave_h_ = s_(56);
  const size_t bytes = static_cast<size_t>(wave_w_) * wave_h_ * 4;
  wave_pixels_ = static_cast<uint8_t *>(heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM));
  if (wave_pixels_ != nullptr) {
    wave_ = lv_canvas_create(view);
    lv_canvas_set_buffer(wave_, wave_pixels_, wave_w_, wave_h_, LV_COLOR_FORMAT_ARGB8888);
    lv_obj_set_pos(wave_, 0, y);
  }
  track_ = box_(view, 0, y + (wave_h_ - s_(8)) / 2, width, s_(8), LV_RADIUS_CIRCLE);
  fill(track_, TEXT, SELECTED);
  fill_bar_ = box_(track_, 0, 0, 0, s_(8), LV_RADIUS_CIRCLE);
  fill(fill_bar_, ACCENT, LV_OPA_COVER);
  seek_zone_ = box_(view, 0, y, width, wave_h_, 0);
  lv_obj_add_flag(seek_zone_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_user_data(seek_zone_, reinterpret_cast<void *>(static_cast<intptr_t>(SEEK)));
  lv_obj_add_event_cb(seek_zone_, on_event_, LV_EVENT_CLICKED, this);
  const int times_y = y + wave_h_ + s_(4);
  elapsed_ = words_(view, Font::BODY, DIM, 0, times_y, width / 2);
  remaining_ = words_(view, Font::BODY, DIM, width / 2, times_y, width / 2);
  lv_obj_set_style_text_align(remaining_, LV_TEXT_ALIGN_RIGHT, 0);
}

// Shuffle, previous, the orange play key, next and repeat, in the middle.
void PlayerCard::build_keys_(lv_obj_t *view, int width, int y) {
  const int bare = s_(56), key = s_(68), play = s_(92), near = s_(16), far = s_(22);
  int x = (width - (2 * bare + 2 * key + play + 2 * near + 2 * far)) / 2;
  shuffle_ = box_(view, x, y + (play - bare) / 2, bare, bare, LV_RADIUS_CIRCLE);
  clickable_(shuffle_, SHUFFLE);
  glyph_(shuffle_, ICON_SHUFFLE, Font::ICON_SMALL, MUTED);
  x += bare + far;
  prev_ = glass_(view, x, y + (play - key) / 2, key, key, LV_RADIUS_CIRCLE, PREV);
  glyph_(prev_, ICON_PREV, Font::ICON, TEXT);
  x += key + near;
  play_ = box_(view, x, y, play, play, LV_RADIUS_CIRCLE);
  fill(play_, ACCENT, LV_OPA_COVER);
  clickable_(play_, PLAY);
  lv_obj_set_style_bg_opa(play_, LV_OPA_80, LV_STATE_PRESSED);
  glyph_(play_, ICON_PLAY, Font::ICON, ON_ACCENT);
  x += play + near;
  next_ = glass_(view, x, y + (play - key) / 2, key, key, LV_RADIUS_CIRCLE, NEXT);
  glyph_(next_, ICON_NEXT, Font::ICON, TEXT);
  x += key + far;
  repeat_ = box_(view, x, y + (play - bare) / 2, bare, bare, LV_RADIUS_CIRCLE);
  clickable_(repeat_, REPEAT);
  glyph_(repeat_, ICON_REPEAT, Font::ICON_SMALL, MUTED);
}

// The volume pill along the bottom: mute, the slider, the percentage.
void PlayerCard::build_volume_(lv_obj_t *view, int width, int y) {
  const int h = s_(64);
  lv_obj_t *pill = glass_(view, 0, y, width, h, h / 2, 0);
  mute_ = box_(pill, s_(10), s_(10), h - s_(20), h - s_(20), LV_RADIUS_CIRCLE);
  fill(mute_, TEXT, 20);
  clickable_(mute_, MUTE);
  glyph_(mute_, ICON_VOLUME, Font::ICON_SMALL, TEXT);
  const int percent_w = ui::text_width("100%", Font::BODY_LARGE) + s_(8), slider_x = h + s_(12);
  volume_ = slider_(pill, VOLUME);
  lv_obj_set_pos(volume_, slider_x, (h - s_(12)) / 2);
  lv_obj_set_size(volume_, width - slider_x - percent_w - s_(32), s_(12));
  percent_ = words_(pill, Font::BODY_LARGE, TEXT, width - percent_w - s_(20),
                    (h - ui::line_height(Font::BODY_LARGE)) / 2, percent_w);
  lv_obj_set_style_text_align(percent_, LV_TEXT_ALIGN_RIGHT, 0);
}

// ---- Rooms: a glass row each, with what plays, play or pause, the group key and the volume.

void PlayerCard::build_rooms_(int width, int height) {
  rooms_list_ = box_(views_[ROOMS], 0, 0, width, height, 0);
  lv_obj_add_flag(rooms_list_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(rooms_list_, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(rooms_list_, LV_SCROLLBAR_MODE_OFF);
  const int h = s_(16) + s_(52) + s_(16) + s_(10) + s_(18);
  for (int i = 0; i < MAX_ROOMS; ++i) build_room_(i, width, i * (h + s_(10)), h);
}

void PlayerCard::build_room_(int index, int width, int y, int h) {
  Room &r = rooms_[index];
  const int pad = s_(16), key = s_(52), name_h = ui::line_height(Font::BODY_LARGE), sub_h = ui::line_height(Font::BODY);
  r.root = glass_(rooms_list_, 0, y, width, h, s_(20), ROOM + index);
  const int words_w = width - 2 * pad - 2 * key - s_(24);
  const int head_y = pad + (key - name_h - sub_h) / 2;
  r.name = words_(r.root, Font::BODY_LARGE, TEXT, pad, head_y, words_w);
  r.sub = words_(r.root, Font::BODY, MUTED, pad, head_y + name_h, words_w);
  r.play = glass_(r.root, width - pad - key, pad, key, key, LV_RADIUS_CIRCLE, ROOM_PLAY + index);
  glyph_(r.play, ICON_PLAY, Font::ICON_SMALL, TEXT);
  r.group = glass_(r.root, width - pad - 2 * key - s_(12), pad, key, key, LV_RADIUS_CIRCLE, ROOM_GROUP + index);
  glyph_(r.group, ICON_GROUP, Font::ICON_SMALL, MUTED);
  r.volume = slider_(r.root, ROOM_VOLUME + index);
  lv_obj_set_pos(r.volume, pad + s_(8), pad + key + s_(16));
  lv_obj_set_size(r.volume, width - 2 * pad - s_(16), s_(10));
  lv_obj_add_flag(r.root, LV_OBJ_FLAG_HIDDEN);
}

// ---- The queue and the library: a segmented control over one list.

void PlayerCard::build_lists_(int width, int height) {
  lv_obj_t *view = views_[LISTS];
  const int seg_h = s_(52), seg_w = (width - s_(12)) / SHELVES;
  segments_ = glass_(view, 0, 0, width, seg_h, seg_h / 2, 0);
  for (int s = 0; s < SHELVES; ++s) {
    shelves_[s] = box_(segments_, s_(6) + s * seg_w, s_(6), seg_w, seg_h - s_(12), (seg_h - s_(12)) / 2);
    clickable_(shelves_[s], SHELF + s);
    shelf_text_[s] = words_(shelves_[s], Font::BODY, TEXT, s_(4), (seg_h - s_(12) - ui::line_height(Font::BODY)) / 2,
                            seg_w - s_(8));
    lv_obj_set_style_text_align(shelf_text_[s], LV_TEXT_ALIGN_CENTER, 0);
    ui::set_text(shelf_text_[s], plugin_->text(SHELF_KEYS[s]));
  }
  const int top = seg_h + s_(14);
  list_ = box_(view, 0, top, width, height - top, 0);
  lv_obj_add_flag(list_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(list_, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(list_, LV_SCROLLBAR_MODE_OFF);
  for (int i = 0; i < TesseraMusic::MAX_ROWS; ++i) build_row_(i, width);
  more_ = glass_(list_, 0, 0, width, s_(56), s_(28), MORE);
  more_text_ = words_(more_, Font::BODY_LARGE, TEXT, 0, (s_(56) - ui::line_height(Font::BODY_LARGE)) / 2, width);
  lv_obj_set_style_text_align(more_text_, LV_TEXT_ALIGN_CENTER, 0);
  empty_ = words_(view, Font::BODY_LARGE, MUTED, 0, top + s_(40), width);
  lv_obj_set_style_text_align(empty_, LV_TEXT_ALIGN_CENTER, 0);
}

void PlayerCard::build_row_(int index, int width) {
  Row &r = rows_[index];
  const int title_h = ui::line_height(Font::BODY_LARGE), sub_h = ui::line_height(Font::BODY);
  const int row_h = std::max(s_(64), title_h + sub_h + s_(16)), inset = (row_h - s_(48)) / 2;
  thumb_side_ = s_(48);
  row_step_ = row_h + s_(8);
  r.root = glass_(list_, 0, index * row_step_, width, row_h, s_(16), ROW + index);
  r.art = box_(r.root, inset, inset, thumb_side_, thumb_side_, s_(10));
  glass(r.art);
  lv_obj_set_style_clip_corner(r.art, true, 0);
  r.note = glyph_(r.art, ICON_MUSIC, Font::ICON_SMALL, MUTED);
  r.image = lv_image_create(r.art);
  lv_obj_set_size(r.image, thumb_side_, thumb_side_);
  lv_image_set_inner_align(r.image, LV_IMAGE_ALIGN_CENTER);
  lv_obj_center(r.image);
  lv_obj_add_flag(r.image, LV_OBJ_FLAG_HIDDEN);
  const int side_w = ui::text_width("00:00", Font::BODY) + s_(16);
  const int x = inset + thumb_side_ + s_(16), w = width - x - side_w - s_(8), y = (row_h - title_h - sub_h) / 2;
  r.title = words_(r.root, Font::BODY_LARGE, TEXT, x, y, w);
  r.sub = words_(r.root, Font::BODY, MUTED, x, y + title_h, w);
  r.side = words_(r.root, Font::BODY, DIM, width - side_w - s_(8), (row_h - sub_h) / 2, side_w);
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
  if (code == PILL || code == GRID) return show_(ROOMS);
  if (code == NEXT_UP) {
    shelf_ = 0;
    return show_(LISTS);
  }
  auto *ma = plugin_->ma();
  if (ma == nullptr || !ma->is_connected()) return;
  if (code >= SHELF && code < SHELF + SHELVES) return choose_shelf_(code - SHELF);
  if (code >= ROOM_GROUP) return toggle_member_(code - ROOM_GROUP);
  if (code >= ROOM_PLAY) {
    if (const MAPlayer *p = ma->player(code - ROOM_PLAY)) ma->cmd_play_pause(p->player_id);
    return;
  }
  if (code >= ROOM) return choose_room_(code - ROOM);
  if (code >= ROW) return tap_list_row_(code - ROW);
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
      if (shelf_ == 0)
        ma->set_queue_limit(std::min(ma->queue_limit() + QUEUE_PAGE, (int) music_assistant::MusicAssistant::MAX_QUEUE_ROWS));
      else if (!ma->library_loading())
        ma->request_library(SHELF_KINDS[shelf_], true);
      return;
    default: return;
  }
}

void PlayerCard::tap_list_row_(int row) {
  auto *ma = plugin_->ma();
  if (shelf_ == 0) {
    const auto &queue = ma->queue();
    if (row > 0 && row < (int) queue.size()) ma->cmd_play_item(queue[row].queue_item_id);
    return;
  }
  const auto &library = ma->library();
  if (row >= (int) library.size()) return;
  ma->cmd_play_media(library[row].uri, library[row].media_type == "track" ? "play" : "replace");
  show_(NOW);
}

void PlayerCard::slide_(int code, lv_obj_t *slider, bool released) {
  auto *ma = plugin_->ma();
  dragging_ = released ? nullptr : slider;
  hold_volume_until_ = millis() + VOLUME_HOLD_MS;
  const int value = lv_slider_get_value(slider);
  if (code == VOLUME) ui::set_text(percent_, std::to_string(value) + "%");
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
  lv_obj_get_coords(seek_zone_, &area);
  const int w = std::max(1, (int) lv_area_get_width(&area));
  const int at = std::clamp((int) point.x - (int) area.x1, 0, w);
  ma->cmd_seek((uint32_t) ((uint64_t) duration * at / w));
}

void PlayerCard::show_(View view) {
  view_ = view;
  for (int v = 0; v < VIEWS; ++v) hide(views_[v], v != view);
  list_view_ = -1;
  if (view == LISTS) choose_shelf_(shelf_);
  if (view == ROOMS) lv_obj_scroll_to_y(rooms_list_, 0, LV_ANIM_OFF);
  refresh_();
}

void PlayerCard::choose_shelf_(int shelf) {
  shelf_ = shelf;
  list_view_ = -1;
  lv_obj_scroll_to_y(list_, 0, LV_ANIM_OFF);
  auto *ma = plugin_->ma();
  if (shelf_ == 0 || ma == nullptr || !ma->is_connected() || ma->library_loading()) return;
  if (ma->library().empty() || ma->library_kind() != SHELF_KINDS[shelf_]) ma->request_library(SHELF_KINDS[shelf_]);
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

// ---- painting

void PlayerCard::refresh_() {
  paint_glow_();
  switch (view_) {
    case NOW: return paint_now_();
    case ROOMS: return paint_rooms_();
    default: return paint_list_();
  }
}

// The cover's colour, softened into the dark ground, lights the card from the top.
void PlayerCard::paint_glow_() {
  const uint32_t tone = plugin_->card_cover().tone();
  if (tone == glow_) return;
  glow_ = tone;
  const lv_color_t top = tone == 0 ? lv_color_hex(BG) : lv_color_mix(lv_color_hex(tone), lv_color_hex(BG), 120);
  lv_obj_set_style_bg_color(ground_, top, 0);
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
  const std::string room = player != nullptr ? player->display_name : "";
  ui::set_text(room_, room);
  const int members = leader != nullptr ? (int) leader->group_members.size() : 0;
  hide(badge_, members < 2);
  ui::set_text(badge_text_, "+" + std::to_string(std::max(0, members - 1)));
  const bool playing = player != nullptr && player->playback_state == "playing";
  ui::set_text(status_, upper(tessera::fill(plugin_->text(playing ? "playing_in" : "paused_in"), "room", room)));
  fill(status_dot_, playing ? PLAYING : DIM, LV_OPA_COVER);
  ui::set_text(title_, now.title.empty() ? plugin_->text("nothing") : now.title);
  ui::set_text(artist_, now.artist);
  ui::set_text(album_, now.album);

  const uint32_t duration = now.duration_s, elapsed = std::min((uint32_t) ma->elapsed_now(), duration);
  ui::set_text(elapsed_, duration ? clock_of(elapsed) : "");
  ui::set_text(remaining_, duration ? "-" + clock_of(duration - elapsed) : "");
  paint_wave_(elapsed, duration);

  paint_now_keys_(player);
  if (player != nullptr && player->volume_level >= 0 && dragging_ != volume_ &&
      (int32_t) (millis() - hold_volume_until_) >= 0) {
    if (lv_slider_get_value(volume_) != player->volume_level)
      lv_slider_set_value(volume_, player->volume_level, LV_ANIM_OFF);
    ui::set_text(percent_, std::to_string(player->volume_level) + "%");
  }
  paint_next_up_();
  paint_cover_();
}

void PlayerCard::paint_now_keys_(const MAPlayer *player) {
  const auto &now = plugin_->ma()->now_playing();
  const bool playing = player != nullptr && player->playback_state == "playing";
  ui::set_text(glyph_of(play_), ui::icon(playing ? ICON_PAUSE : ICON_PLAY));
  ink(glyph_of(shuffle_), now.shuffle ? ACCENT : MUTED);
  ui::set_text(glyph_of(repeat_), ui::icon(now.repeat == "one" ? ICON_REPEAT_ONE : ICON_REPEAT));
  ink(glyph_of(repeat_), now.repeat == "off" ? MUTED : ACCENT);
  ink(glyph_of(favorite_), now.favorite ? ACCENT : MUTED);
  const bool muted = player != nullptr && player->muted;
  ui::set_text(glyph_of(mute_), ui::icon(muted ? ICON_MUTED : ICON_VOLUME));
}

// The waveform: one bar per bin, what has played bright, the rest faint. Drawn again only when a bin turns.
void PlayerCard::paint_wave_(uint32_t elapsed, uint32_t duration) {
  const auto &now = plugin_->ma()->now_playing();
  const auto &bins = now.waveform;
  const bool has_wave = wave_ != nullptr && !bins.empty();
  hide(track_, has_wave);
  if (wave_ != nullptr) hide(wave_, !has_wave);
  if (!has_wave) {
    const int filled = duration ? (int) ((int64_t) wave_w_ * elapsed / duration) : 0;
    if (lv_obj_get_width(fill_bar_) != filled) lv_obj_set_width(fill_bar_, filled);
    return;
  }
  const int count = (int) bins.size();
  const int played = duration ? (int) ((int64_t) count * elapsed / duration) : 0;
  if (now.uri == wave_uri_ && played == wave_played_) return;
  wave_uri_ = now.uri;
  wave_played_ = played;
  auto *pixels = reinterpret_cast<uint32_t *>(wave_pixels_);
  std::fill(pixels, pixels + wave_w_ * wave_h_, 0u);
  const uint32_t bright = 0xFF000000u | TEXT, faint = 0x55000000u | TEXT;
  for (int b = 0; b < count; ++b) {
    const int x0 = b * wave_w_ / count, x1 = std::max(x0 + 1, (b + 1) * wave_w_ / count - 1);
    const int h = std::max(2, bins[b] * wave_h_ / 255), y0 = (wave_h_ - h) / 2;
    const uint32_t color = b < played ? bright : faint;
    for (int y = y0; y < y0 + h; ++y)
      for (int x = x0; x < x1 && x < wave_w_; ++x) pixels[y * wave_w_ + x] = color;
  }
  lv_obj_invalidate(wave_);
}

void PlayerCard::paint_next_up_() {
  auto *ma = plugin_->ma();
  const auto &queue = ma->queue();
  const int left = std::max(0, ma->queue_total_items() - ma->queue_current_index() - 1);
  ui::set_text(next_count_, left > 0 ? tessera::format(plugin_->text("tracks"), left) : "");
  for (int i = 0; i < NEXT_ROWS; ++i) {
    const int row = i + 1;  // row 0 of the queue is the track that plays
    const bool shown = row < (int) queue.size();
    ui::set_text(next_title_[i], shown ? queue[row].title : "");
    ui::set_text(next_artist_[i], shown ? queue[row].artist : "");
    CoverLoader &thumb = plugin_->thumb(TesseraMusic::MAX_ROWS + i);
    thumb.request(shown ? queue[row].image_url : "", next_thumb_, false);
    if (thumb.revision() == next_revision_[i]) continue;
    next_revision_[i] = thumb.revision();
    const lv_image_dsc_t *picture = thumb.picture();
    if (picture != nullptr) lv_image_set_src(next_image_[i], picture);
    hide(next_image_[i], picture == nullptr);
  }
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
  const int chosen = online ? ma->selected_player() : -1;
  const MAPlayer *leader = online ? leader_of(ma, ma->player(chosen)) : nullptr;
  for (int i = 0; i < MAX_ROOMS; ++i) {
    Room &r = rooms_[i];
    hide(r.root, i >= count);
    if (i >= count) continue;
    const MAPlayer *p = ma->player(i);
    if (i == chosen)
      selected(r.root);
    else
      glass(r.root);
    ui::set_text(r.name, p->display_name);
    const std::string words = p->title.empty() ? "" : p->artist.empty() ? p->title : p->title + " · " + p->artist;
    ui::set_text(r.sub, words.empty() ? plugin_->text("nothing") : words);
    ui::set_text(glyph_of(r.play), ui::icon(p->playback_state == "playing" ? ICON_PAUSE : ICON_PLAY));
    const bool lead = leader != nullptr && p->player_id == leader->player_id;
    hide(r.group, leader == nullptr || lead);
    ink(glyph_of(r.group), member_of(leader, p->player_id) ? ACCENT : MUTED);
    if (p->volume_level >= 0 && dragging_ != r.volume && (int32_t) (millis() - hold_volume_until_) >= 0 &&
        lv_slider_get_value(r.volume) != p->volume_level)
      lv_slider_set_value(r.volume, p->volume_level, LV_ANIM_OFF);
  }
}

void PlayerCard::paint_list_() {
  for (int s = 0; s < SHELVES; ++s) {
    const bool on = s == shelf_;
    fill(shelves_[s], ACCENT, on ? LV_OPA_COVER : LV_OPA_TRANSP);
    ink(shelf_text_[s], on ? ON_ACCENT : TEXT);
  }
  auto *ma = plugin_->ma();
  const bool online = ma != nullptr && ma->is_connected(), queue = shelf_ == 0;
  int rows = 0;
  if (online) rows = std::min((int) (queue ? ma->queue().size() : ma->library().size()), (int) TesseraMusic::MAX_ROWS);
  const bool fresh = online && (ma->revision() != list_revision_ || list_view_ != view_ || list_shelf_ != shelf_);
  if (!online) paint_more_(false, false, 0);
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
  paint_thumbs_(rows, 0);
}

void PlayerCard::paint_row_(int index, const std::string &title, const std::string &sub, const std::string &side,
                            const std::string &image, bool current) {
  Row &r = rows_[index];
  hide(r.root, false);
  if (current)
    selected(r.root);
  else
    glass(r.root);
  ui::set_text(r.title, title);
  ink(r.title, current ? ACCENT : TEXT);
  ui::set_text(r.sub, sub);
  ui::set_text(r.side, side);
  plugin_->thumb(index).request(image, thumb_side_, false);
}

void PlayerCard::paint_thumbs_(int rows, int first_thumb) {
  for (int i = 0; i < rows; ++i) {
    Row &r = rows_[i];
    CoverLoader &thumb = plugin_->thumb(first_thumb + i);
    if (thumb.revision() == r.cover_revision) continue;
    r.cover_revision = thumb.revision();
    const lv_image_dsc_t *picture = thumb.picture();
    if (picture != nullptr) lv_image_set_src(r.image, picture);
    hide(r.image, picture == nullptr);
    hide(r.note, picture != nullptr);
  }
}

void PlayerCard::paint_more_(bool more, bool loading, int rows) {
  hide(more_, !more && !loading);
  lv_obj_set_y(more_, rows * row_step_);
  ui::set_text(more_text_, plugin_->text(loading ? "loading" : "more"));
  hide(empty_, rows > 0);
  if (rows > 0) return;
  auto *ma = plugin_->ma();
  const bool online = ma != nullptr && ma->is_connected();
  const char *key = !online ? "connecting" : loading ? "loading" : shelf_ == 0 ? "queue_empty" : "list_empty";
  ui::set_text(empty_, plugin_->text(key));
}

}  // namespace esphome::tessera_music
