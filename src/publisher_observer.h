#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>

#include "AgoraBase.h"
#include "NGIAgoraRtcConnection.h"
#include "abr_controller.h"
#include "logging.h"

namespace bwe {

// Bridges the SDK connection + network callbacks into the ABR controller.
// Implements every pure-virtual of IRtcConnectionObserver (this SDK build has a
// few more than the bundled sample) and the downlink half of INetworkObserver.
class PublisherObserver : public agora::rtc::IRtcConnectionObserver,
                          public agora::rtc::INetworkObserver {
 public:
  PublisherObserver(AbrController& abr, const std::string& local_uid)
      : abr_(abr), local_uid_(local_uid) {}

  // Block until onConnected fires or timeout. Returns true if connected.
  bool wait_until_connected(int timeout_ms) {
    std::unique_lock<std::mutex> lk(mtx_);
    return cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                        [this] { return connected_; });
  }

  // ---- INetworkObserver ----------------------------------------------------
  // LOCAL downlink: this publisher's own downlink (feeds the trend-mode ABR). Not
  // logged — it's noise for this experiment (it's the server's own receive path,
  // not a subscriber's downlink).
  void onDownlinkNetworkInfoUpdated(const agora::rtc::DownlinkNetworkInfo& info) override {
    const int64_t bwe = info.bandwidth_estimation_bps;
    if (info.peer_downlink_info && info.total_received_video_count > 0) {
      for (int i = 0; i < info.total_received_video_count; ++i) {
        const auto& peer = info.peer_downlink_info[i];
        std::string uid = peer.userId ? peer.userId : ("peer" + std::to_string(i));
        abr_.add_bwe_sample(uid, bwe, peer.expected_bitrate_bps);
      }
    } else if (bwe > 0) {
      abr_.add_bwe_sample("downlink", bwe, -1);
    }
  }

  // LOCAL uplink: the SDK's recommended SEND bitrate. Stored for the periodic STATUS
  // line and fed to the ABR (recommended selector).
  void onUplinkNetworkInfoUpdated(const agora::rtc::UplinkNetworkInfo& info) override {
    last_uplink_bps_.store(info.video_encoder_target_bitrate_bps, std::memory_order_relaxed);
    abr_.set_recommended_bps(info.video_encoder_target_bitrate_bps);
  }

  int last_uplink_bps() const { return last_uplink_bps_.load(std::memory_order_relaxed); }

  // ---- IRtcConnectionObserver ----------------------------------------------
  void onConnected(const agora::rtc::TConnectionInfo& info,
                   agora::rtc::CONNECTION_CHANGED_REASON_TYPE reason) override {
    (void)reason;
    LOG_INFO("connection: connected to channel '%s' as uid %s",
             info.channelId ? info.channelId->c_str() : "?",
             info.localUserId ? info.localUserId->c_str() : "?");
    std::lock_guard<std::mutex> lk(mtx_);
    connected_ = true;
    cv_.notify_all();
  }
  void onDisconnected(const agora::rtc::TConnectionInfo& info,
                      agora::rtc::CONNECTION_CHANGED_REASON_TYPE reason) override {
    (void)info;
    LOG_INFO("connection: disconnected (reason %d)", static_cast<int>(reason));
  }
  void onConnecting(const agora::rtc::TConnectionInfo&,
                    agora::rtc::CONNECTION_CHANGED_REASON_TYPE) override {}
  void onReconnecting(const agora::rtc::TConnectionInfo&,
                      agora::rtc::CONNECTION_CHANGED_REASON_TYPE) override {
    LOG_WARN("connection: reconnecting");
  }
  void onReconnected(const agora::rtc::TConnectionInfo&,
                     agora::rtc::CONNECTION_CHANGED_REASON_TYPE) override {
    LOG_INFO("connection: reconnected");
  }
  void onCustomUserInfoUpdated(agora::user_id_t, const char*) override {}
  void onConnectionLost(const agora::rtc::TConnectionInfo&) override {
    LOG_WARN("connection: lost");
  }
  void onLastmileQuality(const agora::rtc::QUALITY_TYPE) override {}
  void onLastmileProbeResult(const agora::rtc::LastmileProbeResult&) override {}
  void onTokenPrivilegeWillExpire(const char*) override {
    LOG_WARN("connection: token privilege will expire");
  }
  void onTokenPrivilegeDidExpire() override {
    LOG_WARN("connection: token privilege expired");
  }
  void onConnectionFailure(const agora::rtc::TConnectionInfo&,
                           agora::rtc::CONNECTION_CHANGED_REASON_TYPE reason) override {
    LOG_ERROR("connection: failure (reason %d)", static_cast<int>(reason));
  }
  void onUserJoined(agora::user_id_t userId) override {
    abr_.on_receiver_joined(userId ? userId : "?");
  }
  void onUserLeft(agora::user_id_t userId, agora::rtc::USER_OFFLINE_REASON_TYPE) override {
    abr_.on_receiver_left(userId ? userId : "?");
  }
  void onTransportStats(const agora::rtc::RtcStats&) override {}
  void onChannelMediaRelayStateChanged(int, int) override {}

 private:
  AbrController& abr_;
  std::string local_uid_;
  std::mutex mtx_;
  std::atomic<int> last_uplink_bps_{-1};  // -1 until first report
  std::condition_variable cv_;
  bool connected_ = false;
};

}  // namespace bwe
