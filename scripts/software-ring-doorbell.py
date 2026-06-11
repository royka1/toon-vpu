#!/usr/bin/env python3
"""
Ring to Toon Streamer (software / portable variant)
---------------------------------------------------
An automated event-driven service that receives motion/person/doorbell events
from `ring-mqtt` via MQTT and automatically transcodes the camera's RTSP stream
to a Toon 1 thermostat over RTP.

This is the PORTABLE variant: it uses pure-software ffmpeg decode + scale, so it
runs on any Linux box (x86, Raspberry Pi, a NAS, …) — no Rockchip hardware
needed. For an Orange Pi 5 with hardware decode/scale (lower CPU), use
orangepi-ring-doorbell.py instead.
"""

import os
import sys
import json
import time
import signal
import logging
import subprocess
import threading
import urllib.request
from typing import Optional

# Setup Logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s [%(levelname)s] %(message)s',
    handlers=[logging.StreamHandler(sys.stdout)]
)
logger = logging.getLogger("ring_toon_streamer")

try:
    import paho.mqtt.client as mqtt
except ImportError:
    logger.error("paho-mqtt module is missing. Please install it using: pip install paho-mqtt")
    sys.exit(1)

# ==========================================
# CONFIGURATION
# ==========================================
MQTT_HOST = "127.0.0.1"
MQTT_PORT = 1883
MQTT_USER = ""
MQTT_PASSWORD = ""

RING_MQTT_HOST = "127.0.0.1"
RING_MQTT_PORT = 8554

TOON_IP = "192.168.18.22"
TOON_PORT = 5588          # RTP destination port for the video stream
TOON_HTTP_PORT = 8765     # HTTP control port for /show and /hide

# --- What triggers a stream (device IDs exactly as they appear in the MQTT topics) ---
# Look these up in your own ring-mqtt topics: ring/<location>/camera/<DEVICE_ID>/...
# Cameras whose MOTION (motion/state ON) should start a stream:
MOTION_TRIGGER_CAMERAS = ["DOORBELL_ID", "FRONT_CAMERA_ID"]
# Cameras whose DOORBELL press (ding/state ON) should start a stream:
DING_TRIGGER_CAMERAS = ["DOORBELL_ID"]
# Camera that is ALWAYS shown on Toon, regardless of which event triggered the stream:
STREAM_CAMERA_ID = "FRONT_CAMERA_ID"

STREAM_COOLDOWN = 40
FFMPEG_PATH = "ffmpeg"

def toon_trigger(action: str):
    """Call the Toon HTTP control endpoint (e.g. /show or /hide)."""
    url = f"http://{TOON_IP}:{TOON_HTTP_PORT}/{action}"
    try:
        with urllib.request.urlopen(url, timeout=5) as resp:
            logger.info(f"Toon '{action}' triggered -> HTTP {resp.status}")
    except Exception as e:
        logger.error(f"Failed to trigger Toon '{action}' ({url}): {e}")

class StreamState:
    def __init__(self):
        self.active_process: Optional[subprocess.Popen] = None
        self.active_camera_id: Optional[str] = None
        self.stop_timer: Optional[threading.Timer] = None
        self.lock = threading.Lock()

state = StreamState()

def get_rtsp_url(camera_id: str) -> str:
    # ring-mqtt exposes the live stream under the "<device_id>_live" path in go2rtc
    return f"rtsp://{RING_MQTT_HOST}:{RING_MQTT_PORT}/{camera_id}_live"

def run_ffmpeg(rtsp_url: str):
    # NOTE: this is a subprocess argv LIST (no shell), so every ffmpeg token
    # must be its own element — "-c:v mpeg4" is two elements, not one string.
    cmd = [
        FFMPEG_PATH,
        "-hide_banner",
        "-loglevel", "warning",
        "-nostdin",
        "-rtsp_transport", "tcp",
        "-fflags", "nobuffer+genpts+discardcorrupt",
        "-use_wallclock_as_timestamps", "1",
        "-flags", "low_delay",
        "-timeout", "5000000",
        "-i", rtsp_url,
        "-an",                                # No audio for Toon
        # Pure-software decode + scale (runs on any machine, no Rockchip MPP/RGA):
        # crop to 16:9, downscale to 512x288, convert to yuv420p for the Toon's
        # MPEG-4 VPU (which then HW-upscales 512x288 -> 800x450). Adjust the crop
        # (W:H:X:Y) to your camera's resolution/framing.
        "-vf", "fps=20,crop=1440:810:0:0,scale=512:288:flags=fast_bilinear,format=yuv420p",
        "-c:v", "mpeg4",
        "-profile:v", "0",
        "-bf:v", "0",
        "-g:v", "20",
        "-b:v", "1200k",
        "-maxrate", "1500k",
        "-bufsize", "300k",
        "-flush_packets", "1",
        "-max_delay", "0",
        "-avioflags", "direct",
        "-f", "rtp",
        f"rtp://{TOON_IP}:{TOON_PORT}?pkt_size=1200",
    ]

    logger.info(f"Launching FFmpeg: {' '.join(cmd)}")
    try:
        return subprocess.Popen(
            cmd,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
            preexec_fn=os.setsid if os.name != 'nt' else None
        )
    except Exception as e:
        logger.error(f"Failed to start FFmpeg process: {e}")
        return None

