#!/bin/bash
# Orange Pi 5: low-latency doorbell variant — pulls the camera's NATIVE
# substream resolution (no scaling) and runs the smallest possible buffer.
# Use this when you want minimum glass-to-glass delay and your camera has a
# substream at a Toon-friendly resolution (e.g. 640x360 H.264).
#
# Required env:
#   RTSP_URL   substream URL, e.g. rtsp://user:pass@CAM/Streaming/Channels/102
#   TOON_HOST  IP / hostname of the Toon
# Optional:
#   PORT       Toon listener port (default 5000)

URL="${RTSP_URL:?set RTSP_URL=rtsp://user:pass@CAMERA_IP:554/substream-path}"
TOON="${TOON_HOST:?set TOON_HOST=<toon-ip>}"
PORT="${PORT:-5000}"

while true; do
    ffmpeg -hide_banner -loglevel warning \
        -rtsp_transport tcp -fflags nobuffer -flags low_delay \
        -probesize 32 -analyzeduration 0 \
        -i "$URL" \
        -an \
        -vf "scale=640:360:in_range=pc:out_range=tv, \
             colormatrix=src=bt709:dst=bt601, \
             format=yuv420p" \
        -c:v mpeg4 -profile:v 0 \
        -b:v 800k -maxrate 800k -bufsize 32k \
        -g 5 -sc_threshold 0 \
        -max_delay 0 -avioflags direct \
        -f m4v "tcp://${TOON}:${PORT}"
    echo "stream dropped; reconnecting in 2s..." >&2
    sleep 2
done
