# HANDOFF — Toon doorbell video pipeline

Single-doc onboarding for any tool (Codex / Gemini / DeepSeek / next Claude
session) to pick up this project without re-deriving context. Read this
top-to-bottom, then go.

## Project

Display a live H.264/MPEG-4 doorbell stream on a **Toon thermostat**
(i.MX27 SoC, 800×480 LCD, 2.6.36 kernel, ARMv5, glibc 2.21, busybox 1.27,
no udev) running a custom LVGL UI (`freetoon-lvgl`). Stream source is an
RTSP camera reachable via an Orange Pi 5 that transcodes RTSP →
MPEG-4 SP → TCP to the Toon.

**Where we are**: ~15 fps @ 640×360 or ~11 fps @ 720×480 with eMMA PP doing
YUV→RGB565+resize and a guarded one-frame-late PP pipeline, sub-second
tile-open, HA-triggerable via HTTP, self-heals from wedges in ~12-24s instead
of needing reboot. **Realistic ceiling without clock/display-path risk**:
~15-17 fps at 640×360.

## Hardware reality

| Block | On i.MX27? | Status in our build |
|---|---|---|
| Codadx6 VPU | yes | used; firmware loaded via `vpu_Init`, decodes MPEG-4 SP and H.264 baseline |
| eMMA **PrP** (Pre-Processor, 0x10026400) | yes | **used** for YUV→RGB565 + resize — *wrong block, but it works* |
| eMMA **PP** (Post-Processor, 0x10026000) | yes | **wired via `VPU_IOC_PP_CONVERT`; active at 640×360 and 720×480→640×360; can overlap VPU decode** |
| **IPU** (full Image Processing Unit, with IC + IDMAC + SDC + ADC + CSI + PF) | no for this Toon/i.MX27 path | `CONFIG_MX3_IPU` is for ARCH_MX3, not MACH_MX27; do not chase IPU IC on this hardware tree |
| LCDC | yes | used via `/dev/fb0` (`imx-fb`) |

Key gotchas the docs don't shout about:
- **No IRAM available on the Codadx6 variant**, so VPU work buffers live in DRAM (slower). libvpu warns *"VPU iram is less than needed"* — that's expected, not fixable.
- **HCLK** defaults to 103 MHz (CSCR.AHB_PDF=2). Our kernel module accepts `mxc_vpu hclk_max=1` which bumps it to 155 MHz (CSCR.AHB_PDF=1, against mpll_main2=310 MHz). Always set in `/etc/modules`.
- **VPU clock refcount underflow** can happen if userspace toggles `CLKGATE_SETTING` disable more than enable; produces `WARNING at __clk_disable+0x78/0x84` in dmesg. Avoid manual CLKGATE_SETTING use.
- **`/dev/mxc_vpu` node is created by `mknod` in `/mnt/data/ui_launcher.sh`** (no udev). If missing → camera_init's poll waits forever.
- **PrP CANNOT run concurrently with VPU decode** on the AHB bus. The PrP IRQ never fires while VPU is bursting; userspace SIGALRM doesn't help because libvpu doesn't return. This was the root cause of every PrP "pipelining" attempt deadlocking.
- **PP can overlap VPU decode** in current tests. `vpu_stream` now renders the previous decoded frame through PP while the VPU decodes the next frame, then falls back to sequential PP/PrP if an overlap ioctl fails.

## Repo layout

