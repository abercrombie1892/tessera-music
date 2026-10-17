#pragma once
#include "esphome/components/smart_display/plugin_api.h"

namespace esphome::tessera_music {

class TesseraMusic;

// A scene or a light as a tile on a ground of colour: a soft gradient in the colours of the scene (from the Hue
// bridge) or of the light, the name in white on it. A tap starts the scene or switches the light.
tessera::Tile *make_scene_tile(TesseraMusic *plugin);

}  // namespace esphome::tessera_music
