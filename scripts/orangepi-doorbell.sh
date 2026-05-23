#!/bin/bash
# Orange Pi 5 (or any Linux host): pull doorbell RTSP -> 640x360 MPEG-4 ->
# stream over TCP to the Toon. Software decode + scale; the most reliable
# variant. Auto-reconnects if the camera or the Toon listener disappears.
#
# Required env:
#   RTSP_URL   e.g. rtsp://user:pass@CAMERA_IP:554/Streaming/Channels/101
#   TOON_HOST  IP / hostname of the Toon running toon-doorbell.sh
# Optional:
#   PORT       TCP port the Toon is listening on (default 5000)
#   WIDTH      output width  (default 640)
#   HEIGHT     output height (default 360)

URL="${RTSP_URL:?set RTSP_URL=rtsp://user:pass@CAMERA_IP:554/path}"
TOON="${TOON_HOST:?set TOON_HOST=<toon-ip>}"
PORT="${PORT:-5000}"
WIDTH="${WIDTH:-640}"
HEIGHT="${HEIGHT:-360}"

# Notes on the filter chain:
#   - in_range=pc:out_range=tv  : many H.264 cameras emit full-range YUV;
#     the Toon's eMMA PrP CSC assumes studio (limited) range, so squeeze
#     full->limited here to avoid washed/clipped colors.
#   - colormatrix=src=bt709:dst=bt601 : Toon CSC coefficients are BT.601;
#     convert from the camera's BT.709 first.
#   - format=yuv420p : the VPU MPEG-4 decoder expects planar 4:2:0.

while true; do
    ffmpeg -hide_banner -loglevel warning \
        -rtsp_transport tcp -fflags nobuffer -flags low_delay \
        -i "$URL" \
        -an \
        -vf "scale=${WIDTH}:${HEIGHT}:in_range=pc:out_range=tv, \
             colormatrix=src=bt709:dst=bt601, \
             format=yuv420p" \
        -c:v mpeg4 -profile:v 0 -b:v 1200k -g 15 \
        -f m4v "tcp://${TOON}:${PORT}"
    echo "stream dropped; reconnecting in 2s..." >&2
    sleep 2
done
