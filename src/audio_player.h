#pragma once

#include <atomic>
#include <thread>

#include "NGIAgoraMediaNode.h"
#include "config.h"

namespace bwe {

// Streams the shared PCM audio track to Agora in 10 ms frames at real-time pace.
// Independent of the video path (encoded or YUV) and the video level — audio never
// interrupts on a video switch.
class AudioPlayer {
 public:
  AudioPlayer(const Config& config,
              agora::agora_refptr<agora::rtc::IAudioPcmDataSender> audio_sender);
  ~AudioPlayer();

  void start();
  void stop();

 private:
  void audio_loop();

  const Config& config_;
  agora::agora_refptr<agora::rtc::IAudioPcmDataSender> audio_sender_;
  std::atomic<bool> running_{false};
  std::thread thread_;
};

}  // namespace bwe
