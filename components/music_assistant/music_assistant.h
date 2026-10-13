#pragma once

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/components/socket/socket.h"

#include <memory>
#include <string>
#include <vector>
#include <ArduinoJson.h>

namespace esphome {
namespace music_assistant {

/// The same MA image-proxy URL at another proxy size. The proxy only serves
/// 0 (original), 80, 160, 256, 512, 1024 (verified: other sizes -> HTTP 400);
/// thumbnails must never pull the ~1200 px original (a 2.9 MB decode each).
std::string ma_image_url(const std::string &url, int size);
/// Big now-playing cover: 512 px from MA's image proxy instead of the ~1200 px
/// original (a third of the bytes over WiFi, a much shorter HW decode — both
/// compete with the 10" panel's scan-out). Non-proxy URLs stay untouched.
std::string ma_cover_url(const std::string &url);

/// One entry of a library list (screen 3 tabs), from `music/*/library_items`.
struct MALibraryItem {
    std::string name, subtitle, uri, image_url, media_type;  // media_type: playlist|radio|album|track
    uint32_t duration_s = 0;
};

enum class MALibrary : uint8_t { PLAYLISTS, RADIOS, FAVORITES };

/// One Music Assistant player, as delivered by `players/all`.
struct MAPlayer {
    std::string player_id;
    std::string display_name;
    std::string playback_state;   // "playing" | "paused" | "idle"
    std::string group_name;
    std::string active_queue_id;
    std::string provider;         // "sonos" | "airplay" | ... (screen 2 filters)
    std::vector<std::string> group_members;  // player_ids incl. the leader, empty if solo
    std::string synced_to;        // leader's player_id when this player is a group member
    int volume_level = -1;        // 0..100, -1 unknown
    bool muted = false;
    // current_media (verified probe 23.09., MA 2.10.4 / schema 65)
    std::string title, artist, album, image_url;
    uint32_t duration_s = 0;
    float elapsed_s = 0;
    double elapsed_updated = 0;   // epoch seconds when MA sampled elapsed_s
};

/// Now-playing data for the selected player (from its `current_media`, plus
/// shuffle/repeat from the queue).
struct MANowPlaying {
    std::string title, artist, album, image_url;
    uint32_t duration_s = 0;
    float elapsed_s = 0;
    uint32_t elapsed_at_ms = 0;   // local millis() when elapsed_s was sampled
    bool shuffle = false;
    std::string repeat = "off";   // "off" | "one" | "all"
    std::string queue_id;         // verified: queue_id == player_id
    // favourite state of the playing track (queue current_item.media_item)
    bool favorite = false;
    std::string item_id, item_provider, media_type, uri;
    // RMS waveform (audio_analysis/wave_form), WAVE_BINS values 0..255; empty
    // when MA has no analysis for the track (radio, not yet analysed)
    std::vector<uint8_t> waveform;
};

static constexpr int WAVE_BINS = 216;

/// One row of the queue list (`player_queues/items`, limit 20..40).
struct MAQueueItem {
    std::string queue_item_id, title, artist, image_url;
    uint32_t duration_s = 0;
};

/// Music Assistant WebSocket client (Phase 1).
///
/// Message flow (MA 2.x API, schema >= 28):
///   1. connect to ws://host:port/ws
///   2. first text frame = server info (consumed)
///   3. send {"message_id","command":"auth","args":{"token":...}}
///   4. on result: send "players/all", parse into players_
///   5. poll players/all every poll_interval
///
/// Thread model: everything runs on the ESPHome loop; the socket is
/// non-blocking and polled in loop(). No LVGL calls from here — the UI reads
/// players_ via player()/player_count().
class MusicAssistant : public Component {
  public:
    void setup() override;
    void loop() override;
    void dump_config() override;

    void set_server(const std::string &host, uint16_t port) {
        host_ = host;
        port_ = port;
    }
    /// First token = default. More tokens (e.g. MA stable + MA beta, only one
    /// server runs at a time): after "auth failed" the next one is tried.
    void set_token(const std::string &token) { tokens_.insert(tokens_.begin(), token); token_ = token; }
    void add_token(const std::string &token) { if (!token.empty()) tokens_.push_back(token); }
    void set_poll_interval(uint32_t ms) { poll_interval_ms_ = ms; }
    void add_player_filter(const std::string &display_name) { player_filter_.push_back(display_name); }

