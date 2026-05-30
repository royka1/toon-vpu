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
| eMMA **PrP** (Pre-Processor, 0x10026400) | yes | present but **disabled by default**; enable only with `allow_prp=1` for explicit testing |
| eMMA **PP** (Post-Processor, 0x10026000) | yes | **wired via `VPU_IOC_PP_CONVERT`; active for resize/CSC including 720×480→800×450; can overlap VPU decode** |
| **IPU** (full Image Processing Unit, with IC + IDMAC + SDC + ADC + CSI + PF) | no for this Toon/i.MX27 path | `CONFIG_MX3_IPU` is for ARCH_MX3, not MACH_MX27; do not chase IPU IC on this hardware tree |
| LCDC | yes | used via `/dev/fb0` (`imx-fb`) |

Key gotchas the docs don't shout about:
- **IRAM exists on the Toon/i.MX27 and can be exposed to libvpu** with
  `mxc_vpu iram_size=0xb000`. The current kernel can report
  `start=0xffff4c00 end=0xfffffbff`, but this alone did **not** improve decode
  speed. The likely missing piece is in the DX6 userspace/firmware register
  setup: local `imx-lib` only calls the generic IRAM programming helper for
  `cpu_is_mx37()`, and the non-mx5 decoder registration path writes
  `BIT_AXI_SRAM_USE=0`. Do not revert to the old "no IRAM available" theory.
- **HCLK** defaults to 103 MHz (CSCR.AHB_PDF=2). Our kernel module accepts `mxc_vpu hclk_max=1` which bumps it to 155 MHz (CSCR.AHB_PDF=1, against mpll_main2=310 MHz). Always set in `/etc/modules`.
- **VPU clock refcount underflow** can happen if userspace toggles `CLKGATE_SETTING` disable more than enable; produces `WARNING at __clk_disable+0x78/0x84` in dmesg. Avoid manual CLKGATE_SETTING use.
- **`/dev/mxc_vpu` node is created by `mknod` in `/mnt/data/ui_launcher.sh`** (no udev). If missing → camera_init's poll waits forever.
- **PrP CANNOT run concurrently with VPU decode** on the AHB bus. The PrP IRQ never fires while VPU is bursting; userspace SIGALRM doesn't help because libvpu doesn't return. This was the root cause of every PrP "pipelining" attempt deadlocking.
- **PrP is kernel-disabled by default**. `VPU_IOC_PRP_CONVERT` returns `-EOPNOTSUPP` unless `mxc_vpu` is loaded with `allow_prp=1`. Normal operation should stay PP-only.
- **PP can overlap VPU decode** in current tests. `vpu_stream` now renders the previous decoded frame through PP while the VPU decodes the next frame, then falls back to sequential PP if an overlap ioctl fails.

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
        ├── PrP ioctl: disabled unless module param `allow_prp=1`
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

### PrP-to-fb-phys (16bpp only, opt-in)
`dst_rgb` in `VPU_IOC_PRP_CONVERT` can be set to `fb_phys + off_y*fb_stride + off_x*2`, `dst_stride = fb_stride`. PrP DMA writes RGB565 straight into the framebuffer; **no CPU memcpy**. 32bpp path still goes via RGB565 bounce buffer + CPU widen because PrP only emits RGB565.

PrP is not a normal fallback anymore. The kernel module parameter `allow_prp`
defaults to `0`; `VPU_IOC_PRP_CONVERT` returns `-EOPNOTSUPP` unless the module
is explicitly loaded with `allow_prp=1`, for example:

```sh
/sbin/insmod /lib/modules/2.6.36-R10-h28/kernel/drivers/mxc/vpu/mxc_vpu.ko hclk_max=1 allow_prp=1
```

