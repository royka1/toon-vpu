URL="rtsp://127.0.0.1:8554/front_door"
TOON=192.168.2.102

#     -probesize 256000 \
#    -analyzeduration 1000000 \

while true; do
  ffmpeg -hide_banner -loglevel warning \
    -rtsp_transport tcp -fflags nobuffer -flags low_delay \
    -i "$URL" \
    -an \
    -vf "fps=20,crop=1440:810:0:0,scale=512:288:flags=fast_bilinear,format=yuv420p" \
    -c:v mpeg4 -profile:v 0 \
    -bf:v 0 -g:v 20 \
    -b:v 1200k -maxrate 1500k -bufsize 300k \
    -flush_packets 1 \
    -max_delay 0 \
    -avioflags direct \
    -f rtp "rtp://$TOON:5588?pkt_size=1200"

  echo "stream dropped; reconnecting in 2s..."; sleep 2
done

# crop=1800:1080:0:0,\
