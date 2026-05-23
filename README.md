# toon-vpu

Hardware-accelerated video decode on a rooted **Toon thermostat** (Quby/Eneco) —
the i.MX27 Codadx6 VPU brought back to life on a kernel it was never meant to
run on, then wired up as a live front-door camera display.

The i.MX27 SoC inside the Toon has a perfectly capable Chips&Media Codadx6
VPU and a Freescale eMMA pre-processor, but **Quby stripped the entire VPU
stack — driver, headers, firmware — out of their fork of the BSP**, so a
factory Toon can't use either block. This repo puts the hardware back in
service: it backports the Freescale 2.6.35 BSP Codadx6 driver onto the
Toon's 2.6.36-R10-h28 kernel, adds the kernel-side workarounds the
platform needs (no udev, no userspace peripheral access, no `vpu_clk`
binding on this board, no contiguous DMA at runtime), bundles the
Freescale firmware blobs the driver loads at init, and ships the
userspace pieces to actually display a stream.

End-to-end pipeline:

```
   ┌─────────────┐  RTSP/H.264   ┌─────────────────┐  MPEG-4 / TCP   ┌──────────┐
   │ doorbell IP │ ────────────▶ │ Orange Pi 5     │ ──────────────▶ │  Toon    │
   │ camera      │               │ (transcode +    │                 │ (decode  │
   └─────────────┘               │  scale to 640p) │                 │  + show) │
                                 └─────────────────┘                 └──────────┘
```

The Orange Pi handles the things the Toon can't (RTSP, H.264, scaling) and
hands the Toon a small MPEG-4 Simple-Profile stream the i.MX27 VPU can
decode in hardware. The Toon's eMMA PrP block does YUV→RGB565 + resize, and
the result is blitted to `/dev/fb0`.

## Layout

| Path | What it is |
|------|------------|
| `kernel/`     | The modified VPU driver and its ioctl header — drop into `drivers/mxc/vpu/` and `arch/arm/plat-mxc/include/mach/` of a 2.6.36 i.MX27 kernel. |
| `lib/`        | Freescale `imx-lib/vpu` patched to talk to this driver (kernel-side register ioctls, no MX27 detection assumption, etc.). Builds `libvpu.a`. |
| `apps/`       | Two userspace players linked against `libvpu.a`. |
| `scripts/`    | Run-on-the-Toon + run-on-the-sender helpers. |
| `prebuilt/`   | **Ready-to-install binaries for the Toon's exact kernel** (`2.6.36-R10-h28`, ARM `armv5tejl`): `mxc_vpu.ko`, `libvpu.a`, `vpu_stream`, `vpu_dec_fb3`, and the Freescale VPU firmware (`firmware/vpu_fw_imx27_TO{1,2}.bin`), with `SHA256SUMS`. |
| `reference/`  | Original Freescale 2.6.35 BSP eMMA PrP code we ported into the driver. |
| `INSTALL.md`  | **Step-by-step Toon install using `prebuilt/`** — start here if you don't want to compile anything. |
| `HANDOVER.md` | Detailed technical notes — every gotcha, every dead end, the wiring of the eMMA path, register addresses, etc. Read this before changing the driver. |

