#pragma once

// Screen 2 "Räume & Player": one card per (filtered) MA player plus a group
// bar. Everything is created in C++ inside the YAML ma_screen2 container.

#include "esphome/components/music_assistant/music_assistant.h"
#include "esphome/components/music_assistant/ma_widgets.h"

#include "esphome/components/lvgl/lvgl_esphome.h"
#include "esphome/components/music_assistant/ma_artwork.h"

#include <functional>

namespace esphome {
namespace music_assistant {
namespace ma_rooms {

static constexpr int MAX_CARDS = 9;    // 3x3 grid
static constexpr int COVER_SLOTS = 4;  // cover downloaders, shared by the cards that have media

using Fonts = widgets::Fonts;

struct Hooks {
  std::function<void()> go_back;  // show screen 1
  std::function<void()> touched;  // clear the screensaver wake guard
};

// compact: 720x720 list layout instead of the 3x3 panel cards
void build(lv_obj_t *screen, MusicAssistant *client, const Fonts &fonts,
           artwork_image::ArtworkImage *const cover_dl[COVER_SLOTS], bool compact, Hooks hooks);
void refresh();                      // player data changed or screen shown
void tick(const char *clock_text);   // 1 s while visible: clock + cover arrivals

}  // namespace ma_rooms
}  // namespace music_assistant
}  // namespace esphome
