#pragma once

// The dashboard's static skeleton, as the espcontrol P4-86 device built it from ma_dashboard.yaml (lvgl top_layer),
// made in C++ with the same places, sizes, colours and fonts. ma_ui::bind() then wires it up as before.

#include "esphome/components/music_assistant/ma_dashboard_ui.h"

namespace esphome {
namespace music_assistant {
namespace ma_skeleton {

// The fonts of ma_dashboard.yaml: Outfit 14/17/20/26/30/34 and the MDI icons 24/28/34/48; text52 is the panel's
// title (none: the 720 x 720 layout everywhere).
struct Fonts {
  const lv_font_t *text14, *text17, *text20, *text26, *text30, *text34, *icon24, *icon28, *icon34, *icon48;
  const lv_font_t *text52;
};

// Makes the hidden dashboard (1280 x 800 on a panel that large, else 720 x 720) on `parent` and fills the handles
// bind() takes; `clock` is a label the caller keeps on the time (the YAML's display_time).
ma_ui::Handles build(lv_obj_t *parent, const Fonts &fonts, lv_obj_t *clock);

}  // namespace ma_skeleton
}  // namespace music_assistant
}  // namespace esphome
