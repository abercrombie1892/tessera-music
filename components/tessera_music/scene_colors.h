#pragma once
#include <cstdint>

namespace esphome::tessera_music {

// The colours of Hue scenes (up to three, as the bridge has them for the scene's lights), by the scene's entity id:
// the gradient of a scene tile. A scene that is not here gets a gradient of its own from its name.
struct SceneColors {
  const char *entity;
  uint8_t count;
  uint32_t colors[3];
};

static constexpr SceneColors SCENE_COLORS[] = {
    {"scene.kuche_kochen", 1, {0xFEBB51, 0, 0}},
    {"scene.kuche_hell", 1, {0xFFA759, 0, 0}},
    {"scene.kuche_gedimmt", 1, {0xFECE77, 0, 0}},
    {"scene.kuche_entspannen", 1, {0xFF942B, 0, 0}},
    {"scene.kuche_lesen", 1, {0xFFAD66, 0, 0}},
    {"scene.kuche_konzentration", 1, {0xFFD4B2, 0, 0}},
    {"scene.kuche_aktivieren", 2, {0xF6FEFE, 0xFFFCF7, 0}},
    {"scene.kuche_nachtlicht", 1, {0xFEA026, 0, 0}},
    {"scene.kuche_nordlichter", 3, {0xFFFEF9, 0x37FEDD, 0x79C0FE}},
    {"scene.kuche_miami", 3, {0x3848FE, 0xFE6EBC, 0xFE7769}},
    {"scene.kuche_miami_2", 3, {0xFE9F57, 0x77ADFE, 0xFE5C88}},
    {"scene.kuche_sonnenuntergang_savanne", 2, {0xFE5B23, 0xFECC59, 0}},
    {"scene.kuche_vapor_wave", 3, {0xFEAF27, 0x8913FE, 0xFE55E9}},
    {"scene.kuche_waldabenteuer", 3, {0xFF9A3C, 0xA2FE44, 0x3AB0FE}},
    {"scene.kuche_waldabenteuer_2", 3, {0xFF9A3C, 0xA2FE44, 0x3AB0FE}},
    {"scene.kuche_bernsteinblute", 1, {0xFE6A35, 0, 0}},
    {"scene.kuche_fruhlingsbluten", 3, {0xFECCEB, 0xFE7DBF, 0xFE907C}},
    {"scene.kuche_gemalter_himmel", 3, {0xFEAC8B, 0x8559FE, 0xFE9AE1}},
    {"scene.kuche_hue_lily", 3, {0xFE931F, 0xFEAC8D, 0xFE778E}},
    {"scene.kuche_tropendammerung", 3, {0xFEB681, 0x944BFE, 0xFEBAED}},
    {"scene.kuche_winterschonheit", 3, {0xFEEF7D, 0x6DFEA4, 0x5EEBFE}},
};

}  // namespace esphome::tessera_music
