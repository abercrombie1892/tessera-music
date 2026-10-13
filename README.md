# Music Assistant for Tessera

What plays in your rooms, with covers, straight from Music Assistant: a dashboard in the spirit of the espcontrol MA
dashboard, as a Tessera plugin for the ESP32-P4 screens (Guition 10.1-inch, Waveshare 4-inch square).

**Private plugin.** The screen talks to Music Assistant itself, over a WebSocket and its image proxy on the home
network. The rules of Tessera's plugin index forbid a screen's own network connections, so this plugin is not meant
for the index: add it to your screens by its link. Its player card also takes taps on its own keys and sliders through
LVGL, which `tools/check.py` refuses: a card has no `on_tap` of its own in plugin API 0.8.

## Set up

1. In Tessera, Plugins, add this repository by its link and choose your screen. The screen builds once.
2. Fill in the address of Music Assistant (its IP address on your network) and a long-lived token from Music
   Assistant (Settings, Profile): its first 200 characters in the first token field, the rest in the second
   (Tessera takes at most 256 characters per field).
3. In Layout, place the **Now playing** tile from the library's Plugins group (search for it: the group is the last
   one). Drag its edges to make it larger, one way at a time.
4. In the inspector, type part of the room's player name as Music Assistant shows it (such as `Bad`), or leave it
   empty to follow whatever plays.

## Good to know

- A tap on the tile opens the player: Now, Rooms, Queue and Library along the top. Back goes to Now first, then
  closes the player.
- A room chosen in the player stays while the player is open; the tile keeps its own room.
- Music Assistant is asked often only while a tile of this plugin is on the glass.
- Covers come from Music Assistant's image proxy and are decoded by the screen's JPEG hardware; a PNG or progressive
  JPEG cover shows the music note instead.
- The token is written into the screen's configuration in Home Assistant, as any value of a plugin's inputs.

## Privacy

Nothing leaves your home network: the screen talks only to the Music Assistant server you name.