### PP-to-fb-phys and guarded PP pipeline
Kernel `mxc_vpu.c` direct-pokes eMMA PP at `0x10026000` and exposes
`VPU_IOC_PP_CONVERT` (`_IO('V', 23)`). Userspace tries PP first with the
visible frame height (`ii.picHeight`, not the VPU-aligned height), then falls
back to PrP only when the module was explicitly loaded with `allow_prp=1`.
Current PP path is deliberately conservative: planar YUV420 input, RGB565
output, exact resize ratios that fit in the hardware's 40-entry table. The old
userspace clamp that blocked upscaling was removed after PrP became opt-in, so
PP can now do cases like 640×360→800×450.

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

Good 720×480→800×450 log after allowing PP upscale:

```text
Video: 720x480 minFB=2
output rect: 800x450 at (0,15) (hw-resized)
10.9 fps over 60 frames | per-frame avg: dec 91ms csc 10ms blit 0ms | pp 60 prp 0 pipe 60
```

Interpretation: `blit 0ms` means no CPU framebuffer copy; PP DMA writes the
framebuffer directly. At full-screen-ish output the PP/FB cost is about 10ms,
while 720×480 decode is around 90ms, so the 10fps cap is still decode-bound.
For the intended 640×360→800×450 path, the expected win comes from lower decode
time, not cheaper framebuffer writes.

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

## External DX6 branch findings

Checked `https://github.com/jmartinc/video_visstrim` branch `mx27-codadx6`
locally at `/tmp/video_visstrim-mx27-codadx6`.

Useful commits:
- `aa7eb4fdd9` adds DX6 firmware loading. It copies the full firmware to the
  external code buffer and copies the first 4 KB into the BIT internal SRAM via
  `BIT_CODE_DOWN`. Our local `imx-lib/vpu_lib.c` already does the same 2048
  halfword `BIT_CODE_DOWN` load.
- `04a5aa76d9` / current `codadx6_enc.c` hardcodes
  `CODADX6_REG_BIT_SEARCH_RAM_BASE_ADDR = 0xffff4c00`, matching the i.MX27
  IRAM base we now expose. This is encoder search RAM, not direct evidence for
  decoder DBK/IP/BIT IRAM setup.
- `4a552dd418` reserves encoder auxiliary buffers as `64K code +
  (288K + 32*8K FMO) work + 10K para`. Our local MX27 `imx-lib/vpu_reg.h` was
  reduced to `WORK_BUF_SIZE = 128K` to fit earlier Qt-era memory constraints.
  Now that Qt is gone, the larger MX27 work buffer was tested:
  `FMO_SLICE_SAVE_BUF_SIZE=32`, `WORK_BUF_SIZE=(288K + 32*8K)=0x88000`.
  A module reload into the larger buffer wedged in `vpu_ioctl`, but a cold boot
  with the same binary worked and displayed video. The allocation logged as
  `work phy=0xa6800000`, `code phy=0xa6888000` (`code-work=0x88000`). Clean
  windows were still about `14.9 fps`, `dec ~67ms`, `csc ~7ms`, so this does
  not appear to be a decode performance win by itself.
- Added runtime `camera_fb_extra` support in `vpu_stream`. Default/recovered
  value is `4` (`fbcount=minFB+4`, typically 6). It logs:
  `buffers: fbcount=... fbextra=... fbsize=... streambuf=... workbuf=...`
  and per-window bitstream/socket occupancy:
  `bs avg ... max ... sock max ...`.
- Testing `camera_fb_extra=8` at 720x432 produced a green screen, then repeated
  warm-child exits (`status=0xa`) after SIGUSR1 re-arm. Reverted to
  `camera_fb_extra=4` and cold-rebooted. Fresh good line:
  `buffers: fbcount=6 fbextra=4 fbsize=467K streambuf=1024K workbuf=0x88000`.
  Fresh stats showed `bs max ~159K` and `sock max ~50K`, so 720x432 slowdown is
  not due to bitstream ring fullness or socket backlog.
