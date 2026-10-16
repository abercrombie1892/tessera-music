#include "music_assistant.h"

#include "esphome/core/log.h"
#include "esphome/components/network/util.h"

#include <ArduinoJson.h>
#include <algorithm>
#include <sys/time.h>
#include <esp_random.h>
#include <esp_heap_caps.h>
#include <mbedtls/base64.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <cerrno>

namespace esphome {
namespace music_assistant {

// PSRAM-backed ArduinoJson allocator: the players/all document (~60KB JSON)
// must never compete for the fragmented internal SRAM on the P4 panel.
#include <ArduinoJson/Memory/Allocator.hpp>

class PsramAllocator : public ArduinoJson::Allocator {
  public:
    void *allocate(size_t size) override { return heap_caps_malloc(size, MALLOC_CAP_SPIRAM); }
    void deallocate(void *ptr) override { heap_caps_free(ptr); }
    void *reallocate(void *ptr, size_t new_size) override {
        return heap_caps_realloc(ptr, new_size, MALLOC_CAP_SPIRAM);
    }
};
static PsramAllocator psram_alloc;

static const char *const TAG = "music_assistant";

// ── JSON field filters ──────────────────────────────────────────────────────
// A player carries ~45 fields, a queue item its full streamdetails: with a
// filter ArduinoJson only materialises what the UI reads (less PSRAM pool,
// noticeably shorter parse on the loop).

static void add_keys(JsonObject obj, std::initializer_list<const char *> keys) {
    for (const char *key : keys) obj[key] = true;
}

static JsonObject filter_base(JsonDocument &f) {
    add_keys(f.to<JsonObject>(), {"message_id", "error_code", "details"});
    return f.as<JsonObject>();
}

static void add_player_keys(JsonObject p) {
    add_keys(p, {"player_id", "display_name", "name", "playback_state", "group_name", "active_queue_id",
                 "provider", "volume_level", "volume_muted", "synced_to", "group_members", "elapsed_time",
                 "elapsed_time_last_updated"});
    add_keys(p["current_media"].to<JsonObject>(), {"title", "artist", "album", "image_url", "duration"});
}

static void build_players_filter(JsonDocument &f) {
    add_player_keys(filter_base(f)["result"].to<JsonArray>().add<JsonObject>());
}

static void build_player_filter(JsonDocument &f) { add_player_keys(filter_base(f)["result"].to<JsonObject>()); }

static void build_queue_filter(JsonDocument &f) {
    JsonObject q = filter_base(f)["result"].to<JsonObject>();
    add_keys(q, {"queue_id", "shuffle_enabled", "repeat_mode", "current_index", "items", "elapsed_time",
                 "elapsed_time_last_updated"});
    add_keys(q["current_item"]["media_item"].to<JsonObject>(), {"favorite", "item_id", "provider", "media_type", "uri"});
}

static void build_queue_items_filter(JsonDocument &f) {
    JsonObject it = filter_base(f)["result"].to<JsonArray>().add<JsonObject>();
    add_keys(it, {"queue_item_id", "name", "duration"});
    it["media_item"]["artists"].to<JsonArray>().add<JsonObject>()["name"] = true;
    it["image"]["proxy_id"] = true;
}

static void build_favorite_filter(JsonDocument &f) {
    JsonObject item = filter_base(f)["result"].to<JsonObject>();
    add_keys(item, {"favorite", "item_id", "provider", "media_type", "uri"});
    add_keys(item["provider_mappings"].to<JsonArray>().add<JsonObject>(), {"item_id", "provider_instance"});
}

static void build_library_filter(JsonDocument &f) {
    JsonObject it = filter_base(f)["result"].to<JsonArray>().add<JsonObject>();
    add_keys(it, {"name", "uri", "media_type", "duration", "owner"});
    it["artists"].to<JsonArray>().add<JsonObject>()["name"] = true;
    it["metadata"]["images"].to<JsonArray>().add<JsonObject>()["proxy_id"] = true;
}

// message_id comes first in every MA reply; read it before parsing so the
// matching filter can be chosen.
static std::string peek_message_id(const char *payload, size_t len) {
    const std::string head(payload, std::min<size_t>(len, 160));
    size_t pos = head.find("\"message_id\"");
    if (pos == std::string::npos) return "";
    pos = head.find('"', head.find(':', pos) + 1);
    const size_t end = pos == std::string::npos ? pos : head.find('"', pos + 1);
    if (end == std::string::npos) return "";
    return head.substr(pos + 1, end - pos - 1);
}

// ── WebSocket framing (client side, minimal RFC6455 subset) ────────────────
//
// Client -> server frames are always MASKED text frames; server -> client
// frames arrive unmasked. Fragmentation is handled by buffering until a FIN
// frame completes a message. Control frames (ping/pong/close) are answered
// inline.

static constexpr uint8_t WS_TEXT = 0x1, WS_PONG = 0xA;

static std::string ws_client_frame_(const std::string &payload, uint8_t opcode = WS_TEXT) {
    std::string out;
    out.push_back(static_cast<char>(0x80 | opcode));  // FIN + opcode
    uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};  // fixed mask: interceptable
    // but irrelevant — MA is local-LAN only; keep deterministic for debuggability

