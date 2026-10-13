#include "cover_loader.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include "driver/jpeg_decode.h"
#include "driver/ppa.h"
#include "esp_heap_caps.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome::tessera_music {

static const char *const TAG = "tessera_music.cover";

static constexpr size_t MAX_BODY = 768 * 1024;
static constexpr size_t READ_PER_PASS = 32 * 1024;
static constexpr uint32_t TIMEOUT_MS = 8000;
static constexpr uint32_t RETRY_MS = 15000;
static constexpr size_t ALIGNMENT = 64;
static constexpr uint32_t SCALE_STEPS = 16;  // the PPA scales in sixteenths

static jpeg_decoder_handle_t decoder() {
  static jpeg_decoder_handle_t handle = nullptr;
  static bool tried = false;
  if (!tried) {
    tried = true;
    jpeg_decode_engine_cfg_t config{};
    config.timeout_ms = 200;
    if (jpeg_new_decoder_engine(&config, &handle) != ESP_OK) handle = nullptr;
  }
  return handle;
}

static ppa_client_handle_t scaler() {
  static ppa_client_handle_t handle = nullptr;
  static bool tried = false;
  if (!tried) {
    tried = true;
    ppa_client_config_t config{};
    config.oper_type = PPA_OPERATION_SRM;
    config.max_pending_trans_num = 1;
    if (ppa_register_client(&config, &handle) != ESP_OK) handle = nullptr;
  }
  return handle;
}

static uint32_t align_up(uint32_t value, uint32_t to) { return (value + to - 1) / to * to; }

// The cover's colour, as the dashboard's glow finds it (vivid_mean): vivid pixels count, grey and black barely do.
static uint32_t mean_tone(const uint8_t *pixels, uint32_t stride_px, uint32_t side) {
  float r = 0, g = 0, b = 0, total = 0;
  const uint32_t step = std::max<uint32_t>(1, side / 32);
  for (uint32_t y = 0; y < side; y += step)
    for (uint32_t x = 0; x < side; x += step) {
      const uint8_t *p = pixels + (y * stride_px + x) * 2;
      const uint16_t v = static_cast<uint16_t>(p[0] | (p[1] << 8));
      const float pr = ((v >> 11) & 0x1F) * (255.0f / 31), pg = ((v >> 5) & 0x3F) * (255.0f / 63),
                  pb = (v & 0x1F) * (255.0f / 31);
      const float hi = std::max({pr, pg, pb}), lo = std::min({pr, pg, pb});
      const float weight = (hi > 0 ? (hi - lo) / 255.0f : 0) + 0.02f;
      r += pr * weight, g += pg * weight, b += pb * weight, total += weight;
    }
  if (total <= 0) return 0;
  const auto channel = [&](float sum) { return static_cast<uint32_t>(std::min(255.0f, sum / total)); };
  return (channel(r) << 16) | (channel(g) << 8) | channel(b);
}

// One download at a time, into one buffer the JPEG hardware reads from: the loader that has it, and the buffer.
static CoverLoader *owner = nullptr;
static uint8_t *body = nullptr;
static size_t body_len = 0, body_cap = 0;

// The proxy serves only these sizes; the next one up from the side we draw.
static int proxy_size(int side) {
  for (int size : {80, 160, 256, 512, 1024})
    if (side <= size) return size;
  return 1024;
}

static std::string sized_url(const std::string &url, int side) {
  const size_t query = url.find("size=");
  const std::string size = std::to_string(proxy_size(side));
  if (query == std::string::npos) return url + (url.find('?') == std::string::npos ? "?size=" : "&size=") + size;
  const size_t end = url.find('&', query);
  return url.substr(0, query + 5) + size + (end == std::string::npos ? "" : url.substr(end));
}

void CoverLoader::request(const std::string &url, int side, bool keep) {
  if (url.empty()) {
    clear();
    return;
  }
  const std::string sized = sized_url(url, side);
  if (sized == wanted_url_ && side == side_) return;
  if (!keep && shown_ >= 0) {
    shown_ = -1;
    revision_++;
  }
  wanted_url_ = sized;
  side_ = side;
  retry_after_ms_ = 0;
  close_();
}

CoverLoader::~CoverLoader() { release(); }

void CoverLoader::release() {
  clear();
  for (Slot &slot : slots_) {
    heap_caps_free(slot.pixels);
    slot = Slot{};
  }
}

void CoverLoader::clear() {
  close_();
  wanted_url_.clear();
  loaded_url_.clear();
  if (shown_ >= 0) {
    shown_ = -1;
    revision_++;
  }
}

void CoverLoader::close_() {
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
  step_ = Step::IDLE;
  sent_ = 0;
  if (owner == this) {
    owner = nullptr;
    body_len = 0;
  }
}

void CoverLoader::fail_(const char *why) {
  ESP_LOGW(TAG, "Cover not loaded (%s): %s", why, wanted_url_.c_str());
  close_();
  retry_after_ms_ = millis() + RETRY_MS;
}

