# BWE Adaptive-Bitrate Publisher (Agora Linux C++ SDK)

A standalone C++ tool that loops football content into an Agora channel and adapts the
published video bitrate to the **subscriber's downlink**, using the Agora Linux Server
SDK (v4.4.32.156). It supports two send paths and was used to work out **how a
publisher can obtain a subscriber's downlink bandwidth, in bps** — the core question
this repo answers.

> **TL;DR** — The usable per-subscriber downlink signal on the publisher is
> `onUplinkNetworkInfoUpdated.video_encoder_target_bitrate_bps` ("**send_target**" in
> our logs): the SDK's recommended send bitrate, capped end-to-end by the worst
> subscriber's downlink. To make it react to a **view-only (audience)** subscriber,
> that subscriber's web client must call
> `AgoraRTC.setParameter("EXPERIMENTS", {"FeedbackConfig":2})` **before join**.

---

## How to get `send_target` (the recommended send bitrate)

`send_target` is `UplinkNetworkInfo.video_encoder_target_bitrate_bps`, delivered via
`INetworkObserver::onUplinkNetworkInfoUpdated`. It is the SDK's recommended encoder
bitrate; under congestion it is pulled down toward what the network — including the
worst subscriber's downlink — can carry, and it recovers when the path clears.

It fires in **both** publish modes, but the setup differs:

### Encoded mode (we push pre-encoded H.264 via `IVideoEncodedImageSender`)
Two requirements (confirmed with Agora support):
1. `connection->registerNetworkObserver(observer)`
2. Create the video track with congestion control on:
   ```cpp
   agora::rtc::SenderOptions opts;
   opts.ccMode = agora::rtc::CC_ENABLED;          // required
   service->createCustomVideoTrack(encodedImageSender, opts);
   ```
The recommendation's **ceiling is `SenderOptions.targetBitrate`** (Kbps) — confirmed:
`video_encoder_target_bitrate_bps` never exceeds it, CC only recommends *down* from
it. Set it via `config.json → encoded_target_kbps` (default 6500). Keep it ≥ your top
ladder level (3500) so the ABR can reach the top level; lower it to model a capped
link. The SDK does *not* encode here, so you use `send_target` to **choose which
pre-encoded level to publish** (see ABR below).

### YUV mode (we push raw I420 via `IVideoFrameSender`; the SDK encodes)
1. `connection->registerNetworkObserver(observer)`
2. `service->createCustomVideoTrack(videoFrameSender)` + set a
   `VideoEncoderConfiguration` (codec/dimensions/frameRate/bitrate).
Here the SDK owns the encoder and **adapts the bitrate itself** to `send_target`;
the recommendation's ceiling is `VideoEncoderConfiguration.bitrate`.

### In BOTH modes
- A remote **subscriber must be connected** for `send_target` to be meaningful (with
  no subscriber it sits at the ceiling).
- **For an audience (non-publishing) subscriber, the subscriber's web client must set
  `AgoraRTC.setParameter("EXPERIMENTS", {"FeedbackConfig":2})` before `join()`** —
  otherwise the audience client never feeds its downlink condition back to the
  publisher and `send_target` won't react. (A *publishing* peer feeds back without the
  flag, but a pure viewer needs it.) This was verified empirically: with the flag,
  throttling the viewer's downlink dropped `send_target` from ~2 Mbps to ~9 kbps and
  it recovered on release.

### Signals that do NOT give a subscriber's downlink (for reference)
- **`onUserNetworkQuality(uid, tx, rx)`** — per-user up/down network *quality*, but a
  coarse 1–6 enum (excellent…down), and **only reported for *publishing* peers**. A
  pure audience subscriber never appears. Not a bitrate.
- **`onDownlinkNetworkInfoUpdated` / `DownlinkNetworkInfo.bandwidth_estimation_bps`** —
  this is the **publisher's own** downlink, not a subscriber's. (In local testing it
  often doesn't fire at all for a send-only publisher.)

---

## Build

Prerequisites: `cmake g++ ffmpeg`. The Agora SDK is expected at `./agora_sdk/`
(copied from the commentary extract); if missing:
```bash
cp -r /home/ubuntu/commentary/go-audio-video-publisher/agora-sdk/agora_sdk ./agora_sdk
```
Then:
```bash
mkdir -p build && cd build
cmake .. && make -j
```
The binary's RPATH points at `../agora_sdk`, so it finds the SDK (and its transitive
`.so`s) without `LD_LIBRARY_PATH`.

## Prepare content

Pre-encode the 6 GOP-aligned H.264 variants + shared PCM audio (encoded mode), which
also serve as the source for YUV mode:
```bash
./prepare_content.sh            # uses source_clip/source.mp4 -> content/
```
All levels share fps (30) and GOP (60 frames = 2 s), use closed GOPs, no B-frames, and
repeat SPS/PPS before every keyframe — so they're frame-aligned and switchable on any
keyframe. Ladder (override in `config.json`):

| Level | Resolution | Bitrate |
|---|---|---|
| 0 | 320×180   | 200 kbps  |
| 1 | 480×270   | 400 kbps  |
| 2 | 640×360   | 700 kbps  |
| 3 | 960×540   | 1200 kbps |
| 4 | 1280×720  | 2000 kbps |
| 5 | 1280×720  | 3500 kbps |

## Run

