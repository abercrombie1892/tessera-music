"""ESPHome component: Music Assistant client.

Connects to a Music Assistant 2.x server over its WebSocket API, authenticates
with an API token and keeps the players, the playing track and the queue. The
tessera_music plugin draws them. Without a host or a token it stays idle.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_PORT

DEPENDENCIES = ["network"]
AUTO_LOAD = ["socket"]

CONF_HOST = "host"
CONF_TOKEN = "token"
CONF_FALLBACK_TOKENS = "fallback_tokens"
CONF_POLL_INTERVAL = "poll_interval"
CONF_PLAYERS = "players"

music_assistant_ns = cg.esphome_ns.namespace("music_assistant")
MusicAssistant = music_assistant_ns.class_("MusicAssistant", cg.Component)

def rooms(value):
    """The rooms to show, in order: a list, or one text with commas (a plugin input in Tessera). "MA name=Shown name"
    shows a room under a name of its own."""
    if isinstance(value, str):
        value = [name.strip() for name in value.split(",") if name.strip()]
    return cv.ensure_list(cv.string_strict)(value)


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(MusicAssistant),
        cv.Optional(CONF_HOST, default=""): cv.string,
        cv.Optional(CONF_PORT, default=8095): cv.port,
        cv.Optional(CONF_TOKEN, default=""): cv.string,
        # tried after "auth failed" (e.g. the MA beta instance's token)
        cv.Optional(CONF_FALLBACK_TOKENS, default=[]): cv.ensure_list(cv.string),
        cv.Optional(CONF_POLL_INTERVAL, default="2s"): cv.positive_time_period_milliseconds,
        # MA display names to show, in this order; empty = every player
        cv.Optional(CONF_PLAYERS, default=[]): rooms,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add_define("USE_MUSIC_ASSISTANT")
    cg.add(var.set_server(config[CONF_HOST], config[CONF_PORT]))
    if config[CONF_TOKEN]:
        cg.add(var.set_token(config[CONF_TOKEN]))
    for tok in config[CONF_FALLBACK_TOKENS]:
        cg.add(var.add_token(tok))
    cg.add(var.set_poll_interval(config[CONF_POLL_INTERVAL]))
    for room in config[CONF_PLAYERS]:
        name, _, alias = room.partition("=")
        if name.strip():
            cg.add(var.add_player_filter(name.strip(), alias.strip()))
