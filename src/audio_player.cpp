#include "audio_player.h"

#include <chrono>
#include <cstdio>
#include <vector>

#include "logging.h"

namespace bwe {

AudioPlayer::AudioPlayer(const Config& config,
                         agora::agora_refptr<agora::rtc::IAudioPcmDataSender> audio_sender)
    : config_(config), audio_sender_(audio_sender) {}

AudioPlayer::~AudioPlayer() { stop(); }

void AudioPlayer::start() {
  running_.store(true);
  thread_ = std::thread(&AudioPlayer::audio_loop, this);
}

void AudioPlayer::stop() {
  running_.store(false);
  if (thread_.joinable()) thread_.join();
}

void AudioPlayer::audio_loop() {
  using clock = std::chrono::steady_clock;
  const int sample_bytes = sizeof(int16_t) * config_.audio_channels;
  const int samples_per_10ms = config_.audio_sample_rate / 100;
  const int send_bytes = sample_bytes * samples_per_10ms;
  const auto interval = std::chrono::milliseconds(10);

  FILE* file = fopen(config_.audio_file.c_str(), "rb");
  if (!file) {
    LOG_ERROR("audio_player: cannot open audio file %s — continuing video-only",
              config_.audio_file.c_str());
    return;
  }
  LOG_INFO("audio_player: starting (%d Hz, %d ch, 10ms frames)", config_.audio_sample_rate,
           config_.audio_channels);

  std::vector<uint8_t> buf(send_bytes);
  auto next_send = clock::now();

  while (running_.load()) {
    size_t n = fread(buf.data(), 1, send_bytes, file);
    if (n != static_cast<size_t>(send_bytes)) {
      if (feof(file)) {
        if (config_.loop_content) {
          fseek(file, 0, SEEK_SET);
          continue;
        }
        break;
      }
      LOG_WARN("audio_player: read error");
      break;
    }
    if (audio_sender_->sendAudioPcmData(buf.data(), 0, 0, samples_per_10ms,
                                        agora::rtc::TWO_BYTES_PER_SAMPLE, config_.audio_channels,
                                        config_.audio_sample_rate) < 0) {
      LOG_WARN("audio_player: sendAudioPcmData failed");
    }

    next_send += interval;
    auto now = clock::now();
    if (next_send > now)
      std::this_thread::sleep_for(next_send - now);
    else
      next_send = now;
  }
  fclose(file);
}

}  // namespace bwe