def stop_streaming():
    with state.lock:
        if state.active_process:
            logger.info(f"Stopping RTSP stream for camera {state.active_camera_id}...")
            try:
                if os.name != 'nt':
                    os.killpg(os.getpgid(state.active_process.pid), signal.SIGTERM)
                else:
                    state.active_process.terminate()
                state.active_process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                logger.warning("FFmpeg did not close gracefully. Forcing SIGKILL...")
                try:
                    if os.name != 'nt':
                        os.killpg(os.getpgid(state.active_process.pid), signal.SIGKILL)
                    else:
                        state.active_process.kill()
                except Exception:
                    pass
            except Exception as e:
                logger.error(f"Error during streaming cleanup: {e}")

            logger.info("Streaming stopped successfully.")
            state.active_process = None
            state.active_camera_id = None
            # Tell Toon to hide the video now that the stream has stopped.
            toon_trigger("hide")

        if state.stop_timer:
            state.stop_timer.cancel()
            state.stop_timer = None

def start_streaming(camera_id: str):
    with state.lock:
        rtsp_url = get_rtsp_url(camera_id)

        if state.active_process and state.active_camera_id == camera_id:
            logger.info(f"Stream is already active for {camera_id}. Extending cooldown timer (+{STREAM_COOLDOWN}s).")
            if state.stop_timer:
                state.stop_timer.cancel()
            state.stop_timer = threading.Timer(STREAM_COOLDOWN, stop_streaming)
            state.stop_timer.start()
            return

        if state.active_process:
            logger.info(f"Switching stream from {state.active_camera_id} to {camera_id}...")
            try:
                if os.name != 'nt':
                    os.killpg(os.getpgid(state.active_process.pid), signal.SIGTERM)
                else:
                    state.active_process.terminate()
                state.active_process.wait(timeout=2)
            except Exception:
                pass
            state.active_process = None
            state.active_camera_id = None
            if state.stop_timer:
                state.stop_timer.cancel()
                state.stop_timer = None

        logger.info(f"Initializing stream for camera {camera_id} on Toon @ {TOON_IP}:{TOON_PORT}")
        # Tell Toon to show the video before we start transcoding.
        toon_trigger("show")
        proc = run_ffmpeg(rtsp_url)
        if proc:
            state.active_process = proc
            state.active_camera_id = camera_id

            state.stop_timer = threading.Timer(STREAM_COOLDOWN, stop_streaming)
            state.stop_timer.start()
        else:
            logger.error("Could not spawn FFmpeg. Streaming aborted.")
            # Undo the show since no stream will be sent.
            toon_trigger("hide")

def on_connect(client, userdata, flags, rc):
    if rc == 0:
        logger.info("Connected to MQTT Broker successfully!")
        client.subscribe("ring/+/camera/+/motion/state")
        client.subscribe("ring/+/camera/+/ding/state")
        logger.info("Subscribed to motion and doorbell (ding) state topics.")
    else:
        logger.error(f"Failed to connect to MQTT Broker. Return code: {rc}")

def on_message(client, userdata, msg):
    try:
        topic = msg.topic
        payload = msg.payload.decode('utf-8').strip().upper()

        # DEBUG: log every message we actually receive from the broker.
        # If you see motion in Domoticz but nothing here, the script is on the wrong broker.
        logger.info(f"MQTT RX: {topic} = {payload}")

        parts = topic.split('/')
        if len(parts) < 6:
            return

        device_type = parts[2]   # "camera"
        device_id = parts[3]     # e.g. 0123456789ab
        event_type = parts[4]    # "motion" or "ding"

        if device_type != "camera":
            return

        if payload not in ["ON", "TRUE", "MOTION", "1"]:
            return

        # Decide whether this event should start a stream. Whatever the trigger,
        # we always show STREAM_CAMERA_ID on the Toon.
        if event_type == "motion" and device_id in MOTION_TRIGGER_CAMERAS:
            logger.info(f"Motion on camera {device_id} -> streaming {STREAM_CAMERA_ID}")
            start_streaming(STREAM_CAMERA_ID)
        elif event_type == "ding" and device_id in DING_TRIGGER_CAMERAS:
            logger.info(f"Doorbell (ding) on camera {device_id} -> streaming {STREAM_CAMERA_ID}")
            start_streaming(STREAM_CAMERA_ID)

    except Exception as e:
        logger.error(f"Error handling message on topic {msg.topic}: {e}")

def signal_handler(sig, frame):
    logger.info("Shutting down service gracefully...")
    stop_streaming()
    sys.exit(0)

def main():
    # paho-mqtt 2.x requires an explicit callback API version; fall back for 1.x.
    try:
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION1)
    except AttributeError:
        client = mqtt.Client()
    if MQTT_USER and MQTT_PASSWORD:
        client.username_pw_set(MQTT_USER, MQTT_PASSWORD)

    client.on_connect = on_connect
    client.on_message = on_message

    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    try:
        client.connect(MQTT_HOST, MQTT_PORT, 60)
    except Exception as e:
        logger.error(f"Failed to connect to broker @ {MQTT_HOST}:{MQTT_PORT} - {e}")

    client.loop_forever()

if __name__ == "__main__":
    main()