```bash
A=20b7c51ff4c644ab80cf5a4e646b0537   # no-certificate App ID (token not required)

# ENCODED: push pre-encoded H.264; our ABR selects the level from send_target
./build/bwe_publisher --appid $A --token $A --channel bwe_test --uid 1000 \
  --mode encoded --config config.json

# YUV: push raw frames; the SDK encodes and adapts the bitrate itself
./build/bwe_publisher --appid $A --token $A --channel bwe_test --uid 1000 \
  --mode yuv --config config.json
```
`--mode` overrides `config.json`'s `mode`. `--channel`, `--uid`, `--start-level` also
override. Logs are **truncated at each start**. Ctrl-C stops cleanly.

## Viewer (audience) page

A minimal view-only viewer is served (static, over the existing nginx vhost):

**https://sip.dev.gw.01.agora.io/bwe.html?channel=bwe_test&feedback=1**

- `rtc` mode, **view-only** (never publishes) — a pure audience subscriber.
- `?feedback=1` (default) calls `setParameter("EXPERIMENTS", {"FeedbackConfig":2})`
  before join; `?feedback=0` disables it (to A/B the flag).
- `?appid=`, `?channel=`, `?uid=` params; App ID defaults to the no-cert one.
- Shows the receiver-side downlink directly: **receive bitrate (kbps)**, resolution,
  fps, packet loss, delay. This is the browser-side mirror of `send_target`.

Source: `web/viewer.html` (deploy with `sudo cp web/viewer.html /var/www/html/bwe.html`).

**Audience vs host:** a *host* (e.g. Agora's `basicVideoCall`) also publishes a camera,
so it appears in `onUserNetworkQuality` and adds a `LOCAL downlink … receiving uid=…`
on the publisher. A pure *audience* viewer (this page) does neither — which is why the
only way to see its downlink on the publisher is `send_target` + `FeedbackConfig:2`.

## ABR (encoded mode)

`config.json → abr.selector`:
- **`"recommended"`** (default) — publish the highest level whose bitrate ≤
  `send_target × abr.headroom` (default 0.85). Downgrades immediately (uses the min
  recommendation in the window); upgrades one level at a time after
  `cooldown_after_switch_s` + `br_upgrade_duration_s` sustained (avoids flapping).
- **`"trend"`** — legacy instantaneous-vs-rolling-average BWE heuristic.

In **YUV mode the SDK adapts the encoder itself**, so the ABR/level machinery is not
used (the STATUS line shows `level=n/a (SDK encodes …)`).

## Logging

`config.json → network_log` (default `/tmp/bwe_network.log`): one consolidated STATUS
line every 2 s — recommended bitrate, measured average outgoing bitrate, selected level:
```
[2026-05-29 11:46:01] STATUS send_target=6483750bps  out_avg=1230kbps  abr_level=3 (960x540 1200kbps)
```
- `send_target` — `video_encoder_target_bitrate_bps` (recommended; `-1` until first report)
- `out_avg` — measured outgoing bitrate (encoded: real bytes pushed/time; YUV: SDK
  encoder `media_bitrate_bps`)
- `abr_level` — selected level (encoded) / `n/a` (YUV)

`tail -f /tmp/bwe_network.log` to watch live. Switch reasoning (`RECO …` per tick,
`DECISION upgrade/downgrade …` per switch) goes to stderr / `config.log_file`:
```bash
grep -E "RECO|DECISION" /tmp/bwe_run.log
```

## Testing the adaptation

1. Run the publisher (encoded or yuv).
2. Open the viewer with `?feedback=1`; confirm video plays.
3. Throttle the viewer's downlink (browser DevTools → Network throttling, or `tc` on
   the receiver host) for ~15 s, then release.
4. Watch `send_target` collapse and recover in `/tmp/bwe_network.log`. In encoded mode
   `abr_level`/`out_avg` step down then climb back; in YUV mode the SDK lowers/raises
   the encoded bitrate itself.

## Findings summary

| Question | Answer |
|---|---|
| Per-subscriber downlink **in bps** on the publisher? | Yes — `send_target` (`video_encoder_target_bitrate_bps`), end-to-end. |
| Works for a **view-only audience** subscriber? | Yes, **only if** the viewer sets `EXPERIMENTS FeedbackConfig:2` before join. |
| `send_target` in encoded mode? | Yes — needs `registerNetworkObserver` + `CC_ENABLED`; ceiling = `SenderOptions.targetBitrate`. |
| `send_target` in YUV mode? | Yes — SDK encodes/adapts; ceiling = `VideoEncoderConfiguration.bitrate`. |
| `onUserNetworkQuality` rxQuality? | Per-user downlink *quality* enum, **publishing peers only**, not audience, not bps. |
| `onDownlinkNetworkInfoUpdated`? | The **publisher's own** downlink, not a subscriber's. |

### YUV-mode gotchas (fixed in this code; note if you reimplement)
- `VideoEncoderConfiguration.bitrate` is in **bps**, not kbps. Passing kbps (e.g. 2000)
  sets the encoder to ~2 kbps → no usable video → the receiver sees the track muted.
- `ExternalVideoFrame.timestamp` should be **0** (let the SDK use arrival time). Feeding
  an incrementing clip-relative timestamp made the SDK drop every frame after the first
  (`input_frame_rate` went to 0) → muted video.
