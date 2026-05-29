#include "content_player.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "abr_controller.h"
#include "logging.h"

namespace bwe {

ContentPlayer::ContentPlayer(const Config& config, AbrController& abr,
                             agora::agora_refptr<agora::rtc::IVideoEncodedImageSender> video_sender)
    : config_(config), abr_(abr), video_sender_(video_sender) {}

ContentPlayer::~ContentPlayer() { stop(); }

bool ContentPlayer::initialize() {
  readers_.clear();
  frame_count_ = 0;
  for (const LevelConfig& lv : config_.levels) {
    auto reader = std::make_unique<H264Reader>();
    if (!reader->open(lv.file)) {
      LOG_ERROR("content_player: failed to open level %d file %s", lv.id, lv.file.c_str());
      return false;
    }
    size_t fc = reader->frame_count();
    frame_count_ = (frame_count_ == 0) ? fc : std::min(frame_count_, fc);
    readers_.push_back(std::move(reader));
  }

  // Sanity: levels should be frame-aligned. Warn (don't fail) on mismatch — we loop
  // over the shortest common length and switch only on keyframe boundaries, so a
  // small mismatch is tolerable but worth flagging.
  for (size_t i = 0; i < readers_.size(); ++i) {
    if (readers_[i]->frame_count() != frame_count_) {
      LOG_WARN("content_player: level %zu has %zu frames, common length is %zu — levels may not be "
               "perfectly frame-aligned; re-run prepare_content.sh if switches glitch",
               i, readers_[i]->frame_count(), frame_count_);
    }
  }
  LOG_INFO("content_player: %zu levels loaded, %zu common frames (%.1fs @ %dfps)",
           readers_.size(), frame_count_,
           frame_count_ / static_cast<double>(config_.frame_rate), config_.frame_rate);
  return true;
}

void ContentPlayer::start() {
  running_.store(true);
  video_thread_ = std::thread(&ContentPlayer::video_loop, this);
}

void ContentPlayer::stop() {
  running_.store(false);
  if (video_thread_.joinable()) video_thread_.join();
}

void ContentPlayer::video_loop() {
  using clock = std::chrono::steady_clock;
  const auto frame_interval = std::chrono::microseconds(1000000 / config_.frame_rate);
  const int64_t frame_interval_ms = 1000 / config_.frame_rate;

  int current_level = abr_.target_level();
  size_t frame_index = 0;
  int64_t pts_ms = 0;  // monotonic; does NOT reset on loop, so timestamps never regress
  auto next_send = clock::now();

  LOG_INFO("content_player: video starting at level %d (%dx%d)", current_level,
           config_.levels[current_level].width, config_.levels[current_level].height);

  while (running_.load()) {
    // Apply a pending level switch only on a keyframe boundary, so the receiver
    // always starts the new level from a clean IDR.
    int target = abr_.target_level();
    if (target != current_level && readers_[target]->is_keyframe(frame_index)) {
      LOG_INFO("content_player: switching video L%d -> L%d at frame %zu (keyframe)", current_level,
               target, frame_index);
      current_level = target;
    }

    H264Reader::Frame frame;
    if (!readers_[current_level]->frame_at(frame_index, frame)) {
      // Shouldn't happen within frame_count_, but guard anyway.
      frame_index = 0;
      continue;
    }

    const LevelConfig& lv = config_.levels[current_level];
    agora::rtc::EncodedVideoFrameInfo info;
    info.codecType = agora::rtc::VIDEO_CODEC_H264;
    info.width = lv.width;
    info.height = lv.height;
    info.framesPerSecond = config_.frame_rate;
    info.frameType = frame.is_keyframe ? agora::rtc::VIDEO_FRAME_TYPE_KEY_FRAME
                                       : agora::rtc::VIDEO_FRAME_TYPE_DELTA_FRAME;
    info.rotation = agora::rtc::VIDEO_ORIENTATION_0;
    info.captureTimeMs = pts_ms;
    info.streamType = agora::rtc::VIDEO_STREAM_HIGH;

    if (!video_sender_->sendEncodedVideoImage(frame.data, frame.length, info)) {
      LOG_WARN("content_player: sendEncodedVideoImage failed at frame %zu", frame_index);
    } else {
      bytes_sent_.fetch_add(frame.length, std::memory_order_relaxed);
    }

    // Advance.
    pts_ms += frame_interval_ms;
    frame_index++;
    if (frame_index >= frame_count_) {
      if (config_.loop_content) {
        frame_index = 0;
      } else {
        LOG_INFO("content_player: end of content, stopping video");
        break;
      }
    }

    // Pace to real time using an absolute schedule (avoids drift).
    next_send += frame_interval;
    auto now = clock::now();
    if (next_send > now) {
      std::this_thread::sleep_for(next_send - now);
    } else {
      // Fell behind (e.g. slow send); reset schedule to avoid a burst catch-up.
      next_send = now;
    }
  }
}

}  // namespace bwe