- Added codec selection to `vpu_stream`: default `mpeg4`, optional `--codec h264`
  or `/mnt/data/toonui.cfg` key `camera_codec=h264`. Startup log now includes
  `vpu_stream ready; codec=... listening tcp/5000`. H.264 backlog resync scans
  Annex-B NALs and skips to the latest IDR, preferably starting at the preceding
  SPS/PPS. Do not set `camera_codec=h264` until the OPi is sending raw Annex-B
  H.264 (`-f h264`), otherwise prime will fail or decode garbage.
  Suggested H.264 software-encode test on OPi:
  `-c:v libx264 -preset ultrafast -tune zerolatency -profile:v baseline -bf 0 -g 15 -keyint_min 15 -sc_threshold 0 -x264-params repeat-headers=1:annexb=1:deblock=0,0 -f h264 tcp://toon:5000`.

Non-findings:
- The branch is V4L2 mem2mem **encoder-only** (`CODADX6_INST_ENCODER`); it
  does not contain a decoder path or decode IRAM programming.
- It references `v4l-codadx6-imx27.bin`, but does not carry the firmware blob.
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

## 2026-05-25 runtime note

User reported the stream had played for ~2 hours, then became stuck while CPU
usage was high. Live state did **not** look like a memory leak:

- `vpu_stream` RSS was only ~404 kB, with ~64 MB free memory.
- The process was runnable (`RN`) and no longer producing frame stats.
- The old log had stopped after normal `15.x fps` hidden-warm decode samples.

Added a userspace decoder watchdog in `/home/roy/QB2-OSS/vpu_stream.c`:

- `NO_FRAME_TIMEOUT_MS=8000`: reconnect if VPU/libvpu stops producing display
  frames.
- `KEYFRAME_TIMEOUT_MS=15000`: reconnect if visible mode waits too long for an
  I-VOP after `SIGUSR1`.

Built and deployed `/root/vpu/vpu_stream`; killed only `vpu_stream` and let
`toonui` respawn it. No module reload was needed for this recovery. Verified:

```
Video: 640x360 minFB=2
output rect: 800x450 at (0,15) (hw-resized)
streaming...
14.8-15.2 fps | dec 65-67ms csc 7-8ms blit 0ms | pp 60 prp 0 pipe 60
```

## 2026-05-25 IRAM experiment

User correctly pointed out that i.MX27 uses CODA DX6 and mainline mentions
IRAM. Findings:

- The Toon BSP VPU driver has a `VPU_IOC_IRAM_SETTING` ioctl and platform-data
  IRAM support, but the board data had `.iram_enable=false`.
- This kernel tree also has a stub `include/linux/iram_alloc.h`; `iram_alloc()`
  always returns NULL.
- Added a local module parameter `iram_size=` and a fixed i.MX27 fallback for
  experiments. With `iram_size=0xb000`, the module reported:
  `vpu: IRAM enabled start=0xffff4c00 end=0xfffffbff size=0xb000`.
- After a clean reboot with `iram_size=0xb000`, the stream is stable:
  `avg_fps=15.04 avg_dec=66.00ms avg_csc=7.40ms` over 60 recent windows.
  So exposing IRAM is harmless so far, but it does **not** improve decode by
  itself.
- Older NXP 3.0.35 BSP driver uses a real `CONFIG_IRAM_ALLOC` gen_pool and the
  same kernel-side model: driver allocates/reports an IRAM range; userspace
  imx-lib decides which VPU registers to program.
- The likely missing piece is in `imx-lib/vpu/vpu_lib.c`: decode IRAM layout is
  computed by `SetDecSecondAXIIRAM()`, but the frame-buffer IRAM address
  registers are only written for `cpu_is_mx5x()`. i.MX27 falls through to no
  address setup. `vpu_DecStartOneFrame()` still writes `BIT_AXI_SRAM_USE`, so
  inspect/patch this carefully rather than blindly copying the encoder path.

Conclusion: keep `iram_size=0xb000` as an experimental module parameter, but do
not expect speedup until the DX6 decode address/use-bit path is understood.