bool CoverLoader::start_() {
  // http://host[:port]/path, the only kind Music Assistant hands out on the home network.
  const std::string &url = wanted_url_;
  if (url.rfind("http://", 0) != 0) {
    // Only the home network's proxy speaks plain http: a cover elsewhere never loads, so it is not asked again.
    loaded_url_ = url;
    return false;
  }
  const size_t slash = url.find('/', 7);
  const std::string authority = url.substr(7, slash == std::string::npos ? std::string::npos : slash - 7);
  path_ = slash == std::string::npos ? "/" : url.substr(slash);
  const size_t colon = authority.find(':');
  host_ = authority.substr(0, colon);
  port_ = colon == std::string::npos ? 80 : static_cast<uint16_t>(atoi(authority.c_str() + colon + 1));

  addrinfo hints{}, *found = nullptr;
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(host_.c_str(), std::to_string(port_).c_str(), &hints, &found) != 0 || found == nullptr) {
    fail_("host unknown");
    return false;
  }
  fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd_ < 0) {
    freeaddrinfo(found);
    fail_("no socket");
    return false;
  }
  fcntl(fd_, F_SETFL, fcntl(fd_, F_GETFL, 0) | O_NONBLOCK);
  const int result = ::connect(fd_, found->ai_addr, found->ai_addrlen);
  freeaddrinfo(found);
  if (result != 0 && errno != EINPROGRESS) {
    fail_("connect");
    return false;
  }
  head_ = "GET " + path_ + " HTTP/1.0\r\nHost: " + authority + "\r\nConnection: close\r\n\r\n";
  sent_ = 0;
  owner = this;
  body_len = 0;
  started_ms_ = millis();
  step_ = Step::CONNECTING;
  return true;
}

void CoverLoader::poll() {
  if (step_ == Step::IDLE) {
    if (wanted_url_.empty() || wanted_url_ == loaded_url_) return;
    if (retry_after_ms_ && (int32_t) (millis() - retry_after_ms_) < 0) return;
    if (owner != nullptr) return;
    start_();
    return;
  }
  if (millis() - started_ms_ > TIMEOUT_MS) {
    fail_("timeout");
    return;
  }
  if (step_ == Step::CONNECTING) {
    fd_set writable;
    FD_ZERO(&writable);
    FD_SET(fd_, &writable);
    timeval none{};
    if (::select(fd_ + 1, nullptr, &writable, nullptr, &none) <= 0) return;
    int error = 0;
    socklen_t length = sizeof(error);
    getsockopt(fd_, SOL_SOCKET, SO_ERROR, &error, &length);
    if (error != 0) {
      fail_("refused");
      return;
    }
    step_ = Step::SENDING;
  }
  if (step_ == Step::SENDING) {
    const ssize_t n = ::send(fd_, head_.data() + sent_, head_.size() - sent_, 0);
    if (n < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK) fail_("send");
      return;
    }
    sent_ += n;
    if (sent_ < head_.size()) return;
    if (body == nullptr) {
      jpeg_decode_memory_alloc_cfg_t config{};
      config.buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER;
      body = static_cast<uint8_t *>(jpeg_alloc_decoder_mem(MAX_BODY, &config, &body_cap));
      if (body == nullptr) {
        fail_("no memory");
        return;
      }
    }
    step_ = Step::RECEIVING;
  }
  size_t read_now = 0;
  while (read_now < READ_PER_PASS) {
    if (body_len >= body_cap) {
      fail_("too large");
      return;
    }
    const ssize_t n = ::recv(fd_, body + body_len, std::min(body_cap - body_len, READ_PER_PASS), 0);
    if (n > 0) {
      body_len += n;
      read_now += n;
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
    if (n < 0) {
      fail_("receive");
      return;
    }
    break;  // the server closed: the answer is whole
  }
  if (read_now >= READ_PER_PASS) return;
  ::close(fd_);
  fd_ = -1;
  if (decode_()) {
    loaded_url_ = wanted_url_;
    retry_after_ms_ = 0;
  } else {
    retry_after_ms_ = millis() + RETRY_MS;
  }
  close_();
}