```
/home/roy/QB2-OSS/
  vpu_stream.c                 # canonical userspace decoder (long-lived)
  IMX27_PDK10_UG.pdf           # NXP i.MX27 platform docs
  emma-ref/{mx27_prp.h, mx27_prphw.c}   # Freescale BSP PrP source -- read before touching PrP
  imx-lib/vpu/                 # Freescale userspace VPU lib (libvpu.a) -- ARM-side ONLY
  oe/.../linux_r07/            # Toon kernel source (R07 base, R10-h28 ABI compat)
    drivers/mxc/vpu/mxc_vpu.c  # our kernel module (heavily modified)
    drivers/dma/ipu/           # MX3 IPU driver; not applicable to Toon/MX27 path
    arch/arm/plat-mxc/include/mach/mxc_vpu.h  # IOCTL nums + BIT register offsets
  freetoon-lvgl/lvgl_ui_recovered/src/
    main.c                     # entry; calls camera_init(), ui_cmd_start()
    camera.c                   # warm-spawn vpu_stream + signal driver + heartbeat watchdog
    camera.h
    ui_cmd.c                   # UNIX-socket listener for "show"/"hide" -> camera_open/close
    ui_cmd.h
    doorbell_daemon.c          # standalone HTTP shim on :8765, POST /show /hide -> /tmp/toonui.cmd
    Makefile                   # TARGET=toon1 builds toonui-toon1 + doorbell_daemon

/home/roy/toon-vpu/             # canonical git repo for kernel + userspace utility
  branch: vpu-stream-warm-fb4-prp-to-fb (PR pushed, not merged)
  apps/vpu_stream.c             # syncs with /home/roy/QB2-OSS/vpu_stream.c
  kernel/mxc_vpu.c              # syncs with the kernel-tree path above
  prebuilt/{mxc_vpu.ko, vpu_stream, libvpu.a, firmware/*}
  scripts/                      # OPi-side scripts (sanitized creds)

GitHub remotes:
  https://github.com/royka1/toon-vpu          (active branch: vpu-stream-warm-fb4-prp-to-fb)
  https://github.com/royka1/freetoon-lvgl     (fork of Ierlandfan/, active: warm-camera-ha-trigger)
```

## On-device layout (Toon @ 192.168.2.102)

```
/root/vpu/
  vpu_stream                   # the decoder binary (also called as warm child)
  doorbell.sh                  # legacy mknod helper -- now done by ui_launcher.sh
/mnt/data/
  toonui                       # LVGL UI binary
  doorbell_daemon              # HA HTTP shim, port 8765
  ui_launcher.sh               # respawned from inittab; mknod /dev/mxc_vpu; bpp; exec toonui
  toonui.cfg                   # camera_enabled, camera_size_pct, camera_src_w/h, camera_x/y
/lib/modules/2.6.36-R10-h28/kernel/drivers/mxc/vpu/mxc_vpu.ko
/etc/modules:                  # autoload at boot
  mxc_vpu hclk_max=1
/etc/inittab:
  toon:345:respawn:/mnt/data/ui_launcher.sh >> /var/volatile/tmp/toonui.log 2>&1
  dbel:345:respawn:/mnt/data/doorbell_daemon >> /var/volatile/tmp/doorbell.log 2>&1
/etc/default/iptables.conf     # persistent firewall; ports 5000 and 8765 ACCEPT'd
/tmp/vpu_stream.tick           # heartbeat file (mtime watched by camera.c watchdog)
/tmp/toonui.cmd                # UNIX socket (ui_cmd listens, daemon writes)
```

## Data flow

```
OPi(192.168.2.254): ffmpeg -i rtsp:// -c:v mpeg4 -f m4v tcp://toon:5000
                                                    │
                                                    ▼
Toon: vpu_stream (warm, --rect X Y W H 5000)
        ├── decoder: Codadx6 VPU @ 155 MHz → YUV420 frames
        ├── PP ioctl: previous frame YUV → RGB565 while next frame decodes
        ├── PrP ioctl: sequential fallback only
        └── output: /dev/fb0 rect, gated by g_show/g_armed
                                                    │
                  HA / user:                        ▼
                  POST /show ──► doorbell_daemon ──► /tmp/toonui.cmd
                                                    │
                                                    ▼  (UNIX socket)
                                            toonui's ui_cmd thread
                                                    │
                                                    ▼  (sig_atomic_t flag, 100ms LVGL timer drains)
                                            camera_open()
                                                    │
                                                    ▼
                                            kill(s_pid, SIGUSR1) → vpu_stream blits
                                            fbdev_set_cutout(rect)  ← LVGL skips repainting rect
                                            create transparent overlay (tap → camera_close → SIGUSR2)
```

## Working architecture in detail

