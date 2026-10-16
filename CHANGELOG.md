# Changelog

## 0.4.3
- Room names in upper case keep their umlauts: "SPIELT IN KÜCHE" instead of "KüCHE".

## 0.4.2
- 10-inch dashboard: the player pill starts with the right column instead of over the cover.
- Queue and library tabs as wide as their words: "Warteschlange" no longer runs over its tab.
- The waveform is asked for again every 30 s (up to six times) while Music Assistant has no analysis of the track yet.

## 0.4.1
- The dashboard on the 10-inch Guition panel (1280 x 800): the espcontrol panel's own layout, a 480 px cover, the title
  at 52 px, four next-up tiles in a row, the rooms as cards and the queue beside the cover. The 4-inch screen keeps its
  720 x 720 layout.

## 0.4.0
- Keys on the tile: previous, play or pause, next, round under the words (light on a cover-coloured ground). The new
  option Keys hides them.
- Only a tap on the cover opens the dashboard; the rest of the tile takes no tap, so a stray touch changes nothing.

## 0.3.9
- The small print one step larger again: 18, 21 and 24 px (the panel had 14, 17 and 20). The album line and the queue
  rows' second line move down a little to make room.

## 0.3.8
- Larger small print on the dashboard: Outfit 14, 17 and 20 px are now 16, 19 and 22 px (status, next up, times, album,
  artist, room names, list rows). The title, the clock, the icons and every place stay as they were; a line with a fixed
  height grows to its font, so nothing is cut.

## 0.3.7
- Grouped rooms show it: every room of a group has a frame in the group's colour, the first group in the list blue,
  the second green, then purple, pink, yellow and teal. A room on its own keeps its glass frame.

## 0.3.6
- The tile's cover colours show no bands: its ground is a picture of the card's size, a soft diagonal blend
  Floyd-Steinberg dithered into RGB565 as the dashboard's glow is, drawn once per track, instead of LVGL's gradient.

## 0.3.5
- The screen never freezes while Music Assistant is away: the connect runs without waiting (select() on every loop pass,
  given up after 4 s), and an IP address needs no DNS. Before, each retry could hold the screen for up to 2 s.

## 0.3.4
- Names in Turkish, Polish, Czech, Hungarian and the Baltic languages show whole: the dashboard's fonts have the letters
  of Latin Extended-A that Outfit draws (İ ı ğ ş, ł ą ę, č ř ž, ő ű, …), where they showed as boxes before.

## 0.3.3
- The tile's new option Background: plain like every tile, or in the colours of the cover that plays (lighter at the
  top, darker at the foot, white words), found as the dashboard's glow finds them. Its colour follows the track.

## 0.3.2
- Covers show again: a picture cut to its exact square told LVGL fewer bytes than stride x height, and LVGL refused to
  open it ("Failed to open image").

## 0.3.1
- A library key beside the grid key opens the playlists at once (the clock moves left for it).
- A tap on the "next up" box opens the list wherever it lands: its two tracks no longer take the tap (clickable: false,
  as in the YAML), and nor do the speaker dot and the group badge in the room pill.
- Rooms on the dashboard can have a name of their own: "Wohnzimmer Sonos=Wohnzimmer, Bad, PC".

## 0.3.0
- A tap on the tile opens the espcontrol MA dashboard itself, over the whole glass: its own code (ma_dashboard_ui,
  ma_rooms_ui, ma_queue_ui, ma_widgets, ma_theme), its skeleton from ma_dashboard.yaml made in C++ with the same places,
  and its Outfit and MDI fonts. The glow behind the cover is the dashboard's own, dithered, without banding. Covers come
  from the plugin's cover loader instead of artwork_image, which stays with espcontrol. It closes with its back key, after
  two minutes untouched, and whenever Tessera closes a card.
- A new input, the rooms on the dashboard in order, with commas.

## 0.2.1
- The player looks like the espcontrol MA dashboard: a dark ground lit by the cover's colour, glass keys, the orange
  play key, the track's waveform as its progress, the time left, a volume pill along the bottom.
- The room pill (with the group's size) and the grid key open the rooms; the "next up" box shows the next two tracks
  and opens the queue and the library, a segmented control over one list. The tabs along the top are gone.

## 0.2.0
- A tap on the tile opens the player over the whole glass, as the espcontrol dashboard did: the cover, the title, the
  artist and the album, a bar to seek, previous, play or pause and next, shuffle, repeat, favourite, mute and volume.
- Rooms: every player with what it plays, play or pause, its volume, and a key that adds it to the group of the room
  shown or takes it out. A tap on a room shows it.
- The queue, with small covers; a tap plays that track.
- The library: playlists, radio and favourites, with small covers; a tap plays it.

## 0.1.2
- The tile starts at one cell: the editor places a new tile at 1x1, and its handles grow it one way at a time.
- The cover fills its square: it is scaled to at least the square and trimmed, so no frame shows around it.

## 0.1.1
- The token goes in two fields: Tessera takes at most 256 characters per field, a Music Assistant token has about 390.

## 0.1.0
- First version: a Now playing tile with the cover, the title, the artist and the track's progress, for one room or
  whatever plays. A tap plays or pauses.