bool CoverLoader::decode_() {
  const char *ok = "HTTP/1.0 200";
  const char *ok11 = "HTTP/1.1 200";
  if (body_len < 16 || (memcmp(body, ok, 12) != 0 && memcmp(body, ok11, 12) != 0)) {
    ESP_LOGW(TAG, "Cover refused by the server: %s", wanted_url_.c_str());
    return false;
  }
  const uint8_t *end = nullptr;
  for (size_t i = 0; i + 3 < body_len; i++) {
    if (body[i] == '\r' && body[i + 1] == '\n' && body[i + 2] == '\r' && body[i + 3] == '\n') {
      end = body + i + 4;
      break;
    }
  }
  if (end == nullptr) return false;
  const size_t jpeg_len = body_len - (end - body);
  // The decoder reads from the start of its own buffer: move the picture over the header.
  memmove(body, end, jpeg_len);

  jpeg_decoder_handle_t engine = decoder();
  ppa_client_handle_t ppa = scaler();
  jpeg_decode_picture_info_t info{};
  if (engine == nullptr || ppa == nullptr || jpeg_decoder_get_info(body, jpeg_len, &info) != ESP_OK ||
      info.width == 0 || info.height == 0 || info.sample_method == JPEG_DOWN_SAMPLING_GRAY) {
    ESP_LOGW(TAG, "Cover is no picture the hardware decodes (PNG, progressive or grey)");
    return false;
  }
  const uint32_t padded_w = align_up(info.width, 16), padded_h = align_up(info.height, 16);
  const size_t decoded_bytes = static_cast<size_t>(padded_w) * padded_h * 2;
  static uint8_t *decoded = nullptr;
  static size_t decoded_cap = 0;
  if (decoded_cap < decoded_bytes) {
    free(decoded);
    jpeg_decode_memory_alloc_cfg_t config{};
    config.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER;
    decoded = static_cast<uint8_t *>(jpeg_alloc_decoder_mem(decoded_bytes, &config, &decoded_cap));
    if (decoded == nullptr) {
      decoded_cap = 0;
      return false;
    }
  }
  jpeg_decode_cfg_t config{};
  config.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
  config.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;  // little-endian RGB565, as LVGL's own pixels
  config.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;
  uint32_t written = 0;
  const uint32_t started = millis();
  if (jpeg_decoder_process(engine, &config, body, jpeg_len, decoded, decoded_cap, &written) != ESP_OK) {
    ESP_LOGW(TAG, "JPEG hardware refused the cover");
    return false;
  }

  // A square from the middle, scaled in sixteenths to at least the side we draw: the tile trims the rest, so no
  // background shows around the cover.
  const uint32_t crop = std::min(info.width, info.height);
  const uint32_t steps =
      std::max<uint32_t>(1, std::min<uint32_t>(255, (side_ * SCALE_STEPS + crop - 1) / crop));
  const uint32_t out = std::max<uint32_t>(1, (crop * steps + SCALE_STEPS - 1) / SCALE_STEPS);
  Slot &slot = slots_[shown_ == 0 ? 1 : 0];
  const size_t out_bytes = align_up(out * out * 2, ALIGNMENT);
  if (slot.capacity < out_bytes) {
    heap_caps_free(slot.pixels);
    slot.pixels = static_cast<uint8_t *>(heap_caps_aligned_alloc(ALIGNMENT, out_bytes, MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM));
    slot.capacity = slot.pixels == nullptr ? 0 : out_bytes;
    if (slot.pixels == nullptr) return false;
  }
  ppa_srm_oper_config_t scale{};
  scale.in.buffer = decoded;
  scale.in.pic_w = padded_w;
  scale.in.pic_h = padded_h;
  scale.in.block_w = crop;
  scale.in.block_h = crop;
  scale.in.block_offset_x = (info.width - crop) / 2;
  scale.in.block_offset_y = (info.height - crop) / 2;
  scale.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
  scale.out.buffer = slot.pixels;
  scale.out.buffer_size = slot.capacity;
  scale.out.pic_w = out;
  scale.out.pic_h = out;
  scale.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
  scale.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
  scale.scale_x = static_cast<float>(steps) / SCALE_STEPS;
  scale.scale_y = scale.scale_x;
  scale.mode = PPA_TRANS_MODE_BLOCKING;
  if (ppa_do_scale_rotate_mirror(ppa, &scale) != ESP_OK) {
    ESP_LOGW(TAG, "PPA could not scale the cover");
    return false;
  }
  // Exactly the side asked for, from the middle of what the PPA made: the stride keeps the rows apart.
  const uint32_t scaled = crop * steps / SCALE_STEPS, side = std::min<uint32_t>(scaled, side_);
  const uint32_t stride = out * 2, inset = (scaled - side) / 2;
  lv_image_header_t &h = slot.dsc.header;
  h.magic = LV_IMAGE_HEADER_MAGIC;
  h.cf = LV_COLOR_FORMAT_RGB565;
  h.w = side;
  h.h = side;
  h.stride = stride;
  slot.dsc.data = slot.pixels + inset * stride + inset * 2;
  // LVGL wants stride x height bytes from the first pixel on; the inset keeps them inside the buffer.
  slot.dsc.data_size = stride * side;
  tone_ = mean_tone(slot.dsc.data, out, side);
  shown_ = shown_ == 0 ? 1 : 0;
  revision_++;
  ESP_LOGI(TAG, "Cover %ux%u -> %u px in %u ms", (unsigned) info.width, (unsigned) info.height, (unsigned) h.w,
           (unsigned) (millis() - started));
  return true;
}

}  // namespace esphome::tessera_music