## 2026-05-26 H.264 striped-screen mitigation

User copied a PHYTEC i.MX27 BSP into `bsp/` and CODA firmware split/disassembly
into `fw/coda-bits/splitted/`. Findings from the first pass:

- `bsp/patches/0032-MXC-Add-vpu-driver.patch/drivers/mxc/vpu/mxc_vpu.c` and
  `.h` are zero-length in this copy. The useful BSP artifacts are the packaged
  runtime libraries (`libfsl-vpu-0.1.0.so.0.0.0`, `libmfw_gst_vpu.so`) plus the
  clock/platform patches.
- The BSP `libfsl-vpu` exports the expected decode entry points but is much
  smaller than our current `imx-lib/vpu/libvpu.a`; it is a reference, not an
  obvious drop-in fix.
- The firmware disassembly has no readable H.264/AVC strings; use it for
  register/control-flow archaeology only.

New symptom: after H.264 playback and switching streams, the screen can show
horizontal colored/black stripes. MPEG-4 still works, but H.264 only recovers
after a full reboot; module unload/reload alone is not sufficient in the
observed state.

Local mitigation in `vpu_stream.c`, built and deployed to `/root/vpu/vpu_stream`:

- H.264 no longer uses the one-frame-late PP pipeline. MPEG-4 still can overlap
  PP with decode, but H.264 now renders sequentially because decoded pictures
  may still be live references while the next frame is decoding.
- After an H.264 client disconnect, `vpu_stream` closes the extra PP fd, calls
  `vpu_UnInit()`, closes fb/listen fds, and `execv()`s itself. This forces a real
  `/dev/mxc_vpu` last-close boundary and kernel reset between H.264 sessions
  without relying on the UI.
- After a reboot the H.264 test wedged at `client connected` with high CPU.
  Root cause candidate: `/mnt/data/toonui.cfg` did not contain
  `camera_codec=h264`, so a no-arg `vpu_stream` would default to MPEG-4 and try
  to parse H.264 as MPEG-4. Set `camera_codec=h264` on Toon for the H.264 test.
- Added a `SIGALRM` hard timeout around `vpu_DecGetInitialInfo()` and
  `vpu_DecClose()`. If libvpu spins in one of its raw busy-wait loops, the
  process exits and closes `/dev/mxc_vpu` instead of burning CPU forever.
- H.264 test at 640x368 showed only ~8 fps and green/blue small blocks:
  `dec 105-125ms csc 6-8ms`, so performance is decode-bound, not PP-bound.
  Added another local mitigation/diagnostic build:
  - disable `feed_skip_to_latest()` for H.264; do not flush/jump to later IDR
    inside an active H.264 decoder state;
  - do not fall back to PrP for H.264 if PP fails;
  - log H.264 crop and per-window `numOfErrMBs`, decode failures, and PS/slice
    buffer shortage counters;
  - apply simple top-left-zero H.264 crop so a 640x360 source encoded as
    640x368 displays as 640x360 when crop metadata is present.
  - after a `320x192` coded / `320x180` cropped test showed
    `eMMA PP unavailable (errno=22)` and then continued decoding with `pp 0`,
    added H.264 PP-geometry preflight. Unsupported geometry such as
    `320x180 -> 640x360` now exits/re-execs before streaming instead of
    decoding forever with PP disabled and a full bitstream ring.
- Audited `deepseek-edits.txt`: PS save and AVC slice buffers are necessary
  enough to keep for now, but the DeepSeek `bufMvCol` conclusion was wrong for
  CODA DX6. Mainline `drivers/media/platform/chips-media/coda/coda-bit.c`
  explicitly skips MV-col allocation and skips writing parameter-buffer
  entries `96+i` when `product == CODA_DX6`. Reverted the local i.MX27
  `virt_paraBuf[96+i] = bufMvCol` change in `imx-lib/vpu/vpu_lib.c`, rebuilt
  `libvpu.a`, rebuilt/deployed `/root/vpu/vpu_stream`.
