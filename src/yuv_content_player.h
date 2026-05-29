#pragma once

#include <atomic>
#include <cstdio>
#include <thread>

#include "AgoraMediaBase.h"
#include "NGIAgoraMediaNode.h"
#include "config.h"

namespace bwe {

// YUV-mode video: decodes the source clip to raw I420 frames (via an ffmpeg
// subprocess) and pushes them to Agora via IVideoFrameSender at real-time pace.
// The SDK encodes and adapts the bitrate itself (per the track's
// VideoEncoderConfiguration + its own congestion control), so there is no ABR
// level switching here — this path exists to observe the SDK's own uplink bitrate
// adaptation (onUplinkNetworkInfoUpdated.video_encoder_target_bitrate_bps).
class YuvContentPlayer {
 public:
  YuvContentPlayer(const Config& config,
                   agora::agora_refptr<agora::rtc::IVideoFrameSender> video_sender);
  ~YuvContentPlayer();

  void start();
  void stop();

 private:
  void video_loop();
  FILE* open_decoder();  // launches ffmpeg, returns a pipe to read raw I420 from

  const Config& config_;
  agora::agora_refptr<agora::rtc::IVideoFrameSender> video_sender_;
  std::atomic<bool> running_{false};
  std::thread video_thread_;
};

}  // namespace bwe
