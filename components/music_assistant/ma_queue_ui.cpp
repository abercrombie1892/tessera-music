#include "ma_queue_ui.h"
#include "ma_theme.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <esp_heap_caps.h>
#include <cstring>
#include <cctype>

namespace esphome {
namespace music_assistant {
namespace ma_queue {

using namespace theme;
using namespace widgets;

static const char *const TAG = "ma_queue";

static constexpr int COL_X = 48, COL_W = 360, COVER_Y = 104;
// Panel (1280x800): now-playing column left of the list. Compact (720x720):
// full-width tabs and list, no column (the playing row is highlighted).
struct Geometry {
  bool compact;
  int list_x, list_y, list_w, list_h, row_h, row_step, thumb;
  int thumb_x, text_x, text_w;
  int seg_x, seg_y, seg_w, seg_h, tab_w;
  int back_x, back_y, back_size, title_x, title_w, clock_x;
};
static constexpr Geometry PANEL{false, 440, 104, 792, 664, 72, 78, 52,
                                66, 134, 540,
                                440, 24, 590, 56, 144,
                                COL_X, 24, 56, 118, 290, 1056};
static constexpr Geometry COMPACT{true, 20, 156, 680, 548, 64, 70, 44,
                                  54, 112, 470,
                                  20, 88, 680, 52, 167,
                                  20, 20, 52, 86, 380, 500};
static Geometry G = PANEL;
// queue: up to MAX_QUEUE_ROWS; library lists: two pages (rows live in PSRAM)
static constexpr int MAX_ROWS = 2 * MusicAssistant::LIBRARY_PAGE;
static constexpr int PAGE = 20;
static constexpr uint32_t COL_CURRENT_TITLE = 0xF7B98C;

// Cache and rows key thumbnails by a hash of the URL: URL strings would sit
// in internal RAM (96 of them ~13 KB), the pixels themselves are in PSRAM.
static uint32_t url_key(const std::string &url) {
  if (url.empty()) return 0;
  uint32_t h = 2166136261u;  // FNV-1a
  for (unsigned char c : url) h = (h ^ c) * 16777619u;
  return h != 0 ? h : 1;
}

struct Row {
  lv_obj_t *root = nullptr, *num = nullptr, *cover = nullptr, *cover_ph = nullptr;
  lv_obj_t *title = nullptr, *artist = nullptr, *dur = nullptr;
  uint32_t cover_key = 0;   // cached thumbnail shown in this row (0 = placeholder)
  int current = -1;         // tri-state so the first refresh always styles
};

// Decoded thumbnails are copied into a PSRAM cache keyed by URL, so switching
// tabs back shows known covers at once — no second download, no second HW
// decode (each decode burst competes with the display refresh and flashed
// the screen). The downloaders are only a fetch pool.
struct Thumb {
  uint32_t key = 0;
  lv_image_dsc_t dsc{};
  uint8_t *pixels = nullptr;
  uint32_t used_ms = 0;
};
static constexpr int CACHE_SIZE = 96;  // ~5.4 KB each at 52x52 RGB565
static constexpr uint32_t FETCH_TIMEOUT_MS = 10000;

static MusicAssistant *s_client = nullptr;
static Fonts F;
static Hooks s_hooks;
static artwork_image::ArtworkImage *s_cover_dl = nullptr;
static artwork_image::ArtworkImage *s_thumb_dl[THUMB_SLOTS] = {};
static Thumb s_cache[CACHE_SIZE];
static std::string s_fetch_url[THUMB_SLOTS];  // URL each downloader is fetching ("" = idle)
static uint32_t s_fetch_ms[THUMB_SLOTS] = {};
static Row s_rows[MAX_ROWS];
static int s_rows_built = 0;
static lv_obj_t *s_list = nullptr, *s_more = nullptr, *s_more_lbl = nullptr;
static lv_obj_t *s_kicker = nullptr, *s_clock = nullptr, *s_subtitle = nullptr;
static lv_obj_t *s_tabs[4] = {}, *s_tab_lbl[4] = {};
static int s_tab = 0;  // 0 = queue, 1..3 = library lists
static const char *const TABS[] = {"Warteschlange", "Playlists", "Radio", "Favoriten"};
static const MALibrary TAB_LIBRARY[] = {MALibrary::PLAYLISTS, MALibrary::PLAYLISTS, MALibrary::RADIOS,
                                        MALibrary::FAVORITES};
static lv_obj_t *s_cover = nullptr, *s_cover_ph = nullptr, *s_title = nullptr, *s_artist = nullptr;
static lv_obj_t *s_progress = nullptr, *s_time_cur = nullptr, *s_time_rem = nullptr;
static lv_obj_t *s_play_icon = nullptr;
static std::string s_cover_bound_url;
static bool s_built = false;
// Thumbnails have their own 100 ms pacing (the 1 s refresh would make one
// decode at a time crawl) and start only after a list redraw has settled.
static lv_timer_t *s_thumb_timer = nullptr;
static uint32_t s_thumbs_not_before_ms = 0;
static constexpr uint32_t THUMB_SETTLE_MS = 300;

static std::string fmt_mmss(uint32_t s) {
  char b[12];
  snprintf(b, sizeof(b), "%u:%02u", (unsigned) (s / 60), (unsigned) (s % 60));
  return b;
}

static void touched() {
  if (s_hooks.touched) s_hooks.touched();
}

// ── events ──────────────────────────────────────────────────────────────────

static void on_back(lv_event_t *) {
  touched();
  s_client->set_queue_limit(PAGE);  // big pages only while the list is open
  s_client->clear_library();
  if (s_hooks.go_back) s_hooks.go_back();
}

static void on_transport(lv_event_t *e) {
  touched();
  switch ((int) (intptr_t) lv_event_get_user_data(e)) {
    case 0: s_client->cmd_previous(); break;
    case 1: s_client->cmd_play_pause(); break;
    default: s_client->cmd_next(); break;
  }
}

static void select_tab(int tab);
static void kick_thumbs(uint32_t settle_ms);
static void thumb_timer_cb(lv_timer_t *timer);

static void play_library_row(int i) {
  const auto &lib = s_client->library();
  if (i < 0 || i >= (int) lib.size()) return;
  const MALibraryItem &item = lib[i];
  ESP_LOGI(TAG, "play %s: %s", item.media_type.c_str(), item.name.c_str());
  // a single track joins the current queue; lists and stations replace it
  s_client->cmd_play_media(item.uri, item.media_type == "track" ? "play" : "replace");
  select_tab(0);  // show what is playing now
}

static void on_row(lv_event_t *e) {
  const int i = (int) (intptr_t) lv_event_get_user_data(e);
  touched();
  if (s_tab != 0) {
    play_library_row(i);
    return;
  }
  const auto &q = s_client->queue();
  if (i <= 0 || i >= (int) q.size()) return;  // row 0 is already playing
  ESP_LOGI(TAG, "play row %d: %s", i, q[i].title.c_str());
  s_client->cmd_play_item(q[i].queue_item_id);
}

static void on_more(lv_event_t *) {
  touched();
  if (s_tab == 0)
    s_client->set_queue_limit(s_client->queue_limit() + PAGE);
  else if (!s_client->library_loading())
    s_client->request_library(TAB_LIBRARY[s_tab], true);
  set_text(s_more_lbl, "Lade …");
}

static void on_tab(lv_event_t *e) {
  touched();
  select_tab((int) (intptr_t) lv_event_get_user_data(e));
}

static void on_scroll_end(lv_event_t *) {
  kick_thumbs(THUMB_SETTLE_MS);
  refresh();
}

// ── build ───────────────────────────────────────────────────────────────────

static void build_topbar(lv_obj_t *screen) {
  lv_obj_t *back = make_box(screen, G.back_x, G.back_y, G.back_size, G.back_size, G.back_size / 2);
  style_glass(back);
  add_press_feedback(back, OPA_GLASS + 26);
  make_icon(back, F.icon34, GLYPH_BACK, COL_TEXT);
  lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, nullptr);