    bool is_connected() const { return stage_ == Stage::READY; }
    size_t player_count() const { return players_.size(); }
    const MAPlayer *player(size_t idx) const {
        return idx < players_.size() ? &players_[idx] : nullptr;
    }
    /// Selected player index for the UI (persisted choice comes later).
    int selected_player() const { return selected_player_; }
    /// Clamped programmatic selection (auto-focus keeps working).
    void select_player(int idx);
    /// Keeps the selection on the first player whose name contains `name`
    /// (case-insensitive), with auto-focus off; "" goes back to auto-focus.
    void pin_player(const std::string &name);
    /// Clamped selection from the UI: pauses auto-focus for a grace period so
    /// browsing players is not overridden by the next poll.
    void select_player_user(int idx);

    /// Dashboard on screen: fast polling + queue data; otherwise a slow
    /// player poll only (keeps WiFi RX buffers and the loop free).
    void set_ui_active(bool active);

    /// Index of a player_id in the (filtered) list, -1 if absent.
    int player_index(const std::string &player_id) const;

    // Phase-1 command surface (fire-and-forget; server state arrives via poll)
    void cmd_play_pause();
    void cmd_next();
    void cmd_previous();
    void cmd_set_volume(int pct);
    void cmd_mute(bool muted);
    void cmd_shuffle(bool enabled);
    void cmd_repeat(const char *mode);   // "off" | "one" | "all"
    void cmd_seek(uint32_t seconds);
    void cmd_play_item(const std::string &queue_item_id);
    /// Adds the playing track to / removes it from MA's favourites.
    void cmd_toggle_favorite();
    /// Loads the first page of a library list, or appends the next (more=true).
    void request_library(MALibrary kind, bool more = false);
    const std::vector<MALibraryItem> &library() const { return library_; }
    MALibrary library_kind() const { return library_kind_; }
    bool library_loading() const { return !library_ids_.empty(); }
    bool library_has_more() const { return library_has_more_; }
    /// Frees the list (its strings live in scarce internal RAM).
    void clear_library() {
        library_ = {};
        library_ids_.clear();
        library_has_more_ = false;
    }
    static constexpr int LIBRARY_PAGE = 40;
    /// Dashboard closed: drop queue rows and library (every string of them
    /// sits in internal RAM — ~45 KB after browsing, which pushed WiFi/lwIP
    /// buffers into PSRAM and made the cover-art screensaver flash).
    /// set_ui_active(true) reloads the queue.
    void release_lists() {
        clear_library();
        std::vector<MAQueueItem>().swap(queue_);
        queue_limit_ = 20;
        revision_++;
    }
    /// option = MA QueueOption: "replace" (playlist/album/radio), "play" (track).
    void cmd_play_media(const std::string &uri, const char *option);

    // per-player commands (screen 2 cards)
    void cmd_play_pause(const std::string &player_id);
    void cmd_set_volume(const std::string &player_id, int pct);
    // grouping (verified against MA's /api-docs/commands.json)
    void cmd_set_members(const std::string &leader_id, const std::vector<std::string> &add,
                         const std::vector<std::string> &remove);
    void cmd_ungroup(const std::vector<std::string> &player_ids);

    // Queue / now-playing data (Phase 1)
    const MANowPlaying &now_playing() const { return now_playing_; }
    /// Locally interpolated position: elapsed + wall time while playing.
    float elapsed_now() const;
    const std::vector<MAQueueItem> &queue() const { return queue_; }
    int queue_current_index() const { return queue_current_index_; }
    int queue_total_items() const { return queue_total_items_; }
    /// Rows to load from the current track on (20 default, screen 3 pages up to 40:
    /// ~5 KB JSON per item, the 384 KB rx buffer must hold the whole frame).
    void set_queue_limit(int rows);
    int queue_limit() const { return queue_limit_; }
    static constexpr int MAX_QUEUE_ROWS = 40;
    /// Counts up on every data change; the UI refreshes only when it moves.
    uint32_t revision() const { return revision_; }

  protected:
    enum class Stage {
        WAIT_NETWORK,   // wait for WiFi/IP
        CONNECT,        // TCP connect + WS upgrade in flight
        WAIT_INFO,      // waiting for server info frame
        WAIT_AUTH,      // auth sent, waiting result
        READY,          // authenticated, polling
    };

    // Response routing: MA answers every command with the same message_id;
    // remember what each in-flight id was asking for.
    enum class Pending : uint8_t { NONE, AUTH, PLAYERS, PLAYER, QUEUES, QUEUE_ITEMS, LIBRARY, FAVORITE, WAVEFORM };

