// BWE adaptive-bitrate publisher — standalone experiment.
//
// Loops pre-encoded football content into an Agora channel and adapts the published
// video level (1 of 6) in response to downlink BWE feedback. See plan_bwe.md and
// README.md for the full design.
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include "AgoraBase.h"
#include "IAgoraService.h"
#include "NGIAgoraAudioTrack.h"
#include "NGIAgoraLocalUser.h"
#include "NGIAgoraMediaNode.h"
#include "NGIAgoraMediaNodeFactory.h"
#include "NGIAgoraRtcConnection.h"
#include "NGIAgoraVideoTrack.h"

#include "abr_controller.h"
#include "audio_player.h"
#include "config.h"
#include "content_player.h"
#include "logging.h"
#include "publisher_observer.h"
#include "yuv_content_player.h"

using namespace bwe;

namespace {

volatile std::sig_atomic_t g_stop = 0;
void signal_handler(int) { g_stop = 1; }

struct Args {
  std::string appid;
  std::string token;
  std::string channel;
  std::string uid;
  std::string config = "config.json";
  int start_level = -1;       // -1 => use config value
  std::string mode;           // "" => use config value; else "yuv" | "encoded"
};

void print_usage(const char* prog) {
  fprintf(stderr,
          "Usage: %s --appid <id> [--token <t>] [--channel <c>] [--uid <n>]\n"
          "          [--config <path>] [--start-level <0-5>]\n\n"
          "  --appid        Agora App ID (required)\n"
          "  --token        Channel token (optional; omit for testing-mode/no-cert projects)\n"
          "  --channel      Channel name (overrides config)\n"
          "  --uid          Publisher UID (overrides config)\n"
          "  --config       Path to config.json (default: config.json)\n"
          "  --start-level  Initial bitrate level 0-5 (overrides config; encoded mode)\n"
          "  --mode         Video send path: 'encoded' (pre-encoded H.264 + our ABR)\n"
          "                 or 'yuv' (push raw frames, SDK encodes/adapts). Overrides config.\n",
          prog);
}

bool parse_args(int argc, char** argv, Args& a) {
  for (int i = 1; i < argc; ++i) {
    std::string k = argv[i];
    auto next = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        fprintf(stderr, "Missing value for %s\n", name);
        return nullptr;
      }
      return argv[++i];
    };
    if (k == "--appid") { const char* v = next("--appid"); if (!v) return false; a.appid = v; }
    else if (k == "--token") { const char* v = next("--token"); if (!v) return false; a.token = v; }
    else if (k == "--channel") { const char* v = next("--channel"); if (!v) return false; a.channel = v; }
    else if (k == "--uid") { const char* v = next("--uid"); if (!v) return false; a.uid = v; }
    else if (k == "--config") { const char* v = next("--config"); if (!v) return false; a.config = v; }
    else if (k == "--start-level") { const char* v = next("--start-level"); if (!v) return false; a.start_level = atoi(v); }
    else if (k == "--mode") { const char* v = next("--mode"); if (!v) return false; a.mode = v; }
    else if (k == "-h" || k == "--help") { return false; }
    else { fprintf(stderr, "Unknown argument: %s\n", k.c_str()); return false; }
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!parse_args(argc, argv, args) || args.appid.empty()) {
    print_usage(argv[0]);
    return 1;
  }

  // Load config and apply CLI overrides.
  Config config;
  try {
    config = Config::load(args.config);
  } catch (const std::exception& e) {
    fprintf(stderr, "Failed to load config '%s': %s\n", args.config.c_str(), e.what());
    return 1;
  }
  if (!args.mode.empty()) {
    if (args.mode == "yuv") config.mode = VideoMode::YUV;
    else if (args.mode == "encoded") config.mode = VideoMode::ENCODED;
    else { fprintf(stderr, "--mode must be 'yuv' or 'encoded'\n"); return 1; }
  }
  if (!args.channel.empty()) config.channel = args.channel;
  if (!args.uid.empty()) config.publisher_uid = atoi(args.uid.c_str());
  if (args.start_level >= 0) {
    if (args.start_level >= static_cast<int>(config.levels.size())) {
      fprintf(stderr, "--start-level out of range\n");
      return 1;
    }
    config.start_level = args.start_level;
  }
  std::string uid_str = std::to_string(config.publisher_uid);

  set_log_file(config.log_file);
  set_net_log_file(config.network_log);
  LOG_INFO("BWE publisher starting: channel='%s' uid=%s start_level=%d levels=%zu",
           config.channel.c_str(), uid_str.c_str(), config.start_level, config.levels.size());
  LOG_INFO("network log -> %s  (tail -f; LOCAL=this publisher, REMOTE=other users)",
           config.network_log.c_str());

  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);
  std::signal(SIGQUIT, signal_handler);

  // ---- Agora service ---------------------------------------------------------
  agora::base::IAgoraService* service = createAgoraService();
  if (!service) {
    LOG_ERROR("createAgoraService failed");
    return 1;
  }
  agora::base::AgoraServiceConfiguration scfg;
  scfg.appId = args.appid.c_str();
  scfg.enableAudioProcessor = true;
  scfg.enableAudioDevice = false;  // server-side: no local mic/speaker
  scfg.enableVideo = true;
  scfg.useStringUid = false;
  if (service->initialize(scfg) != agora::ERR_OK) {
    LOG_ERROR("service->initialize failed (check App ID)");
    service->release();
    return 1;
  }
  service->setLogFile("./io.agora.rtc_sdk/agorasdk.log", 512 * 1024);

  // ---- Connection ------------------------------------------------------------
  agora::rtc::RtcConnectionConfiguration ccfg;
  ccfg.autoSubscribeAudio = false;
  // Subscribing to remote video is what makes the SDK report downlink BWE.
  ccfg.autoSubscribeVideo = true;
  ccfg.clientRoleType = agora::rtc::CLIENT_ROLE_BROADCASTER;
  agora::agora_refptr<agora::rtc::IRtcConnection> connection = service->createRtcConnection(ccfg);
  if (!connection) {
    LOG_ERROR("createRtcConnection failed");
    service->release();
    return 1;
  }

  // ---- ABR controller + observer --------------------------------------------
  AbrController abr(config.abr, static_cast<int>(config.levels.size()), config.start_level,
                    config.levels);
  PublisherObserver observer(abr, uid_str);
  connection->registerObserver(&observer);
  connection->registerNetworkObserver(&observer);

  const char* token = args.token.empty() ? nullptr : args.token.c_str();
  if (connection->connect(token, config.channel.c_str(), uid_str.c_str())) {
    LOG_ERROR("connect() failed");
    connection->unregisterObserver(&observer);
    connection->unregisterNetworkObserver(&observer);
    service->release();
    return 1;
  }

  const bool yuv_mode = (config.mode == VideoMode::YUV);
  LOG_INFO("video mode: %s", yuv_mode ? "YUV (SDK encodes & adapts)"
                                      : "ENCODED (pre-encoded H.264 + our ABR)");

  // ---- Media nodes -----------------------------------------------------------
  agora::agora_refptr<agora::rtc::IMediaNodeFactory> factory = service->createMediaNodeFactory();
  agora::agora_refptr<agora::rtc::IAudioPcmDataSender> audio_sender =
      factory->createAudioPcmDataSender();
  if (!factory || !audio_sender) {
    LOG_ERROR("failed to create media node factory / audio sender");
    service->release();
    return 1;
  }
  agora::agora_refptr<agora::rtc::ILocalAudioTrack> audio_track =
      service->createCustomAudioTrack(audio_sender);

  // Video sender + track depend on the mode.
  agora::agora_refptr<agora::rtc::IVideoEncodedImageSender> enc_sender;  // ENCODED
  agora::agora_refptr<agora::rtc::IVideoFrameSender> yuv_sender;         // YUV
  agora::agora_refptr<agora::rtc::ILocalVideoTrack> video_track;
  if (yuv_mode) {
    yuv_sender = factory->createVideoFrameSender();
    if (yuv_sender) video_track = service->createCustomVideoTrack(yuv_sender);
    if (video_track) {
      agora::rtc::VideoEncoderConfiguration enc_cfg;
      enc_cfg.codecType = agora::rtc::VIDEO_CODEC_H264;
      enc_cfg.dimensions = agora::rtc::VideoDimensions(config.yuv.width, config.yuv.height);
      enc_cfg.frameRate = config.frame_rate;
      // NB: VideoEncoderConfiguration.bitrate is in BPS in this NG SDK (the YUV
      // sample uses 1000000 for ~1 Mbps), so convert from our kbps config. Setting
      // it in kbps (e.g. 2000) means 2 kbps — far too low to encode, which makes
      // the SDK emit no video and the receiver see the track as muted.
      enc_cfg.bitrate = config.yuv.bitrate_kbps * 1000;
      if (config.yuv.min_bitrate_kbps > 0) enc_cfg.minBitrate = config.yuv.min_bitrate_kbps * 1000;
      enc_cfg.degradationPreference = agora::rtc::MAINTAIN_QUALITY;
      video_track->setVideoEncoderConfiguration(enc_cfg);
    }
  } else {
    enc_sender = factory->createVideoEncodedImageSender();
    agora::rtc::SenderOptions vopts;
    vopts.ccMode = agora::rtc::CC_ENABLED;          // enable congestion control / BWE
    vopts.codecType = agora::rtc::VIDEO_CODEC_H264;  // match the frames we push
    // Ceiling for the SDK's recommended bitrate (video_encoder_target_bitrate_bps).
    vopts.targetBitrate = config.encoded_target_kbps;
    if (enc_sender) video_track = service->createCustomVideoTrack(enc_sender, vopts);
  }
  if (!audio_track || !video_track) {
    LOG_ERROR("failed to create custom tracks");
    service->release();
    return 1;
  }
  audio_track->setEnabled(true);
  video_track->setEnabled(true);

  connection->getLocalUser()->publishAudio(audio_track);
  connection->getLocalUser()->publishVideo(video_track);

  if (!observer.wait_until_connected(5000)) {
    LOG_WARN("not connected after 5s — continuing anyway (will publish once connected)");
  }

  // ---- Players --------------------------------------------------------------
  AudioPlayer audio_player(config, audio_sender);
  std::unique_ptr<ContentPlayer> enc_player;
  std::unique_ptr<YuvContentPlayer> yuv_player;
  if (yuv_mode) {
    yuv_player = std::make_unique<YuvContentPlayer>(config, yuv_sender);
  } else {
    enc_player = std::make_unique<ContentPlayer>(config, abr, enc_sender);
    if (!enc_player->initialize()) {
      LOG_ERROR("content player init failed — did you run ./prepare_content.sh?");
      g_stop = 1;
    }
  }

  if (!g_stop) {
    if (!yuv_mode) abr.start();  // ABR drives level switching only in ENCODED mode
    audio_player.start();
    if (enc_player) enc_player->start();
    if (yuv_player) yuv_player->start();
    LOG_INFO("publishing. Ctrl-C to stop.");
    // One consolidated STATUS line every 2s: recommended send bitrate, measured
    // average outgoing bitrate, and the selected ABR level.
    int ticks = 0;
    uint64_t prev_bytes = enc_player ? enc_player->bytes_sent() : 0;
    uint64_t prev_ms = mono_ms();
    while (!g_stop) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      if (++ticks % 10 != 0) continue;  // ~2s

      int send_target = observer.last_uplink_bps();
      uint64_t now = mono_ms();
      int out_kbps = 0;
      if (yuv_mode) {
        agora::rtc::LocalVideoTrackStats vs;
        if (video_track->getStatistics(vs)) out_kbps = vs.media_bitrate_bps / 1000;
      } else if (enc_player) {
        uint64_t b = enc_player->bytes_sent();
        uint64_t dms = (now > prev_ms) ? (now - prev_ms) : 1;
        out_kbps = static_cast<int>((b - prev_bytes) * 8 / dms);  // bits/ms == kbps
        prev_bytes = b;
      }
      prev_ms = now;

      char line[256];
      if (yuv_mode) {
        snprintf(line, sizeof(line),
                 "STATUS send_target=%dbps  out_avg=%dkbps  level=n/a (SDK encodes %dx%d)",
                 send_target, out_kbps, config.yuv.width, config.yuv.height);
      } else {
        int lvl = abr.target_level();
        const LevelConfig& L = config.levels[lvl];
        snprintf(line, sizeof(line),
                 "STATUS send_target=%dbps  out_avg=%dkbps  abr_level=%d (%dx%d %dkbps)",
                 send_target, out_kbps, lvl, L.width, L.height, L.bitrate_kbps);
      }
      LOG_INFO("%s", line);
      log_net(line);
    }
  }

  LOG_INFO("shutting down...");
  if (enc_player) enc_player->stop();
  if (yuv_player) yuv_player->stop();
  audio_player.stop();
  abr.stop();  // safe even if never started

  connection->getLocalUser()->unpublishAudio(audio_track);
  connection->getLocalUser()->unpublishVideo(video_track);
  connection->unregisterObserver(&observer);
  connection->unregisterNetworkObserver(&observer);
  connection->disconnect();

  audio_sender = nullptr;
  enc_sender = nullptr;
  yuv_sender = nullptr;
  audio_track = nullptr;
  video_track = nullptr;
  factory = nullptr;
  connection = nullptr;

  service->release();
  close_log_file();
  LOG_INFO("done");
  return 0;
}
