#include "abr_controller.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <sstream>

#include "logging.h"

namespace bwe {

void AbrController::Receiver::add_sample(uint64_t now_ms, int64_t bwe, double window_s) {
  window.emplace_back(now_ms, bwe);
  instantaneous_bwe = bwe;
  last_update_ms = now_ms;
  // Drop samples older than the window.
  const uint64_t cutoff = (now_ms > static_cast<uint64_t>(window_s * 1000))
                              ? now_ms - static_cast<uint64_t>(window_s * 1000)
                              : 0;
  while (window.size() > 1 && window.front().first < cutoff) window.pop_front();
  double sum = 0;
  for (auto& s : window) sum += static_cast<double>(s.second);
  rolling_avg_bwe = sum / static_cast<double>(window.size());
}

AbrController::AbrController(const AbrConfig& abr, int num_levels, int start_level,
                             const std::vector<LevelConfig>& levels)
    : abr_(abr), num_levels_(num_levels), levels_(levels), target_level_(start_level) {
  last_change_ms_ = mono_ms();
}

void AbrController::add_bwe_sample(const std::string& uid, int64_t bwe_bps,
                                   int expected_bitrate_bps) {
  std::lock_guard<std::mutex> lk(mtx_);
  Receiver& r = receivers_[uid];
  bool first = r.window.empty();
  r.add_sample(mono_ms(), bwe_bps, abr_.bwe_window_s);
  r.expected_bitrate_bps = expected_bitrate_bps;
  if (first) {
    LOG_INFO("uid=%s first BWE sample bwe=%lld expected=%d (now tracking %zu receiver%s)",
             uid.c_str(), static_cast<long long>(bwe_bps), expected_bitrate_bps, receivers_.size(),
             receivers_.size() == 1 ? "" : "s");
  }
}

void AbrController::on_receiver_joined(const std::string& uid) {
  std::lock_guard<std::mutex> lk(mtx_);
  LOG_INFO("uid=%s joined (now tracking %zu receiver%s)", uid.c_str(), receivers_.size(),
           receivers_.size() == 1 ? "" : "s");
}

void AbrController::on_receiver_left(const std::string& uid) {
  std::lock_guard<std::mutex> lk(mtx_);
  receivers_.erase(uid);
  LOG_INFO("uid=%s left (now tracking %zu receiver%s)", uid.c_str(), receivers_.size(),
           receivers_.size() == 1 ? "" : "s");
}

void AbrController::start() {
  running_.store(true);
  thread_ = std::thread(&AbrController::tick_loop, this);
}

void AbrController::stop() {
  running_.store(false);
  if (thread_.joinable()) thread_.join();
}

void AbrController::tick_loop() {
  while (running_.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(abr_.tick_interval_ms));
    evaluate();
  }
}

int AbrController::level_for_bps(double bps) const {
  int chosen = 0;
  for (int i = 0; i < num_levels_; ++i) {
    if (levels_[i].bitrate_kbps * 1000.0 <= bps) chosen = i;
  }
  return chosen;  // 0 (lowest) if even level 0 doesn't fit
}

void AbrController::evaluate() {
  if (abr_.selector == "recommended")
    evaluate_recommended();
  else
    evaluate_trend();
}