### vpu_stream signal protocol
- `SIGUSR1` → `g_show=1; g_armed=0`. Wait for next I-VOP, then blit.
- `SIGUSR2` → `g_show=0`. Stop blitting; keep decoder warm.
- `SIGALRM` → emergency exit (prime watchdog, internal, 5 s).
- `SIGTERM` → clean exit (calls vpu_DecClose → vpu_release in kernel → hardware reset).

Handlers installed at the **very top of main**, before `--warm` arg parsing — fork+exec race fix (see "Failed experiments" #6).

### Heartbeat watchdog
- vpu_stream has a dedicated pthread that touches `/tmp/vpu_stream.tick` every 3 s.
- The thread only touches when **either**:
  - `g_active == 0` (idle in `accept()`, fine to be slow), **or**
  - `g_progress == 1` (main set it within the last tick).
- camera.c's lv_timer (2 s tick) reads the file's `mtime`; if > 10 s old AND `s_pid > 0`, `SIGKILL`s vpu_stream and unlinks the file.
- After kill, camera.c waits 12 s (6 ticks) before respawning — gives port 5000 time to clear TIME_WAIT.
- New child gets a freshly-touched heartbeat from spawn_warm() so the watchdog gives it 10 s grace to do vpu_Init.

### Kernel hardware reset (single sane place: last close)
`vpu_release()` in `mxc_vpu.c`, when `open_count` reaches 0:
```c
WRITE_REG(0, BIT_CODE_RUN);     /* halt BIT proc */
WRITE_REG(1, BIT_CODE_RESET);   /* assert reset */
udelay(10);
WRITE_REG(0, BIT_CODE_RESET);   /* deassert */
WRITE_REG(1, BIT_INT_CLEAR);
codec_done = 0;
```
This guarantees that any **new** process (after the wedged one was SIGKILL'd) sees a clean VPU. The same code is also gated behind `VPU_IOC_SYS_SW_RESET` ioctl, but **DO NOT call that ioctl from a long-lived process** — it halts the BIT processor while libvpu still thinks the firmware is loaded, and the next `vpu_DecOpen` writes to a dead chip.

### PrP-to-fb-phys (16bpp only)
`dst_rgb` in `VPU_IOC_PRP_CONVERT` can be set to `fb_phys + off_y*fb_stride + off_x*2`, `dst_stride = fb_stride`. PrP DMA writes RGB565 straight into the framebuffer; **no CPU memcpy**. 32bpp path still goes via RGB565 bounce buffer + CPU widen because PrP only emits RGB565.

### PP-to-fb-phys and guarded PP pipeline
Kernel `mxc_vpu.c` direct-pokes eMMA PP at `0x10026000` and exposes
`VPU_IOC_PP_CONVERT` (`_IO('V', 23)`). Userspace tries PP first with the
visible frame height (`ii.picHeight`, not the VPU-aligned height), then falls
back to PrP for unsupported modes. Current PP path is deliberately conservative:
planar YUV420 input, RGB565 output, exact resize ratios that fit in the
hardware's 40-entry table.

Important PP color/layout details:
- PP must process the visible height, but Cb/Cr plane base addresses must be
  derived from the VPU's 16-line-aligned Y plane. Using visible height for the
  chroma base causes a translucent yellow band/top chroma corruption.
- PP A0 CSC coefficients should match Freescale's table:
  `0x94cc3268` and `(1 << 9) | 0x104`.
- PP now has runtime hardware CSC control via `VPU_IOC_PP_SET_CSC` /
  `VPU_IOC_PP_GET_CSC`. `vpu_stream` reads these `/mnt/data/toonui.cfg` keys:
  `camera_pp_luma_pct`, `camera_pp_sat_pct`, `camera_pp_red_pct`,
  `camera_pp_green_pct`, `camera_pp_blue_pct` (0..200), plus raw overrides
  `camera_pp_c0`..`camera_pp_c4`, `camera_pp_x0`, or
  `camera_pp_csc=0x95,0xcc,0x32,0x68,0x104,1`.
- Send `kill -HUP $(pidof vpu_stream)` to reload PP color settings without
  restarting the UI. The log prints `PP CSC: ...` after a successful reload.
- If PP still looks blue after lowering `camera_pp_blue_pct`, test a Cb/Cr
  pointer swap next. Do not assume ffmpeg color settings are the primary cause
  until PP plane order is ruled out.

The display loop holds one decoded display index and, on the next iteration,
starts decode of frame N+1 before converting frame N through PP. Runtime stats
print `pp`, `prp`, and `pipe` counters. Good current log:

```text
Video: 640x360 minFB=2
streaming...
15.6 fps over 60 frames | per-frame avg: dec 64ms csc 6ms blit 0ms | pp 60 prp 0 pipe 60
```

Good 720×480→640×360 log after PP resize support:

```text
Video: 720x480 minFB=2
streaming...
11.4 fps over 60 frames | per-frame avg: dec 88ms csc 9ms blit 0ms | pp 60 prp 0 pipe 60
```

If overlapped PP ever fails, the code logs `eMMA PP pipeline failed ...` and
continues sequentially rather than risking the PrP/VPU deadlock path.

### Frame buffer pool sized minFB+4
The decoder stalls on buffer recycling more often than expected with minFB+2. +4 gave a clean +22% fps (9→11 at 720×480). Cost ~2 MB extra DMA from the reserved pool.

### I-VOP-only resync
`feed_skip_to_latest` scans for `00 00 01 B6 + (vop_coding_type==0)`. If no I-VOP in the drained backlog, push the data through unchanged. Resyncing to a P/B-VOP would leave the decoder reference-less = blocky garbage for an entire GOP. With OPi's `-g 5` (333 ms GOP @ 15 fps), I-VOPs are frequent.

### Listen socket: SO_REUSEADDR + SO_REUSEPORT
Port 5000 TIME_WAIT residue after a respawn-killed prior vpu_stream can otherwise block `bind()`. Both options set.

## What works (shipped wins)

1. **Warm vpu_stream**: ~10 s tile-open → < 1 s
2. **HA daemon on :8765**: external trigger via `POST /show` / `/hide`, optional `X-Doorbell-Token` auth
3. **I-VOP-only resync**: skips are invisible (frame freeze, not blocky garbage)
4. **PrP-to-fb-phys for 16bpp**: -5 to -12 ms per frame
5. **fbcount = minFB+4**: +22% fps
6. **OPi `-g 5` short GOP**: <500 ms wait for first visible frame after /show
7. **Kernel reset on last-close**: any new process starts from clean VPU
8. **Heartbeat watchdog**: wedge → 12 s detect → SIGKILL → kernel reset → 12 s TIME_WAIT skip → respawn → fresh state
9. **Deferred post-respawn SIGUSR1 rearm**: doesn't race the fork+exec handler-install window
10. **Inittab respawn for doorbell_daemon + ui_launcher.sh**: survives reboots
11. **eMMA PP ioctl + guarded PP pipeline**: PP replaces PrP for normal 640×360 output and overlaps with VPU decode (`pipe 60` in steady logs)

## Failed experiments (DO NOT REPEAT)

1. **Pipelining decode + PrP in one thread**: PrP ioctl hangs when called during VPU decode. Diagnosed as hardware-level AHB bus arbitration starving the PrP. No userspace fix exists.
2. **Pthread pipelining (Gemini's `vpu_stream2.c`)**: same root cause as #1. Added more code, didn't fix the wedge.
3. **One-shot vpu_stream** (`exit(0)` on disconnect, camera.c respawn): TIME_WAIT bind failures, repeated vpu_Init triggered kernel clk_disable refcount underflow (`WARNING at __clk_disable`), VPU eventually permanently wedged across processes.
4. **Per-disc `IOSysSWReset`**: kernel reset halts BIT processor while libvpu state still expects firmware running → next `vpu_DecOpen` writes to dead chip → no decode, no IRQ, /show shows nothing.
5. **SIGALRM as wedge detector** for libvpu calls: libvpu does `pthread_mutex_timedlock` + kernel ioctls that don't return promptly on signal. Heartbeat-file + external SIGKILL is the only reliable way.
6. **Immediate post-respawn SIGUSR1 from camera.c**: vpu_stream's `signal(SIGUSR1, ...)` was after arg parsing — race window of a few ms where SIGUSR1 hits default action (terminate). Fixed by moving handler-install to the very top of `main()` AND deferring rearm by one watchdog tick.
7. **Cutting OPi bitrate 2400 → 600 kbps**: zero measurable effect. Decode time on this VPU is **pixel-count bound (MC + IDCT), not entropy/bitrate bound**. Don't waste effort tuning bitrate.

## Open issues (current state)

| Issue | Symptom | Current mitigation | Real fix |
|---|---|---|---|
| `vpu_DecGetInitialInfo` hangs in libvpu after disconnect | wedged warm child, no fps progress | heartbeat watchdog → SIGKILL → kernel reset → respawn (~24 s) | still unknown; keep watchdog and collect logs if PP changes behavior |
| AVC (H.264 baseline) decoder segfaults at ≥480x270 | not currently in use | none — MPEG-4 SP path works | debug player-side; psSaveBuffer/sliceSaveBuffer wiring may be off |
| Decode capped around ~15 fps @ 640×360 | "spec sheet says 30 fps D1" | hclk_max=1 (155 MHz), fbcount+4, PP pipeline | clock/display path work, or lower pixel count |
| HCLK not at chip max (155 MHz, NXP rates VPU to ~200 MHz) | conservative throughput | accepted | switch CCM AHB parent to SPLL @ ~240 MHz (risk: NAND/SDRAM stability) |

## Build / deploy / test

### vpu_stream userspace

```sh
arm-linux-gnueabi-gcc -static -O2 -Wall \
  --sysroot=/tmp/toon-sysroot \
  -I/home/roy/QB2-OSS/imx-lib/vpu \
  -L/home/roy/toon-vpu/prebuilt \
  -o /tmp/vpu_stream /home/roy/QB2-OSS/vpu_stream.c -lvpu -lpthread
arm-linux-gnueabi-strip /tmp/vpu_stream
scp -q /tmp/vpu_stream root@toon:/root/vpu/vpu_stream.new
ssh root@toon 'chmod 0755 /root/vpu/vpu_stream.new && mv /root/vpu/vpu_stream.new /root/vpu/vpu_stream && killall vpu_stream 2>/dev/null || true'
```

### Kernel module

```sh
cd /home/roy/QB2-OSS/oe/homeautomationeurope/recipes/linux/linux-quby2/linux-r07-vpu/linux_r07
make ARCH=arm CROSS_COMPILE=/home/roy/toolchain-build/install/bin/arm-linux-gnueabi- \
  -j$(nproc) M=drivers/mxc/vpu modules
# Verify ABI: this_module section MUST be 0x148 (= R10-h28 expected size)
arm-linux-gnueabi-objdump -h drivers/mxc/vpu/mxc_vpu.ko | grep this_module
scp drivers/mxc/vpu/mxc_vpu.ko \
  root@toon:/lib/modules/2.6.36-R10-h28/kernel/drivers/mxc/vpu/mxc_vpu.ko
ssh root@toon '/sbin/depmod -a; /sbin/reboot'   # rmmod tricky if VPU is busy
```

If `.config` changes, must run `make ARCH=arm silentoldconfig` first to refresh
`include/generated/autoconf.h` so the `M=` build sees the new options.
**CRITICAL**: `CONFIG_ARM_UNWIND=y` must stay set or the module's struct module
size shrinks and lsmod/rmmod crash.

### toonui (LVGL) for Toon 1

```sh
cd /home/roy/QB2-OSS/freetoon-lvgl/lvgl_ui_recovered/src
make TARGET=toon1   # outputs ../build-toon1/toonui-toon1
make TARGET=toon1 abi-check   # MUST show "nothing newer than GLIBC_2.21"
scp ../build-toon1/toonui-toon1 root@toon:/mnt/data/toonui
ssh root@toon 'killall toonui; sleep 15'   # ui_launcher.sh respawns it
```

The buildroot toolchain is at `/tmp/qt_rebuild/toon1-toolchain/` (volatile;
re-create via symlink to `/home/roy/buildroot-toon1/buildroot/output/host` if /tmp got cleared).

### Test cycle (do this yourself, not via Claude — eats too many tokens)

```sh
# Confirm warm pipeline alive
ssh root@toon 'ps | grep vpu_stream; grep "fps over" /var/volatile/tmp/toonui.log | tail -3'
# Healthy current steady-state includes: pp 60 prp 0 pipe 60

# Trigger show via HA endpoint
curl -X POST http://192.168.2.102:8765/show
sleep 3
curl -X POST http://192.168.2.102:8765/hide

# Stress: stop+start OPi cycle (the failure pattern that revealed the wedge)
ssh roy@192.168.2.254 'pkill -9 -f "ffmpeg.*102:5000"'
sleep 10
ssh roy@192.168.2.254 'setsid bash /home/roy/doorbell_hw.sh > /tmp/doorbell.log 2>&1 < /dev/null & disown'
# Watch ~30 s, check fps lines and that PID hasn't churned

# Inspect what happened (this is what to paste to a tool if something broke)
ssh root@toon '
  ps | grep -E "vpu_stream|toonui" | grep -v grep
  netstat -tan | grep 5000
  top -bn1 | head -10
  ls -la /tmp/vpu_stream.tick
  grep -E "fps over|client|Video|heartbeat|prime|respawn|warm-spawn" \
    /var/volatile/tmp/toonui.log | tail -20
  dmesg | tail -15
'
```

## Next work

Do **not** spend more time on the IPU IC plan from older notes. This Toon tree
is `CONFIG_MACH_MX27`; the available `CONFIG_MX3_IPU` path depends on
`ARCH_MX3` and does not describe a usable i.MX27 IPU platform device here.
The i.MX27 multimedia path to work with is eMMA_lt: PP + PrP.

Highest-leverage next steps:

1. Sync the PP kernel/userspace changes into `/home/roy/toon-vpu/` and rebuild
   `prebuilt/{mxc_vpu.ko,vpu_stream}` so the canonical repo matches this tree.
2. Stress-test PP pipeline over repeated OPi ffmpeg stop/start and UI show/hide
   cycles. Watch for `pipe 60`, PID churn, and `eMMA PP pipeline failed`.
3. Improve/log-clean the warm hidden stats window. The first visible fps line
   can include hidden warm frames, for example `fps over 1024 frames`;
   subsequent 60-frame windows are the real performance samples.
4. Watch latency behavior. Current `vpu_stream` caps per-read feed size, avoids
   filling the whole 1 MB VPU bitstream ring, and flushes stale compressed bytes
   when a newer I-VOP is visible in the socket backlog. UDP may still be useful,
   but the first-order TCP problem was stale data being hoarded inside the VPU
   ring, not just the kernel socket buffer.
5. Only after PP is stable, revisit clock experiments. Raising AHB/SPLL may
   help decode time, but it risks NAND/SDRAM/display stability.

## Quick reference

```
On-Toon log:                /var/volatile/tmp/toonui.log
On-Toon daemon log:         /var/volatile/tmp/doorbell.log
On-OPi script:              /home/roy/doorbell_hw.sh (roy@192.168.2.254)
Toon IP:                    192.168.2.102
HA HTTP endpoints:          POST :8765/show, /hide ; GET :8765/status
Optional auth token:        /mnt/data/doorbell.token (header: X-Doorbell-Token)
Module param:               mxc_vpu hclk_max=1 (in /etc/modules)
```

## How to spend my (Claude's) remaining time

I have ~9% of weekly usage left. Highest leverage uses:
1. **Review** patches that Codex/Gemini draft, especially kernel changes
2. **Synthesis** decisions across files (the warm/signal/heartbeat invariants are easy to break)
3. **Diagnose** novel failure modes with logs already captured

Don't ask me to:
- Run repeated stress-test SSH loops (you do those, paste the final state)
- Re-read the same files I've worked on for hours
- Iterate on small build-fix-redeploy cycles (use Codex for those)

End of HANDOFF.