- UI was intentionally left stopped for testing:
  `# toon:345:respawn:/mnt/data/ui_launcher.sh >> /var/volatile/tmp/toonui.log 2>&1`

If H.264 stripes still happen, next test is stronger isolation: run H.264 with
PP disabled entirely or make H.264 exit instead of `execv()` after the first
session so an external loop/supervisor starts a fresh process.

## 2026-05-26 direct RTSP experiment

`vpu_stream.c` now has an experimental direct RTSP source:

```sh
/root/vpu/vpu_stream --codec h264 --rtsp 'rtsp://user:pass@192.168.2.197:554/path'
```

Implementation notes/limits:

- H.264 only. MPEG-4 RTSP is not implemented.
- RTSP-over-TCP interleaved only (`Transport: RTP/AVP/TCP;...`).
- Depacketizes RTP H.264 single NAL, STAP-A, and FU-A into Annex-B and feeds
  the existing VPU decoder path through a socketpair.
- Numeric IPv4 host only for now; no DNS, to avoid static glibc NSS issues on
  Toon.
- Basic auth remains supported. Digest auth is now supported for the common
  no-`qop` RTSP challenge form seen from the camera:
  `WWW-Authenticate: Digest realm="BC Streaming Media", nonce="..."`.
  DESCRIBE is retried after the 401 challenge, and SETUP/PLAY then include the
  same Digest credentials.
- Some cameras send interleaved RTP (`$...`) immediately around PLAY. The RTSP
  response reader now discards early interleaved packets while waiting for the
  `RTSP/1.0` response, fixing `rtsp PLAY failed: $`.
- The SDP-provided H.264 SPS/PPS is now written to the decoder input
  immediately after PLAY succeeds. Previously SPS/PPS was only injected before
  later IDR packets, so the VPU prime phase could fail with
  `prime failed (... no sequence header)` even though DESCRIBE had parsed
  `spspps=... bytes`.
- RTSP diagnostics showed the camera is delivering valid H.264 RTP on channel
  0: SPS nal=7, PPS nal=8, then FU-A nal=28 slices. The SDP SPS bytes
  `0000000167640033...` decode as profile_idc 100 = High profile, level 5.1.
  This is likely why CODA DX6 still fails sequence init. `vpu_stream` now logs
  the SPS profile/level explicitly and warns for High-profile streams.
- UI is still intentionally stopped.

## 2026-05-26 VPU bus diagnostics

Added diagnostics to `drivers/mxc/vpu/mxc_vpu.c`:

- maps SDRAMC and M3IF in kernel space and logs register dumps at module load;
- adds module parameter `clear_lhd=1`, which clears SDRAMC `ESDCFG0` bit 5
  (LHD / Latency Hiding Disable), the old i.MX27 MPEG-4 workaround bit;
- builds must use `KCFLAGS='-fno-pic -fno-pie'` with GCC 14, otherwise the
  module references `_GLOBAL_OFFSET_TABLE_` and fails to load.

Live test result before a MAX read fault:

- `SDRAMC ESDCFG0=0x80000004 LHD=0`, so LHD was already clear. The old LHD
  throttle is not active on this boot.
- M3IF dumped successfully.
- Blindly reading the MAX block at `MX27_MAX_BASE_ADDR` caused an external
  abort during module init. The code was changed not to map/read MAX.

The fault left a half-loaded `mxc_vpu` instance (`lsmod` shows use count 1)
that cannot be removed with `rmmod`; reboot is required to clear it. The
on-disk module at `/lib/modules/2.6.36-R10-h28/kernel/drivers/mxc/vpu/mxc_vpu.ko`
has already been replaced with the fixed no-MAX diagnostic build. UI respawn is
currently disabled in `/etc/inittab`.

## 2026-05-26 H.264 latency watchdog

