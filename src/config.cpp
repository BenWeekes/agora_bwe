#include "config.h"

#include <stdexcept>

#include "json.h"

namespace bwe {

Config Config::load(const std::string& path) {
  Json root = Json::parse_file(path);

  Config c;
  std::string mode = root.string_or("mode", "encoded");
  c.mode = (mode == "yuv") ? VideoMode::YUV : VideoMode::ENCODED;
  c.source_clip = root.string_or("source_clip", c.source_clip);
  c.encoded_target_kbps = root.int_or("encoded_target_kbps", c.encoded_target_kbps);
  if (root.has("yuv")) {
    const Json& y = root.at("yuv");
    c.yuv.width = y.int_or("width", c.yuv.width);
    c.yuv.height = y.int_or("height", c.yuv.height);
    c.yuv.bitrate_kbps = y.int_or("bitrate_kbps", c.yuv.bitrate_kbps);
    c.yuv.min_bitrate_kbps = y.int_or("min_bitrate_kbps", c.yuv.min_bitrate_kbps);
  }
  c.channel = root.string_or("channel", c.channel);
  c.publisher_uid = root.int_or("publisher_uid", c.publisher_uid);
  c.frame_rate = root.int_or("frame_rate", c.frame_rate);
  c.gop_frames = root.int_or("gop_frames", c.gop_frames);
  c.audio_sample_rate = root.int_or("audio_sample_rate", c.audio_sample_rate);
  c.audio_channels = root.int_or("audio_channels", c.audio_channels);
  c.audio_file = root.string_or("audio_file", c.audio_file);
  c.start_level = root.int_or("start_level", c.start_level);
  c.log_file = root.string_or("log_file", c.log_file);
  c.network_log = root.string_or("network_log", c.network_log);
  c.loop_content = root.bool_or("loop_content", c.loop_content);

  if (!root.has("levels") || !root.at("levels").is_array())
    throw std::runtime_error("config: 'levels' must be an array");
  for (const Json& lv : root.at("levels").items()) {
    LevelConfig l;
    l.id = lv.at("id").as_int();
    l.width = lv.at("width").as_int();
    l.height = lv.at("height").as_int();
    l.bitrate_kbps = lv.at("bitrate_kbps").as_int();
    l.file = lv.at("file").as_string();
    c.levels.push_back(l);
  }
  if (c.levels.empty()) throw std::runtime_error("config: 'levels' is empty");

  if (root.has("abr")) {
    const Json& a = root.at("abr");
    c.abr.selector = a.string_or("selector", c.abr.selector);
    c.abr.headroom = a.number_or("headroom", c.abr.headroom);
    c.abr.bwe_window_s = a.number_or("bwe_window_s", c.abr.bwe_window_s);
    c.abr.tick_interval_ms = a.int_or("tick_interval_ms", c.abr.tick_interval_ms);
    c.abr.br_downgrade_factor = a.number_or("br_downgrade_factor", c.abr.br_downgrade_factor);
    c.abr.br_downgrade_duration_s = a.number_or("br_downgrade_duration_s", c.abr.br_downgrade_duration_s);
    c.abr.br_upgrade_factor = a.number_or("br_upgrade_factor", c.abr.br_upgrade_factor);
    c.abr.br_upgrade_duration_s = a.number_or("br_upgrade_duration_s", c.abr.br_upgrade_duration_s);
    c.abr.cooldown_after_switch_s = a.number_or("cooldown_after_switch_s", c.abr.cooldown_after_switch_s);
    c.abr.stale_receiver_timeout_s = a.number_or("stale_receiver_timeout_s", c.abr.stale_receiver_timeout_s);
  }

  if (c.start_level < 0 || c.start_level >= static_cast<int>(c.levels.size()))
    throw std::runtime_error("config: start_level out of range");

  return c;
}

}  // namespace bwe