    size_t len = payload.size();
    if (len < 126) {
        out.push_back(static_cast<char>(0x80 | len));
    } else if (len <= 0xFFFF) {
        out.push_back(static_cast<char>(0x80 | 126));
        out.push_back(static_cast<char>(len >> 8));
        out.push_back(static_cast<char>(len & 0xFF));
    } else {
        out.push_back(static_cast<char>(0x80 | 127));
        for (int i = 7; i >= 0; i--) out.push_back(static_cast<char>((len >> (i * 8)) & 0xFF));
    }
    out.append(reinterpret_cast<const char *>(mask), 4);
    for (size_t i = 0; i < len; i++) out.push_back(payload[i] ^ mask[i % 4]);
    return out;
}

// ── connection ──────────────────────────────────────────────────────────────

bool MusicAssistant::start_connect_() {
    // Never waits: an IP address needs no DNS, the connect runs non-blocking and poll_connect_() asks select()
    // without a timeout on every loop pass whether it is done. A blocking connect froze the screen up to 2 s each
    // retry while Music Assistant was away. Only a host name still asks DNS (once per attempt, as before).
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    if (inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) != 1) {
        struct addrinfo hints = {};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo *res = nullptr;
        if (getaddrinfo(host_.c_str(), nullptr, &hints, &res) != 0 || !res) {
            ESP_LOGW(TAG, "DNS resolve failed for %s", host_.c_str());
            return false;
        }
        addr.sin_addr = reinterpret_cast<struct sockaddr_in *>(res->ai_addr)->sin_addr;
        freeaddrinfo(res);
    }
    sock_ = socket::socket(AF_INET, SOCK_STREAM, 0);
    if (!sock_ || sock_->get_fd() < 0) {
        sock_.reset();
        return false;
    }
    sock_->setblocking(false);
    if (sock_->connect(reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0 && errno != EINPROGRESS) {
        ESP_LOGW(TAG, "connect to %s:%u failed (errno=%d)", host_.c_str(), port_, errno);
        sock_.reset();
        return false;
    }
    connect_started_ms_ = millis();
    stage_ = Stage::CONNECT;
    return true;
}

// The connect in flight: done when the socket turns writable, given up after CONNECT_TIMEOUT_MS.
void MusicAssistant::poll_connect_() {
    const int fd = sock_->get_fd();
    fd_set writable;
    FD_ZERO(&writable);
    FD_SET(fd, &writable);
    struct timeval none = {};
    if (::select(fd + 1, nullptr, &writable, nullptr, &none) > 0) {
        finish_connect_();
        return;
    }
    if (millis() - connect_started_ms_ > CONNECT_TIMEOUT_MS) {
        ESP_LOGW(TAG, "connect to %s:%u timed out", host_.c_str(), port_);
        close_socket_();
    }
}

void MusicAssistant::finish_connect_() {
    int err = 0;
    socklen_t elen = sizeof(err);
    sock_->getsockopt(SOL_SOCKET, SO_ERROR, &err, &elen);
    if (err != 0) {
        ESP_LOGW(TAG, "connect to %s:%u failed: %d", host_.c_str(), port_, err);
        close_socket_();
        return;
    }

    // Send the HTTP upgrade request. Sec-WebSocket-Accept is NOT validated —
    // the MA server is trusted LAN infrastructure; we check the 101 status.
    uint8_t key_raw[16];
    for (auto &b : key_raw) b = static_cast<uint8_t>(esp_random());
    unsigned char key_b64[32] = {0};
    size_t key_len = 0;
    mbedtls_base64_encode(key_b64, sizeof(key_b64), &key_len, key_raw, sizeof(key_raw));

    char req[512];
    int n = snprintf(req, sizeof(req),
                     "GET %s HTTP/1.1\r\n"
                     "Host: %s:%u\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Key: %.*s\r\n"
                     "Sec-WebSocket-Version: 13\r\n"
                     "\r\n",
                     WS_PATH, host_.c_str(), port_, (int) key_len, (char *) key_b64);
    ssize_t w = sock_->write(req, n);
    if (w < 0) {
        ESP_LOGW(TAG, "upgrade write failed");
        close_socket_();
        return;
    }
    // upgrade request sent — next frame is the HTTP 101 + server info
    stage_ = Stage::WAIT_INFO;
    rx_len_ = 0;
    ESP_LOGI(TAG, "connecting to %s:%u ...", host_.c_str(), port_);
}

void MusicAssistant::close_socket_() {
    sock_.reset();
    stage_ = Stage::WAIT_NETWORK;
    rx_len_ = 0;  // PSRAM frame buffer is kept allocated; content is stale
    // the next connection starts with an HTTP 101 again — without this reset
    // its "HTTP/1.1" bytes were parsed as a WS frame (0x48 = opcode 8 CLOSE)
    // and every reconnect closed itself immediately
    upgrade_done_ = false;
    pending_.clear();
    library_ids_.clear();
    last_attempt_ms_ = millis();
}

// ── frame pump ──────────────────────────────────────────────────────────────

void MusicAssistant::pump_socket_() {
    if (!sock_) return;

    // drain everything available — straight into the PSRAM frame buffer.
    // The socket is non-blocking; EWOULDBLOCK simply ends the drain pass
    // (the ESPHome ready() gate is bypassed: this socket is not registered
    // with the app's select loop, so ready() would never fire).
    if (!rx_psram_) {
        rx_cap_ = 384 * 1024;
        rx_psram_ = (uint8_t *) heap_caps_malloc(rx_cap_, MALLOC_CAP_SPIRAM);
        if (!rx_psram_) {
            ESP_LOGE(TAG, "could not allocate PSRAM frame buffer");
            return;
        }
        rx_len_ = 0;
    }
    for (;;) {
        if (rx_len_ >= rx_cap_) {
            ESP_LOGW(TAG, "frame buffer full — reconnecting");
            close_socket_();
            return;
        }
        ssize_t len = sock_->read(rx_psram_ + rx_len_, rx_cap_ - rx_len_);
        if (len < 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) break;
            ESP_LOGW(TAG, "socket read error (errno=%d) — reconnecting", errno);
            close_socket_();
            return;
        }
        if (len == 0) {
            ESP_LOGW(TAG, "server closed the connection");
            close_socket_();
            return;
        }
        rx_len_ += (size_t) len;
    }

    // ── HTTP 101 handshake (first response only) ──
    if (!upgrade_done_) {
        const char header_end[5] = {'\r', '\n', '\r', '\n', 0};
        const void *pos = memmem(rx_psram_, rx_len_, header_end, 4);
        if (!pos) return;  // headers not complete yet
        size_t hdr_end = (size_t) ((const uint8_t *) pos - rx_psram_) + 4;
        std::string headers(reinterpret_cast<const char *>(rx_psram_),
                            hdr_end < 400 ? hdr_end : 400);
        if (headers.find("101") == std::string::npos) {
            ESP_LOGW(TAG, "upgrade rejected: %.80s", headers.c_str());
            close_socket_();
            return;
        }
        rx_len_ -= hdr_end;
        if (rx_len_) memmove(rx_psram_, rx_psram_ + hdr_end, rx_len_);
        upgrade_done_ = true;
        upgrade_done_ms_ = millis();
        ESP_LOGI(TAG, "WebSocket upgrade complete, waiting for server info");
        // fall through: the server info frame may already be buffered
    }

