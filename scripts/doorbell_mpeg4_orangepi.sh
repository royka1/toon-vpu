#!/usr/bin/env bash
URL="rtsp://127.0.0.1:8554/front_door"
TOON=192.168.2.102

while true; do
  ffmpeg -hide_banner -loglevel warning -nostdin\
    -hwaccel rkmpp \
    -hwaccel_output_format drm_prime \
    -rtsp_transport tcp \
    -fflags nobuffer+genpts+discardcorrupt \
    -use_wallclock_as_timestamps 1 \
    -flags low_delay \
    -timeout 5000000 \
    -i "$URL" \
    -an \
    -vf "fps=20,vpp_rkrga=cx=0:cy=0:cw=1440:ch=810:w=512:h=288:format=nv12:async_depth=4,hwmap=mode=read,format=nv12,format=yuv420p" \
    -c:v mpeg4 -profile:v 0 \
    -bf:v 0 -g:v 20 \
    -b:v 1200k -maxrate 1500k -bufsize 300k \
    -flush_packets 1 \
    -max_delay 0 \
    -avioflags direct \
    -f rtp "rtp://$TOON:5588?pkt_size=1200"
  echo "stream dropped; reconnecting in 2s..."; sleep 2
done
