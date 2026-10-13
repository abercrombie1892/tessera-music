#pragma once

// The part of artwork_image the dashboard uses, on the plugin's own cover loader (the ESP32-P4's JPEG hardware, MA's
// image proxy on the home network). artwork_image itself is espcontrol's, under another licence: not taken along.

#include <string>
#include <vector>

#include "lvgl.h"
#include "esphome/components/music_assistant/cover_loader.h"

namespace esphome {
namespace artwork_image {

class ArtworkImage {
 public:
  // `side`: the square the picture is made for (the YAML's resize: 320x320, COVER).
  explicit ArtworkImage(int side) : side_(side) { all().push_back(this); }

  std::string request_update_url(const std::string &url, int max_source_dim = 0) {
    url_ = url;
    loader_.request(url, side_, true);
    return url;
  }
  const lv_image_dsc_t *get_lv_image_dsc() const { return loader_.picture(); }
  bool has_image() const { return loader_.ready(); }
  std::string get_loaded_url() const { return loader_.ready() ? url_ : std::string(); }
  int get_last_http_status() const { return loader_.ready() ? 200 : 0; }

  // Every loader moves on a step per loop pass, one download at a time.
  static void poll_all() {
    for (ArtworkImage *image : all()) image->loader_.poll();
  }
  // The dashboard closed: the pixels go back.
  static void release_all() {
    for (ArtworkImage *image : all()) {
      image->loader_.release();
      image->url_.clear();
    }
  }

 protected:
  static std::vector<ArtworkImage *> &all() {
    static std::vector<ArtworkImage *> images;
    return images;
  }
  int side_;
  std::string url_;
  tessera_music::CoverLoader loader_;
};

}  // namespace artwork_image
}  // namespace esphome
