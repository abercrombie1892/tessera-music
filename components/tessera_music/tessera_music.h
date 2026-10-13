#pragma once
#include "esphome/core/component.h"
#include "esphome/components/smart_display/plugin_api.h"
#include "esphome/components/music_assistant/music_assistant.h"
#include "cover_loader.h"

namespace esphome::tessera_music {

// The plugin: Music Assistant on a Tessera screen. The screen talks to Music Assistant itself (music_assistant
// component, a WebSocket on the home network) and loads the covers from its image proxy; Tessera only gives it room.
class TesseraMusic : public Component, public tessera::Plugin {
 public:
  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_music_assistant(music_assistant::MusicAssistant *ma) { ma_ = ma; }
  music_assistant::MusicAssistant *ma() const { return ma_; }
  CoverLoader &cover() { return cover_; }
  // A tile on the glass says so every second: Music Assistant is asked often only while one is.
  void seen() { seen_ms_ = millis(); }

 protected:
  music_assistant::MusicAssistant *ma_{nullptr};
  CoverLoader cover_;
  uint32_t seen_ms_{0};
};

}  // namespace esphome::tessera_music
