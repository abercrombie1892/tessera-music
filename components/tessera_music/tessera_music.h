#pragma once
#include <string>
#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/components/font/font.h"
#include "esphome/components/smart_display/plugin_api.h"
#include "esphome/components/music_assistant/music_assistant.h"
#include "esphome/components/music_assistant/cover_loader.h"

namespace esphome::tessera_music {

// The plugin: Music Assistant on a Tessera screen. The screen talks to Music Assistant itself (music_assistant
// component, a WebSocket on the home network) and loads the covers from its image proxy; Tessera only gives it room.
// A tap on the Now playing tile opens the espcontrol MA dashboard over the whole glass.
class TesseraMusic : public Component, public tessera::Plugin {
 public:
  static constexpr int FONTS = 10;

  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_music_assistant(music_assistant::MusicAssistant *ma) { ma_ = ma; }
  void set_font(int index, font::Font *font) { fonts_[index] = font; }
  music_assistant::MusicAssistant *ma() const { return ma_; }
  CoverLoader &cover() { return cover_; }
  // A tile on the glass says so every second: Music Assistant is asked often only while one is, or the dashboard.
  void seen() { seen_ms_ = millis(); }
  // A tile keeps its room; while the dashboard is open, the room chosen there.
  void follow_room(const std::string &name);
  void open_dashboard();
  void dashboard_closed() { dashboard_open_ = false; }

 protected:
  bool build_dashboard_();

  music_assistant::MusicAssistant *ma_{nullptr};
  font::Font *fonts_[FONTS]{};
  CoverLoader cover_;
  lv_obj_t *clock_{nullptr};
  uint32_t seen_ms_{0}, clock_ms_{0};
  bool built_{false}, dashboard_open_{false};
};

}  // namespace esphome::tessera_music