  s_kicker = make_label(screen, F.text14, COL_MUTED, G.title_x, G.back_y + 4, G.title_w);
  lv_obj_set_style_text_letter_space(s_kicker, 2, LV_PART_MAIN);
  s_subtitle = make_label(screen, F.text20, COL_TEXT, G.title_x, G.back_y + 21, G.title_w);
  lv_label_set_text(s_subtitle, TABS[0]);

  lv_obj_t *seg = make_box(screen, G.seg_x, G.seg_y, G.seg_w, G.seg_h, G.seg_h / 2);
  style_glass(seg);
  lv_obj_remove_flag(seg, LV_OBJ_FLAG_CLICKABLE);
  const int tab_h = G.seg_h - 12;
  for (int i = 0; i < 4; i++) {
    s_tabs[i] = make_box(seg, 6 + i * G.tab_w, 6, G.tab_w, tab_h, tab_h / 2);
    style_fill(s_tabs[i], COL_TEXT, LV_OPA_TRANSP);
    add_press_feedback(s_tabs[i], 26);
    s_tab_lbl[i] = make_icon(s_tabs[i], F.text17, TABS[i], COL_MUTED);
    lv_obj_add_event_cb(s_tabs[i], on_tab, LV_EVENT_CLICKED, (void *) (intptr_t) i);
  }