The MPEG-4 low-latency path can flush stale compressed bytes and resync at the
latest I-VOP. H.264 cannot safely use the same in-place bitstream flush on CODA
DX6; previous attempts caused green/blue macroblock corruption because the
decoder reference state/DPB was no longer coherent.

`vpu_stream.c` now handles H.264 backlog by restarting the session instead:

- if the H.264 VPU bitstream ring stays above 512 KiB, or the TCP socket queue
  stays above 24 KiB, for more than 2500 ms, it logs:
  `decoder watchdog: h264 backlog ... reconnecting`
- the disconnect path closes the socket and, for H.264, `execv()` restarts
  `vpu_stream` for a clean VPU state;
- the Orange Pi ffmpeg loop should then reconnect, bounding latency instead of
  allowing a 20-second delayed stream to continue.

Also added an input-starvation watchdog: if both the VPU ring and socket are
empty for more than 5000 ms, it logs:
`decoder watchdog: input starved ... reconnecting`.

## 2026-05-26 RTP/H.264 input

`vpu_stream.c` now supports a direct UDP RTP/H.264 input mode:

```sh
/root/vpu/vpu_stream --codec h264 --rtp 5004 --rect 16 24 768 432
```

It creates an internal socketpair and a UDP receiver thread, depacketizes
RTP/H.264 single NAL, STAP-A, and FU-A packets into Annex-B, then feeds the
existing H.264 VPU path. This avoids raw TCP's "preserve every stale byte"
behavior. The UDP socket has a small 64 KiB receive buffer, so overload drops
packets instead of accumulating seconds of latency.

The RTP depacketizer now tracks RTP sequence numbers. If a packet is missing,
the RTP path now drops dependent non-IDR H.264 slices until the next IDR frame
arrives. It also buffers a complete RTP timestamp/access unit and only writes it
to CODA when the RTP marker bit arrives and no packet gap occurred inside that
frame. This prevents a partial frame from being fed if an earlier slice of the
same frame was already received before a later packet was lost. Expected logs:

```text
udp rtp sequence gap: expected=... got=...; waiting for IDR
udp rtp resync at IDR
```

Follow-up: RTP mode no longer uses the UDP receiver thread + socketpair for
`--rtp`. It now receives UDP synchronously in the decoder loop and stages each
complete access unit directly in the VPU bitstream ring. The VPU write pointer
is advanced only after a complete marker-terminated access unit with no RTP
sequence gap. Bad access units are discarded by not calling
`vpu_DecUpdateBitstreamBuffer()`.

Follow-up after testing: the first direct build only read one UDP packet per
decoder/feed call, which immediately overflowed the kernel receive queue during
RTP frame bursts. Logs showed rapid sequence gaps and ~1.6 fps. `rtp_feed_direct`
now drains up to 256 queued RTP packets per call and the UDP receive buffer is
512 KiB.

Diagnostic renderer: `vpu_stream` now accepts `--cpu-render`. It maps decoded
YUV frame buffers into userspace and does a simple nearest-neighbor YUV420 to
RGB565 conversion directly into `/dev/fb0`, bypassing eMMA PP entirely. This is
slow and only for A/B testing. If colored blocks disappear with:

```sh
/root/vpu/vpu_stream --codec h264 --rtp 5588 --rect 16 24 768 432 --cpu-render
```

then the H.264 decode is probably fine and the corruption is PP/render-path
specific. If blocks remain, the corruption is already in the decoded YUV.

## 2026-05-26 RTP/MPEG-4 input

`--rtp` now supports both H.264 and MPEG-4 Visual:

```sh
/root/vpu/vpu_stream --codec mpeg4 --rtp 5588 --rect 16 24 768 432
```

For MPEG-4 RTP the payload is treated as raw MPEG-4 elementary-stream bytes
(RFC3016 style). The direct RTP receiver still stages each marker-terminated
access unit in the VPU bitstream ring and only commits it if no RTP sequence
gap occurred. After a gap it waits for the next MPEG-4 I-VOP
(`00 00 01 b6` with coding type 0) before feeding dependent VOPs again.

