#pragma once

// Theme-free LVGL building blocks shared by the dashboard screens built in C++.

#include "esphome/components/lvgl/lvgl_esphome.h"
#include "esphome/components/music_assistant/ma_artwork.h"

#include <cstdint>

namespace esphome {
namespace music_assistant {
namespace widgets {

struct Fonts {
  const lv_font_t *text14 = nullptr, *text17 = nullptr, *text20 = nullptr, *text26 = nullptr,
                  *text30 = nullptr, *icon24 = nullptr, *icon28 = nullptr, *icon34 = nullptr;
};

void set_text(lv_obj_t *lbl, const char *text);  // no-op when unchanged
void set_visible(lv_obj_t *obj, bool visible);
// width > 0: single line of that width, cut with "…"
lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, int x, int y, int width = 0);
lv_obj_t *make_box(lv_obj_t *parent, int x, int y, int w, int h, int radius);
lv_obj_t *make_icon(lv_obj_t *parent, const lv_font_t *font, const char *glyph, uint32_t color);
void style_fill(lv_obj_t *obj, uint32_t color, uint8_t opa);
void style_border(lv_obj_t *obj, int width, uint32_t color, uint8_t opa);
void style_glass(lv_obj_t *obj);
void style_selected(lv_obj_t *obj);
void add_press_feedback(lv_obj_t *obj, uint8_t pressed_opa);
void style_slider(lv_obj_t *s);

// Paces every MA image request (all screens share it): true = the caller may
// start one download now. Each download + HW decode competes with the
// display scan-out for the PSRAM bus; a burst of them flashed the 10" panel.
bool download_slot();

// True only when the downloader's buffer really holds `url`. get_url() moves
// to a queued URL before its download starts (shared single-socket image
// service), which showed the previous picture under the new title.
inline bool image_ready(const artwork_image::ArtworkImage *dl, const std::string &url) {
  return dl != nullptr && !url.empty() && dl->has_image() && dl->get_loaded_url() == url;
}

}  // namespace widgets
}  // namespace music_assistant
}  // namespace esphome
