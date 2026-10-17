#include "scene_tile.h"
#include <cstring>
#include <string>
#include <vector>
#include "esp_heap_caps.h"
#include "ground.h"
#include "scene_colors.h"
#include "tessera_music.h"

// private LVGL header (misc/cache/instance/lv_image_cache.h), not in lvgl.h
extern "C" void lv_image_cache_drop(const void *src);

namespace esphome::tessera_music {

using tessera::Font;
namespace ui = tessera::ui;

namespace {

constexpr uint32_t WHITE = 0xFFFFFF;
// Less than this of the picture's own light on a light that is off: the tile is dimmed, not dark.
constexpr uint8_t OFF_DIM = 120;

// "#RRGGBB, #RRGGBB, ..." (up to three) into colours; anything that is not a colour is left out.
std::vector<uint32_t> parse_colors(const std::string &text) {
  std::vector<uint32_t> colors;
  size_t at = 0;
  while (colors.size() < 3 && at < text.size()) {
    const size_t hash = text.find('#', at);
    if (hash == std::string::npos || hash + 7 > text.size()) break;
    char *end = nullptr;
    const std::string hex = text.substr(hash + 1, 6);
    const unsigned long value = strtoul(hex.c_str(), &end, 16);
    if (end != nullptr && *end == '\0') colors.push_back((uint32_t) value);
    at = hash + 7;
  }
  return colors;
}

// A scene without a colour of its own: two hues from its name, always the same ones.
std::vector<uint32_t> colors_from_name(const std::string &entity) {
  uint32_t h = 2166136261u;  // FNV-1a
  for (unsigned char c : entity) h = (h ^ c) * 16777619u;
  const float hue = (h % 360) / 360.0f, hue2 = std::fmod(hue + 0.12f + ((h >> 9) % 10) / 100.0f, 1.0f);
  const auto rgb = [](float hh) {
    const float r = std::clamp(std::fabs(hh * 6 - 3) - 1, 0.0f, 1.0f), g = std::clamp(2 - std::fabs(hh * 6 - 2), 0.0f, 1.0f),
                b = std::clamp(2 - std::fabs(hh * 6 - 4), 0.0f, 1.0f);
    return ((uint32_t) (255 * (0.35f + 0.65f * r)) << 16) | ((uint32_t) (255 * (0.35f + 0.65f * g)) << 8) |
           (uint32_t) (255 * (0.35f + 0.65f * b));
  };
  return {rgb(hue), rgb(hue2)};
}

void text_color(lv_obj_t *label, uint32_t color) {
  if (!lv_color_eq(lv_obj_get_style_text_color(label, LV_PART_MAIN), lv_color_hex(color)))
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
}

}  // namespace

class SceneTile : public tessera::Tile {
 public:
  explicit SceneTile(TesseraMusic *plugin) : plugin_(plugin) {}
  ~SceneTile() override {
    if (ground_px_ == nullptr) return;
    if (card_ != nullptr && lv_obj_is_valid(card_)) lv_obj_set_style_bg_image_src(card_, nullptr, 0);
    lv_image_cache_drop(&ground_);
    heap_caps_free(ground_px_);
  }

  void create(const tessera::TileContext &c) override {
    card_ = lv_obj_get_parent(c.parent);
    entity_ = c.entity != nullptr ? c.entity : "";
    name_ = c.name != nullptr ? c.name : "";
    custom_ = parse_colors(c.options["colors"] | "");
    light_ = entity_.rfind("light.", 0) == 0;
    const bool large = c.height >= ui::mm(24);
    const Font font = large ? Font::TITLE : Font::BODY_LARGE;

    state_ = ui::label(c.parent, Font::BODY, theme::INK);
    lv_obj_set_pos(state_, ui::px(2), ui::px(2));
    lv_obj_set_size(state_, c.width - ui::px(4), ui::line_height(Font::BODY));
    title_ = ui::label(c.parent, font, theme::INK);
    lv_obj_set_pos(title_, ui::px(2), c.height - ui::line_height(font) - ui::px(2));
    lv_obj_set_size(title_, c.width - ui::px(4), ui::line_height(font));
    ui::set_text(title_, name_);
    paint_();
  }

  void on_state(JsonObjectConst data) override {
    const std::string state = data["state"] | "";
    on_ = light_ && state == "on";
    unavailable_ = state == "unavailable";
    brightness_ = light_ && data["attributes"]["brightness"].is<int>() ? (int) data["attributes"]["brightness"] : -1;
    light_color_ = 0;
    JsonArrayConst rgb = data["attributes"]["rgb_color"];
    if (!rgb.isNull() && rgb.size() == 3)
      light_color_ = ((uint32_t) (int) rgb[0] << 16) | ((uint32_t) (int) rgb[1] << 8) | (uint32_t) (int) rgb[2];
    if (name_.empty()) {
      const std::string name = data["name"] | "";
      ui::set_text(title_, name);
    }
    ui::set_text(state_, state_text_());
    paint_();
  }

  // Tessera colours its cards again when it draws them: every tick puts the ground back (only when it is not there).
  void on_tick(uint32_t) override { paint_(); }

  void on_theme() override { paint_(); }

