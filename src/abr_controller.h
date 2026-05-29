#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "config.h"

namespace bwe {

// Consumes per-receiver downlink BWE samples and decides the target video level.
//
// IMPORTANT — relationship to the real Agora API (deviation from plan_bwe.md):
// The plan assumed onDownlinkNetworkInfoUpdated delivers a per-receiver `bwe_bps`.
// The actual DownlinkNetworkInfo struct delivers ONE connection-level
// `bandwidth_estimation_bps` plus a per-peer array carrying only
// `expected_bitrate_bps`. So `bwe_bps` fed here is the connection-level estimate
// (the true BWE), attributed to each tracked peer; `expected_bitrate_bps` is kept
// for logging only. With a single subscriber (the common test setup) this is
// exactly the receiver's downlink estimate. The worst-case multi-receiver logic is
// retained for when distinct per-peer estimates become available.
class AbrController {
 public:
  AbrController(const AbrConfig& abr, int num_levels, int start_level,
                const std::vector<LevelConfig>& levels);

  // Feed a BWE sample for a receiver. Thread-safe; called from the SDK callback.
  void add_bwe_sample(const std::string& uid, int64_t bwe_bps, int expected_bitrate_bps);

  // Feed the SDK's recommended encoder bitrate (video_encoder_target_bitrate_bps).
  // Drives level selection in "recommended" mode. Thread-safe.
  void set_recommended_bps(int bps) { recommended_bps_.store(bps, std::memory_order_relaxed); }

  // Receiver lifecycle from the connection observer. Thread-safe.
  void on_receiver_joined(const std::string& uid);
  void on_receiver_left(const std::string& uid);

  // The level the publisher should be sending. Lock-free read for the player.
  int target_level() const { return target_level_.load(std::memory_order_relaxed); }

  void start();
  void stop();

 private:
  struct Receiver {
    std::deque<std::pair<uint64_t, int64_t>> window;  // (mono_ms, bwe_bps)
    int64_t instantaneous_bwe = 0;
    double rolling_avg_bwe = 0;
    int expected_bitrate_bps = -1;
    uint64_t last_update_ms = 0;

    void add_sample(uint64_t now_ms, int64_t bwe, double window_s);
  };

  void tick_loop();
  void evaluate();
  void evaluate_trend();        // legacy instantaneous-vs-average heuristic
  void evaluate_recommended();  // pick level from SDK recommended bitrate
  // Highest level index whose bitrate <= bps; 0 if none fit.
  int level_for_bps(double bps) const;

  AbrConfig abr_;
  int num_levels_;
  std::vector<LevelConfig> levels_;

  std::atomic<int> target_level_;
  std::atomic<bool> running_{false};
  std::thread thread_;

  std::atomic<int> recommended_bps_{-1};

  std::mutex mtx_;
  std::map<std::string, Receiver> receivers_;
  std::deque<std::pair<uint64_t, int>> rec_window_;  // (mono_ms, recommended_bps)
  double downgrade_sustained_s_ = 0.0;
  double upgrade_sustained_s_ = 0.0;
  uint64_t last_change_ms_ = 0;
};

}  // namespace bwe
