#pragma once

// Design tokens shared by all dashboard screens (GLM-PROMPT spec, section 5).

#include <cstdint>

namespace esphome {
namespace music_assistant {
namespace theme {

static constexpr uint32_t COL_BG = 0x0F1012;
static constexpr uint32_t COL_TEXT = 0xF4F1EC;
static constexpr uint32_t COL_TEXT2 = 0xE6E1D9;
static constexpr uint32_t COL_MUTED = 0xB3AEA6;
static constexpr uint32_t COL_DIM = 0xA9A49C;
static constexpr uint32_t COL_ACCENT = 0xE8945A;
static constexpr uint32_t COL_ON_ACCENT = 0x1A1208;
static constexpr uint32_t COL_PLAYING = 0x7EE2A8;
static constexpr uint32_t COL_GRAY = 0x8D8880;

static constexpr uint8_t OPA_GLASS = 18;         // white 7 %
static constexpr uint8_t OPA_GLASS_BORDER = 28;  // white 11 %
static constexpr uint8_t OPA_SELECTED = 36;      // accent 14 %

static const char *const GLYPH_BACK = "\U000F0141";
static const char *const GLYPH_PLAY = "\U000F040A";
static const char *const GLYPH_PAUSE = "\U000F03E4";
static const char *const GLYPH_VOL_HIGH = "\U000F057E";
static const char *const GLYPH_VOL_OFF = "\U000F0581";
static const char *const GLYPH_MUSIC = "\U000F075A";
static const char *const GLYPH_HEART = "\U000F02D1";
static const char *const GLYPH_HEART_OUTLINE = "\U000F02D5";

}  // namespace theme
}  // namespace music_assistant
}  // namespace esphome
