#!/bin/sh
# Toon: receive + display the doorbell stream. Run this, THEN start the
# Orange Pi side. Listens on tcp/5000 for a raw MPEG-4 elementary stream
# and renders to /dev/fb0 via VPU decode + eMMA PrP YUV->RGB565.
#
# Survives reconnects: vpu_stream loops on accept(), so the sender can
# come and go without restarting this script.

PORT="${PORT:-5000}"
BIN="${BIN:-/root/vpu/vpu_stream}"

# Create the VPU device node if missing (no udev on the Toon).
if [ ! -e /dev/mxc_vpu ]; then
    major=$(awk '/mxc_vpu/{print $1}' /proc/devices)
    [ -n "$major" ] || { echo "mxc_vpu module not loaded"; exit 1; }
    mknod /dev/mxc_vpu c "$major" 0
fi

# Open the firewall for the stream port (resets each boot).
/usr/sbin/iptables -C INPUT -p tcp --dport "$PORT" -j ACCEPT 2>/dev/null || \
  /usr/sbin/iptables -I INPUT 1 -p tcp --dport "$PORT" -j ACCEPT

echo "Toon ready. Waiting for stream on tcp/$PORT..."
exec "$BIN" "$PORT"