  s_clock = make_label(screen, F.text30, COL_TEXT, G.clock_x, G.back_y + 8, G.compact ? 200 : 176);
  lv_obj_set_style_text_align(s_clock, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
}

static lv_obj_t *make_round_button(lv_obj_t *screen, int x, int y, int size, int action) {
  lv_obj_t *btn = make_box(screen, x, y, size, size, size / 2);
  lv_obj_add_event_cb(btn, on_transport, LV_EVENT_CLICKED, (void *) (intptr_t) action);
  return btn;
}

static void build_now_playing(lv_obj_t *screen) {
  s_cover_ph = make_box(screen, COL_X, COVER_Y, COL_W, COL_W, 28);
  style_glass(s_cover_ph);
  make_icon(s_cover_ph, F.icon34, GLYPH_MUSIC, COL_DIM);
  // shares screen 1's 480 px decode, drawn at 75 %
  s_cover = lv_image_create(screen);
  lv_obj_set_pos(s_cover, COL_X, COVER_Y);
  lv_obj_set_size(s_cover, COL_W, COL_W);
  lv_image_set_inner_align(s_cover, LV_IMAGE_ALIGN_CENTER);
  lv_image_set_scale(s_cover, 256 * COL_W / 480);
  lv_image_set_antialias(s_cover, true);
  lv_obj_set_style_radius(s_cover, 28, LV_PART_MAIN);
  lv_obj_set_style_clip_corner(s_cover, true, LV_PART_MAIN);
  lv_obj_add_flag(s_cover, LV_OBJ_FLAG_HIDDEN);

  s_title = make_label(screen, F.text26, COL_TEXT, COL_X, 482, COL_W);
  s_artist = make_label(screen, F.text20, COL_TEXT2, COL_X, 518, COL_W);

  s_progress = lv_slider_create(screen);
  lv_obj_set_pos(s_progress, COL_X, 562);
  lv_obj_set_size(s_progress, COL_W, 6);
  lv_slider_set_range(s_progress, 0, 1000);
  style_slider(s_progress);
  lv_obj_set_style_bg_color(s_progress, lv_color_hex(COL_TEXT), LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(s_progress, LV_OPA_TRANSP, LV_PART_KNOB);
  lv_obj_remove_flag(s_progress, LV_OBJ_FLAG_CLICKABLE);  // display only; seek lives on screen 1
  s_time_cur = make_label(screen, F.text14, COL_DIM, COL_X, 576, 120);
  s_time_rem = make_label(screen, F.text14, COL_DIM, COL_X + COL_W - 120, 576, 120);
  lv_obj_set_style_text_align(s_time_rem, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);

  // compact transport 72 / 104 / 72, centred under the cover
  const int center = COL_X + COL_W / 2, y = 616;
  lv_obj_t *prev = make_round_button(screen, center - 52 - 28 - 72, y + 16, 72, 0);
  style_glass(prev);
  add_press_feedback(prev, OPA_GLASS + 26);
  make_icon(prev, F.icon34, "\U000F04AE", COL_TEXT);
  lv_obj_t *play = make_round_button(screen, center - 52, y, 104, 1);
  style_fill(play, COL_ACCENT, LV_OPA_COVER);
  lv_obj_set_style_bg_opa(play, 217 /* 85 % */, LV_PART_MAIN | LV_STATE_PRESSED);
  s_play_icon = make_icon(play, F.icon34, GLYPH_PLAY, COL_ON_ACCENT);
  lv_obj_t *next = make_round_button(screen, center + 52 + 28, y + 16, 72, 2);
  style_glass(next);
  add_press_feedback(next, OPA_GLASS + 26);
  make_icon(next, F.icon34, "\U000F04AD", COL_TEXT);
}

static void build_row(int i) {
  Row &r = s_rows[i];
  const int ROW_H = G.row_h, THUMB = G.thumb;
  r.root = make_box(s_list, 0, i * G.row_step, G.list_w, ROW_H, G.compact ? 18 : 20);
  style_fill(r.root, COL_TEXT, 10 /* 4 % */);
  add_press_feedback(r.root, 36);
  lv_obj_add_event_cb(r.root, on_row, LV_EVENT_CLICKED, (void *) (intptr_t) i);

  r.num = make_label(r.root, F.text17, COL_GRAY, 0, 0, G.compact ? 36 : 44);
  lv_obj_set_style_text_align(r.num, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(r.num, LV_ALIGN_LEFT_MID, G.compact ? 10 : 12, 0);

  const int thumb_radius = G.compact ? 10 : 12;
  r.cover_ph = make_box(r.root, G.thumb_x, (ROW_H - THUMB) / 2, THUMB, THUMB, thumb_radius);
  style_glass(r.cover_ph);
  make_icon(r.cover_ph, F.icon24, GLYPH_MUSIC, COL_DIM);
  r.cover = lv_image_create(r.root);
  lv_obj_set_pos(r.cover, G.thumb_x, (ROW_H - THUMB) / 2);
  lv_obj_set_size(r.cover, THUMB, THUMB);
  lv_obj_set_style_radius(r.cover, thumb_radius, LV_PART_MAIN);
  lv_obj_set_style_clip_corner(r.cover, true, LV_PART_MAIN);
  lv_obj_remove_flag(r.cover, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(r.cover, LV_OBJ_FLAG_HIDDEN);

  const int title_y = G.compact ? 7 : 12, artist_y = G.compact ? 38 : 42;  // compact: room for the larger fonts
  r.title = make_label(r.root, F.text20, COL_TEXT, G.text_x, title_y, G.text_w);
  r.artist = make_label(r.root, F.text14, COL_DIM, G.text_x, artist_y, G.text_w);
  r.dur = make_label(r.root, F.text17, COL_DIM, 0, 0, G.compact ? 60 : 80);
  lv_obj_set_style_text_align(r.dur, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
  lv_obj_align(r.dur, LV_ALIGN_RIGHT_MID, G.compact ? -20 : -22, 0);
}

static void build_list(lv_obj_t *screen) {
  s_list = make_box(screen, G.list_x, G.list_y, G.list_w, G.list_h, 0);
  lv_obj_add_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);
  lv_obj_remove_flag(s_list, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s_list, on_scroll_end, LV_EVENT_SCROLL_END, nullptr);

  s_more = make_box(s_list, 0, 0, G.list_w, G.row_h, 20);
  style_glass(s_more);
  add_press_feedback(s_more, OPA_GLASS + 26);
  s_more_lbl = make_icon(s_more, F.text17, "", COL_TEXT);
  lv_obj_add_event_cb(s_more, on_more, LV_EVENT_CLICKED, nullptr);
  lv_obj_add_flag(s_more, LV_OBJ_FLAG_HIDDEN);
}

void build(lv_obj_t *screen, MusicAssistant *client, const widgets::Fonts &fonts,
           artwork_image::ArtworkImage *cover_dl, artwork_image::ArtworkImage *const thumb_dl[THUMB_SLOTS],
           bool compact, Hooks hooks) {
  if (screen == nullptr || client == nullptr) return;
  G = compact ? COMPACT : PANEL;
  s_client = client;
  F = fonts;
  s_hooks = std::move(hooks);
  s_cover_dl = cover_dl;
  for (int i = 0; i < THUMB_SLOTS; i++) s_thumb_dl[i] = thumb_dl[i];
  lv_obj_clean(screen);  // drop the YAML placeholder
  build_topbar(screen);
  if (!G.compact) build_now_playing(screen);
  build_list(screen);
  s_thumb_timer = lv_timer_create(thumb_timer_cb, 100, nullptr);
  lv_timer_pause(s_thumb_timer);
  s_built = true;
  ESP_LOGI(TAG, "screen 3 built");
}

// ── refresh ─────────────────────────────────────────────────────────────────

static void update_now_playing() {
  const MANowPlaying &np = s_client->now_playing();
  const MAPlayer *p = s_client->player(s_client->selected_player());
  std::string room = p != nullptr ? p->display_name : "";
  if (p != nullptr && p->group_members.size() > 1) room += " +" + to_string((int) p->group_members.size() - 1);
  if (G.compact) {
    // no now-playing column: kicker = list, subtitle = room
    std::string kicker = TABS[s_tab];
    if (s_tab == 0) kicker += " · " + to_string((int) s_client->queue_total_items()) + " Titel";
    for (char &c : kicker) c = (char) toupper((unsigned char) c);
    set_text(s_kicker, kicker.c_str());
    set_text(s_subtitle, room.c_str());
    return;
  }
  for (char &c : room) c = (char) toupper((unsigned char) c);
  set_text(s_kicker, room.c_str());
  set_text(s_title, np.title.empty() ? "Nichts läuft" : np.title.c_str());
  set_text(s_artist, np.artist.c_str());
  set_text(s_play_icon, p != nullptr && p->playback_state == "playing" ? GLYPH_PAUSE : GLYPH_PLAY);

  const std::string cover_url = ma_cover_url(np.image_url);  // screen 1's download
  const bool ready = image_ready(s_cover_dl, cover_url);
  if (ready && s_cover_bound_url != cover_url) {
    lv_image_set_src(s_cover, nullptr);
    lv_image_set_src(s_cover, s_cover_dl->get_lv_image_dsc());
    s_cover_bound_url = cover_url;
  }
  set_visible(s_cover, ready);
  set_visible(s_cover_ph, !ready);
}

static void update_progress() {
  if (s_progress == nullptr) return;
  const MANowPlaying &np = s_client->now_playing();
  const uint32_t elapsed = (uint32_t) s_client->elapsed_now();
  const uint32_t dur = np.duration_s;
  lv_slider_set_value(s_progress, dur > 0 ? (int32_t) std::min<uint64_t>(1000, (uint64_t) elapsed * 1000 / dur) : 0,
                      LV_ANIM_OFF);
  set_text(s_time_cur, fmt_mmss(elapsed).c_str());
  set_text(s_time_rem, dur > 0 ? ("−" + fmt_mmss(dur > elapsed ? dur - elapsed : 0)).c_str() : "");
}

static void style_row_current(Row &r, bool current) {
  if ((int) current == r.current) return;
  r.current = current;
  if (current) {
    style_fill(r.root, COL_ACCENT, OPA_SELECTED);
    style_border(r.root, 1, COL_ACCENT, 140);
  } else {
    style_fill(r.root, COL_TEXT, 10);
    style_border(r.root, 0, COL_ACCENT, LV_OPA_TRANSP);
  }
  const lv_font_t *num_font = current ? F.icon24 : F.text17;
  lv_obj_set_style_text_font(r.num, num_font, LV_PART_MAIN);
  lv_obj_set_height(r.num, lv_font_get_line_height(num_font));  // single-line box follows the font
  lv_obj_set_style_text_color(r.num, lv_color_hex(current ? COL_ACCENT : COL_GRAY), LV_PART_MAIN);
  lv_obj_set_style_text_color(r.title, lv_color_hex(current ? COL_CURRENT_TITLE : COL_TEXT), LV_PART_MAIN);
}

static void fill_row(int i, bool current, const char *num, const std::string &title, const std::string &sub,
                     uint32_t duration_s) {
  Row &r = s_rows[i];
  style_row_current(r, current);
  set_text(r.num, num);
  set_text(r.title, title.c_str());
  set_text(r.artist, sub.c_str());
  set_text(r.dur, duration_s > 0 ? fmt_mmss(duration_s).c_str() : "");
}

static void update_queue_row(int i, const MAQueueItem &item) {
  // the loaded window starts at the playing track
  fill_row(i, i == 0, i == 0 ? GLYPH_PLAY : to_string(i).c_str(), item.title, item.artist, item.duration_s);
}

static void update_library_row(int i, const MALibraryItem &item) {
  fill_row(i, false, "", item.name, item.subtitle, item.duration_s);
}

static std::string row_image_url(int row) {
  if (s_tab == 0) return s_client->queue()[row].image_url;
  return s_client->library()[row].image_url;
}

static int list_rows() {
  const int n = s_tab == 0 ? (int) s_client->queue().size() : (int) s_client->library().size();
  return std::min(n, MAX_ROWS);
}

// The row after the list: "load more", or the loading / empty state.
static void update_more_button(int rows) {
  std::string text;
  if (s_tab == 0) {
    const int remaining = s_client->queue_total_items() - s_client->queue_current_index() - rows;
    if (rows >= s_client->queue_limit() && s_client->queue_limit() < MusicAssistant::MAX_QUEUE_ROWS &&
        remaining > 0)
      text = "Weitere " + to_string(std::min(PAGE, remaining)) + " Titel laden";
  } else if (s_client->library_loading()) {
    text = "Lade …";
  } else if (rows == 0) {
    text = "Keine Einträge";
  } else if (s_client->library_has_more() && rows < MAX_ROWS) {
    text = "Weitere laden";
  }
  set_visible(s_more, !text.empty());
  if (text.empty()) return;
  lv_obj_set_y(s_more, rows * G.row_step);
  set_text(s_more_lbl, text.c_str());
}

static Thumb *cache_find(const std::string &url) {
  const uint32_t key = url_key(url);
  for (Thumb &t : s_cache) {
    if (t.pixels == nullptr || t.key != key) continue;
    t.used_ms = millis();
    return &t;
  }
  return nullptr;
}

static bool shown_on_a_row(uint32_t key) {
  for (int i = 0; i < s_rows_built; i++)
    if (s_rows[i].cover_key == key) return true;
  return false;
}

// Free entry first, else the least recently used one no row is showing.
static Thumb *cache_victim() {
  Thumb *victim = nullptr;
  for (Thumb &t : s_cache) {
    if (t.pixels == nullptr) return &t;
    if (!shown_on_a_row(t.key) && (victim == nullptr || t.used_ms < victim->used_ms)) victim = &t;
  }
  return victim;
}

static void cache_store(const std::string &url, const lv_image_dsc_t *src) {
  Thumb *t = cache_victim();
  if (t == nullptr || src == nullptr || src->data == nullptr) return;
  const size_t bytes = src->data_size != 0 ? src->data_size : (size_t) src->header.stride * src->header.h;
  if (t->pixels == nullptr || t->dsc.data_size != bytes) {
    heap_caps_free(t->pixels);
    t->pixels = static_cast<uint8_t *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM));
    if (t->pixels == nullptr) return;
  }
  memcpy(t->pixels, src->data, bytes);
  t->dsc = *src;
  t->dsc.data = t->pixels;
  t->dsc.data_size = bytes;
  t->key = url_key(url);
  t->used_ms = millis();
}

static void show_thumb(Row &r, const Thumb *t) {
  const uint32_t key = t != nullptr ? t->key : 0;
  if (r.cover_key == key) return;
  r.cover_key = key;
  if (t != nullptr) {
    lv_image_set_src(r.cover, nullptr);
    lv_image_set_src(r.cover, &t->dsc);
  }
  set_visible(r.cover, t != nullptr);
  set_visible(r.cover_ph, t == nullptr);
}

static bool fetching(const std::string &url) {
  for (const std::string &f : s_fetch_url)
    if (f == url) return true;
  return false;
}

// Finished downloads move into the cache; a failed one frees its downloader
// after FETCH_TIMEOUT_MS so the queue cannot jam.
static void harvest_fetches() {
  for (int k = 0; k < THUMB_SLOTS; k++) {
    if (s_fetch_url[k].empty()) continue;
    if (image_ready(s_thumb_dl[k], s_fetch_url[k])) {
      cache_store(s_fetch_url[k], s_thumb_dl[k]->get_lv_image_dsc());
      s_fetch_url[k].clear();
    } else if (millis() - s_fetch_ms[k] > FETCH_TIMEOUT_MS) {
      s_fetch_url[k].clear();
    }
  }
}

static int visible_row_range(int rows, int *first) {
  *first = std::max(0, (int) (lv_obj_get_scroll_y(s_list) / G.row_step));
  return std::min(rows, *first + G.list_h / G.row_step + 2);
}

// HW-JPEG decodes, the PPA rotation and the DMA2D flush of a list redraw all
// share the PSRAM bus with the display refresh (code runs from PSRAM too):
// one fetch at a time, none until THUMB_SETTLE_MS after a tab/list change,
// only for rows on screen.
static void start_fetches(int rows) {
  constexpr int MAX_INFLIGHT = 1;
  if (millis() < s_thumbs_not_before_ms) return;
  int inflight = 0;
  for (const std::string &f : s_fetch_url) inflight += !f.empty();
  int first;
  const int last = visible_row_range(rows, &first);
  for (int i = first; i < last && inflight < MAX_INFLIGHT; i++) {
    const std::string url = row_image_url(i);
    if (url.empty() || cache_find(url) != nullptr || fetching(url)) continue;
    for (int k = 0; k < THUMB_SLOTS; k++) {
      if (!s_fetch_url[k].empty() || s_thumb_dl[k] == nullptr) continue;
      if (!download_slot()) return;  // shared pacing across all screens
      s_fetch_url[k] = url;
      s_fetch_ms[k] = millis();
      s_thumb_dl[k]->request_update_url(url);
      inflight++;
      break;
    }
  }
}

static void update_thumbs(int rows) {
  harvest_fetches();
  for (int i = 0; i < s_rows_built; i++) {
    const std::string url = i < rows ? row_image_url(i) : "";
    show_thumb(s_rows[i], url.empty() ? nullptr : cache_find(url));
  }
  start_fetches(rows);
}

static bool thumbs_pending() {
  for (const std::string &f : s_fetch_url)
    if (!f.empty()) return true;
  int first;
  const int last = visible_row_range(list_rows(), &first);
  for (int i = first; i < last; i++)
    if (!row_image_url(i).empty() && s_rows[i].cover_key == 0) return true;
  return false;
}

static void thumb_timer_cb(lv_timer_t *) {
  if (!lv_obj_is_visible(s_list)) {
    lv_timer_pause(s_thumb_timer);
    return;
  }
  update_thumbs(list_rows());
  if (!thumbs_pending()) lv_timer_pause(s_thumb_timer);
}

static void kick_thumbs(uint32_t settle_ms) {
  s_thumbs_not_before_ms = millis() + settle_ms;
  if (s_thumb_timer != nullptr) lv_timer_resume(s_thumb_timer);
}

void refresh() {
  if (!s_built) return;
  update_now_playing();
  const int rows = list_rows();
  while (s_rows_built < rows) build_row(s_rows_built++);
  for (int i = 0; i < s_rows_built; i++) {
    set_visible(s_rows[i].root, i < rows);
    if (i >= rows) continue;
    if (s_tab == 0)
      update_queue_row(i, s_client->queue()[i]);
    else
      update_library_row(i, s_client->library()[i]);
  }
  update_more_button(rows);
  update_thumbs(rows);
  if (thumbs_pending() && s_thumb_timer != nullptr) lv_timer_resume(s_thumb_timer);
}

static void select_tab(int tab) {
  s_tab = tab;
  for (int i = 0; i < 4; i++) {
    style_fill(s_tabs[i], COL_TEXT, i == tab ? LV_OPA_COVER : LV_OPA_TRANSP);
    lv_obj_set_style_text_color(s_tab_lbl[i], lv_color_hex(i == tab ? 0x141414 : COL_MUTED), LV_PART_MAIN);
  }
  if (!G.compact) set_text(s_subtitle, TABS[tab]);
  if (tab != 0) s_client->request_library(TAB_LIBRARY[tab]);
  kick_thumbs(THUMB_SETTLE_MS);  // let the list redraw finish before decoding
  lv_obj_scroll_to_y(s_list, 0, LV_ANIM_OFF);
  refresh();
}

void shown(int tab) {
  if (!s_built) return;
  select_tab(tab);  // the list button lands on the queue, the library button on the playlists
  update_progress();
}

void tick(const char *clock_text) {
  if (!s_built) return;
  if (clock_text != nullptr) set_text(s_clock, clock_text);
  update_progress();
  refresh();  // change-detected; picks up finished cover downloads
}

void release() {
  if (!s_built) return;
  if (s_thumb_timer != nullptr) lv_timer_pause(s_thumb_timer);
  for (std::string &f : s_fetch_url) std::string().swap(f);
}

}  // namespace ma_queue
}  // namespace music_assistant
}  // namespace esphome