    // ── WebSocket frame pump with fragment reassembly (PSRAM in-place) ──
    for (;;) {
        if (rx_len_ < 2) break;
        const uint8_t *d = rx_psram_;
        bool fin = d[0] & 0x80;
        uint8_t opcode = d[0] & 0x0F;
        bool masked = d[1] & 0x80;
        size_t plen = d[1] & 0x7F;
        size_t hdr = 2;
        if (plen == 126) {
            if (rx_len_ < 4) break;
            plen = (d[2] << 8) | d[3];
            hdr = 4;
        } else if (plen == 127) {
            if (rx_len_ < 10) break;
            uint64_t big = 0;
            for (int i = 0; i < 8; i++) big = (big << 8) | d[2 + i];
            plen = (size_t) big;
            hdr = 10;
        }
        if (plen > 256 * 1024) {
            ESP_LOGW(TAG, "frame pump lost sync (plen=%u) — reconnecting", (unsigned) plen);
            close_socket_();
            return;
        }
        uint8_t mask[4] = {0};
        if (masked) {
            if (rx_len_ < hdr + 4) break;
            memcpy(mask, d + hdr, 4);
            hdr += 4;
        }
        if (rx_len_ < hdr + plen) break;  // wait for the rest of the frame

        uint8_t *payload = rx_psram_ + hdr;  // demask in place
        if (masked) {
            for (size_t i = 0; i < plen; i++) payload[i] ^= mask[i % 4];
        }

        if (opcode == 0x8) {  // close
            ESP_LOGW(TAG, "server closed the websocket");
            close_socket_();
            return;
        }
        if (opcode == 0x9) {  // ping -> pong (mask with same fixed mask)
            // must be a real PONG frame: MA's heartbeat drops the socket after
            // ~25 s otherwise (hidden while the 2 s poll kept it busy)
            std::string pong = ws_client_frame_(std::string((const char *) payload, plen), WS_PONG);
            sock_->write(pong.data(), pong.size());
        } else if (opcode == 0xA) {
            // pong — ignore
        } else if (opcode == 0x1) {  // complete text frame: parse in place
            ESP_LOGD(TAG, "text frame: %u bytes", (unsigned) plen);
            handle_frame_(reinterpret_cast<const char *>(payload), plen);
        } else if (opcode == 0x2) {
            ESP_LOGW(TAG, "binary frame %u bytes — ignoring", (unsigned) plen);
        } else {
            ESP_LOGW(TAG, "unexpected opcode %u, %u bytes", opcode, (unsigned) plen);
        }
        // continuation frames (0x0) are not expected from MA; drop them

        // A handler may have closed the connection (auth failed, ...):
        // close_socket_() already reset rx_len_ to 0, so consuming the frame
        // now underflowed rx_len_ to ~2^64 and the next memmove wrecked the
        // heap (MA beta with a stale token: tlsf assert / store access fault,
        // boot loop every ~28 s). Stop here.
        if (!sock_) return;

        // consume the frame
        size_t consumed = hdr + plen;
        if (consumed >= rx_len_) {
            rx_len_ = 0;
            break;
        }
        memmove(rx_psram_, rx_psram_ + consumed, rx_len_ - consumed);
        rx_len_ -= consumed;
    }
    if (rx_len_ > 512 * 1024) {
        ESP_LOGW(TAG, "rx buffer overflow — reconnecting");
        close_socket_();
    }
}

std::string MusicAssistant::send_command_(const std::string &command, const std::string &args_json,
                                          Pending kind) {
    if (stage_ != Stage::WAIT_AUTH && stage_ != Stage::READY) return "";
    char id[16];
    snprintf(id, sizeof(id), "%u", (unsigned) (millis() ^ 0xA5A5));

    std::string msg = "{\"message_id\":\"" + std::string(id) + "\",\"command\":\"" + command + "\"";
    if (!args_json.empty()) msg += ",\"args\":" + args_json;
    msg += "}";
    // never log the auth frame: it carries the API token
    ESP_LOGD(TAG, "cmd: %s", command == "auth" ? "auth (token hidden)" : msg.c_str());
    if (kind != Pending::NONE) {
        pending_.emplace_back(id, kind);
        if (pending_.size() > 8) pending_.erase(pending_.begin());  // safety valve
    }
    std::string frame = ws_client_frame_(msg);
    sock_->write(frame.data(), frame.size());
    return id;
}

static MAPlayer parse_player(JsonObjectConst po) {
    MAPlayer pl;
    pl.player_id = po["player_id"] | "";
    pl.display_name = po["display_name"] | "";
    if (pl.display_name.empty()) pl.display_name = po["name"] | "";
    pl.playback_state = po["playback_state"] | "";
    pl.group_name = po["group_name"] | "";
    pl.active_queue_id = po["active_queue_id"] | "";
    pl.provider = po["provider"] | "";
    pl.volume_level = po["volume_level"] | -1;
    pl.muted = po["volume_muted"] | false;   // verified field name (probe 23.09.)
    pl.synced_to = po["synced_to"] | "";
    for (JsonVariantConst m : po["group_members"].as<JsonArrayConst>()) {
        const char *mid = m | "";
        if (*mid) pl.group_members.push_back(mid);
    }
    JsonObjectConst cm = po["current_media"].as<JsonObjectConst>();
    pl.title = cm["title"] | "";
    pl.artist = cm["artist"] | "";
    pl.album = cm["album"] | "";
    pl.image_url = cm["image_url"] | "";
    pl.duration_s = cm["duration"] | 0;
    pl.elapsed_s = po["elapsed_time"] | 0.0f;
    pl.elapsed_updated = po["elapsed_time_last_updated"] | 0.0;
    return pl;
}

std::string ma_image_url(const std::string &url, int size) {
    if (url.empty()) return url;
    std::string out = url;
    const size_t pos = out.find("size=");
    if (pos != std::string::npos) {
        const size_t end = out.find('&', pos);
        out.replace(pos, (end == std::string::npos ? out.size() : end) - pos, "size=" + to_string(size));
    } else {
        out += (out.find('?') == std::string::npos ? "?size=" : "&size=") + to_string(size);
    }
    if (out.find("fmt=") == std::string::npos) out += "&fmt=jpg";  // HW JPEG decoder
    return out;
}

std::string ma_cover_url(const std::string &url) {
    return url.find("/imageproxy") != std::string::npos ? ma_image_url(url, 512) : url;
}

// MA reports elapsed_time as of elapsed_time_last_updated (Sonos refreshes it
// only every few seconds): add the sample's age using the SNTP-synced clock,
// otherwise the progress lags and jumps.
static double epoch_now() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return tv.tv_sec < 1600000000 ? 0 : tv.tv_sec + tv.tv_usec / 1e6;  // 0 = clock not synced yet
}

// Position samples carry MA's timestamp: the newest one wins (the queue's is
// fresh every second; a Sonos player's can be a minute old and still say 0.0
// after a seek — verified), and its age is added while playing.
void MusicAssistant::set_position_(float elapsed_s, double sample_epoch) {
    const bool playing = !players_.empty() && players_[selected_player_].playback_state == "playing";
    const double now = epoch_now();
    const double age = playing && now > 0 && sample_epoch > 0 ? now - sample_epoch : 0;
    now_playing_.elapsed_s = elapsed_s + (age > 0 && age < 600 ? (float) age : 0);
    now_playing_.elapsed_at_ms = millis();
    position_sample_epoch_ = sample_epoch;
}

