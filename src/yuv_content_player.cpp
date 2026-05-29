#include "yuv_content_player.h"

#include <chrono>
#include <cstring>
#include <vector>

#include "logging.h"

namespace bwe {

YuvContentPlayer::YuvContentPlayer(const Config& config,
                                   agora::agora_refptr<agora::rtc::IVideoFrameSender> video_sender)
    : config_(config), video_sender_(video_sender) {}

YuvContentPlayer::~YuvContentPlayer() { stop(); }

void YuvContentPlayer::start() {
  running_.store(true);
  video_thread_ = std::thread(&YuvContentPlayer::video_loop, this);
}

void YuvContentPlayer::stop() {
  running_.store(false);
  if (video_thread_.joinable()) video_thread_.join();
}

FILE* YuvContentPlayer::open_decoder() {
  // Decode the source to raw I420 at the configured size/fps. -stream_loop -1 makes
  // ffmpeg loop the clip forever (when loop_content), so we never see EOF.
  char cmd[1024];
  snprintf(cmd, sizeof(cmd),
           "ffmpeg -loglevel error -nostdin %s -i '%s' "
           "-vf 'scale=%d:%d,fps=%d' -pix_fmt yuv420p -f rawvideo - 2>/dev/null",
           config_.loop_content ? "-stream_loop -1" : "", config_.source_clip.c_str(),
           config_.yuv.width, config_.yuv.height, config_.frame_rate);
  FILE* p = popen(cmd, "r");
  if (!p) LOG_ERROR("yuv_player: failed to launch ffmpeg decoder");
  return p;
}

void YuvContentPlayer::video_loop() {
  using clock = std::chrono::steady_clock;
  const int w = config_.yuv.width;
  const int h = config_.yuv.height;
  const size_t frame_bytes = static_cast<size_t>(w) * h * 3 / 2;  // I420
  const auto frame_interval = std::chrono::microseconds(1000000 / config_.frame_rate);

  FILE* dec = open_decoder();
  if (!dec) return;

  LOG_INFO("yuv_player: decoding %s -> %dx%d I420 @ %dfps; SDK encodes/adapts (target %dkbps)",
           config_.source_clip.c_str(), w, h, config_.frame_rate, config_.yuv.bitrate_kbps);

  std::vector<uint8_t> buf(frame_bytes);
  int64_t pts_ms = 0;
  uint64_t frames_sent = 0, send_fail = 0;
  auto next_send = clock::now();

  while (running_.load()) {
    size_t n = fread(buf.data(), 1, frame_bytes, dec);
    if (n != frame_bytes) {
      // EOF (loop disabled) or decoder died — try to restart if still running.
      pclose(dec);
      if (!running_.load()) break;
      LOG_WARN("yuv_player: decoder ended, restarting");
      dec = open_decoder();
      if (!dec) break;
      continue;
    }

    agora::media::base::ExternalVideoFrame frame;
    frame.type = agora::media::base::ExternalVideoFrame::VIDEO_BUFFER_RAW_DATA;
    frame.format = agora::media::base::VIDEO_PIXEL_I420;
    frame.buffer = buf.data();
    frame.stride = w;
    frame.height = h;
    // Match the official YUV sample: timestamp 0 lets the SDK use arrival time.
    // Feeding an incrementing (clip-relative) timestamp made the SDK treat frames
    // after the first as stale and drop them (encoder input fps went to 0).
    frame.timestamp = 0;

    if (video_sender_->sendVideoFrame(frame) < 0) {
      send_fail++;
    } else {
      frames_sent++;
    }
    if (frames_sent % 150 == 0 && n == frame_bytes) {
      LOG_INFO("yuv_player: sent %llu frames (%llu failed), pts=%lldms",
               (unsigned long long)frames_sent, (unsigned long long)send_fail, (long long)pts_ms);
    }

    pts_ms += 1000 / config_.frame_rate;
    next_send += frame_interval;
    auto now = clock::now();
    if (next_send > now)
      std::this_thread::sleep_for(next_send - now);
    else
      next_send = now;
  }
  if (dec) pclose(dec);
}

}  // namespace bwe
