#!/bin/bash
# Pre-encode the source clip into 6 GOP-aligned H.264 variants + one shared PCM track.
#
# Constraints (see plan_bwe.md "Content preparation"):
#   - All levels share the same GOP size (60 frames = 2s @ 30fps) so the publisher
#     can switch levels on any keyframe boundary.
#   - All levels share the same frame rate (30 fps) and frame count, so frame index N
#     in level A corresponds to the same source timeline position as frame index N in B.
#   - Annex B raw H.264 (no container) so the publisher can chunk NAL access units and
#     push them straight through PushVideoEncodedData / sendEncodedVideoImage().
#   - Closed GOP (open-gop=0) + no B-frames so mid-stream switches don't produce
#     decode artifacts on the receiver.
#   - repeat-headers=1 so SPS/PPS are emitted before EVERY IDR. This is REQUIRED:
#     Agora's encoded-image path (and our h264_reader keyframe detection) expects each
#     keyframe access unit to be self-contained (SPS+PPS+IDR), so late-joining
#     receivers and post-switch frames decode cleanly.
set -e

SRC="${1:-source_clip/source.mp4}"
OUT="${2:-content}"
mkdir -p "$OUT"

if [ ! -f "$SRC" ]; then
  echo "ERROR: source clip not found: $SRC" >&2
  echo "Copy it first, e.g.:" >&2
  echo "  cp /home/ubuntu/commentary/clips/bmg_fch_demo_5min/source.mp4 source_clip/source.mp4" >&2
  exit 1
fi

FPS=30
GOP=60

# Ladder: (level, width, height, bitrate)
LADDER=(
  "0 320 180 200k"
  "1 480 270 400k"
  "2 640 360 700k"
  "3 960 540 1200k"
  "4 1280 720 2000k"
  "5 1280 720 3500k"
)

for entry in "${LADDER[@]}"; do
  set -- $entry
  L=$1 W=$2 H=$3 BR=$4
  echo "=== Encoding level $L : ${W}x${H} @ ${BR} ==="
  ffmpeg -y -i "$SRC" \
    -an \
    -vf "scale=${W}:${H},fps=${FPS}" \
    -c:v libx264 -profile:v baseline -level 3.1 \
    -preset slow -tune zerolatency \
    -b:v "$BR" -maxrate "$BR" -bufsize "$BR" \
    -g "$GOP" -keyint_min "$GOP" -sc_threshold 0 \
    -bf 0 \
    -x264-params "open-gop=0:repeat-headers=1:scenecut=0:bframes=0:keyint=${GOP}:min-keyint=${GOP}" \
    -f h264 "$OUT/level${L}.h264"
done

# One audio extraction, shared across all levels: 16 kHz mono s16le raw PCM.
echo "=== Extracting audio (16kHz mono s16le) ==="
ffmpeg -y -i "$SRC" \
  -vn -ar 16000 -ac 1 -f s16le \
  "$OUT/audio.pcm"

echo
echo "Done. Output in $OUT/:"
ls -lh "$OUT/"
