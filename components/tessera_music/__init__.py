"""Music Assistant on a Tessera screen: the ESPHome side. register_plugin() reads the id, version, tile memory and the
screen's texts from the manifest and translations/ beside this folder."""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import font, smart_display
from esphome.const import CONF_ID

from ..music_assistant import MusicAssistant

DEPENDENCIES = ["smart_display", "music_assistant"]

CONF_MUSIC_ASSISTANT_ID = "music_assistant_id"
CONF_FONTS = "fonts"
# The dashboard's fonts, as ma_dashboard.yaml had them: Outfit and the MDI icons.
FONT_KEYS = ["text14", "text17", "text20", "text26", "text30", "text34", "icon24", "icon28", "icon34", "icon48"]

tessera_music_ns = cg.esphome_ns.namespace("tessera_music")
TesseraMusic = tessera_music_ns.class_("TesseraMusic", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(TesseraMusic),
        cv.Required(CONF_MUSIC_ASSISTANT_ID): cv.use_id(MusicAssistant),
        cv.Required(CONF_FONTS): cv.Schema({cv.Required(key): cv.use_id(font.Font) for key in FONT_KEYS}),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_music_assistant(await cg.get_variable(config[CONF_MUSIC_ASSISTANT_ID])))
    for index, key in enumerate(FONT_KEYS):
        cg.add(var.set_font(index, await cg.get_variable(config[CONF_FONTS][key])))
    await smart_display.register_plugin(var, __file__)
