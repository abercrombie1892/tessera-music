#include "ma_widgets.h"
#include "ma_theme.h"

#include "esphome/core/hal.h"

#include <cstring>

namespace esphome {
namespace music_assistant {
namespace widgets {

using namespace theme;

static constexpr uint32_t DOWNLOAD_GAP_MS = 600;

std::string to_upper(std::string text) {
  for (size_t i = 0; i < text.size(); i++) {
    const auto c = (unsigned char) text[i];
    if (c >= 'a' && c <= 'z') {
      text[i] = (char) (c - 0x20);
    } else if (c == 0xC3 && i + 1 < text.size()) {
      // Latin-1 lower case à..þ is U+00E0..U+00FE, its upper case 0x20 below; ÷ (U+00F7) has none.
      const auto next = (unsigned char) text[i + 1];
      if (next >= 0xA0 && next <= 0xBE && next != 0xB7) text[i + 1] = (char) (next - 0x20);
      i++;
    }
  }
  return text;
}

bool download_slot() {
  static uint32_t last_ms = 0;
  static bool started = false;
  if (started && millis() - last_ms < DOWNLOAD_GAP_MS) return false;
  started = true;
  last_ms = millis();
  return true;
}

std::string printable(const char *text, const lv_font_t *font) {
  bool plain = true;
  for (const char *p = text; *p; p++)
    if ((unsigned char) *p >= 0x80) {
      plain = false;
      break;
    }
  if (plain || font == nullptr) return text;
  std::string out;
  bool gap = false;
  const auto *p = reinterpret_cast<const unsigned char *>(text);
  while (*p) {
    uint32_t cp;
    int n;
    if (*p < 0x80) {
      cp = *p, n = 1;
    } else if ((*p & 0xE0) == 0xC0 && p[1]) {
      cp = ((*p & 0x1F) << 6) | (p[1] & 0x3F), n = 2;
    } else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) {
      cp = ((*p & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F), n = 3;
    } else if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
      cp = ((*p & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F), n = 4;
    } else {
      p++;
      continue;
    }
    lv_font_glyph_dsc_t dsc;
    if (cp < 0x80 || lv_font_get_glyph_dsc(font, &dsc, cp, 0)) {
      out.append(reinterpret_cast<const char *>(p), n);
      gap = false;
    } else if (!gap) {
      out += '?';
      gap = true;
    }
    p += n;
  }
  return out;
}

void set_text(lv_obj_t *lbl, const char *text) {
  const std::string clean = printable(text, lv_obj_get_style_text_font(lbl, LV_PART_MAIN));
  const char *cur = lv_label_get_text(lbl);
  if (cur == nullptr || strcmp(cur, clean.c_str()) != 0) lv_label_set_text(lbl, clean.c_str());
}

void set_visible(lv_obj_t *obj, bool visible) {
  if (visible == lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN))
    (visible ? lv_obj_remove_flag : lv_obj_add_flag)(obj, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, int x, int y, int width) {
  lv_obj_t *lbl = lv_label_create(parent);
  lv_obj_set_style_text_font(lbl, font, LV_PART_MAIN);
  lv_obj_set_style_text_color(lbl, lv_color_hex(color), LV_PART_MAIN);
  lv_obj_set_pos(lbl, x, y);
  if (width > 0) {
    // one line, cut with "…": DOTS only applies to a fixed height — with the
    // default content height long titles wrapped onto the row below
    lv_obj_set_size(lbl, width, lv_font_get_line_height(font));
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_MODE_DOTS);
  }
  lv_label_set_text(lbl, "");
  return lbl;
}

// Theme-free box: no default padding, shadow or scrollbars (see the
// scrollbar stripes on screen 1).
lv_obj_t *make_box(lv_obj_t *parent, int x, int y, int w, int h, int radius) {
  lv_obj_t *box = lv_obj_create(parent);
  lv_obj_remove_style_all(box);
  lv_obj_set_pos(box, x, y);
  lv_obj_set_size(box, w, h);
  lv_obj_set_style_radius(box, radius, LV_PART_MAIN);
  lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
  return box;
}

lv_obj_t *make_icon(lv_obj_t *parent, const lv_font_t *font, const char *glyph, uint32_t color) {
  lv_obj_t *icon = make_label(parent, font, color, 0, 0);
  lv_label_set_text(icon, glyph);
  lv_obj_center(icon);
  return icon;
}

// Style setters invalidate even when nothing changes, and the screens
// refresh every second — only touch what differs.
static bool same_color(lv_color_t current, uint32_t color) {
  return (lv_color_to_u32(current) & 0xFFFFFF) == color;
}

void style_fill(lv_obj_t *obj, uint32_t color, uint8_t opa) {
  if (!same_color(lv_obj_get_style_bg_color(obj, LV_PART_MAIN), color))
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), LV_PART_MAIN);
  if (lv_obj_get_style_bg_opa(obj, LV_PART_MAIN) != opa) lv_obj_set_style_bg_opa(obj, opa, LV_PART_MAIN);
}

void style_border(lv_obj_t *obj, int width, uint32_t color, uint8_t opa) {
  if (lv_obj_get_style_border_width(obj, LV_PART_MAIN) != width)
    lv_obj_set_style_border_width(obj, width, LV_PART_MAIN);
  if (!same_color(lv_obj_get_style_border_color(obj, LV_PART_MAIN), color))
    lv_obj_set_style_border_color(obj, lv_color_hex(color), LV_PART_MAIN);
  if (lv_obj_get_style_border_opa(obj, LV_PART_MAIN) != opa)
    lv_obj_set_style_border_opa(obj, opa, LV_PART_MAIN);
}

void style_glass(lv_obj_t *obj) {
  style_fill(obj, COL_TEXT, OPA_GLASS);
  style_border(obj, 1, COL_TEXT, OPA_GLASS_BORDER);
}

void style_selected(lv_obj_t *obj) {
  style_fill(obj, COL_ACCENT, OPA_SELECTED);
  style_border(obj, 2, COL_ACCENT, LV_OPA_COVER);
}

// pressed feedback per spec: glass +10 % opacity
void add_press_feedback(lv_obj_t *obj, uint8_t pressed_opa) {
  lv_obj_set_style_bg_opa(obj, pressed_opa, LV_PART_MAIN | LV_STATE_PRESSED);
}

void style_slider(lv_obj_t *s) {
  lv_obj_set_style_bg_opa(s, 36, LV_PART_MAIN);
  lv_obj_set_style_bg_color(s, lv_color_hex(COL_TEXT), LV_PART_MAIN);
  lv_obj_set_style_bg_color(s, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(s, lv_color_hex(COL_TEXT), LV_PART_KNOB);
  lv_obj_set_style_pad_all(s, 6, LV_PART_KNOB);
  lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, LV_PART_KNOB);
  lv_obj_set_style_shadow_width(s, 0, LV_PART_KNOB);
  lv_obj_set_ext_click_area(s, 18);  // the 10 px track alone is too thin a touch target
}

}  // namespace widgets
}  // namespace music_assistant
}  // namespace esphome