    bool start_connect_();
    void finish_connect_();       // checks connect result, sends WS upgrade
    void close_socket_();
    void pump_socket_();          // read available frames, dispatch messages
    void handle_frame_(const char *payload, size_t payload_len);
    const JsonDocument *filter_for_(const char *payload, size_t payload_len) const;
    /// Returns the message_id (empty if not connected).
    std::string send_command_(const std::string &command, const std::string &args_json,
                              Pending kind = Pending::NONE);
    void parse_players_(JsonArrayConst result);
    void load_now_playing_();
    void set_position_(float elapsed_s, double sample_epoch);
    void parse_player_update_(JsonObjectConst player);
    void after_players_changed_(bool selected_updated);
    int pinned_index_() const;
    void poll_next_player_();
    void schedule_queue_poll_(uint32_t delay_ms);
    void parse_queue_(JsonObjectConst queue);
    void parse_queue_items_(JsonArrayConst result);
    void parse_library_(JsonArrayConst result);
    void parse_favorite_(JsonObjectConst item);
    void parse_waveform_(JsonArrayConst bins);
    void request_favorite_state_();
    void request_players_();
    void request_queues_();
    void request_queue_items_();
    void send_queue_items_request_();

    std::vector<std::pair<std::string, Pending>> pending_;

    std::string host_;
    uint16_t port_ = 8095;
    std::string token_;
    std::vector<std::string> tokens_;
    size_t token_idx_ = 0;  // token of the last (or next) auth attempt
    uint32_t poll_interval_ms_ = 2000;
    uint32_t queue_poll_interval_ms_ = 30000;  // + on track change and after own commands

    std::unique_ptr<socket::Socket> sock_;
    Stage stage_ = Stage::WAIT_NETWORK;
    // Frame buffer lives in PSRAM: the players/all response is ~60KB and the
    // internal SRAM on this panel is too fragmented for contiguous blocks
    // that size (std::string growth would throw bad_alloc -> abort).
    uint8_t *rx_psram_ = nullptr;
    size_t rx_len_ = 0;
    size_t rx_cap_ = 0;
    bool upgrade_done_ = false;
    uint32_t upgrade_done_ms_ = 0;
    uint32_t last_poll_ms_ = 0;
    uint32_t last_attempt_ms_ = 0;
    int selected_player_ = 0;
    std::string selected_id_;       // selection survives list reordering/filtering
    std::vector<std::string> player_filter_;
    std::string pinned_;            // lowercase part of a display name, "" = auto-focus
    uint32_t user_select_ms_ = 0;  // last manual selection, gates auto-focus
    std::vector<MAPlayer> players_;
    std::string players_sig_;  // change detector for INFO-level logging

    MANowPlaying now_playing_;
    std::string now_playing_sig_;
    std::vector<MAQueueItem> queue_;
    int queue_current_index_ = -1;
    int queue_total_items_ = 0;
    std::string queue_sig_;              // queue_id|index|count|shuffle|repeat
    uint32_t last_queue_poll_ms_ = 0;
    uint32_t last_items_request_ms_ = 0;
    bool queue_items_dirty_ = false;
    bool ui_active_ = false;
    uint32_t favorite_check_due_ms_ = 0;  // 0 = none scheduled
    double position_sample_epoch_ = 0;    // MA timestamp of the position sample in now_playing_
    bool queue_position_known_ = false;   // queue sample received for this track: ignore the player's
    uint32_t second_queue_poll_ms_ = 0;   // extra confirmation poll after a seek (0 = none)
    uint32_t selected_seen_playing_ms_ = 0;
    static constexpr uint32_t SELECTION_HOLD_MS = 15000;  // buffering on seek must not steal focus
    std::string waveform_uri_;            // track whose waveform was requested
    static constexpr uint32_t PLAYER_POLL_MS = 1000;  // one players/get per second while open
    int poll_tick_ = 0;
    int round_robin_ = 0;
    std::vector<MALibraryItem> library_;
    MALibrary library_kind_ = MALibrary::PLAYLISTS;
    int library_offset_ = 0;
    bool library_has_more_ = false;
    std::vector<std::string> library_ids_;  // in-flight requests; answers for older tabs are dropped
    int queue_limit_ = 20;
    uint32_t revision_ = 0;
    uint32_t last_heap_log_ms_ = 0;

    static constexpr uint32_t USER_SELECT_GRACE_MS = 30000;
    static constexpr uint32_t QUEUE_ITEMS_THROTTLE_MS = 5000;

    static constexpr const char *WS_PATH = "/ws";
    static constexpr uint32_t CONNECT_TIMEOUT_MS = 4000;
    static constexpr uint32_t RETRY_INTERVAL_MS = 5000;
};

}  // namespace music_assistant
}  // namespace esphome
