#pragma once
#include <algorithm>
#include <cmath>
#include <vector>
#include "esphome/components/smart_display/plugin_api.h"

namespace esphome::tessera_music {

// A cover's colour as a lit ground for white words (the dashboard's glow_color): saturated a little more, and never
// brighter than `peak`, less for a grey cover.
inline lv_color_t lit(uint32_t tone, float peak) {
  float r = (tone >> 16) & 0xFF, g = (tone >> 8) & 0xFF, b = tone & 0xFF;
  const float hi = std::max({r, g, b, 1.0f}), lo = std::min({r, g, b}), avg = (r + g + b) / 3;
  r = std::max(0.0f, avg + (r - avg) * 1.2f), g = std::max(0.0f, avg + (g - avg) * 1.2f),
  b = std::max(0.0f, avg + (b - avg) * 1.2f);
  peak *= 0.4f + 0.6f * std::min(1.0f, (hi - lo) / hi * 2.0f);
  const float scale = peak / std::max({r, g, b, 1.0f});
  return lv_color_make((uint8_t) (r * scale), (uint8_t) (g * scale), (uint8_t) (b * scale));
}

// A picture of `w` x `h` whose colour at (x, y) `want` gives, Floyd-Steinberg dithered into RGB565 as the
// dashboard's glow is, so a soft blend shows no bands.
template <class Want> inline void dither(uint16_t *pixels, int w, int h, Want want_at) {
  std::vector<float> rows(6 * (w + 2), 0.0f);
  float *cur = rows.data(), *next = cur + 3 * (w + 2);
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      float want[3];
      want_at(x, y, want);
      const int levels[3] = {31, 63, 31};
      int q[3];
      for (int c = 0; c < 3; c++) {
        const int i = (x + 1) * 3 + c;
        const float v = std::clamp(want[c] + cur[i], 0.0f, 255.0f);
        q[c] = (int) (v * levels[c] / 255.0f + 0.5f);
        const float e = v - q[c] * 255.0f / levels[c];
        cur[i + 3] += e * 7 / 16;
        next[i - 3] += e * 3 / 16;
        next[i] += e * 5 / 16;
        next[i + 3] += e / 16;
      }
      pixels[y * w + x] = (uint16_t) ((q[0] << 11) | (q[1] << 5) | q[2]);
    }
    std::swap(cur, next);
    std::fill(next, next + 3 * (w + 2), 0.0f);
  }
}

}  // namespace esphome::tessera_music
