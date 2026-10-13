#pragma once
#include <string>
#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/components/smart_display/plugin_api.h"
#include "esphome/components/music_assistant/music_assistant.h"
#include "cover_loader.h"

namespace esphome::tessera_music {

// The plugin: Music Assistant on a Tessera screen. The screen talks to Music Assistant itself (music_assistant
// component, a WebSocket on the home network) and loads the covers from its image proxy; Tessera only gives it room.
class TesseraMusic : public Component, public tessera::Plugin {
 public:
  // The rows of a list on the player card, and so the small covers it loads.
  static constexpr int MAX_ROWS = 80;

  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_music_assistant(music_assistant::MusicAssistant *ma) { ma_ = ma; }
  music_assistant::MusicAssistant *ma() const { return ma_; }
  CoverLoader &cover() { return cover_; }
  CoverLoader &card_cover() { return card_cover_; }
  CoverLoader &thumb(int row);
  // A tile or the card on the glass says so every second: Music Assistant is asked often only while one is.
  void seen() { seen_ms_ = millis(); }
  // A tile keeps its room; while the card is open, the room chosen there.
  void follow_room(const std::string &name);
  void card_opened() { card_open_ = true; }
  void card_closed();

 protected:
  music_assistant::MusicAssistant *ma_{nullptr};
  CoverLoader cover_, card_cover_;
  CoverLoader *thumbs_{nullptr};
  uint32_t seen_ms_{0};
  bool card_open_{false};
};

}  // namespace esphome::tessera_music
