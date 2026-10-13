#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "lvgl.h"

namespace esphome::tessera_music {

// Loads one cover from Music Assistant's image proxy on the home network and decodes it with the ESP32-P4's JPEG
// hardware into an RGB565 picture for LVGL. The download never waits: poll() moves it on a step per loop pass.
// Two pictures take turns, so the one LVGL draws is never written while a new one is decoded. Every loader shares one
// download buffer, so one downloads at a time and the others wait their turn.
class CoverLoader {
 public:
  ~CoverLoader();
  // Asks for `url` at a side of `side` pixels; the same request again does nothing. `keep`: the picture there stays
  // until the new one is ready (a cover that changes), or goes at once (a row that shows something else now).
  void request(const std::string &url, int side, bool keep = true);
  void poll();
  // The picture to show, nullptr while there is none. Changes when a new one is ready (`revision`).
  const lv_image_dsc_t *picture() const { return shown_ < 0 ? nullptr : &slots_[shown_].dsc; }
  uint32_t revision() const { return revision_; }
  // The cover's mean colour as 0xRRGGBB, for a glow behind it; 0 while there is no picture.
  uint32_t tone() const { return shown_ < 0 ? 0 : tone_; }
  void clear();
  // Clears and gives the pixels back.
  void release();

 protected:
  enum class Step : uint8_t { IDLE, CONNECTING, SENDING, RECEIVING };

  struct Slot {
    uint8_t *pixels{nullptr};
    size_t capacity{0};
    lv_image_dsc_t dsc{};
  };

  bool start_();
  void fail_(const char *why);
  void close_();
  bool decode_();

  std::string wanted_url_, loaded_url_, host_, path_;
  uint16_t port_{80};
  int side_{0};
  Step step_{Step::IDLE};
  int fd_{-1};
  uint32_t started_ms_{0}, retry_after_ms_{0};
  size_t sent_{0};
  std::string head_;
  Slot slots_[2];
  int shown_{-1};
  uint32_t revision_{0}, tone_{0};
};

}  // namespace esphome::tessera_music