// now-playing for the selected player (shuffle/repeat come from the queue poll)
void MusicAssistant::load_now_playing_() {
    if (players_.empty()) {
        now_playing_ = MANowPlaying{};
        return;
    }
    const MAPlayer &sel = players_[selected_player_];
    if (sel.player_id != now_playing_.queue_id || sel.title != now_playing_.title) {
        position_sample_epoch_ = 0;
        queue_position_known_ = false;
    }
    now_playing_.title = sel.title;
    now_playing_.artist = sel.artist;
    now_playing_.album = sel.album;
    now_playing_.image_url = sel.image_url;
    now_playing_.duration_s = sel.duration_s;
    now_playing_.queue_id = sel.player_id;   // verified: queue_id == player_id
    // A Sonos player reports 0.0 with a fresh timestamp while it buffers
    // after a seek (seen on the panel): once the queue has given a position
    // for this track, the player's is ignored.
    if (!queue_position_known_ && (sel.elapsed_updated > position_sample_epoch_ || position_sample_epoch_ == 0))
        set_position_(sel.elapsed_s, sel.elapsed_updated);
}

void MusicAssistant::parse_players_(JsonArrayConst result) {
    std::vector<MAPlayer> all;
    for (size_t i = 0; i < result.size() && i < 32; i++) {
        JsonObjectConst po = result[i].as<JsonObjectConst>();
        if (po.isNull()) continue;
        MAPlayer pl = parse_player(po);
        if (!pl.player_id.empty()) all.push_back(std::move(pl));
    }
    // configured filter: only these players, in the configured order
    players_.clear();
    if (player_filter_.empty()) {
        players_ = std::move(all);
    } else {
        for (const auto &room : player_filter_)
            for (auto &pl : all)
                if (pl.display_name == room.first) {
                    rename_(pl);
                    players_.push_back(std::move(pl));
                    break;
                }
    }
    ESP_LOGI(TAG, "player list: %u of %u players", (unsigned) players_.size(), (unsigned) result.size());
    after_players_changed_(true);
}

void MusicAssistant::rename_(MAPlayer &player) const {
    for (const auto &room : player_filter_)
        if (room.first == player.display_name && !room.second.empty()) {
            player.display_name = room.second;
            return;
        }
}

// One player from the round-robin players/get poll.
void MusicAssistant::parse_player_update_(JsonObjectConst po) {
    MAPlayer pl = parse_player(po);
    const int idx = player_index(pl.player_id);
    if (idx < 0) return;
    rename_(pl);
    const bool selected = idx == selected_player_;
    players_[idx] = std::move(pl);
    after_players_changed_(selected);
}

void MusicAssistant::after_players_changed_(bool selected_updated) {
    // keep the selection on the same player_id
    selected_player_ = std::max(0, player_index(selected_id_));
    if (selected_player_ >= (int) players_.size()) selected_player_ = 0;

    // auto-focus: prefer a playing player unless the current selection IS the
    // one playing, or the user picked a player recently (UI browsing grace).
    // Makes the dashboard land on the active music on open.
    int playing = -1;
    for (size_t i = 0; i < players_.size(); i++) {
        if (players_[i].playback_state == "playing") { playing = (int) i; break; }
    }
    const int pinned = pinned_index_();
    if (pinned >= 0) {
        selected_player_ = pinned;
        playing = -1;
    }
    const bool selected_playing = players_[selected_player_].playback_state == "playing";
    if (selected_playing) selected_seen_playing_ms_ = millis();
    // switch only when the selection has really stopped (not while it
    // buffers after a seek) — 0 = never seen playing, e.g. right after connect
    const bool selection_stopped =
        selected_seen_playing_ms_ == 0 || millis() - selected_seen_playing_ms_ > SELECTION_HOLD_MS;
    if (playing >= 0 && !selected_playing && selection_stopped &&
        millis() - user_select_ms_ > USER_SELECT_GRACE_MS) {
        selected_player_ = playing;
        selected_seen_playing_ms_ = millis();
    }
    const std::string previous_id = selected_id_;
    selected_id_ = players_.empty() ? "" : players_[selected_player_].player_id;
    // another player's update must not reset the interpolated position
    if (selected_updated || selected_id_ != previous_id) load_now_playing_();

    std::string sig;
    for (const auto &pl : players_) {
        sig += pl.player_id + "/" + pl.playback_state + "/" + to_string(pl.volume_level) + "/" +
               pl.title + "/" + pl.image_url + "/" + pl.synced_to + "/" +
               to_string((int) pl.group_members.size()) + (pl.muted ? "m" : "") + ";";
    }
    if (sig != players_sig_) {
        players_sig_ = sig;
        revision_++;
    }
    std::string np_sig = selected_id_ + "|" + now_playing_.title + "|" + now_playing_.artist + "|" +
                         to_string((uint32_t) now_playing_.elapsed_s);
    if (np_sig != now_playing_sig_) {
        const bool new_track = now_playing_sig_.compare(0, np_sig.rfind('|'), np_sig, 0, np_sig.rfind('|')) != 0;
        now_playing_sig_ = np_sig;
        revision_++;
        if (new_track) {
            ESP_LOGI(TAG, "now: %s | %s", now_playing_.title.c_str(), now_playing_.artist.c_str());
            schedule_queue_poll_(0);  // new track: queue index + "up next" rows
        }
    }
}

// Next players/get: every other tick the selected player (drives screen 1),
// in between the others round-robin — one ~3.5 KB frame per second instead
// of players/all's 30 KB burst, which starved the WiFi RX buffers.
void MusicAssistant::poll_next_player_() {
    if (players_.empty()) {
        request_players_();
        return;
    }
    int idx = selected_player_;
    if (++poll_tick_ % 2 != 0 && players_.size() > 1) {
        do {
            round_robin_ = (round_robin_ + 1) % (int) players_.size();
        } while (round_robin_ == selected_player_);
        idx = round_robin_;
    }
    send_command_("players/get", "{\"player_id\":\"" + players_[idx].player_id + "\"}", Pending::PLAYER);
}

void MusicAssistant::schedule_queue_poll_(uint32_t delay_ms) {
    last_queue_poll_ms_ = millis() + delay_ms - queue_poll_interval_ms_;
}

void MusicAssistant::request_players_() { send_command_("players/all", "", Pending::PLAYERS); }

// Only the selected player's queue (player_queues/all carries every queue
// with its current_item — several times the size, every 10 s).
void MusicAssistant::request_queues_() {
    last_queue_poll_ms_ = millis();
    if (now_playing_.queue_id.empty()) return;
    send_command_("player_queues/get", "{\"queue_id\":\"" + now_playing_.queue_id + "\"}", Pending::QUEUES);
}

void MusicAssistant::set_ui_active(bool active) {
    if (active == ui_active_) return;
    ui_active_ = active;
    if (!active) return;
    last_poll_ms_ = 0;          // fresh data right away when the dashboard opens
    schedule_queue_poll_(0);
    queue_items_dirty_ = true;
    // a configured room that was offline at connect is picked up by a full list
    if (!player_filter_.empty() && players_.size() < player_filter_.size()) request_players_();
}

// Marks the rows stale; loop() sends the request once the throttle allows,
// so a second queue change inside the throttle window is not lost.
void MusicAssistant::request_queue_items_() { queue_items_dirty_ = true; }

