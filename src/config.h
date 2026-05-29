#pragma once

#include <string>
#include <vector>

namespace bwe {

struct LevelConfig {
  int id;
  int width;
  int height;
  int bitrate_kbps;
  std::string file;
};

struct AbrConfig {
  // "recommended" = pick the level from the SDK's recommended bitrate
  // (video_encoder_target_bitrate_bps, end-to-end / subscriber-downlink aware).
  // "trend" = legacy instantaneous-vs-rolling-average BWE heuristic.
  std::string selector = "recommended";
  double headroom = 0.85;  // publish level whose bitrate <= recommended * headroom
  double bwe_window_s = 2.0;
  int tick_interval_ms = 500;
  double br_downgrade_factor = 1.3;
  double br_downgrade_duration_s = 3.0;
  double br_upgrade_factor = 1.2;
  double br_upgrade_duration_s = 10.0;
  double cooldown_after_switch_s = 5.0;
  double stale_receiver_timeout_s = 10.0;
};

// Send path for video.
//   ENCODED: push pre-encoded H.264 access units via IVideoEncodedImageSender. The
//            SDK does NO encoding/congestion-control; our ABR picks the level.
//   YUV:     push raw I420 frames via IVideoFrameSender; the SDK encodes and adapts
//            bitrate itself (driven by VideoEncoderConfiguration + uplink BWE).
enum class VideoMode { ENCODED, YUV };

struct YuvConfig {
  int width = 1280;
  int height = 720;
  int bitrate_kbps = 2000;
  int min_bitrate_kbps = 0;  // 0 => SDK default
};

struct Config {
  VideoMode mode = VideoMode::ENCODED;
  std::string channel = "bwe_test";
  int publisher_uid = 1000;
  int frame_rate = 30;
  int gop_frames = 60;
  std::string source_clip = "source_clip/source.mp4";  // YUV mode decodes from here
  YuvConfig yuv;
  // Encoded-mode SenderOptions.targetBitrate (Kbps). This is the CEILING for the
  // SDK's recommended bitrate (video_encoder_target_bitrate_bps never exceeds it).
  int encoded_target_kbps = 6500;
  int audio_sample_rate = 16000;
  int audio_channels = 1;
  std::vector<LevelConfig> levels;
  std::string audio_file = "content/audio.pcm";
  int start_level = 3;
  AbrConfig abr;
  std::string log_file = "/tmp/bwe_publisher_stats.txt";
  std::string network_log = "/tmp/bwe_network.log";
  bool loop_content = true;

  // Load from a JSON file. Throws std::runtime_error on missing/malformed fields.
  static Config load(const std::string& path);
};

}  // namespace bwe
