#include "ma_rooms_ui.h"
#include "ma_theme.h"
#include "ma_widgets.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cstring>

namespace esphome {
namespace music_assistant {
namespace ma_rooms {

using namespace theme;
using namespace widgets;

static const char *const TAG = "ma_rooms";

// Panel (1280x800): 3x3 cards. Compact (720x720): one list row per room.
struct Geometry {
  bool list;
  int grid_x, grid_y, card_w, card_h, gap, cols, max_cards;
  int bar_y, bar_w, bar_h, bar_btn_h, edit_w, dissolve_w, bar_text_w;
  int back_x, back_y, back_size, title_x, clock_x;
};
static constexpr Geometry PANEL{false, 48, 104, 381, 176, 20, 3, MAX_CARDS,
                                696, 1184, 72, 48, 232, 200, 660,
                                48, 24, 56, 118, 1056};
static constexpr Geometry COMPACT{true, 20, 88, 680, 72, 8, 1, 7,
                                  648, 680, 56, 44, 200, 170, 236,
                                  20, 20, 52, 86, 500};
static Geometry G = PANEL;
static constexpr int PAD = 22, CHIP_W = 96, COVER = 56, PLAY = 56;
static constexpr int ROW_COVER = 48, ROW_PLAY = 48;
static constexpr uint32_t VOL_SEND_THROTTLE_MS = 250;

struct Card {
  lv_obj_t *root = nullptr, *name = nullptr, *chip = nullptr, *chip_lbl = nullptr;
  lv_obj_t *cover = nullptr, *cover_ph = nullptr, *title = nullptr, *artist = nullptr;
  lv_obj_t *play = nullptr, *play_icon = nullptr;
  lv_obj_t *vol_icon = nullptr, *vol = nullptr, *vol_pct = nullptr;
  std::string cover_url;
  int cover_slot = -1;
  bool cover_bound = false, highlighted = false, playing = false, dragging = false;
  int chip_state = -1;
  uint32_t last_vol_send_ms = 0;
};

static MusicAssistant *s_client = nullptr;
static Fonts F;
static Hooks s_hooks;
static artwork_image::ArtworkImage *s_dl[COVER_SLOTS] = {};
static std::string s_slot_url[COVER_SLOTS];
static Card s_cards[MAX_CARDS];
static lv_obj_t *s_clock = nullptr, *s_bar_dot = nullptr, *s_bar_text = nullptr;
static lv_obj_t *s_dissolve_btn = nullptr, *s_edit_btn = nullptr, *s_edit_lbl = nullptr;
static bool s_built = false, s_edit_mode = false;

// ── group model ─────────────────────────────────────────────────────────────

static bool contains(const std::vector<std::string> &ids, const std::string &id) {
  return std::find(ids.begin(), ids.end(), id) != ids.end();
}

static const MAPlayer *selected_player() { return s_client->player(s_client->selected_player()); }

// The group a player belongs to is led by the player it is synced to.
static const MAPlayer *leader_of(const MAPlayer *p) {
  if (p == nullptr || p->synced_to.empty()) return p;
  const MAPlayer *lead = s_client->player(s_client->player_index(p->synced_to));
  return lead != nullptr ? lead : p;
}

static bool is_grouped(const MAPlayer *lead) { return lead != nullptr && lead->group_members.size() > 1; }

static std::string member_names(const MAPlayer *lead) {
  std::string names;
  for (const auto &id : lead->group_members) {
    const MAPlayer *m = s_client->player(s_client->player_index(id));
    if (m == nullptr) continue;
    names += (names.empty() ? "" : " + ") + m->display_name;
  }
  return names;
}

// ── events ──────────────────────────────────────────────────────────────────

static int card_index(lv_event_t *e) { return (int) (intptr_t) lv_event_get_user_data(e); }

static void touched() {
  if (s_hooks.touched) s_hooks.touched();
}

static void toggle_membership(const MAPlayer *p) {
  const MAPlayer *lead = leader_of(selected_player());
  if (lead == nullptr || p->player_id == lead->player_id) return;
  const std::string lead_id = lead->player_id, id = p->player_id;
  const bool member = contains(lead->group_members, id);
  ESP_LOGI(TAG, "%s %s %s group", member ? "remove" : "add", p->display_name.c_str(),
           member ? "from" : "to");
  if (member)
    s_client->cmd_set_members(lead_id, {}, {id});
  else
    s_client->cmd_set_members(lead_id, {id}, {});
}

static void on_card_click(lv_event_t *e) {
  const int i = card_index(e);
  const MAPlayer *p = s_client->player(i);
  if (p == nullptr) return;
  touched();
  if (s_edit_mode) {
    toggle_membership(p);
    return;
  }
  s_client->select_player_user(i);
  if (s_hooks.go_back) s_hooks.go_back();
}

static void on_card_play(lv_event_t *e) {
  const MAPlayer *p = s_client->player(card_index(e));
  if (p == nullptr) return;
  touched();
  s_client->cmd_play_pause(p->player_id);
}

static void send_card_volume(int i, bool force) {
  Card &c = s_cards[i];
  const MAPlayer *p = s_client->player(i);
  if (p == nullptr || (!force && millis() - c.last_vol_send_ms < VOL_SEND_THROTTLE_MS)) return;
  c.last_vol_send_ms = millis();
  s_client->cmd_set_volume(p->player_id, lv_slider_get_value(c.vol));
}

static void on_card_vol_changed(lv_event_t *e) {
  const int i = card_index(e);
  Card &c = s_cards[i];
  c.dragging = true;
  char pct[8];
  snprintf(pct, sizeof(pct), "%d%%", (int) lv_slider_get_value(c.vol));
  set_text(c.vol_pct, pct);
  send_card_volume(i, false);
}

static void on_card_vol_released(lv_event_t *e) {
  const int i = card_index(e);
  touched();
  send_card_volume(i, true);
  s_cards[i].dragging = false;
}

static void on_back(lv_event_t *) {
  touched();
  s_edit_mode = false;
  if (s_hooks.go_back) s_hooks.go_back();
}

static void on_edit(lv_event_t *) {
  touched();
  s_edit_mode = !s_edit_mode;
  refresh();
}

static void on_dissolve(lv_event_t *) {
  touched();
  const MAPlayer *lead = leader_of(selected_player());
  if (!is_grouped(lead)) return;
  std::vector<std::string> members;
  for (const auto &id : lead->group_members)
    if (id != lead->player_id) members.push_back(id);
  ESP_LOGI(TAG, "dissolve group of %s (%u members)", lead->display_name.c_str(),
           (unsigned) members.size());
  s_client->cmd_ungroup(members);
  s_edit_mode = false;
  refresh();
}

// ── build ───────────────────────────────────────────────────────────────────

static void build_topbar(lv_obj_t *screen) {
  lv_obj_t *back = make_box(screen, G.back_x, G.back_y, G.back_size, G.back_size, G.back_size / 2);
  style_glass(back);
  add_press_feedback(back, OPA_GLASS + 26);
  make_icon(back, F.icon34, GLYPH_BACK, COL_TEXT);
  lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *kicker = make_label(screen, F.text14, COL_MUTED, G.title_x, G.back_y + 4);
  lv_obj_set_style_text_letter_space(kicker, 2, LV_PART_MAIN);
  lv_label_set_text(kicker, "MUSIC ASSISTANT");
  lv_label_set_text(make_label(screen, F.text20, COL_TEXT, G.title_x, G.back_y + 21), "Räume & Player");

  s_clock = make_label(screen, F.text30, COL_TEXT, G.clock_x, G.back_y + 8, G.list ? 200 : 176);
  lv_obj_set_style_text_align(s_clock, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
}

static void make_cover(Card &c, int x, int y, int size, int radius);

static void build_card_media(Card &c, int i) {
  make_cover(c, PAD, 62, COVER, 14);

  const int CARD_W = G.card_w;
  const int text_x = PAD + COVER + 12, text_w = CARD_W - text_x - PLAY - PAD - 12;
  c.title = make_label(c.root, F.text17, COL_TEXT, text_x, 66, text_w);
  c.artist = make_label(c.root, F.text14, COL_DIM, text_x, 94, text_w);

  c.play = make_box(c.root, CARD_W - PAD - PLAY, 62, PLAY, PLAY, PLAY / 2);
  style_glass(c.play);
  c.play_icon = make_icon(c.play, F.icon28, GLYPH_PLAY, COL_TEXT);
  lv_obj_add_event_cb(c.play, on_card_play, LV_EVENT_CLICKED, (void *) (intptr_t) i);
}

static void build_card_volume(Card &c, int i) {
  const int CARD_W = G.card_w;
  c.vol_icon = make_label(c.root, F.icon24, COL_MUTED, PAD, 130);
  lv_label_set_text(c.vol_icon, GLYPH_VOL_HIGH);
  c.vol = lv_slider_create(c.root);
  lv_obj_set_pos(c.vol, PAD + 36, 137);
  lv_obj_set_size(c.vol, CARD_W - 2 * PAD - 36 - 60, 10);
  lv_slider_set_range(c.vol, 0, 100);
  style_slider(c.vol);
  lv_obj_add_event_cb(c.vol, on_card_vol_changed, LV_EVENT_VALUE_CHANGED, (void *) (intptr_t) i);
  lv_obj_add_event_cb(c.vol, on_card_vol_released, LV_EVENT_RELEASED, (void *) (intptr_t) i);
  c.vol_pct = make_label(c.root, F.text14, COL_TEXT, CARD_W - PAD - 48, 133, 48);
  lv_obj_set_style_text_align(c.vol_pct, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
}

static void make_chip(Card &c, int x, int y) {
  c.chip = make_box(c.root, x, y, CHIP_W, 26, 13);
  lv_obj_remove_flag(c.chip, LV_OBJ_FLAG_CLICKABLE);
  c.chip_lbl = make_icon(c.chip, F.text14, "", COL_MUTED);
  lv_obj_set_style_text_letter_space(c.chip_lbl, 1, LV_PART_MAIN);
}

static void make_cover(Card &c, int x, int y, int size, int radius) {
  c.cover_ph = make_box(c.root, x, y, size, size, radius);
  style_glass(c.cover_ph);
  make_icon(c.cover_ph, F.icon24, GLYPH_MUSIC, COL_DIM);
  c.cover = lv_image_create(c.root);
  lv_obj_set_pos(c.cover, x, y);
  lv_obj_set_size(c.cover, size, size);
  lv_obj_set_style_radius(c.cover, radius, LV_PART_MAIN);
  lv_obj_set_style_clip_corner(c.cover, true, LV_PART_MAIN);
  lv_obj_remove_flag(c.cover, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(c.cover, LV_OBJ_FLAG_HIDDEN);
}

// Compact list row (680x72): cover · name + track · chip · volume · play.
// The track line carries "title · artist" (or the group hint) in one label.
static void build_row(Card &c, int i) {
  const int w = G.card_w, h = G.card_h, mid = h / 2;
  make_cover(c, 12, (h - ROW_COVER) / 2, ROW_COVER, 12);
  c.name = make_label(c.root, F.text20, COL_TEXT, 72, 10, 238);
  c.title = make_label(c.root, F.text14, COL_DIM, 72, 40, 238);
  c.artist = make_label(c.root, F.text14, COL_DIM, 72, 40, 238);
  lv_obj_add_flag(c.artist, LV_OBJ_FLAG_HIDDEN);  // folded into the title line
  make_chip(c, 318, mid - 13);

  c.vol_icon = make_label(c.root, F.icon24, COL_MUTED, 424, mid - 12);
  lv_label_set_text(c.vol_icon, GLYPH_VOL_HIGH);
  c.vol = lv_slider_create(c.root);
  lv_obj_set_pos(c.vol, 456, mid - 5);
  lv_obj_set_size(c.vol, 102, 10);
  lv_slider_set_range(c.vol, 0, 100);
  style_slider(c.vol);
  lv_obj_add_event_cb(c.vol, on_card_vol_changed, LV_EVENT_VALUE_CHANGED, (void *) (intptr_t) i);
  lv_obj_add_event_cb(c.vol, on_card_vol_released, LV_EVENT_RELEASED, (void *) (intptr_t) i);
  c.vol_pct = make_label(c.root, F.text14, COL_TEXT, 562, mid - 9, 46);
  lv_obj_set_style_text_align(c.vol_pct, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);

  c.play = make_box(c.root, w - 12 - ROW_PLAY, (h - ROW_PLAY) / 2, ROW_PLAY, ROW_PLAY, ROW_PLAY / 2);
  style_glass(c.play);
  c.play_icon = make_icon(c.play, F.icon28, GLYPH_PLAY, COL_TEXT);
  lv_obj_add_event_cb(c.play, on_card_play, LV_EVENT_CLICKED, (void *) (intptr_t) i);
}

static void build_card(lv_obj_t *screen, int i) {
  Card &c = s_cards[i];
  const int x = G.grid_x + (i % G.cols) * (G.card_w + G.gap), y = G.grid_y + (i / G.cols) * (G.card_h + G.gap);
  c.root = make_box(screen, x, y, G.card_w, G.card_h, G.list ? 20 : 28);
  style_glass(c.root);
  add_press_feedback(c.root, OPA_GLASS + 26);
  lv_obj_add_event_cb(c.root, on_card_click, LV_EVENT_CLICKED, (void *) (intptr_t) i);

  if (G.list) {
    build_row(c, i);
  } else {
    c.name = make_label(c.root, F.text26, COL_TEXT, PAD, 16, G.card_w - 2 * PAD - CHIP_W - 10);
    make_chip(c, G.card_w - PAD - CHIP_W, 20);
    build_card_media(c, i);
    build_card_volume(c, i);
  }
  lv_obj_add_flag(c.root, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *make_bar_button(lv_obj_t *bar, int x, int w, const char *text, lv_obj_t **label) {
  lv_obj_t *btn = make_box(bar, x, (G.bar_h - G.bar_btn_h) / 2, w, G.bar_btn_h, G.bar_btn_h / 2);
  lv_obj_t *lbl = make_icon(btn, F.text17, text, COL_TEXT);
  if (label != nullptr) *label = lbl;
  return btn;
}

static void build_bar(lv_obj_t *screen) {
  const int BAR_W = G.bar_w, BAR_H = G.bar_h, inset = (BAR_H - G.bar_btn_h) / 2;
  lv_obj_t *bar = make_box(screen, G.grid_x, G.bar_y, BAR_W, BAR_H, BAR_H / 2);
  style_glass(bar);
  lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
  s_bar_dot = make_box(bar, G.list ? 20 : 28, (BAR_H - 10) / 2, 10, 10, 5);
  style_fill(s_bar_dot, COL_GRAY, LV_OPA_COVER);
  s_bar_text = make_label(bar, F.text17, COL_TEXT, 50, 0, G.bar_text_w);
  lv_obj_align(s_bar_text, LV_ALIGN_LEFT_MID, G.list ? 40 : 50, 0);

  s_edit_btn = make_bar_button(bar, BAR_W - inset - G.edit_w, G.edit_w, "Gruppe bearbeiten", &s_edit_lbl);
  style_fill(s_edit_btn, COL_ACCENT, LV_OPA_COVER);
  lv_obj_set_style_bg_opa(s_edit_btn, 217 /* 85 % */, LV_PART_MAIN | LV_STATE_PRESSED);
  lv_obj_set_style_text_color(s_edit_lbl, lv_color_hex(COL_ON_ACCENT), LV_PART_MAIN);
  lv_obj_add_event_cb(s_edit_btn, on_edit, LV_EVENT_CLICKED, nullptr);

  s_dissolve_btn = make_bar_button(bar, BAR_W - inset - G.edit_w - 12 - G.dissolve_w, G.dissolve_w,
                                   "Gruppe auflösen", nullptr);
  style_border(s_dissolve_btn, 1, COL_TEXT, 90);
  style_fill(s_dissolve_btn, COL_TEXT, LV_OPA_TRANSP);
  add_press_feedback(s_dissolve_btn, 26);
  lv_obj_add_event_cb(s_dissolve_btn, on_dissolve, LV_EVENT_CLICKED, nullptr);
}

void build(lv_obj_t *screen, MusicAssistant *client, const Fonts &fonts,
           artwork_image::ArtworkImage *const cover_dl[COVER_SLOTS], bool compact, Hooks hooks) {
  if (screen == nullptr || client == nullptr) return;
  G = compact ? COMPACT : PANEL;
  s_client = client;
  F = fonts;
  s_hooks = std::move(hooks);
  for (int i = 0; i < COVER_SLOTS; i++) s_dl[i] = cover_dl[i];
  lv_obj_clean(screen);  // drop the YAML placeholder
  build_topbar(screen);
  for (int i = 0; i < G.max_cards; i++) build_card(screen, i);
  build_bar(screen);
  s_built = true;
  ESP_LOGI(TAG, "screen 2 built");
}

// ── refresh ─────────────────────────────────────────────────────────────────

enum ChipState { CHIP_IDLE, CHIP_PAUSED, CHIP_PLAYING };

static void update_chip(Card &c, const MAPlayer &p) {
  const int state = p.playback_state == "playing" ? CHIP_PLAYING
                    : p.playback_state == "paused" ? CHIP_PAUSED
                                                   : CHIP_IDLE;
  if (state == c.chip_state) return;
  c.chip_state = state;
  static const char *const TEXT[] = {"BEREIT", "PAUSE", "SPIELT"};
  set_text(c.chip_lbl, TEXT[state]);
  const bool playing = state == CHIP_PLAYING;
  style_fill(c.chip, playing ? COL_PLAYING : COL_TEXT, playing ? 41 /* 16 % */ : 15);
  lv_obj_set_style_text_color(c.chip_lbl, lv_color_hex(playing ? COL_PLAYING : COL_GRAY), LV_PART_MAIN);
}

// Only rooms with media show a cover, so a few downloaders are shared: the
// n-th card with media gets slot n. A card whose slot or URL changes drops
// its image first — the slot's buffer is about to be overwritten.
static void update_cover(Card &c, const MAPlayer &p, int slot) {
  const std::string url = ma_image_url(p.image_url, 80);  // 56 px card cover
  if (slot != c.cover_slot || url != c.cover_url) {
    c.cover_slot = slot;
    c.cover_url = url;
    c.cover_bound = false;
    set_visible(c.cover, false);
  }
  // paced: a card whose download has to wait retries on the next refresh
  if (slot >= 0 && s_slot_url[slot] != url && download_slot()) {
    s_slot_url[slot] = url;
    s_dl[slot]->request_update_url(url);
  }
  if (slot >= 0 && !c.cover_bound) {
    artwork_image::ArtworkImage *dl = s_dl[slot];
    if (image_ready(dl, url)) {
      lv_image_set_src(c.cover, nullptr);
      lv_image_set_src(c.cover, dl->get_lv_image_dsc());
      c.cover_bound = true;
    }
  }
  set_visible(c.cover, c.cover_bound);
  set_visible(c.cover_ph, !c.cover_bound);
}

static void update_play_button(Card &c, const MAPlayer &p) {
  const bool playing = p.playback_state == "playing";
  if (playing == c.playing) return;  // built as "not playing"
  c.playing = playing;
  set_text(c.play_icon, playing ? GLYPH_PAUSE : GLYPH_PLAY);
  if (playing) {
    style_fill(c.play, COL_ACCENT, LV_OPA_COVER);
    style_border(c.play, 0, COL_ACCENT, LV_OPA_TRANSP);
  } else {
    style_glass(c.play);
  }
  lv_obj_set_style_text_color(c.play_icon, lv_color_hex(playing ? COL_ON_ACCENT : COL_TEXT), LV_PART_MAIN);
}

static void update_volume(Card &c, const MAPlayer &p) {
  set_text(c.vol_icon, p.muted ? GLYPH_VOL_OFF : GLYPH_VOL_HIGH);
  if (c.dragging || p.volume_level < 0) return;
  if (lv_slider_get_value(c.vol) != p.volume_level) lv_slider_set_value(c.vol, p.volume_level, LV_ANIM_OFF);
  char pct[8];
  snprintf(pct, sizeof(pct), "%d%%", p.volume_level);
  set_text(c.vol_pct, pct);
}

static void update_card(int i, const MAPlayer &p, const MAPlayer *sel, const MAPlayer *lead, int cover_slot) {
  Card &c = s_cards[i];
  set_text(c.name, p.display_name.c_str());
  update_chip(c, p);
  const MAPlayer *my_lead = leader_of(&p);
  const bool follower = my_lead != &p;
  const std::string group_hint = follower ? "in Gruppe mit " + my_lead->display_name : "";
  if (G.list) {
    // one track line: the group hint wins, else "title · artist"
    std::string line = follower ? group_hint : (p.title.empty() ? "Nichts läuft" : p.title);
    if (!follower && !p.title.empty() && !p.artist.empty()) line += " · " + p.artist;
    set_text(c.title, line.c_str());
  } else {
    set_text(c.title, !p.title.empty() ? p.title.c_str() : (follower ? "" : "Nichts läuft"));
    set_text(c.artist, follower ? group_hint.c_str() : p.artist.c_str());
  }
  update_cover(c, p, cover_slot);
  update_play_button(c, p);
  update_volume(c, p);

  // edit mode highlights the group being edited, otherwise the selection
  const bool highlight = s_edit_mode ? lead != nullptr && (&p == lead || contains(lead->group_members, p.player_id))
                                     : sel != nullptr && p.player_id == sel->player_id;
  if (highlight == c.highlighted) return;
  c.highlighted = highlight;
  (highlight ? style_selected : style_glass)(c.root);
}

static void update_bar(int count, const MAPlayer *sel, const MAPlayer *lead) {
  const bool grouped = is_grouped(lead);
  std::string text;
  if (s_edit_mode && lead != nullptr)
    text = "Räume antippen: zur Gruppe mit " + lead->display_name + " hinzufügen / entfernen";
  else if (grouped)
    text = "Gruppe " + member_names(lead) + " · " + to_string(count) + " Räume";
  else
    text = (sel != nullptr ? sel->display_name + " · keine Gruppe · " : std::string()) + to_string(count) + " Räume";
  set_text(s_bar_text, text.c_str());
  const bool active = lead != nullptr && lead->playback_state == "playing";
  style_fill(s_bar_dot, active ? COL_PLAYING : COL_GRAY, LV_OPA_COVER);
  set_visible(s_dissolve_btn, grouped);
  set_text(s_edit_lbl, s_edit_mode ? "Fertig" : "Gruppe bearbeiten");
}

void refresh() {
  if (!s_built) return;
  const int count = std::min<int>((int) s_client->player_count(), G.max_cards);
  const MAPlayer *sel = selected_player();
  const MAPlayer *lead = leader_of(sel);
  int next_slot = 0;
  for (int i = 0; i < G.max_cards; i++) {
    const MAPlayer *p = i < count ? s_client->player(i) : nullptr;
    set_visible(s_cards[i].root, p != nullptr);
    if (p == nullptr) continue;
    const bool has_media = !p->image_url.empty() && next_slot < COVER_SLOTS && s_dl[next_slot] != nullptr;
    update_card(i, *p, sel, lead, has_media ? next_slot++ : -1);
  }
  update_bar(count, sel, lead);
}

void tick(const char *clock_text) {
  if (!s_built) return;
  if (clock_text != nullptr) set_text(s_clock, clock_text);
  refresh();  // cheap (change-detected); picks up finished cover downloads
}

}  // namespace ma_rooms
}  // namespace music_assistant
}  // namespace esphome