void MusicAssistant::send_queue_items_request_() {
    if (!queue_items_dirty_ || now_playing_.queue_id.empty()) return;
    if (millis() - last_items_request_ms_ < QUEUE_ITEMS_THROTTLE_MS) return;
    queue_items_dirty_ = false;
    last_items_request_ms_ = millis();
    // Start at the current index so row 0 is the playing track.
    int offset = queue_current_index_ > 0 ? queue_current_index_ : 0;
    send_command_("player_queues/items",
                  "{\"queue_id\":\"" + now_playing_.queue_id + "\",\"limit\":" + to_string(queue_limit_) +
                      ",\"offset\":" + to_string(offset) + "}",
                  Pending::QUEUE_ITEMS);
}

void MusicAssistant::set_queue_limit(int rows) {
    rows = std::max(20, std::min(MAX_QUEUE_ROWS, rows));
    if (rows == queue_limit_) return;
    queue_limit_ = rows;
    queue_items_dirty_ = true;
    last_items_request_ms_ = 0;  // the list is waiting for these rows: no throttle
}

void MusicAssistant::parse_queue_(JsonObjectConst q) {
    const std::string want = now_playing_.queue_id;
    if (want.empty() || (q["queue_id"] | "") != want) return;
    // The queue item's media_item is a snapshot from enqueue time — its
    // "favorite" never changes (verified: library says true, queue false).
    // Only the URI is taken from here; the state comes from the library.
    const double position_epoch = q["elapsed_time_last_updated"] | 0.0;
    if (position_epoch > 0) queue_position_known_ = true;
    if (position_epoch > position_sample_epoch_) set_position_(q["elapsed_time"] | 0.0f, position_epoch);
    JsonObjectConst mi = q["current_item"]["media_item"].as<JsonObjectConst>();
    const std::string uri = mi["uri"] | "";
    if (uri != now_playing_.uri) {
        now_playing_.uri = uri;
        now_playing_.media_type = mi["media_type"] | "";
        now_playing_.item_id.clear();
        now_playing_.item_provider.clear();
        if (now_playing_.favorite) {
            now_playing_.favorite = false;
            revision_++;
        }
    }
    request_favorite_state_();  // on every queue poll: likes made elsewhere show up too
    const bool shuffle = q["shuffle_enabled"] | false;
    const char *repeat = q["repeat_mode"] | "off";
    const int cur = q["current_index"] | -1;
    const int total = q["items"] | 0;
    std::string sig = want + "|" + to_string(cur) + "|" + to_string(total) + "|" +
                      (shuffle ? "1" : "0") + "|" + repeat;
    if (sig == queue_sig_) return;
    queue_sig_ = sig;
    queue_current_index_ = cur;
    queue_total_items_ = total;
    now_playing_.shuffle = shuffle;
    now_playing_.repeat = repeat;
    revision_++;
    ESP_LOGI(TAG, "queue: %s | index %d/%d | shuffle=%d repeat=%s", want.c_str(), cur, total,
             (int) shuffle, repeat);
    request_queue_items_();   // signature moved -> refresh the visible rows
}

void MusicAssistant::request_library(MALibrary kind, bool more) {
    if (stage_ != Stage::READY) return;
    if (!more || kind != library_kind_) {
        library_.clear();
        library_offset_ = 0;
    }
    library_kind_ = kind;
    library_ids_.clear();
    library_has_more_ = false;
    const std::string page =
        "\"limit\":" + to_string(LIBRARY_PAGE) + ",\"offset\":" + to_string(library_offset_);
    auto ask = [&](const char *command, const char *filter) {
        library_ids_.push_back(send_command_(command, "{" + page + filter + "}", Pending::LIBRARY));
    };
    switch (kind) {
        case MALibrary::PLAYLISTS: ask("music/playlists/library_items", ""); break;
        case MALibrary::RADIOS: ask("music/radios/library_items", ""); break;
        case MALibrary::FAVORITES:  // Frank's favourites are albums + tracks (verified 23.09.)
            ask("music/albums/library_items", ",\"favorite\":true");
            ask("music/tracks/library_items", ",\"favorite\":true");
            break;
    }
    library_offset_ += LIBRARY_PAGE;
    revision_++;
}

void MusicAssistant::parse_library_(JsonArrayConst result) {
    const std::string proxy_base = "http://" + host_ + ":" + to_string((int) port_) + "/imageproxy/";
    for (JsonObjectConst it : result) {
        MALibraryItem item;
        item.name = it["name"] | "";
        item.uri = it["uri"] | "";
        item.media_type = it["media_type"] | "";
        item.duration_s = it["duration"] | 0;
        const std::string artist = it["artists"][0]["name"] | "";
        const std::string owner = it["owner"] | "";
        if (item.media_type == "track") item.subtitle = artist;
        else if (item.media_type == "album") item.subtitle = artist.empty() ? "Album" : artist + " · Album";
        else if (item.media_type == "playlist") item.subtitle = owner.empty() ? "Playlist" : owner;
        else if (item.media_type == "radio") item.subtitle = "Radio";
        const char *proxy_id = it["metadata"]["images"][0]["proxy_id"] | "";
        if (*proxy_id) item.image_url = ma_image_url(proxy_base + proxy_id, 80);
        if (!item.uri.empty()) library_.push_back(std::move(item));
    }
    if ((int) result.size() >= LIBRARY_PAGE) library_has_more_ = true;
    ESP_LOGI(TAG, "library: %u items%s", (unsigned) library_.size(), library_has_more_ ? " (more)" : "");
}

void MusicAssistant::cmd_play_media(const std::string &uri, const char *option) {
    if (now_playing_.queue_id.empty() || uri.empty()) return;
    send_command_("player_queues/play_media", "{\"queue_id\":\"" + now_playing_.queue_id + "\",\"media\":\"" +
                                                  uri + "\",\"option\":\"" + option + "\"}");
    schedule_queue_poll_(800);  // the new queue should show up right away
}

void MusicAssistant::parse_queue_items_(JsonArrayConst result) {
    queue_.clear();
    for (JsonObjectConst it : result) {
        if ((int) queue_.size() >= queue_limit_) break;
        MAQueueItem item;
        item.queue_item_id = it["queue_item_id"] | "";
        item.title = it["name"] | "";
        item.duration_s = it["duration"] | 0;
        JsonObjectConst mi = it["media_item"].as<JsonObjectConst>();
        if (!mi.isNull()) {
            JsonArrayConst arts = mi["artists"].as<JsonArrayConst>();
            if (!arts.isNull() && arts.size() > 0) item.artist = arts[0]["name"] | "";
        }
        // verified image shape: image.proxy_id -> /imageproxy/<proxy_id>
        const char *proxy_id = it["image"]["proxy_id"] | "";
        if (*proxy_id) {
            // only ever shown as 52/72 px thumbnails
            item.image_url = ma_image_url(
                "http://" + host_ + ":" + to_string((int) port_) + "/imageproxy/" + proxy_id, 80);
        }
        if (!item.title.empty()) queue_.push_back(item);
    }
    revision_++;
    ESP_LOGI(TAG, "queue items: %u rows (from index %d)", (unsigned) queue_.size(),
             queue_current_index_);
    for (size_t i = 0; i < queue_.size() && i < 5; i++) {
        ESP_LOGI(TAG, "  [%zu] %s — %s (%u:%02u)", i, queue_[i].title.c_str(),
                 queue_[i].artist.c_str(), (unsigned)(queue_[i].duration_s / 60),
                 (unsigned)(queue_[i].duration_s % 60));
    }
}