// Pick the published level directly from the SDK's recommended encoder bitrate.
// Downgrade reacts fast (uses the worst/min recommendation in the window); upgrade
// reacts slowly (uses the average, plus cooldown + sustain) to avoid flapping.
void AbrController::evaluate_recommended() {
  std::lock_guard<std::mutex> lk(mtx_);
  const uint64_t now = mono_ms();
  const double tick_s = abr_.tick_interval_ms / 1000.0;
  int rec = recommended_bps_.load(std::memory_order_relaxed);
  if (rec <= 0) return;  // no recommendation yet

  rec_window_.emplace_back(now, rec);
  const uint64_t cutoff =
      (now > static_cast<uint64_t>(abr_.bwe_window_s * 1000)) ? now - static_cast<uint64_t>(abr_.bwe_window_s * 1000) : 0;
  while (rec_window_.size() > 1 && rec_window_.front().first < cutoff) rec_window_.pop_front();
  int rec_min = rec, sum = 0;
  for (auto& s : rec_window_) {
    rec_min = std::min(rec_min, s.second);
    sum += s.second;
  }
  double rec_avg = static_cast<double>(sum) / rec_window_.size();

  int cur = target_level_.load(std::memory_order_relaxed);
  int down_level = level_for_bps(rec_min * abr_.headroom);   // fast: worst-case
  int up_level = level_for_bps(rec_avg * abr_.headroom);     // slow: averaged

  char line[320];
  snprintf(line, sizeof(line),
           "RECO rec=%dbps rec_min=%dbps rec_avg=%dbps headroom=%.2f cur_level=%d(%dkbps) "
           "fit_down=%d fit_up=%d up_susp=%.1f",
           rec, rec_min, static_cast<int>(rec_avg), abr_.headroom, cur, levels_[cur].bitrate_kbps,
           down_level, up_level, upgrade_sustained_s_);
  LOG_INFO("%s", line);
  log_stats(line);

  if (down_level < cur) {
    LOG_DECISION("downgrade L%d (%dkbps) -> L%d (%dkbps) reason=\"recommended min %dbps*%.2f fits L%d\"",
                 cur, levels_[cur].bitrate_kbps, down_level, levels_[down_level].bitrate_kbps,
                 rec_min, abr_.headroom, down_level);
    target_level_.store(down_level, std::memory_order_relaxed);
    last_change_ms_ = now;
    upgrade_sustained_s_ = 0.0;
    return;
  }

  if (up_level > cur) {
    const double elapsed_s = (now - last_change_ms_) / 1000.0;
    if (elapsed_s > abr_.cooldown_after_switch_s) {
      upgrade_sustained_s_ += tick_s;
      if (upgrade_sustained_s_ >= abr_.br_upgrade_duration_s) {
        int next = cur + 1;  // step up one level at a time
        LOG_DECISION("upgrade L%d (%dkbps) -> L%d (%dkbps) reason=\"recommended avg %dbps*%.2f fits L%d, sustained %.1fs\"",
                     cur, levels_[cur].bitrate_kbps, next, levels_[next].bitrate_kbps,
                     static_cast<int>(rec_avg), abr_.headroom, up_level, upgrade_sustained_s_);
        target_level_.store(next, std::memory_order_relaxed);
        last_change_ms_ = now;
        upgrade_sustained_s_ = 0.0;
      }
    }
  } else {
    upgrade_sustained_s_ = 0.0;
  }
}