Suggested Orange Pi sender:

```sh
ffmpeg ... \
  -vf "crop=1440:810:0:135,fps=20,scale_rkrga=w=512:h=288:format=nv12" \
  -c:v mpeg4 -profile:v 0 -bf:v 0 -g:v 20 \
  -b:v 1200k -maxrate 1500k -bufsize 300k \
  -f rtp "rtp://192.168.2.102:5588?pkt_size=1200"
```

If prime fails with no sequence header, ffmpeg may be putting the MPEG-4 VOL
config only in SDP instead of in-band; add config injection next if needed.

Plain RTP has no SDP/RTSP setup, so the sender must put SPS/PPS in-band before
IDR frames. With ffmpeg, use RTP output and repeat headers/extradata, for
example:

```sh
-vf "crop=1440:810:0:135,fps=20,scale_rkrga=w=512:h=288:format=nv12" \
-c:v h264_rkmpp -profile:v baseline -level:v 3.0 -bf:v 0 -g:v 20 \
-bsf:v dump_extra=freq=keyframe \
-f rtp "rtp://toon:5004?pkt_size=1200"
```

If the generated SDP lacks `a=fmtp:...sprop-parameter-sets=...`, CODA will fail
prime with `no sequence header`. `vpu_stream` now accepts explicit SPS/PPS:

```sh
/root/vpu/vpu_stream --codec h264 --rtp 5004 \
  --sprop 'BASE64_SPS,BASE64_PPS' \
  --rect 16 24 768 432
```

On startup it logs `rtsp h264 sprop ...` and the decoded SPS profile/level.
`--sprop` now validates that the first decoded NAL is SPS type 7 and the second
is PPS type 8, so the placeholder string `BASE64_SPS,BASE64_PPS` is rejected
instead of being treated as garbage NAL type 4.

RTP mode used to print `client connected` immediately because the decoder reads
from an internal socketpair connected to the UDP receiver thread. That was
misleading: no remote UDP sender had connected. The log now says
`rtp source ready` / `rtp waiting for packets` instead. The UDP socket timeout
was also removed; with no RTP sender, it now waits idle instead of closing the
socketpair and looping with `prime failed (1 tries, no sequence header)`.

Follow-up fix: the main decoder code was still applying a 5s `SO_RCVTIMEO` to
the internal socketpair. That also made idle RTP prime fail even though the UDP
thread was waiting correctly. The timeout is now applied only to raw TCP mode.
UDP `SO_REUSEADDR`/`SO_REUSEPORT` is set as a guard, and if RTP prime fails
after receiving bad/no-SPS data, the process `execv()` restarts so the old UDP
receiver thread cannot keep the port bound.

## 2026-05-26 toonui RTP config

The recovered LVGL UI source now has a config key for spawning `vpu_stream` in
RTP mode:

```ini
camera_enabled=1
camera_codec=mpeg4
camera_rtp=5588
```

`camera-rtp=5588` is accepted too, but `camera_rtp` matches the existing config
style. With `camera_rtp > 0`, the UI warm-spawns:

```sh
vpu_stream --warm --rect X Y W H --rtp 5588
```

With `camera_rtp=0` or no key, it keeps the legacy TCP port 5000 behavior.
This requires rebuilding/redeploying `toonui`; the currently installed UI will
ignore the new key until replaced.

Deployed to Toon after building `freetoon-lvgl/lvgl_ui_recovered/build-toon1/toonui-toon1`
with the local `arm-linux-gnueabi-` toolchain. The installed `/mnt/data/toonui`
now contains `camera_rtp`, `camera-rtp`, and `--rtp`. Verified process state:

```sh
/mnt/data/toonui
vpu_stream --warm --rect 16 24 768 432 --rtp 5588
```

`/mnt/data/toonui.cfg` had duplicate codec keys; the old trailing
`camera_codec=h264` was removed and `camera_codec=mpeg4` is now the final value.

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