float MusicAssistant::elapsed_now() const {
    float e = now_playing_.elapsed_s;
    if (now_playing_.elapsed_at_ms != 0) {
        const bool playing = !players_.empty() &&
                             selected_player_ < (int) players_.size() &&
                             players_[selected_player_].playback_state == "playing";
        if (playing) e += (millis() - now_playing_.elapsed_at_ms) / 1000.0f;
    }
    if (now_playing_.duration_s > 0 && e > (float) now_playing_.duration_s)
        e = (float) now_playing_.duration_s;
    return e;
}

const JsonDocument *MusicAssistant::filter_for_(const char *payload, size_t payload_len) const {
    const std::string id = peek_message_id(payload, payload_len);
    if (id.empty()) return nullptr;
    Pending kind = Pending::NONE;
    for (const auto &entry : pending_)
        if (entry.first == id) { kind = entry.second; break; }
    auto make = [](void (*build)(JsonDocument &)) {
        JsonDocument f(&psram_alloc);
        build(f);
        return f;
    };
    static const JsonDocument players = make(build_players_filter);
    static const JsonDocument player = make(build_player_filter);
    static const JsonDocument queue = make(build_queue_filter);
    static const JsonDocument queue_items = make(build_queue_items_filter);
    static const JsonDocument library = make(build_library_filter);
    static const JsonDocument favorite = make(build_favorite_filter);
    switch (kind) {
        case Pending::PLAYERS: return &players;
        case Pending::PLAYER: return &player;
        case Pending::QUEUES: return &queue;
        case Pending::QUEUE_ITEMS: return &queue_items;
        case Pending::LIBRARY: return &library;
        case Pending::FAVORITE: return &favorite;
        default: return nullptr;
    }
}

void MusicAssistant::handle_frame_(const char *payload, size_t payload_len) {
    // The first TEXT frame after the 101 handshake is the server info
    // (no message_id). MA schema >= 28 requires the auth command next.
    if (stage_ == Stage::WAIT_INFO) {
        JsonDocument info;
        if (deserializeJson(info, payload) == DeserializationError::Ok && info["server_version"].is<const char *>()) {
            ESP_LOGI(TAG, "server info: MA %s (schema %d)", (const char *) info["server_version"],
                     info["schema_version"] | 0);
        }
        stage_ = Stage::WAIT_AUTH;
        if (!tokens_.empty()) token_ = tokens_[token_idx_ % tokens_.size()];
        send_command_("auth", "{\"token\":\"" + token_ + "\"}", Pending::AUTH);
        return;
    }

    // players/all is ~30-60KB of JSON: internal SRAM on this panel is
    // fragmented (largest block ~77KB) — growing the JSON pool there OOMs
    // and the resulting std::bad_alloc aborts (exception stub => reboot
    // loop). Fix: PSRAM-backed pool, unfiltered parse (the pool holds the
    // strings, no internal-RAM copies).
    JsonDocument doc(&psram_alloc);
    const JsonDocument *filter = filter_for_(payload, payload_len);
    DeserializationError err = filter != nullptr
                                   ? deserializeJson(doc, payload, payload_len, DeserializationOption::Filter(*filter))
                                   : deserializeJson(doc, payload, payload_len);
    if (err == DeserializationError::NoMemory) {
        ESP_LOGW(TAG, "JSON pool exhausted on %u-byte frame — skipping", (unsigned) payload_len);
        return;
    }
    if (err != DeserializationError::Ok) return;

    const char *mid = doc["message_id"] | "";
    if (*mid) {
        // route by the pending table (MA echoes the message_id)
        Pending kind = Pending::NONE;
        for (auto it = pending_.begin(); it != pending_.end(); ++it) {
            if (it->first == mid) {
                kind = it->second;
                pending_.erase(it);
                break;
            }
        }
        if (!doc["error_code"].isNull()) {
            ESP_LOGE(TAG, "command %s failed: %s", mid,
                     (const char *) (doc["details"] | "?"));
            if (stage_ == Stage::WAIT_AUTH) {
                if (tokens_.size() > 1) {
                    token_idx_ = (token_idx_ + 1) % tokens_.size();
                    ESP_LOGW(TAG, "auth failed — trying token #%u next (stable/beta switch?)",
                             (unsigned) (token_idx_ + 1));
                } else {
                    ESP_LOGE(TAG, "auth failed — check the API token (music_assistant config)");
                }
                close_socket_();
            }
            return;
        }
        if (kind == Pending::AUTH) {
            ESP_LOGI(TAG, "authenticated (token #%u)", (unsigned) (token_idx_ % (tokens_.empty() ? 1 : tokens_.size()) + 1));
            stage_ = Stage::READY;
            last_poll_ms_ = millis();
            pending_.clear();
            request_players_();
        } else if (kind == Pending::PLAYERS) {
            JsonArrayConst result = doc["result"].as<JsonArrayConst>();
            if (!result.isNull()) parse_players_(result);
        } else if (kind == Pending::PLAYER) {
            JsonObjectConst result = doc["result"].as<JsonObjectConst>();
            if (!result.isNull()) parse_player_update_(result);
        } else if (kind == Pending::QUEUES) {
            JsonObjectConst result = doc["result"].as<JsonObjectConst>();
            if (!result.isNull()) parse_queue_(result);
        } else if (kind == Pending::QUEUE_ITEMS) {
            JsonArrayConst result = doc["result"].as<JsonArrayConst>();
            if (!result.isNull()) parse_queue_items_(result);
        } else if (kind == Pending::WAVEFORM) {
            JsonArrayConst result = doc["result"].as<JsonArrayConst>();  // null = no analysis
            if (!result.isNull()) parse_waveform_(result);
        } else if (kind == Pending::FAVORITE) {
            JsonObjectConst result = doc["result"].as<JsonObjectConst>();
            if (!result.isNull()) parse_favorite_(result);
        } else if (kind == Pending::LIBRARY) {
            auto it = std::find(library_ids_.begin(), library_ids_.end(), std::string(mid));
            if (it == library_ids_.end()) return;  // answer for a tab that is no longer shown
            library_ids_.erase(it);
            // library_items answers with a bare list (verified); tolerate {items: [...]}
            JsonArrayConst result = doc["result"].as<JsonArrayConst>();
            if (result.isNull()) result = doc["result"]["items"].as<JsonArrayConst>();
            if (!result.isNull()) parse_library_(result);
            revision_++;
        }
    }
    // server-pushed events land here in phase 2 (start_listening)
}