> ⚠️ **The prebuilt module is kernel-version-specific.** It only loads on
> `2.6.36-R10-h28` with `CONFIG_ARM_UNWIND=y` (`struct module` size `0x148`).
> Any other Toon kernel needs a rebuild from `kernel/`. See
> [`INSTALL.md`](INSTALL.md#%EF%B8%8F-kernel-version-this-must-match-exactly).

## What's in `kernel/mxc_vpu.c` that isn't in the stock driver

- Platform-bus init bypassed; the driver directly `ioremap`s `MX27_VPU_BASE_ADDR`,
  registers a misc char device, and requests IRQ 53 itself. The Toon doesn't
  have a platform device for the VPU and synthesizing one crashed
  `platform_device_add` on this kernel.
- `clk_get("vpu_clk")` is optional — this board exposes the gate via CCM
  bits directly (PCCR1 bits 6+16), driven from `vpu_clk_force_on()`.
- 12 MB contiguous DMA pool reserved at module load (3 × MAX_ORDER pages,
  bitmap allocator). The runtime page allocator can't give MB-scale
  contiguous blocks once the system has been up for a while.
- Kernel-side **register-access ioctls** (`VPU_IOC_REG_READ`/`_WRITE`) —
  userspace peripheral-bus accesses external-abort on this SoC config, so
  the library proxies every VPU register read/write through the driver.
- Kernel-side **eMMA PrP YUV→RGB565 + resize** ioctl (`VPU_IOC_PRP_CONVERT`),
  with the i.MX27 PrP block fully programmed in-driver: CSC (BT.601 studio
  range, Q1.7 coefficients), bilinear/average scaler, IRQ-driven completion.
- `vmalloc_user` + `remap_vmalloc_range` for the library's
  PROCESS-SHARED pthread mutex (avoids `VM_IO` so futex GUP works — without
  this the streamer aborts after ~minutes).

## Build

### Kernel module

```sh
# from the kernel tree root (after dropping kernel/* into the right places)
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- M=drivers/mxc/vpu modules
```

Toolchain used here: GCC 4.5.3. Newer kernels and newer GCCs need the usual
host-compiler workarounds (e.g. a `compiler-gcc14.h -> compiler-gcc4.h`
symlink to satisfy `prepare`).

### Userspace library and apps

```sh
# library
cd lib && make PLATFORM=IMX27ADS CROSS_COMPILE=arm-linux-gnueabi-

# streamer (link against the static lib + the kernel header from kernel/)
cd ../apps
arm-linux-gnueabi-gcc -static --sysroot=/path/to/toon-sysroot \
    -I../lib -L../lib \
    -o vpu_stream vpu_stream.c -lvpu -lpthread
```

The static link + Toon glibc 2.21 sysroot is needed because the Toon's
runtime is older than anything a modern distro will give you.

## Deploy

```sh
# kernel module
scp kernel/mxc_vpu.ko root@toon:/lib/modules/$(uname -r -on -toon)/kernel/drivers/mxc/vpu/
ssh root@toon "/sbin/depmod -a && /sbin/insmod /lib/modules/.../mxc_vpu.ko"

# streamer + script
scp apps/vpu_stream                 root@toon:/root/vpu/vpu_stream
scp scripts/toon-doorbell.sh        root@toon:/root/vpu/doorbell.sh
ssh root@toon "chmod +x /root/vpu/*"
```

Module is `[permanent]` (it takes a self-reference via `class_create(THIS_MODULE,...)`),
so every driver change needs a reboot.

## Run

On the Toon:

```sh
/root/vpu/doorbell.sh
```

On the sender (Orange Pi 5, x86 box, anything with ffmpeg):

```sh
export RTSP_URL='rtsp://user:pass@CAMERA_IP:554/Streaming/Channels/101'
export TOON_HOST='192.168.x.x'
./scripts/orangepi-doorbell.sh
```

`orangepi-doorbell.sh` is the reliable software-transcode version.
`orangepi-doorbell-hw.sh` is a low-latency variant that pulls the camera's
substream at its native resolution.

Both scripts auto-reconnect; the Toon listener stays up across reconnects.

## Hardware notes

- SoC: NXP/Freescale i.MX27 (ARM926EJ-S, ARMv5).
- VPU: Chips&Media Codadx6, MPEG-4 Simple Profile + H.264 Baseline.
- Real-world max on this board: ~640×360 @ ~10 fps MPEG-4 Simple Profile.
  H.264 decode segfaults at ≥480×270 (the doorbell is High Profile, so the
  Orange Pi transcodes down to MPEG-4 for the Toon).
- Display: 800×480 RGB565, two planes (`fb0`=BG, `fb1`=FG). The video goes
  to `fb0` today; the Toon UI redraws over the centre, which is the next
  thing to fix (move video to `fb1`).

## Status

| Piece | State |
|-------|-------|
| Driver loads, ioctls work | ✅ |
| Contiguous DMA pool | ✅ |
| VPU MPEG-4 decode | ✅ |
| eMMA PrP YUV→RGB565 + resize | ✅ |
| Live RTSP doorbell pipeline | ✅ |
| Stream stability (no futex abort) | ✅ |
| Auto-reconnect on either end | ✅ |
| Toon UI not overdrawing the video | ❌ (needs FG-plane move) |
| Boot autostart | ❌ (init scripts not yet wired) |

See [`HANDOVER.md`](HANDOVER.md) for the full story.

## License

MIT for the project glue (this repo's scripts, the README, the apps).
The kernel driver in `kernel/` and the library in `lib/` derive from the
Freescale i.MX BSP and are GPLv2 / LGPLv2.1 respectively (see the headers
on individual files).
