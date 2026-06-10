#!/usr/bin/env bash
# Orange Pi 5: 1080p camera -> D1 (720x480) MPEG-4 SP @ 30 fps over TCP.
# The proven full-quality pipeline: rkmpp hardware H.264 decode, RGA
# hardware crop+scale, software MPEG-4 encode (fast enough at D1).
#
# Geometry: the Toon panel is 800x480 (5:3) and the Codadx6 decoder tops
# out at 720 wide, so we deliver anamorphic 720x480 and let the Toon's
# eMMA PP stretch only the width (720->800); height maps 1:1, which is the
# most detail this display can show. CW/CH must be a 5:3 region of the
# source; CX/CY position it (defaults give a slight zoom of a 1080p frame).
#
# Transport is TCP, not RTP: the Toon's USB WiFi drops UDP bursts, and the
# RTP path discards to the next intact I-frame on every gap (~4 fps at D1).
# TCP retransmits instead and vpu_stream bounds the latency by skipping to
# the latest I-VOP when backlog builds.
#
# Required env:
#   RTSP_URL   e.g. rtsp://user:pass@CAMERA_IP:554/stream  (or a relay)
#   TOON_HOST  IP / hostname of the Toon
# Optional:
#   PORT       Toon listener port (default 5000)
#   CX CY CW CH  crop of the source, 5:3 aspect (default 60 0 1350 810)
#   FPS        output frame rate (default 30; match your camera)
#   GOP        keyframe interval (default 15 = 0.5 s: fast recovery after
#              a backlog skip, small enough to fit vpu_stream's drain window)
#   BITRATE    target bitrate (default 2000k; decode speed is unaffected,
#              spend what the WiFi can carry)

URL="${RTSP_URL:?set RTSP_URL=rtsp://user:pass@CAMERA_IP:554/path}"
TOON="${TOON_HOST:?set TOON_HOST=<toon-ip>}"
PORT="${PORT:-5000}"
CX="${CX:-60}"; CY="${CY:-0}"; CW="${CW:-1350}"; CH="${CH:-810}"
FPS="${FPS:-30}"
GOP="${GOP:-15}"
BITRATE="${BITRATE:-2000k}"
MAXRATE="${MAXRATE:-2400k}"

while true; do
  ffmpeg -hide_banner -loglevel warning -nostdin \
    -hwaccel rkmpp \
    -hwaccel_output_format drm_prime \
    -rtsp_transport tcp \
    -fflags nobuffer+genpts+discardcorrupt \
    -use_wallclock_as_timestamps 1 \
    -flags low_delay \
    -timeout 5000000 \
    -i "$URL" \
    -an \
    -vf "fps=${FPS},vpp_rkrga=cx=${CX}:cy=${CY}:cw=${CW}:ch=${CH}:w=720:h=480:format=nv12:async_depth=4,hwmap=mode=read,format=nv12,format=yuv420p" \
    -c:v mpeg4 -profile:v 0 \
    -bf:v 0 -g:v "$GOP" \
    -b:v "$BITRATE" -maxrate "$MAXRATE" -bufsize 600k \
    -flush_packets 1 \
    -max_delay 0 \
    -avioflags direct \
    -f m4v "tcp://${TOON}:${PORT}"
  echo "stream dropped; reconnecting in 2s..." >&2
  sleep 2
done