// ── state machine ───────────────────────────────────────────────────────────

void MusicAssistant::setup() {
    // Network comes up asynchronously; loop() promotes the state machine.
    stage_ = Stage::WAIT_NETWORK;
}

void MusicAssistant::dump_config() {
    ESP_LOGCONFIG(TAG, "Music Assistant client:");
    ESP_LOGCONFIG(TAG, "  Server: %s:%u", host_.c_str(), port_);
    ESP_LOGCONFIG(TAG, "  Tokens: %u", (unsigned) tokens_.size());
    ESP_LOGCONFIG(TAG, "  Poll interval: %ums", poll_interval_ms_);
    ESP_LOGCONFIG(TAG, "  Players known: %u", (unsigned) players_.size());
}

void MusicAssistant::loop() {
    switch (stage_) {
    case Stage::WAIT_NETWORK:
        if (host_.empty() || tokens_.empty()) break;
        if (network::is_connected() && millis() - last_attempt_ms_ > RETRY_INTERVAL_MS) {
            last_attempt_ms_ = millis();
            start_connect_();
        }
        break;
    case Stage::CONNECT:
        if (sock_) poll_connect_();
        break;
    case Stage::WAIT_INFO:
    case Stage::WAIT_AUTH:
    case Stage::READY:
        pump_socket_();
        // Every frame costs WiFi RX buffers in scarce internal RAM (the
        // hosted-WiFi SDIO driver drops packets and lwIP asserts when they
        // run out): no polling at all while the dashboard is closed, small
        // frames while it is open.
        if (sock_ && stage_ == Stage::READY && ui_active_ && millis() - last_poll_ms_ > PLAYER_POLL_MS) {
            last_poll_ms_ = millis();
            poll_next_player_();
        }
        if (sock_ && stage_ == Stage::READY && ui_active_ &&
            millis() - last_queue_poll_ms_ > queue_poll_interval_ms_) {
            request_queues_();
        }
        if (sock_ && stage_ == Stage::READY && ui_active_) send_queue_items_request_();
        if (second_queue_poll_ms_ != 0 && (int32_t) (millis() - second_queue_poll_ms_) >= 0) {
            second_queue_poll_ms_ = 0;
            schedule_queue_poll_(0);
        }
        if (sock_ && stage_ == Stage::READY && favorite_check_due_ms_ != 0 &&
            (int32_t) (millis() - favorite_check_due_ms_) >= 0)
            request_favorite_state_();
        if (sock_ && stage_ != Stage::READY && upgrade_done_ &&
            millis() - upgrade_done_ms_ > CONNECT_TIMEOUT_MS) {
            ESP_LOGW(TAG, "handshake timeout (no server info after upgrade)");
            close_socket_();
        }
        // Phase-1 acceptance: heap/PSRAM watch, one line per minute
        if (millis() - last_heap_log_ms_ > 60000) {
            last_heap_log_ms_ = millis();
            ESP_LOGI(TAG, "heap internal=%u psram_free=%u players=%u queue_rows=%u rev=%u",
                     (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned) heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                     (unsigned) players_.size(), (unsigned) queue_.size(),
                     (unsigned) revision_);
        }
        break;
    }
}

// ── command surface ─────────────────────────────────────────────────────────

int MusicAssistant::pinned_index_() const {
    if (pinned_.empty()) return -1;
    for (size_t i = 0; i < players_.size(); i++) {
        std::string name = players_[i].display_name;
        std::transform(name.begin(), name.end(), name.begin(), ::tolower);
        if (name.find(pinned_) != std::string::npos) return (int) i;
    }
    return -1;
}

void MusicAssistant::pin_player(const std::string &name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (lower == pinned_) return;
    pinned_ = lower;
    if (!players_.empty()) after_players_changed_(true);
}

void MusicAssistant::select_player(int idx) {
    if (players_.empty()) { selected_player_ = 0; return; }
    if (idx < 0) idx = 0;
    if (idx >= (int) players_.size()) idx = (int) players_.size() - 1;
    selected_player_ = idx;
}

void MusicAssistant::select_player_user(int idx) {
    select_player(idx);
    user_select_ms_ = millis();
    selected_seen_playing_ms_ = 0;
    // the dashboard follows the pick immediately, not on the next poll
    if (selected_id_ != players_[selected_player_].player_id) {
        selected_id_ = players_[selected_player_].player_id;
        load_now_playing_();
        schedule_queue_poll_(0);   // shuffle/repeat + queue of the new player asap
        revision_++;
    }
}

int MusicAssistant::player_index(const std::string &player_id) const {
    if (player_id.empty()) return -1;
    for (size_t i = 0; i < players_.size(); i++)
        if (players_[i].player_id == player_id) return (int) i;
    return -1;
}

void MusicAssistant::cmd_play_pause() {
    if (selected_player() < 0 || selected_player() >= (int) players_.size()) return;
    cmd_play_pause(players_[selected_player()].player_id);
}

void MusicAssistant::cmd_play_pause(const std::string &player_id) {
    send_command_("players/cmd/play_pause", "{\"player_id\":\"" + player_id + "\"}");
}

void MusicAssistant::cmd_next() {
    if (selected_player() < 0 || selected_player() >= (int) players_.size()) return;
    const auto &p = players_[selected_player()];
    send_command_("players/cmd/next", "{\"player_id\":\"" + p.player_id + "\"}");
}

void MusicAssistant::cmd_previous() {
    if (selected_player() < 0 || selected_player() >= (int) players_.size()) return;
    const auto &p = players_[selected_player()];
    send_command_("players/cmd/previous", "{\"player_id\":\"" + p.player_id + "\"}");
}

void MusicAssistant::cmd_set_volume(int pct) {
    if (selected_player() < 0 || selected_player() >= (int) players_.size()) return;
    cmd_set_volume(players_[selected_player()].player_id, pct);
}

void MusicAssistant::cmd_set_volume(const std::string &player_id, int pct) {
    pct = std::max(0, std::min(100, pct));
    send_command_("players/cmd/volume_set",
                  "{\"player_id\":\"" + player_id + "\",\"volume_level\":" + to_string(pct) + "}");
}

static std::string json_id_array(const std::vector<std::string> &ids) {
    std::string out = "[";
    for (size_t i = 0; i < ids.size(); i++) out += (i ? ",\"" : "\"") + ids[i] + "\"";
    return out + "]";
}