  void on_tap() override {
    if (entity_.rfind("scene.", 0) == 0)
      tessera::action("scene.turn_on", entity_);
    else if (light_)
      tessera::action("light.toggle", entity_);
  }

 protected:
  std::string state_text_() const {
    if (!light_) return "";
    if (!on_) return plugin_->text("off");
    std::string text = plugin_->text("on");
    if (brightness_ >= 0) text += " · " + std::to_string(brightness_ * 100 / 255) + " %";
    return text;
  }

  std::vector<uint32_t> colors_() const {
    if (!custom_.empty()) return custom_;
    for (const SceneColors &s : SCENE_COLORS)
      if (entity_ == s.entity) return std::vector<uint32_t>(s.colors, s.colors + s.count);
    if (light_ && light_color_ != 0) return {light_color_};
    if (light_) return {0xFFB060};  // a light without a colour: warm white
    return colors_from_name(entity_);
  }

  void paint_() {
    if (card_ == nullptr || !lv_obj_is_valid(card_)) return;
    text_color(title_, WHITE);
    text_color(state_, WHITE);
    const int w = lv_obj_get_width(card_), h = lv_obj_get_height(card_);
    if (w <= 0 || h <= 0) return;
    const std::vector<uint32_t> colors = colors_();
    uint32_t sign = (uint32_t) w * 31 + h;
    for (uint32_t c : colors) sign = sign * 16777619u ^ c;
    if (sign != signature_) {
      if (!make_ground_(w, h, colors)) return;
      signature_ = sign;
      lv_obj_set_style_bg_image_src(card_, nullptr, 0);
      lv_obj_invalidate(card_);
    }
    if (lv_obj_get_style_bg_image_src(card_, LV_PART_MAIN) != &ground_) lv_obj_set_style_bg_image_src(card_, &ground_, 0);
    if (lv_obj_get_style_bg_grad_dir(card_, LV_PART_MAIN) != LV_GRAD_DIR_NONE)
      lv_obj_set_style_bg_grad_dir(card_, LV_GRAD_DIR_NONE, 0);
    // a light that is off, or gone, is dimmed; one that is on and a scene shine
    const uint8_t dim = unavailable_ ? 170 : (light_ && !on_) ? OFF_DIM : 0;
    if (lv_obj_get_style_bg_image_recolor_opa(card_, LV_PART_MAIN) != dim) {
      lv_obj_set_style_bg_image_recolor(card_, lv_color_hex(0x000000), 0);
      lv_obj_set_style_bg_image_recolor_opa(card_, dim, 0);
    }
  }

  // The gradient from the top left to the foot right through the colours, each as a lit ground for white words.
  bool make_ground_(int w, int h, const std::vector<uint32_t> &colors) {
    const size_t bytes = (size_t) w * h * 2;
    if (bytes > ground_cap_) {
      lv_obj_set_style_bg_image_src(card_, nullptr, 0);
      heap_caps_free(ground_px_);
      ground_px_ = static_cast<uint8_t *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM));
      ground_cap_ = ground_px_ == nullptr ? 0 : bytes;
      if (ground_px_ == nullptr) return false;
    }
    std::vector<lv_color_t> stops;
    if (colors.size() == 1) {
      stops = {lit(colors[0], 150), lit(colors[0], 62)};  // one colour: light to dark
    } else {
      for (size_t i = 0; i < colors.size(); i++) stops.push_back(lit(colors[i], i + 1 == colors.size() ? 105 : 140));
    }
    const int last = (int) stops.size() - 1;
    dither(reinterpret_cast<uint16_t *>(ground_px_), w, h, [&](int x, int y, float *want) {
      const float t = std::min(1.0f, 0.55f * y / std::max(1, h - 1) + 0.45f * x / std::max(1, w - 1));
      const float pos = t * last;
      const int i = std::min(last - 1, (int) pos);
      const float f = pos - i;
      want[0] = stops[i].red + (stops[i + 1].red - stops[i].red) * f;
      want[1] = stops[i].green + (stops[i + 1].green - stops[i].green) * f;
      want[2] = stops[i].blue + (stops[i + 1].blue - stops[i].blue) * f;
    });
    lv_image_cache_drop(&ground_);
    ground_.header.magic = LV_IMAGE_HEADER_MAGIC;
    ground_.header.cf = LV_COLOR_FORMAT_RGB565;
    ground_.header.w = w;
    ground_.header.h = h;
    ground_.header.stride = w * 2;
    ground_.data = ground_px_;
    ground_.data_size = bytes;
    return true;
  }

  TesseraMusic *plugin_;
  lv_obj_t *card_{}, *title_{}, *state_{};
  std::string entity_, name_;
  std::vector<uint32_t> custom_;
  bool light_{false}, on_{false}, unavailable_{false};
  int brightness_{-1};
  uint32_t light_color_{0}, signature_{0};
  lv_image_dsc_t ground_{};
  uint8_t *ground_px_{nullptr};
  size_t ground_cap_{0};
};

tessera::Tile *make_scene_tile(TesseraMusic *plugin) { return new SceneTile(plugin); }

}  // namespace esphome::tessera_music
