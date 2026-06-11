#!/bin/sh
# Toon: receive + display the doorbell stream. Run this, THEN start the
# Orange Pi side. Listens on tcp/5000 for a raw MPEG-4 elementary stream
# and renders via VPU decode + eMMA PP YUV->RGB (hardware resize, DMA
# straight into the framebuffer).
#
# Survives reconnects: vpu_stream loops on accept(), so the sender can
# come and go without restarting this script.
#
# RECT defaults to full-screen 800x480: pair with a 720x480 sender
# (orangepi-doorbell-d1-hw.sh) and the PP stretches only the width (10:9)
# in hardware; height maps 1:1. For a pixel-perfect 1:1 blit use
# RECT="40 0 720 480" (40 px black bars left/right).

PORT="${PORT:-5000}"
BIN="${BIN:-/root/vpu/vpu_stream}"
RECT="${RECT:-0 0 800 480}"

# Create the VPU device node if missing (no udev on the Toon).
if [ ! -e /dev/mxc_vpu ]; then
    major=$(awk '/mxc_vpu/{print $1}' /proc/devices)
    [ -n "$major" ] || { echo "mxc_vpu module not loaded"; exit 1; }
    mknod /dev/mxc_vpu c "$major" 0
fi

# Open the firewall for the stream port (resets each boot). Stock Quby
# firewalls filter in the HCB-INPUT chain; an INPUT rule inserted before
# the jump works on both stock and modified setups.
/usr/sbin/iptables -C INPUT -p tcp --dport "$PORT" -j ACCEPT 2>/dev/null || \
  /usr/sbin/iptables -I INPUT 1 -p tcp --dport "$PORT" -j ACCEPT

echo "Toon ready. Waiting for stream on tcp/$PORT..."
# shellcheck disable=SC2086  # RECT is intentionally word-split
exec "$BIN" --rect $RECT "$PORT"