void MusicAssistant::cmd_set_members(const std::string &leader_id, const std::vector<std::string> &add,
                                     const std::vector<std::string> &remove) {
    if (leader_id.empty() || (add.empty() && remove.empty())) return;
    send_command_("players/cmd/set_members",
                  "{\"target_player\":\"" + leader_id + "\",\"player_ids_to_add\":" + json_id_array(add) +
                      ",\"player_ids_to_remove\":" + json_id_array(remove) + "}");
}

void MusicAssistant::cmd_ungroup(const std::vector<std::string> &player_ids) {
    if (player_ids.empty()) return;
    send_command_("players/cmd/ungroup_many", "{\"player_ids\":" + json_id_array(player_ids) + "}");
}

void MusicAssistant::cmd_mute(bool muted) {
    if (selected_player() < 0 || selected_player() >= (int) players_.size()) return;
    const auto &p = players_[selected_player()];
    send_command_("players/cmd/volume_mute",
                  "{\"player_id\":\"" + p.player_id + "\",\"muted\":" + (muted ? "true" : "false") + "}");
}

// Queue commands follow the music-assistant/client reference arg names;
// they are exercised live (and their error path logged) from the Phase-2 UI.
void MusicAssistant::cmd_shuffle(bool enabled) {
    if (now_playing_.queue_id.empty()) return;
    send_command_("player_queues/shuffle",
                  "{\"queue_id\":\"" + now_playing_.queue_id + "\",\"shuffle_enabled\":" +
                      (enabled ? "true" : "false") + "}");
    schedule_queue_poll_(800);  // MA applies it asynchronously
}

void MusicAssistant::cmd_repeat(const char *mode) {
    if (now_playing_.queue_id.empty()) return;
    send_command_("player_queues/repeat",
                  "{\"queue_id\":\"" + now_playing_.queue_id + "\",\"repeat_mode\":\"" + mode + "\"}");
    schedule_queue_poll_(800);  // MA applies it asynchronously
}

// Library tracks are removed by their library id; anything else is added via
// its URI (MA puts it into the library), or via the player as a fallback.
void MusicAssistant::request_favorite_state_() {
    favorite_check_due_ms_ = 0;
    if (now_playing_.uri.empty()) return;
    send_command_("music/item_by_uri", "{\"uri\":\"" + now_playing_.uri + "\"}", Pending::FAVORITE);
}

void MusicAssistant::parse_favorite_(JsonObjectConst item) {
    if ((item["uri"] | "") != now_playing_.uri) return;  // answer for an older track
    const bool favorite = item["favorite"] | false;
    now_playing_.item_id = item["item_id"] | "";
    now_playing_.item_provider = item["provider"] | "";
    now_playing_.media_type = item["media_type"] | "track";
    // waveform once per track: audio_analysis/wave_form wants the source
    // provider's id (verified: the library id answers None)
    const bool new_track = waveform_uri_ != now_playing_.uri;
    const bool ask_again = !new_track && now_playing_.waveform.empty() && waveform_asks_ < WAVEFORM_MAX_ASKS &&
                           millis() - waveform_asked_ms_ >= WAVEFORM_RETRY_MS;
    if (new_track || ask_again) {
        if (new_track) {
            waveform_uri_ = now_playing_.uri;
            waveform_asks_ = 0;
            now_playing_.waveform.clear();
            revision_++;
        }
        waveform_asks_++;
        waveform_asked_ms_ = millis();
        JsonObjectConst map = item["provider_mappings"][0].as<JsonObjectConst>();
        const std::string id = map["item_id"] | "", prov = map["provider_instance"] | "";
        if (!id.empty() && !prov.empty())
            send_command_("audio_analysis/wave_form",
                          "{\"item_id\":\"" + id + "\",\"provider_instance_id_or_domain\":\"" + prov + "\"}",
                          Pending::WAVEFORM);
    }
    if (favorite == now_playing_.favorite) return;
    now_playing_.favorite = favorite;
    revision_++;
}

// 1800 normalised RMS bins -> WAVE_BINS bars (max per group, scaled to the
// loudest bar so quiet masters still fill the height).
void MusicAssistant::parse_waveform_(JsonArrayConst bins) {
    const size_t n = bins.size();
    if (n < WAVE_BINS) return;
    std::vector<float> bars(WAVE_BINS, 0.0f);
    float peak = 0;
    for (size_t i = 0; i < n; i++) {
        const int bar = (int) (i * WAVE_BINS / n);
        const float v = bins[i] | 0.0f;
        bars[bar] = std::max(bars[bar], v);
        peak = std::max(peak, v);
    }
    if (peak <= 0) return;
    now_playing_.waveform.resize(WAVE_BINS);
    for (int b = 0; b < WAVE_BINS; b++)
        now_playing_.waveform[b] = (uint8_t) std::min(255.0f, bars[b] / peak * 255.0f);
    revision_++;
}

void MusicAssistant::cmd_toggle_favorite() {
    const MANowPlaying &np = now_playing_;
    if (np.queue_id.empty()) return;
    if (np.favorite && np.item_provider == "library" && !np.item_id.empty()) {
        send_command_("music/favorites/remove_item",
                      "{\"media_type\":\"" + np.media_type + "\",\"library_item_id\":\"" + np.item_id + "\"}");
    } else if (!np.favorite && !np.uri.empty()) {
        send_command_("music/favorites/add_item", "{\"item\":\"" + np.uri + "\"}");
    } else if (!np.favorite) {
        send_command_("players/add_currently_playing_to_favorites", "{\"player_id\":\"" + np.queue_id + "\"}");
    } else {
        return;
    }
    now_playing_.favorite = !now_playing_.favorite;  // optimistic; the library confirms
    revision_++;
    favorite_check_due_ms_ = millis() + 1500;
}

void MusicAssistant::cmd_seek(uint32_t seconds) {
    if (now_playing_.queue_id.empty()) return;
    send_command_("player_queues/seek",
                  "{\"queue_id\":\"" + now_playing_.queue_id + "\",\"position\":" + to_string(seconds) + "}");
    set_position_((float) seconds, epoch_now());  // show the target at once; stale samples lose
    queue_position_known_ = true;                 // from here on only the queue may move it
    revision_++;
    schedule_queue_poll_(1500);
    second_queue_poll_ms_ = millis() + 4000;      // once more after buffering has settled
}

// MA's play_index also takes a queue_item_id — the only stable key: the
// items' own "index" field is always 0 (verified MA 2.10.4, shuffle or not).
void MusicAssistant::cmd_play_item(const std::string &queue_item_id) {
    if (now_playing_.queue_id.empty() || queue_item_id.empty()) return;
    send_command_("player_queues/play_index",
                  "{\"queue_id\":\"" + now_playing_.queue_id + "\",\"index\":\"" + queue_item_id + "\"}");
    schedule_queue_poll_(800);  // MA applies it asynchronously
}

}  // namespace music_assistant
}  // namespace esphome
