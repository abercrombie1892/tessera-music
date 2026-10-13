# Changelog

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
