#pragma once
// Classic guard on top: the panel config also pulls this header into src/ via
// esphome.includes, and #pragma once can't tell the two copies apart.
#ifndef ESPHOME_MA_DASHBOARD_UI_H_
#define ESPHOME_MA_DASHBOARD_UI_H_

// LVGL UI layer for the Music Assistant dashboard (Screen 1 "Jetzt läuft").
// Static skeleton lives in the device lvgl.yaml; this file owns everything
// dynamic. Pure LVGL + MusicAssistant* — all espcontrol interop (screensaver
// pause, wake guard) is injected as std::function hooks from device.yaml,
// where id() is available.

#include "esphome/components/music_assistant/music_assistant.h"
#include "esphome/components/music_assistant/ma_rooms_ui.h"
#include "esphome/components/music_assistant/ma_queue_ui.h"

#include "esphome/components/lvgl/lvgl_esphome.h"
#include "esphome/components/music_assistant/ma_artwork.h"

#include <functional>

namespace esphome {
namespace music_assistant {
namespace ma_ui {

struct Handles {
  lv_obj_t *dashboard = nullptr;
  lv_obj_t *screen1 = nullptr;
  lv_obj_t *screen2 = nullptr;
  lv_obj_t *grid_btn = nullptr;  // topbar grid button -> screen 2
  lv_obj_t *library_btn = nullptr;  // topbar library button -> screen 3, playlists
  lv_obj_t *screen3 = nullptr;
  lv_obj_t *queue_btn = nullptr;  // "Als Nächstes" list button -> screen 3
  ma_rooms::Fonts fonts;
  lv_obj_t *glow_img = nullptr;  // full-screen glow rendered from the cover's colors
  // topbar
  lv_obj_t *back_btn = nullptr;
  lv_obj_t *player_pill = nullptr;
  lv_obj_t *player_name = nullptr;
  lv_obj_t *group_badge = nullptr;
  lv_obj_t *group_badge_lbl = nullptr;
  lv_obj_t *clock_lbl = nullptr;
  lv_obj_t *clock_src = nullptr;  // top-layer fork clock label (text source)
  // cover
  lv_obj_t *cover_ph = nullptr;
  lv_obj_t *cover_img = nullptr;
  // right column
  lv_obj_t *status_dot = nullptr;
  lv_obj_t *fav_btn = nullptr;
  lv_obj_t *fav_icon = nullptr;
  const lv_font_t *title_font_small = nullptr;  // fallback when the 52 px title overflows
  lv_obj_t *status_lbl = nullptr;
  lv_obj_t *title = nullptr;
  lv_obj_t *artist = nullptr;
  lv_obj_t *album = nullptr;
  lv_obj_t *progress = nullptr;
  lv_obj_t *time_cur = nullptr;
  lv_obj_t *time_rem = nullptr;
  // transport
  lv_obj_t *shuffle_btn = nullptr;
  lv_obj_t *shuffle_icon = nullptr;
  lv_obj_t *prev_btn = nullptr;
  lv_obj_t *play_btn = nullptr;
  lv_obj_t *play_icon = nullptr;
  lv_obj_t *next_btn = nullptr;
  lv_obj_t *repeat_btn = nullptr;
  lv_obj_t *repeat_icon = nullptr;
  // volume
  lv_obj_t *mute_btn = nullptr;
  lv_obj_t *mute_icon = nullptr;
  lv_obj_t *vol = nullptr;
  lv_obj_t *vol_pct = nullptr;
  // next up
  lv_obj_t *next_count = nullptr;
  lv_obj_t *tiles[4] = {nullptr, nullptr, nullptr, nullptr};
  lv_obj_t *tile_title[4] = {nullptr, nullptr, nullptr, nullptr};
  lv_obj_t *tile_artist[4] = {nullptr, nullptr, nullptr, nullptr};
  lv_obj_t *tile_img[4] = {nullptr, nullptr, nullptr, nullptr};
  // artwork pipeline
  artwork_image::ArtworkImage *cover_dl = nullptr;
  artwork_image::ArtworkImage *tile_dl[4] = {nullptr, nullptr, nullptr, nullptr};
  artwork_image::ArtworkImage *card_dl[ma_rooms::COVER_SLOTS] = {};
  artwork_image::ArtworkImage *row_dl[ma_queue::THUMB_SLOTS] = {};
};

/// One-time wiring: widget handles + espcontrol interop hooks.
void bind(MusicAssistant *client, const Handles &h,
          std::function<void()> hold_screensaver,      // keep cover art away while open
          std::function<void()> return_to_screensaver, // close: hand screen back
          std::function<void()> clear_wake_guard);     // touch guard off

bool bound();
void open(int screen = 1);  // reveal the dashboard on this screen (1 now playing, 2 rooms, 3 queue)
void close();   // hide the dashboard, re-arm the screensaver
void refresh(); // 1s tick: interop, revision-gated data, progress, clock

}  // namespace ma_ui
}  // namespace music_assistant
}  // namespace esphome

#endif  // ESPHOME_MA_DASHBOARD_UI_H_
