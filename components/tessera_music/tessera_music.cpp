#include "tessera_music.h"
#include "ground.h"
#include <algorithm>
#include <cmath>
#include <string>
#include "esphome/components/music_assistant/ma_artwork.h"
#include "esphome/components/music_assistant/ma_skeleton.h"
#include <vector>
#include "esp_heap_caps.h"

// private LVGL header (misc/cache/instance/lv_image_cache.h), not in lvgl.h
extern "C" void lv_image_cache_drop(const void *src);

namespace esphome::tessera_music {

using tessera::Font;
namespace ui = tessera::ui;

static constexpr uint32_t ACTIVE_MS = 3000;
static constexpr uint32_t ICON_PLAY = 0xF040A, ICON_PAUSE = 0xF03E4, ICON_NEXT = 0xF04AD, ICON_PREV = 0xF04AE;
// In the plugin's own MDI font (playlist-play, speaker, volume-high), not Tessera's icon set.
static constexpr const char *GLYPH_QUEUE = "\U000F0411", *GLYPH_SPEAKER = "\U000F04C3", *GLYPH_VOLUME = "\U000F057E";
static constexpr uint32_t VOLUME_OPEN_MS = 5000;

static std::string clock_of(uint32_t seconds) {
  char text[16];
  if (seconds >= 3600)
    snprintf(text, sizeof(text), "%u:%02u:%02u", (unsigned) (seconds / 3600), (unsigned) (seconds / 60 % 60),
             (unsigned) (seconds % 60));
  else
    snprintf(text, sizeof(text), "%u:%02u", (unsigned) (seconds / 60), (unsigned) (seconds % 60));
  return text;
}

// The ground of a tile in the cover's colours: a soft diagonal blend from `top` (top left) to `foot` (bottom right).
static void paint_ground(uint16_t *pixels, int w, int h, lv_color_t top, lv_color_t foot) {
  const float tr = top.red, tg = top.green, tb = top.blue, fr = foot.red, fg = foot.green, fb = foot.blue;
  dither(pixels, w, h, [&](int x, int y, float *want) {
    const float t = std::min(1.0f, 0.8f * y / std::max(1, h - 1) + 0.2f * x / std::max(1, w - 1));
    want[0] = tr + (fr - tr) * t, want[1] = tg + (fg - tg) * t, want[2] = tb + (fb - tb) * t;
  });
}

// The ground of a tile as the cover itself, blurred and darkened for white words, as a frosted pane over it: the
// cover's middle (cut to the tile's shape) averaged into a small grid, the grid softened, then spread over the tile.
// Made once per cover, never live.
static void paint_blur(uint16_t *pixels, int w, int h, const lv_image_dsc_t *cover) {
  const int side = cover->header.w, stride = cover->header.stride / 2;
  const auto *src = reinterpret_cast<const uint16_t *>(cover->data);
  const int gw = 24, gh = std::max(2, gw * h / std::max(1, w));
  // the part of the square cover with the tile's shape, from its middle
  const int cut_w = w >= h ? side : side * w / h, cut_h = w >= h ? side * h / w : side;
  const int x0 = (side - cut_w) / 2, y0 = (side - cut_h) / 2;
  std::vector<float> grid(3 * gw * gh), soft(3 * gw * gh);
  for (int gy = 0; gy < gh; gy++)
    for (int gx = 0; gx < gw; gx++) {
      float sum[3] = {0, 0, 0};
      int n = 0;
      const int ya = y0 + gy * cut_h / gh, yb = y0 + (gy + 1) * cut_h / gh;
      const int xa = x0 + gx * cut_w / gw, xb = x0 + (gx + 1) * cut_w / gw;
      for (int y = ya; y < yb; y += 2)
        for (int x = xa; x < xb; x += 2) {
          const uint16_t v = src[y * stride + x];
          sum[0] += ((v >> 11) & 0x1F) * (255.0f / 31), sum[1] += ((v >> 5) & 0x3F) * (255.0f / 63);
          sum[2] += (v & 0x1F) * (255.0f / 31), n++;
        }
      for (int c = 0; c < 3; c++) grid[3 * (gy * gw + gx) + c] = n ? sum[c] / n : 0;
    }
  for (int pass = 0; pass < 3; pass++) {  // a 3x3 box, three times: close to a gaussian
    for (int gy = 0; gy < gh; gy++)
      for (int gx = 0; gx < gw; gx++)
        for (int c = 0; c < 3; c++) {
          float sum = 0;
          for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
              const int yy = std::clamp(gy + dy, 0, gh - 1), xx = std::clamp(gx + dx, 0, gw - 1);
              sum += grid[3 * (yy * gw + xx) + c];
            }
          soft[3 * (gy * gw + gx) + c] = sum / 9;
        }
    grid.swap(soft);
  }
  // darker and a little more saturated, the foot darker than the top, so white words read on any cover
  for (int gy = 0; gy < gh; gy++)
    for (int gx = 0; gx < gw; gx++) {
      float *p = &grid[3 * (gy * gw + gx)];
      const float avg = (p[0] + p[1] + p[2]) / 3, shade = 0.62f - 0.22f * gy / (gh - 1);
      for (int c = 0; c < 3; c++) p[c] = std::clamp(avg + (p[c] - avg) * 1.25f, 0.0f, 255.0f) * shade;
    }
  dither(pixels, w, h, [&](int x, int y, float *want) {
    const float fx = std::clamp((x + 0.5f) * gw / w - 0.5f, 0.0f, gw - 1.0f);
    const float fy = std::clamp((y + 0.5f) * gh / h - 0.5f, 0.0f, gh - 1.0f);
    const int ax = std::min((int) fx, gw - 2), ay = std::min((int) fy, gh - 2);
    // smoothstep between the grid points: no creases where the cells meet
    float tx = fx - ax, ty = fy - ay;
    tx = tx * tx * (3 - 2 * tx), ty = ty * ty * (3 - 2 * ty);
    const float *a = &grid[3 * (ay * gw + ax)], *b = a + 3, *c = a + 3 * gw, *d = c + 3;
    for (int k = 0; k < 3; k++)
      want[k] = (a[k] * (1 - tx) + b[k] * tx) * (1 - ty) + (c[k] * (1 - tx) + d[k] * tx) * ty;
  });
}