void AbrController::evaluate_trend() {
  std::lock_guard<std::mutex> lk(mtx_);
  const uint64_t now = mono_ms();
  const double tick_s = abr_.tick_interval_ms / 1000.0;

  // Prune stale receivers (no BWE update within the timeout).
  const uint64_t stale_ms = static_cast<uint64_t>(abr_.stale_receiver_timeout_s * 1000);
  for (auto it = receivers_.begin(); it != receivers_.end();) {
    if (now - it->second.last_update_ms > stale_ms) {
      LOG_INFO("uid=%s pruned (stale, no BWE for >%.1fs)", it->first.c_str(),
               abr_.stale_receiver_timeout_s);
      it = receivers_.erase(it);
    } else {
      ++it;
    }
  }

  if (receivers_.empty()) {
    downgrade_sustained_s_ = 0.0;
    upgrade_sustained_s_ = 0.0;
    return;
  }

  int cur = target_level_.load(std::memory_order_relaxed);

  // Per-receiver stats + worst-case aggregation.
  int64_t worst_instant = std::numeric_limits<int64_t>::max();
  double worst_rolling = std::numeric_limits<double>::max();
  bool all_above_upgrade = true;
  for (const auto& kv : receivers_) {
    const Receiver& r = kv.second;
    worst_instant = std::min<int64_t>(worst_instant, r.instantaneous_bwe);
    worst_rolling = std::min<double>(worst_rolling, r.rolling_avg_bwe);
    if (!(r.instantaneous_bwe > r.rolling_avg_bwe * abr_.br_upgrade_factor))
      all_above_upgrade = false;

    double ratio = r.rolling_avg_bwe > 0 ? r.instantaneous_bwe / r.rolling_avg_bwe : 0.0;
    char line[512];
    snprintf(line, sizeof(line),
             "tick uid=%s inst=%lld avg%.1fs=%lld inst_vs_avg_ratio=%.2f cur_level=%d "
             "down_susp=%.1f up_susp=%.1f",
             kv.first.c_str(), static_cast<long long>(r.instantaneous_bwe), abr_.bwe_window_s,
             static_cast<long long>(r.rolling_avg_bwe), ratio, cur, downgrade_sustained_s_,
             upgrade_sustained_s_);
    LOG_INFO("%s", line);
    log_stats(line);
  }

  const double down_threshold = worst_rolling / abr_.br_downgrade_factor;
  const double up_threshold = worst_rolling * abr_.br_upgrade_factor;

  {
    double agg_ratio = worst_rolling > 0 ? worst_instant / worst_rolling : 0.0;
    char line[512];
    snprintf(line, sizeof(line),
             "worst_inst=%lld worst_avg=%lld ratio_to_avg=%.2f down_threshold=%lld "
             "up_threshold=%lld cur_level=%d",
             static_cast<long long>(worst_instant), static_cast<long long>(worst_rolling),
             agg_ratio, static_cast<long long>(down_threshold),
             static_cast<long long>(up_threshold), cur);
    LOG_INFO("%s", line);
    log_stats(line);
  }

  // --- Downgrade check --------------------------------------------------------
  if (static_cast<double>(worst_instant) < down_threshold) {
    downgrade_sustained_s_ += tick_s;
    if (downgrade_sustained_s_ >= abr_.br_downgrade_duration_s) {
      if (cur > 0) {
        int next = cur - 1;
        LOG_DECISION(
            "downgrade from L%d (%dkbps) to L%d (%dkbps) reason=\"worst_inst %lld < worst_avg/%.2f "
            "= %lld sustained %.1fs\"",
            cur, levels_[cur].bitrate_kbps, next, levels_[next].bitrate_kbps,
            static_cast<long long>(worst_instant), abr_.br_downgrade_factor,
            static_cast<long long>(down_threshold), downgrade_sustained_s_);
        target_level_.store(next, std::memory_order_relaxed);
        last_change_ms_ = now;
      }
      downgrade_sustained_s_ = 0.0;
      upgrade_sustained_s_ = 0.0;
    }
  } else {
    downgrade_sustained_s_ = 0.0;
  }

  // --- Upgrade check ----------------------------------------------------------
  cur = target_level_.load(std::memory_order_relaxed);  // may have changed above
  const double elapsed_since_change_s = (now - last_change_ms_) / 1000.0;
  if (elapsed_since_change_s > abr_.cooldown_after_switch_s) {
    if (all_above_upgrade) {
      upgrade_sustained_s_ += tick_s;
      if (upgrade_sustained_s_ >= abr_.br_upgrade_duration_s) {
        if (cur < num_levels_ - 1) {
          int next = cur + 1;
          LOG_DECISION(
              "upgrade from L%d (%dkbps) to L%d (%dkbps) reason=\"all receivers inst>avg*%.2f "
              "sustained %.1fs\"",
              cur, levels_[cur].bitrate_kbps, next, levels_[next].bitrate_kbps,
              abr_.br_upgrade_factor, upgrade_sustained_s_);
          target_level_.store(next, std::memory_order_relaxed);
          last_change_ms_ = now;
        }
        upgrade_sustained_s_ = 0.0;
        downgrade_sustained_s_ = 0.0;
      }
    } else {
      upgrade_sustained_s_ = 0.0;
    }
  }
}

}  // namespace bwe
