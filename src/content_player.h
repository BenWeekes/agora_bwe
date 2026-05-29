#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "AgoraBase.h"
#include "NGIAgoraMediaNode.h"
#include "config.h"
#include "h264_reader.h"

namespace bwe {

class AbrController;

// ENCODED-mode video: streams pre-encoded H.264 (the active level) to Agora at
// real-time pace via IVideoEncodedImageSender. Video level is chosen by the
// AbrController; switches are applied only on keyframe boundaries so the receiver
// never sees a broken GOP. Audio is handled separately by AudioPlayer.
class ContentPlayer {
 public:
  ContentPlayer(const Config& config, AbrController& abr,
                agora::agora_refptr<agora::rtc::IVideoEncodedImageSender> video_sender);
  ~ContentPlayer();

  // Open all level files. Returns false on failure.
  bool initialize();

  void start();
  void stop();

  // Cumulative bytes pushed to the SDK (for measuring real outgoing bitrate).
  uint64_t bytes_sent() const { return bytes_sent_.load(std::memory_order_relaxed); }

 private:
  void video_loop();

  const Config& config_;
  AbrController& abr_;
  agora::agora_refptr<agora::rtc::IVideoEncodedImageSender> video_sender_;

  std::vector<std::unique_ptr<H264Reader>> readers_;
  size_t frame_count_ = 0;  // common loopable length across levels

  std::atomic<bool> running_{false};
  std::atomic<uint64_t> bytes_sent_{0};
  std::thread video_thread_;
};

}  // namespace bwe