static void text_color(lv_obj_t *label, uint32_t color) {
  if (!lv_color_eq(lv_obj_get_style_text_color(label, LV_PART_MAIN), lv_color_hex(color)))
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
}

// Now playing: the cover, the title, the artist and how far the track is, for one room or whatever plays.
class NowTile : public tessera::Tile {
 public:
  explicit NowTile(TesseraMusic *plugin) : plugin_(plugin) {}
  ~NowTile() override {
    if (ground_px_ == nullptr) return;
    if (card_ != nullptr && lv_obj_is_valid(card_)) lv_obj_set_style_bg_image_src(card_, nullptr, 0);
    lv_image_cache_drop(&ground_);
    heap_caps_free(ground_px_);
  }

  void create(const tessera::TileContext &c) override {
    width_ = c.width;
    height_ = c.height;
    tile_ = c.tile;
    card_ = lv_obj_get_parent(c.parent);
    const std::string background = c.options["background"] | "theme";
    tint_ = background == "cover" || background == "blur";
    blur_ = background == "blur";
    player_ = c.options["player"] | "";
    const std::string title = c.options["title"] | "large";
    show_title_ = title != "off";
    title_font_ = title == "normal" ? Font::TITLE : Font::HEADLINE;
    side_by_side_ = width_ >= height_ * 13 / 10;
    roomy_ = side_by_side_ && height_ >= ui::mm(70);
    // A big tile (the 10-inch's) takes the plugin's own larger Outfit for the words: Tessera's fonts stop at its
    // headline size.
    if (roomy_ && title == "large") {
      title_big_ = plugin_->text_font(34);
      artist_big_ = plugin_->text_font(26);
    }
    keys_ = std::string(c.options["keys"] | "show") == "show";
    side_by_side_ = width_ >= height_ * 13 / 10;
    // The bar (queue, room, volume) only where it fits: a small tile stays as it was.
    bar_ = std::string(c.options["bar"] | "show") == "show" && plugin_->icon_font(0) != nullptr;
    int text_h = layout_();
    if (bar_ && !bar_fits_(text_h)) {
      bar_ = false;
      text_h = layout_();
    }
    side_ = side_by_side_ ? height_ : std::max(0, std::min(width_, height_ - text_h - ui::px(10)));

    art_ = ui::block(c.parent, theme::TRACK);
    lv_obj_set_size(art_, side_, side_);
    lv_obj_set_pos(art_, side_by_side_ ? 0 : (width_ - side_) / 2, 0);
    note_ = ui::label(art_, Font::ICON, theme::MUTED);
    ui::set_text(note_, ui::icon(0xF075A));  // music-note
    lv_obj_center(note_);
    image_ = lv_image_create(art_);
    lv_obj_set_size(image_, side_, side_);
    lv_image_set_inner_align(image_, LV_IMAGE_ALIGN_CENTER);
    lv_obj_center(image_);
    lv_obj_add_flag(image_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_radius(art_, ui::px(12), 0);
    lv_obj_set_style_clip_corner(art_, true, 0);

    const int x = side_by_side_ ? side_ + ui::px(24) : 0;
    const int text_w = width_ - x;
    // Beside a tall cover the words sit in the free space above the wave, which with the keys sits at its foot; beside a small one all
    // of it in the middle.
    const int words_h = (show_title_ ? title_lh_() : 0) + artist_lh_();
    int y = side_by_side_ ? std::max(0, roomy_ ? (height_ - (text_h - words_h) - words_h) / 2 : (height_ - text_h) / 2)
                            : side_ + ui::px(10);
    title_ = ui::label(c.parent, title_font_, theme::INK);
    if (title_big_ != nullptr) lv_obj_set_style_text_font(title_, title_big_, 0);
    lv_obj_set_pos(title_, x, y);
    lv_obj_set_size(title_, text_w, show_title_ ? title_lh_() : 1);
    if (!show_title_) lv_obj_add_flag(title_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_align(title_, LV_TEXT_ALIGN_CENTER, 0);
    y += show_title_ ? title_lh_() : 0;
    artist_ = ui::label(c.parent, Font::BODY_LARGE, theme::MUTED);
    if (artist_big_ != nullptr) lv_obj_set_style_text_font(artist_, artist_big_, 0);
    lv_obj_set_pos(artist_, x, y);
    lv_obj_set_size(artist_, text_w, artist_lh_());
    lv_obj_set_style_text_align(artist_, LV_TEXT_ALIGN_CENTER, 0);
    y += artist_lh_() + ui::px(8);
    if (roomy_) y = height_ - (text_h - words_h) + ui::px(8);
    build_wave_(c.parent, x, y, text_w);
    y += wave_h_ + ui::px(6);
    elapsed_ = ui::label(c.parent, Font::BODY, theme::SUBTLE);
    lv_obj_set_pos(elapsed_, x, y);
    lv_obj_set_size(elapsed_, text_w / 2, ui::line_height(Font::BODY));
    total_ = ui::label(c.parent, Font::BODY, theme::SUBTLE);
    lv_obj_set_pos(total_, x + text_w / 2, y);
    lv_obj_set_size(total_, text_w - text_w / 2, ui::line_height(Font::BODY));
    lv_obj_set_style_text_align(total_, LV_TEXT_ALIGN_RIGHT, 0);
    text_w_ = text_w;
    lv_obj_add_flag(art_, LV_OBJ_FLAG_CLICKABLE);  // only the cover opens the dashboard
    lv_obj_set_user_data(art_, reinterpret_cast<void *>(static_cast<intptr_t>(KEY_COVER)));
    lv_obj_add_event_cb(art_, on_key_, LV_EVENT_CLICKED, this);
    y += ui::line_height(Font::BODY) + gap_;
    if (keys_) build_keys_(c.parent, x, y, text_w);
    if (keys_) y += key_h_ + gap_;
    if (bar_) build_bar_(c.parent, x, y, text_w);
    theme_keys_();
  }

  void on_tick(uint32_t) override {
    plugin_->seen();
    auto *ma = plugin_->ma();
    if (ma == nullptr || !ma->is_connected()) {
      ui::set_text(title_, plugin_->text("connecting"));
      ui::set_text(artist_, "");
      return;
    }
    plugin_->follow_room(player_);
    const auto &now = ma->now_playing();
    const auto *player = ma->player(ma->selected_player());
    const bool playing = player != nullptr && player->playback_state == "playing";
    ui::set_text(title_, now.title.empty() ? (player ? player->display_name : plugin_->text("nothing")) : now.title);
    ui::set_text(artist_, now.artist);
    const uint32_t elapsed = (uint32_t) ma->elapsed_now();
    const uint32_t duration = now.duration_s;
    ui::set_text(elapsed_, duration ? clock_of(std::min(elapsed, duration)) : "");
    ui::set_text(total_, duration ? clock_of(duration) : "");
    move_wave_(duration ? (int) ((int64_t) text_w_ * std::min(elapsed, duration) / duration) : 0);
    if (!tint_) ui::set_color(artist_, playing ? theme::MUTED : theme::SUBTLE);
    if (keys_) ui::set_text(glyph_of(play_), ui::icon(playing ? ICON_PAUSE : ICON_PLAY));
    if (bar_) tick_bar_(player);

    plugin_->cover().request(now.image_url, side_);
    paint_cover_();
    if (tint_) paint_tint_();
  }

  void on_theme() override {
    ui::set_color(title_, theme::INK);
    ui::set_color(artist_, theme::MUTED);
    ui::set_color(elapsed_, theme::SUBTLE);
    ui::set_color(total_, theme::SUBTLE);
    ui::set_color(note_, theme::MUTED);
    theme_keys_();
    if (!tint_) return;
    lv_obj_set_style_bg_color(ball_, ui::color(theme::ACCENT), 0);  // the keys take the cover's colours again
    paint_tint_();
  }

  // The cover and the keys take their own taps; the rest of the card does nothing, so a stray touch changes nothing.
  void on_tap() override {}

 protected:
  void paint_cover_() {
    CoverLoader &cover = plugin_->cover();
    if (cover.revision() == cover_revision_) return;
    cover_revision_ = cover.revision();
    const lv_image_dsc_t *picture = cover.picture();
    if (picture == nullptr) {
      lv_obj_add_flag(image_, LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(note_, LV_OBJ_FLAG_HIDDEN);
      return;
    }
    lv_image_set_src(image_, picture);
    lv_obj_clear_flag(image_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(note_, LV_OBJ_FLAG_HIDDEN);
  }

  // The card in the cover's colours, top lighter than the foot, with light words. Tessera colours its cards again when
  // it draws them: every tick puts the cover's colours back (only when they differ).
  void paint_tint_() {
    const uint32_t tone = plugin_->cover().tone();
    if (tone != 0) tone_ = tone;
    if (tone_ == 0) return;
    paint_ground_();
    text_color(title_, 0xF4F1EC);
    text_color(artist_, 0xE6E1D9);
    text_color(elapsed_, 0xD9D4CC);
    text_color(total_, 0xD9D4CC);
    if ((lv_color_to_u32(lv_obj_get_style_bg_color(ball_, LV_PART_MAIN)) & 0xFFFFFF) != 0xFFFFFF) {
      lv_obj_set_style_bg_color(ball_, lv_color_hex(0xFFFFFF), 0);
      lv_obj_invalidate(track_);
      lv_obj_set_style_line_color(dim_, lv_color_hex(0xFFFFFF), 0);
      lv_obj_set_style_line_opa(dim_, 90, 0);
      lv_obj_set_style_line_color(fill_, lv_color_hex(0xFFFFFF), 0);
      for (lv_obj_t *key : {prev_, play_, next_, queue_, pill_, volume_}) {
        if (key == nullptr) continue;
        text_color(glyph_of(key), key == play_ ? 0x1A1208 : 0xFFFFFF);
        if (key != play_) {
          glass_(key, true);
          continue;
        }
        lv_obj_set_style_bg_color(key, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(key, LV_OPA_COVER, 0);
      }
      if (room_ != nullptr) text_color(room_, 0xFFFFFF);
      if (slider_ != nullptr) {
        lv_obj_set_style_bg_color(slider_, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(slider_, 70, LV_PART_MAIN);
        lv_obj_set_style_bg_color(slider_, lv_color_hex(0xFFFFFF), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(slider_, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
      }
    }
  }

  // The progress as a wave: the whole track in a soft line, the part played thicker and bright, and a ball that rides
  // the crest at the front. The shape is fixed; only how much of it is lit moves.
  void build_wave_(lv_obj_t *parent, int x, int y, int w) {
    ball_r_ = std::max(ui::px(3), wave_h_ / 5);
    wave_amp_ = wave_h_ / 2 - ball_r_;
    const int length = ui::px(150);  // one wave: long and low reads as a swing, short as a scribble
    wave_.clear();
    for (int px = ball_r_; px <= w - ball_r_; px += WAVE_STEP) {
      lv_point_precise_t p;
      p.x = px;
      p.y = wave_y_(px, length);
      wave_.push_back(p);
    }
    wave_len_ = length;
    text_w_ = w;
    track_ = lv_obj_create(parent);
    lv_obj_remove_style_all(track_);
    lv_obj_add_event_cb(track_, on_wave_draw_, LV_EVENT_DRAW_MAIN, this);
    lv_obj_set_pos(track_, x, y);
    lv_obj_set_size(track_, w, wave_h_);
    lv_obj_remove_flag(track_, LV_OBJ_FLAG_CLICKABLE);
    dim_ = lv_line_create(track_);
    lv_line_set_points(dim_, wave_.data(), wave_.size());
    lv_obj_set_style_line_width(dim_, ui::px(3), 0);
    lv_obj_set_style_line_rounded(dim_, true, 0);
    fill_ = lv_line_create(track_);
    lv_line_set_points(fill_, wave_.data(), 2);
    lv_obj_set_style_line_width(fill_, ui::px(5), 0);
    lv_obj_set_style_line_rounded(fill_, true, 0);
    ball_ = ui::block(track_, theme::ACCENT);
    lv_obj_set_size(ball_, 2 * ball_r_, 2 * ball_r_);
    lv_obj_set_style_radius(ball_, LV_RADIUS_CIRCLE, 0);
    lv_obj_remove_flag(ball_, LV_OBJ_FLAG_CLICKABLE);
    theme_wave_();
    move_wave_(0);
  }

  // The track's own waveform from Music Assistant's audio analysis (bars from the middle, the loudest as high as the
  // track): the part played bright, the rest soft. Only analysed tracks have one; the others keep the sine wave.
  bool has_waveform_() const {
    auto *ma = plugin_->ma();
    return ma != nullptr && (int) ma->now_playing().waveform.size() == music_assistant::WAVE_BINS;
  }

  static void on_wave_draw_(lv_event_t *e) {
    auto *tile = static_cast<NowTile *>(lv_event_get_user_data(e));
    if (!tile->has_waveform_()) return;
    const auto &wave = tile->plugin_->ma()->now_playing().waveform;
    lv_area_t area;
    lv_obj_get_coords(tile->track_, &area);
    const int w = lv_area_get_width(&area), h = lv_area_get_height(&area), mid = area.y1 + h / 2;
    const int pitch = ui::px(7), bar_w = ui::px(4), n = std::max(8, w / pitch);
    const int played = (int) (tile->played_ * n);
    const bool lit = tile->tint_ && tile->tone_ != 0;
    lv_draw_rect_dsc_t on, off;
    lv_draw_rect_dsc_init(&on);
    lv_draw_rect_dsc_init(&off);
    on.bg_color = lit ? lv_color_hex(0xFFFFFF) : ui::color(theme::ACCENT);
    off.bg_color = lit ? lv_color_hex(0xFFFFFF) : ui::color(theme::TRACK);
    on.bg_opa = LV_OPA_COVER;
    off.bg_opa = lit ? 90 : LV_OPA_COVER;
    on.radius = off.radius = bar_w / 2;
    int peak = 1;
    for (uint8_t v : wave) peak = std::max(peak, (int) v);
    const float per = (float) music_assistant::WAVE_BINS / n;
    lv_layer_t *layer = lv_event_get_layer(e);
    for (int i = 0; i < n; i++) {
      const int from = (int) (i * per), to = std::max(from + 1, std::min(music_assistant::WAVE_BINS, (int) ((i + 1) * per)));
      int sum = 0;
      for (int b = from; b < to; b++) sum += wave[b];
      const int half = std::max(bar_w / 2, sum / (to - from) * h / peak / 2);
      const int x = area.x1 + (w - n * pitch) / 2 + i * pitch;
      const lv_area_t bar = {x, mid - half, x + bar_w - 1, mid + half};
      lv_draw_rect(layer, i < played ? &on : &off, &bar);
    }
  }

  int wave_y_(int px, int length) const {
    return wave_h_ / 2 + (int) lroundf(wave_amp_ * sinf(6.2831853f * px / length));
  }

  void theme_wave_() {
    lv_obj_invalidate(track_);
    lv_obj_set_style_line_color(dim_, ui::color(theme::TRACK), 0);
    lv_obj_set_style_line_opa(dim_, LV_OPA_COVER, 0);
    lv_obj_set_style_line_color(fill_, ui::color(theme::ACCENT), 0);
  }

  // `filled` pixels of the track played: light that much of the wave and put the ball there.
  void move_wave_(int filled) {
    const bool analysed = has_waveform_();
    if (analysed != analysed_) {
      analysed_ = analysed;
      for (lv_obj_t *obj : {dim_, fill_, ball_}) (analysed ? lv_obj_add_flag : lv_obj_remove_flag)(obj, LV_OBJ_FLAG_HIDDEN);
      lv_obj_invalidate(track_);
    }
    if (analysed) {
      const float played = text_w_ > 0 ? (float) filled / text_w_ : 0.0f;
      const int step = (int) (played * 100);
      const uint8_t *data = plugin_->ma()->now_playing().waveform.data();
      if (step != played_step_ || data != wave_data_) lv_obj_invalidate(track_);
      played_ = played, played_step_ = step, wave_data_ = data;
      return;
    }
    const int x = std::clamp(filled, ball_r_, text_w_ - ball_r_);
    const int count = std::max(2, std::min((int) wave_.size(), (x - ball_r_) / WAVE_STEP + 2));
    if (count != wave_lit_) {
      wave_lit_ = count;
      lv_line_set_points(fill_, wave_.data(), count);
    }
    const int bx = x - ball_r_, by = wave_y_(x, wave_len_) - ball_r_;
    if (lv_obj_get_x(ball_) != bx || lv_obj_get_y(ball_) != by) lv_obj_set_pos(ball_, bx, by);
  }

  enum Key : int { KEY_PREV = 1, KEY_PLAY, KEY_NEXT, KEY_COVER, KEY_QUEUE, KEY_ROOMS, KEY_VOLUME };

  // The words and keys under the cover, their height; the keys take what the card has left, a finger's size at most.
  int layout_() {
    // The wave as high as 7 mm (40 px at most), never more than a sixth of the tile: a small tile keeps its cover.
    wave_h_ = std::clamp(std::min(ui::mm(7), height_ / 6), ui::px(14), ui::px(40));
    int text_h = (show_title_ ? title_lh_() : 0) + artist_lh_() + ui::px(14) +
                 wave_h_ + ui::line_height(Font::BODY);
    // Beside a tall cover the keys grow with the column, a palm's width at most; under it they stay a finger's size.
    const int column = side_by_side_ ? width_ - height_ - ui::px(24) : width_;
    roomy_ = side_by_side_ && height_ >= ui::mm(70);
    const bool roomy = roomy_;
    gap_ = roomy ? ui::mm(6) : ui::px(10);
    bar_h_ = bar_ ? (roomy ? ui::mm(9) : ui::mm(7)) : 0;
    if (bar_) text_h += bar_h_ + gap_;
    const int largest = std::min(roomy ? ui::mm(15) : ui::mm(9), column * 10 / 33);
    key_h_ = keys_ ? std::max(ui::mm(6), std::min(largest, height_ - text_h - ui::px(10))) : 0;
    if (keys_) text_h += key_h_ + gap_;
    return text_h;
  }

  // Beside the cover the words must fit the height; under it the cover keeps a third of the tile at least.
  bool bar_fits_(int text_h) const {
    const int text_w = side_by_side_ ? width_ - height_ - ui::px(24) : width_;
    if (text_w < 2 * bar_h_ + ui::mm(20)) return false;
    return side_by_side_ ? text_h <= height_ : height_ - text_h - ui::px(10) >= height_ / 3;
  }

  // As Sonos has it: the queue on the left, the room as a pill in the middle (the rooms screen), the volume on the
  // right, which turns the row into a slider for a few seconds. Queue and volume are bare icons, the pill is the one
  // solid part of the row.
  void build_bar_(lv_obj_t *parent, int x, int y, int w) {
    const lv_font_t *icons = plugin_->icon_font(bar_h_ * 2 / 3), *speaker = plugin_->icon_font(bar_h_ / 2);
    queue_ = key_(parent, KEY_QUEUE, GLYPH_QUEUE, icons, x, y, bar_h_, GLASS);
    volume_ = key_(parent, KEY_VOLUME, GLYPH_VOLUME, icons, x + w - bar_h_, y, bar_h_, GLASS);
    pill_x_ = x + bar_h_ + ui::px(12);
    pill_max_ = w - 2 * (bar_h_ + ui::px(12));
    pill_pad_ = bar_h_ / 3;
    pill_ = key_(parent, KEY_ROOMS, GLYPH_SPEAKER, speaker, pill_x_, y, bar_h_, GLASS);
    lv_obj_set_style_radius(pill_, bar_h_ / 2, 0);
    lv_obj_align(glyph_of(pill_), LV_ALIGN_LEFT_MID, pill_pad_, 0);
    room_x_ = pill_pad_ + lv_font_get_line_height(speaker) + ui::px(8);
    room_ = ui::label(pill_, Font::BODY_LARGE, theme::INK);
    lv_obj_align(room_, LV_ALIGN_LEFT_MID, room_x_, 0);

    slider_ = lv_slider_create(parent);
    lv_obj_set_pos(slider_, pill_x_, y + bar_h_ / 2 - ui::px(5));
    lv_obj_set_size(slider_, x + w - bar_h_ - ui::px(12) - pill_x_ - ui::px(8), ui::px(10));
    lv_slider_set_range(slider_, 0, 100);
    lv_obj_set_style_pad_all(slider_, ui::px(6), LV_PART_KNOB);
    lv_obj_set_style_radius(slider_, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(slider_, 0, LV_PART_KNOB);
    lv_obj_set_ext_click_area(slider_, bar_h_ / 2);
    lv_obj_add_event_cb(slider_, on_slider_, LV_EVENT_VALUE_CHANGED, this);
    lv_obj_add_event_cb(slider_, on_slider_, LV_EVENT_RELEASED, this);
    lv_obj_add_flag(slider_, LV_OBJ_FLAG_HIDDEN);
  }

  void theme_keys_() {
    theme_wave_();
    for (lv_obj_t *key : {prev_, next_, queue_, volume_, pill_}) {
      if (key == nullptr) continue;
      glass_(key, dark_look_());
      ui::set_color(glyph_of(key), key == queue_ || key == volume_ ? theme::MUTED : theme::INK);
    }
    if (play_ != nullptr) ui::set_color(glyph_of(play_), theme::ON_ACCENT);
    if (!bar_) return;
    ui::set_color(glyph_of(pill_), theme::ACCENT);
    ui::set_color(room_, theme::INK);
    color_volume_();
    lv_obj_set_style_bg_color(slider_, ui::color(theme::TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider_, ui::color(theme::ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider_, ui::color(theme::INK), LV_PART_KNOB);
  }

  // The volume icon lights up while its slider is open.
  void color_volume_() {
    if (tint_ && tone_ != 0) {
      text_color(glyph_of(volume_), 0xFFFFFF);
      return;
    }
    ui::set_color(glyph_of(volume_), volume_open_ ? theme::ACCENT : theme::MUTED);
  }

  void tick_bar_(const music_assistant::MAPlayer *player) {
    const std::string room = player != nullptr ? player->display_name : "";
    if (room != room_text_) {
      room_text_ = room;
      ui::set_text(room_, room);
      // The pill as wide as its words, never wider than the room between the keys.
      const int text_w = ui::text_width(room, Font::BODY_LARGE) + ui::px(4);
      const int pill_w = std::min(pill_max_, std::max(bar_h_, room_x_ + text_w + pill_pad_ + ui::px(4)));
      lv_obj_set_width(room_, std::max(0, pill_w - room_x_ - pill_pad_));
      lv_obj_set_width(pill_, pill_w);
      lv_obj_set_x(pill_, pill_x_ + (pill_max_ - pill_w) / 2);
    }
    if (!volume_open_) return;
    const bool dragging = lv_obj_has_state(slider_, LV_STATE_PRESSED);
    if (!dragging && player != nullptr && player->volume_level >= 0 &&
        lv_slider_get_value(slider_) != player->volume_level && millis() - volume_ms_ > 1500)
      lv_slider_set_value(slider_, player->volume_level, LV_ANIM_OFF);
    if (!dragging && millis() - volume_ms_ > VOLUME_OPEN_MS) show_volume_(false);
  }

  void show_volume_(bool open) {
    volume_open_ = open;
    volume_ms_ = millis();
    for (lv_obj_t *obj : {queue_, pill_}) (open ? lv_obj_add_flag : lv_obj_clear_flag)(obj, LV_OBJ_FLAG_HIDDEN);
    (open ? lv_obj_clear_flag : lv_obj_add_flag)(slider_, LV_OBJ_FLAG_HIDDEN);
    color_volume_();
    if (!open) return;
    auto *ma = plugin_->ma();
    const auto *player = ma != nullptr ? ma->player(ma->selected_player()) : nullptr;
    if (player != nullptr && player->volume_level >= 0) lv_slider_set_value(slider_, player->volume_level, LV_ANIM_OFF);
  }

  static void on_slider_(lv_event_t *e) {
    auto *tile = static_cast<NowTile *>(lv_event_get_user_data(e));
    tile->volume_ms_ = millis();
    if (lv_event_get_code(e) != LV_EVENT_RELEASED) return;
    auto *ma = tile->plugin_->ma();
    if (ma != nullptr && ma->is_connected()) ma->cmd_set_volume(lv_slider_get_value(tile->slider_));
  }

  // Previous, play or pause, next under the words: play a solid disc, the other two bare icons beside it.
  void build_keys_(lv_obj_t *parent, int x, int y, int w) {
    const int small = key_h_ * 3 / 4;
    const int gap = std::max(ui::px(8), std::min(key_h_ / 2, (w - key_h_ - 2 * small) / 4));
    int at = x + (w - (key_h_ + 2 * small + 2 * gap)) / 2;
    const lv_font_t *beside = plugin_->icon_font(small * 4 / 5), *middle = plugin_->icon_font(key_h_ * 3 / 5);
    prev_ = key_(parent, KEY_PREV, ui::icon(ICON_PREV).c_str(), beside, at, y + (key_h_ - small) / 2, small, GLASS);
    at += small + gap;
    play_ = key_(parent, KEY_PLAY, ui::icon(ICON_PLAY).c_str(), middle, at, y, key_h_, SOLID);
    at += key_h_ + gap;
    next_ = key_(parent, KEY_NEXT, ui::icon(ICON_NEXT).c_str(), beside, at, y + (key_h_ - small) / 2, small, GLASS);
  }

  // GLASS: a milky pane over the card (liquid glass, faked: LVGL blurs nothing live); SOLID: the accent.
  enum Look { GLASS, SOLID };

  // An even milky pane (a gradient bands in RGB565: LVGL does not dither it), a light rim.
  // On a light card the pane is near white so it stands out; on a dark one, or on the cover's colours, a breath of white.
  static void glass_(lv_obj_t *key, bool on_dark) {
    lv_obj_set_style_bg_color(key, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(key, on_dark ? 40 : 220, 0);
    lv_obj_set_style_bg_opa(key, on_dark ? 90 : 255, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(key, 1, 0);
    lv_obj_set_style_border_color(key, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(key, on_dark ? 90 : 255, 0);
  }

  // The look is dark when its ink is light.
  static bool dark_look_() { return lv_color_brightness(ui::color(theme::INK)) > 128; }

  lv_obj_t *key_(lv_obj_t *parent, int code, const char *glyph, const lv_font_t *font, int x, int y, int side,
                 Look look) {
    lv_obj_t *key;
    if (look == GLASS) {
      key = lv_obj_create(parent);
      lv_obj_remove_style_all(key);
      lv_obj_remove_flag(key, LV_OBJ_FLAG_SCROLLABLE);
      glass_(key, dark_look_());
    } else {
      key = ui::block(parent, theme::ACCENT);
      lv_obj_set_style_bg_opa(key, LV_OPA_60, LV_STATE_PRESSED);
    }
    lv_obj_set_pos(key, x, y);
    lv_obj_set_size(key, side, side);
    lv_obj_set_style_radius(key, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_flag(key, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(key, reinterpret_cast<void *>(static_cast<intptr_t>(code)));
    lv_obj_add_event_cb(key, on_key_, LV_EVENT_CLICKED, this);
    lv_obj_t *label = ui::label(key, side >= ui::mm(8) ? Font::ICON : Font::ICON_SMALL,
                                look == SOLID ? theme::ON_ACCENT : theme::INK);
    if (font != nullptr) {
      lv_obj_set_style_text_font(label, font, 0);
      lv_obj_set_size(label, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    }
    lv_label_set_text(label, glyph);
    lv_obj_center(label);
    return key;
  }

  static lv_obj_t *glyph_of(lv_obj_t *key) { return lv_obj_get_child(key, 0); }

  static void on_key_(lv_event_t *e) {
    auto *tile = static_cast<NowTile *>(lv_event_get_user_data(e));
    auto *target = static_cast<lv_obj_t *>(lv_event_get_current_target(e));
    tile->press_(static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(target))));
  }

  void press_(int code) {
    if (code == KEY_VOLUME) {
      show_volume_(!volume_open_);
      return;
    }
    if (code == KEY_COVER || code == KEY_QUEUE || code == KEY_ROOMS) {
      plugin_->open_dashboard_at(code == KEY_QUEUE ? 3 : code == KEY_ROOMS ? 2 : 1);
      tessera::open_card(plugin_->plugin_id(), "player", "", tile_);
      return;
    }
    auto *ma = plugin_->ma();
    if (ma == nullptr || !ma->is_connected()) return;
    if (code == KEY_PREV) ma->cmd_previous();
    if (code == KEY_PLAY) ma->cmd_play_pause();
    if (code == KEY_NEXT) ma->cmd_next();
  }

  // The dithered ground as the card's background picture, drawn again for a new colour or size. Tessera colours its
  // cards again when it draws them: every tick puts the picture back (only when it is not there).
  void paint_ground_() {
    const int w = lv_obj_get_width(card_), h = lv_obj_get_height(card_);
    if (w <= 0 || h <= 0) return;
    const lv_image_dsc_t *cover = blur_ ? plugin_->cover().picture() : nullptr;
    const uint32_t made_of = cover != nullptr ? plugin_->cover().revision() : 0;
    if (tone_ != ground_tone_ || made_of != ground_cover_ || w != ground_.header.w || h != ground_.header.h) {
      const size_t bytes = (size_t) w * h * 2;
      if (bytes > ground_cap_) {
        lv_obj_set_style_bg_image_src(card_, nullptr, 0);
        heap_caps_free(ground_px_);
        ground_px_ = static_cast<uint8_t *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM));
        ground_cap_ = ground_px_ == nullptr ? 0 : bytes;
        if (ground_px_ == nullptr) return;
      }
      if (cover != nullptr)
        paint_blur(reinterpret_cast<uint16_t *>(ground_px_), w, h, cover);
      else
        paint_ground(reinterpret_cast<uint16_t *>(ground_px_), w, h, lit(tone_, 140), lit(tone_, 60));
      lv_image_cache_drop(&ground_);
      ground_.header.magic = LV_IMAGE_HEADER_MAGIC;
      ground_.header.cf = LV_COLOR_FORMAT_RGB565;
      ground_.header.w = w;
      ground_.header.h = h;
      ground_.header.stride = w * 2;
      ground_.data = ground_px_;
      ground_.data_size = bytes;
      ground_tone_ = tone_;
      ground_cover_ = made_of;
      lv_obj_set_style_bg_image_src(card_, nullptr, 0);
      lv_obj_invalidate(card_);
    }
    if (lv_obj_get_style_bg_image_src(card_, LV_PART_MAIN) != &ground_)
      lv_obj_set_style_bg_image_src(card_, &ground_, 0);
    if (lv_obj_get_style_bg_grad_dir(card_, LV_PART_MAIN) != LV_GRAD_DIR_NONE)
      lv_obj_set_style_bg_grad_dir(card_, LV_GRAD_DIR_NONE, 0);
  }

  TesseraMusic *plugin_;
  std::string player_;
  lv_obj_t *card_{};
  bool tint_{false}, blur_{false};
  uint32_t ground_cover_{0};
  uint32_t tone_{0}, ground_tone_{0};
  lv_image_dsc_t ground_{};
  uint8_t *ground_px_{nullptr};
  size_t ground_cap_{0};
  bool show_title_{true}, side_by_side_{false};
  Font title_font_{Font::HEADLINE};
  const lv_font_t *title_big_{nullptr}, *artist_big_{nullptr};
  int title_lh_() const { return title_big_ != nullptr ? lv_font_get_line_height(title_big_) : ui::line_height(title_font_); }
  int artist_lh_() const {
    return artist_big_ != nullptr ? lv_font_get_line_height(artist_big_) : ui::line_height(Font::BODY_LARGE);
  }
  int width_{0}, height_{0}, side_{0}, text_w_{0}, tile_{-1};
  uint32_t cover_revision_{0};
  static constexpr int WAVE_STEP = 3;
  std::vector<lv_point_precise_t> wave_;
  lv_obj_t *dim_{}, *ball_{};
  int wave_h_{0}, wave_amp_{0}, ball_r_{0}, wave_len_{1}, wave_lit_{0}, played_step_{-1};
  bool analysed_{false};
  float played_{0};
  const uint8_t *wave_data_{nullptr};
  lv_obj_t *art_{}, *note_{}, *image_{}, *title_{}, *artist_{}, *track_{}, *fill_{}, *elapsed_{}, *total_{};
  lv_obj_t *prev_{}, *play_{}, *next_{};
  bool keys_{true};
  int key_h_{0}, gap_{0};
  bool roomy_{false};
  lv_obj_t *queue_{}, *pill_{}, *volume_{}, *room_{}, *slider_{};
  bool bar_{true}, volume_open_{false};
  int bar_h_{0}, pill_x_{0}, pill_max_{0}, pill_pad_{0}, room_x_{0};
  uint32_t volume_ms_{0};
  std::string room_text_;
};

// The card a tap on the tile opens. It draws nothing itself: the dashboard lies over the whole glass, and the card
// is what Tessera closes on standby, an alert or Back to page 1, as it closes its own.
class DashboardCard : public tessera::Card {
 public:
  explicit DashboardCard(TesseraMusic *plugin) : plugin_(plugin) {}
  ~DashboardCard() override {
    plugin_->dashboard_closed();
    music_assistant::ma_ui::close();
  }
  void open(const tessera::CardContext &) override { plugin_->open_dashboard(); }
  void on_tick(uint32_t) override { plugin_->seen(); }

 protected:
  TesseraMusic *plugin_;
};

void TesseraMusic::setup() {
  add_tile("now_playing", [this]() { return new NowTile(this); });
  add_card("player", [this]() { return new DashboardCard(this); }, true);
}

void TesseraMusic::follow_room(const std::string &name) {
  if (!dashboard_open_ && ma_ != nullptr) ma_->pin_player(name);
}

bool TesseraMusic::build_dashboard_() {
  if (built_) return true;
  for (font::Font *font : fonts_)
    if (font == nullptr || ma_ == nullptr) return false;
  // The time for the dashboard's clock, as espcontrol's display_time label was.
  clock_ = lv_label_create(lv_layer_top());
  lv_obj_add_flag(clock_, LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text(clock_, "--:--");
  const music_assistant::ma_skeleton::Fonts fonts{
      fonts_[0]->get_lv_font(), fonts_[1]->get_lv_font(), fonts_[2]->get_lv_font(), fonts_[3]->get_lv_font(),
      fonts_[4]->get_lv_font(), fonts_[5]->get_lv_font(), fonts_[6]->get_lv_font(), fonts_[7]->get_lv_font(),
      fonts_[8]->get_lv_font(), fonts_[9]->get_lv_font(), fonts_[10]->get_lv_font()};
  const auto handles = music_assistant::ma_skeleton::build(lv_layer_top(), fonts, clock_);
  music_assistant::ma_ui::bind(
      ma_, handles, []() {},
      // the dashboard closed itself (its back key, two minutes untouched): the card goes with it
      [this]() {
        dashboard_open_ = false;
        tessera::close_card();
      },
      []() {});
  built_ = true;
  return true;
}

void TesseraMusic::open_dashboard() {
  if (!build_dashboard_()) {
    tessera::close_card();
    return;
  }
  // The rooms screen chooses the room now: the tile's room stays chosen for a while, then the dashboard follows
  // what plays, as it always did.
  const int room = ma_->selected_player();
  ma_->pin_player("");
  if (room >= 0 && room < (int) ma_->player_count()) ma_->select_player_user(room);
  dashboard_open_ = true;
  seen();
  music_assistant::ma_ui::open(start_screen_);
  start_screen_ = 1;
}

void TesseraMusic::loop() {
  const bool active = seen_ms_ != 0 && millis() - seen_ms_ < ACTIVE_MS;
  if (ma_ != nullptr && !dashboard_open_) ma_->set_ui_active(active);
  if (active) cover_.poll();
  if (!dashboard_open_) return;
  artwork_image::ArtworkImage::poll_all();
  if (millis() - clock_ms_ >= 1000) {
    clock_ms_ = millis();
    const uint32_t now = tessera::epoch();
    if (now != 0) lv_label_set_text(clock_, tessera::clock_text(now).c_str());
  }
  music_assistant::ma_ui::refresh();
}

}  // namespace esphome::tessera_music
