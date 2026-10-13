#pragma once

// Screen 3 "Warteschlange": now-playing column + scrollable list with tabs for
// the queue, playlists, radio stations and favourites.
// Everything is created in C++ inside the YAML ma_screen3 container.

#include "esphome/components/music_assistant/music_assistant.h"
#include "esphome/components/music_assistant/ma_widgets.h"

#include "esphome/components/lvgl/lvgl_esphome.h"
#include "esphome/components/music_assistant/ma_artwork.h"

#include <functional>

namespace esphome {
namespace music_assistant {
namespace ma_queue {

// Thumbnail downloaders, re-assigned to whichever rows are on screen.
static constexpr int THUMB_SLOTS = 8;

struct Hooks {
  std::function<void()> go_back;  // show screen 1
  std::function<void()> touched;  // clear the screensaver wake guard
};

void build(lv_obj_t *screen, MusicAssistant *client, const widgets::Fonts &fonts,
           artwork_image::ArtworkImage *cover_dl,
           artwork_image::ArtworkImage *const thumb_dl[THUMB_SLOTS],
           bool compact, Hooks hooks);  // compact: 720x720, no now-playing column
void shown();                        // screen became visible
void release();                      // dashboard closed: drop pending fetches
void refresh();                      // data changed
void tick(const char *clock_text);   // 1 s while visible: clock, progress, covers

}  // namespace ma_queue
}  // namespace music_assistant
}  // namespace esphome
