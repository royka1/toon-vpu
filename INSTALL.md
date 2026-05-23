# Installing on a Toon

This walks through dropping the prebuilt artifacts in `prebuilt/` onto a
rooted Toon and getting the doorbell display running. **Read the kernel
version section first — using the wrong module will crash the kernel.**

---

## ⚠️ Kernel version: this MUST match exactly

The prebuilt module in `prebuilt/mxc_vpu.ko` is compiled against, and only
ABI-compatible with, the Toon kernel:

```
Linux 2.6.36-R10-h28 #1 PREEMPT  armv5tejl
gcc 4.5.3 (CodeSourcery cross)
CONFIG_ARM_UNWIND=y     # critical — see below
```

### Why this matters

The Linux 2.6 module loader reads metadata at offsets inside `struct module`
when you `insmod` a `.ko`. The layout of that struct is determined by the
kernel's `.config`, not just its version string. On i.MX27 / ARM, the key
config is **`CONFIG_ARM_UNWIND`**:

| `CONFIG_ARM_UNWIND` | `sizeof(struct module)` | `.gnu.linkonce.this_module` |
|---------------------|-------------------------|------------------------------|
| `=n` (off)          | 0x124                   | 0x124                        |
| `=y` (on)           | 0x148                   | 0x148                        |

The Toon's 2.6.36-R10-h28 kernel has `CONFIG_ARM_UNWIND=y` → expects
**0x148**. A module built with `CONFIG_ARM_UNWIND` off ends up 36 bytes
too small, and on `insmod` the kernel writes module bookkeeping past the
end of the struct → silent corruption, `lsmod` Oops, garbage ioctl output.
This burned a lot of hours before being diagnosed (see `HANDOVER.md`).

### Verify before installing

**On the Toon — confirm the kernel:**

```sh
uname -srvm
# expected: Linux 2.6.36-R10-h28 #1 PREEMPT ... armv5tejl
```

If `uname -r` says anything other than `2.6.36-R10-h28`, **do not use the
prebuilt `.ko`**. Rebuild from `kernel/` against your exact kernel tree
(see [`README.md`](README.md#kernel-module)).

**On your build host — confirm the ABI marker in the .ko:**

```sh
arm-linux-gnueabi-objdump -h prebuilt/mxc_vpu.ko | grep this_module
#  14 .gnu.linkonce.this_module 00000148  ...     ← must be 00000148
```

If it says `00000124`, the toolchain didn't see `CONFIG_ARM_UNWIND=y`.
Regenerate the kernel's autoconf headers (`make ARCH=arm
CROSS_COMPILE=… silentoldconfig`) and rebuild the module.

### Verify the artifacts you're about to install

```sh
cd prebuilt && sha256sum -c SHA256SUMS
```

Expected:

```
f30d3a2069b59670c08f64e68363f36c1f32a1a222de3d7e018d6c036acad858  ./firmware/vpu_fw_imx27_TO1.bin
48861403fb5246e1f5dc48d01ea601a2e06b7857b10fcb8612ff9b470470786b  ./firmware/vpu_fw_imx27_TO2.bin
64ff0073b55c8f2f65ad573a9b4221145cc33991559b2f63d1168f7b2cc5f933  ./libvpu.a
499b6e4d62b1300cfb1acb0f005628ea8f0ac668d9cae16a28ce7f275d82e800  ./mxc_vpu.ko
511b53add846a12c4e6b7c71ff893162f3d044eb17f1471c177e17885c7186aa  ./vpu_dec_fb3
989a55b9922be131002cb11037aab4dc95e836b99767415172c417ce2725e75d  ./vpu_stream
```

---

## Install

All steps assume you can `ssh root@<toon>` and that the Toon is rooted.
`<toon>` is whatever address resolves for you (`toon`, an IP, ...).

### 1. VPU firmware (bundled in `prebuilt/firmware/`)

The VPU needs a Freescale firmware blob at `/lib/firmware/vpu/`. Both
silicon revisions are bundled here:

```
prebuilt/firmware/vpu_fw_imx27_TO1.bin     (65552 bytes — TO1 silicon)
prebuilt/firmware/vpu_fw_imx27_TO2.bin     (65552 bytes — TO2 silicon; the Toon)
```

See [`prebuilt/firmware/README.md`](prebuilt/firmware/README.md) for the
provenance and license (Freescale `firmware-imx` EULA — redistributable
for use on i.MX hardware).

Copy them onto the Toon:

```sh
ssh root@<toon> 'mkdir -p /lib/firmware/vpu'
scp prebuilt/firmware/vpu_fw_imx27_TO{1,2}.bin root@<toon>:/lib/firmware/vpu/
ssh root@<toon> 'ls -la /lib/firmware/vpu/'
# expect both .bin files, 65552 bytes each
```

(Most Toons already have these from the factory image; copying is harmless
since the SHA256s match.)

### 2. Back up the existing module

There is usually a stock (broken) `mxc_vpu.ko` already in place. Back it
up so you can revert:

```sh
ssh root@<toon> '
    cd /lib/modules/2.6.36-R10-h28/kernel/drivers/mxc/vpu/
    [ -f mxc_vpu.ko ] && cp mxc_vpu.ko mxc_vpu.ko.orig
'
```

### 3. Copy the prebuilt files onto the Toon

```sh
# kernel module
scp prebuilt/mxc_vpu.ko \
    root@<toon>:/lib/modules/2.6.36-R10-h28/kernel/drivers/mxc/vpu/mxc_vpu.ko

# userspace
ssh root@<toon> 'mkdir -p /root/vpu'
scp prebuilt/vpu_stream prebuilt/vpu_dec_fb3 root@<toon>:/root/vpu/

# helper scripts
scp scripts/toon-doorbell.sh root@<toon>:/root/vpu/doorbell.sh

ssh root@<toon> 'chmod +x /root/vpu/vpu_stream /root/vpu/vpu_dec_fb3 /root/vpu/doorbell.sh'
```

### 4. Load the module

The Toon's installer typically autoloads modules under `/lib/modules/$(uname -r)/`
once `depmod` knows about them:

```sh
ssh root@<toon> '
    /sbin/depmod -a
    # if it is not already loaded:
    grep -q mxc_vpu /proc/modules || /sbin/insmod \
        /lib/modules/2.6.36-R10-h28/kernel/drivers/mxc/vpu/mxc_vpu.ko
    dmesg | tail -15
'
```

In `dmesg` you should see lines like:

```
VPU registered as char major <NNN>
mxc_vpu: eMMA PrP ready
mxc_vpu: DMA pool reserved 12 MB
```

### 5. Create the device node (no udev on the Toon)

`toon-doorbell.sh` does this automatically, but to verify by hand:

```sh
ssh root@<toon> '
    major=$(awk "/mxc_vpu/{print \$1}" /proc/devices)
    echo "major=$major"
    [ -e /dev/mxc_vpu ] || mknod /dev/mxc_vpu c $major 0
    ls -l /dev/mxc_vpu
'
```

### 6. Run the doorbell pipeline

On the Toon:

```sh
ssh root@<toon> '/root/vpu/doorbell.sh'
```

You should see `Toon ready. Waiting for stream on tcp/5000...` and the
process stays foregrounded (it loops on `accept()` so it survives sender
reconnects).

On the sender (Orange Pi 5 or any Linux host with `ffmpeg`):

```sh
export RTSP_URL='rtsp://USER:PASS@CAMERA_IP:554/Streaming/Channels/101'
export TOON_HOST='<toon-ip>'
./scripts/orangepi-doorbell.sh
```

Within a few seconds the camera feed should appear on the Toon display.

---

## Reverting

```sh
ssh root@<toon> '
    /sbin/rmmod mxc_vpu 2>/dev/null
    cd /lib/modules/2.6.36-R10-h28/kernel/drivers/mxc/vpu/
    [ -f mxc_vpu.ko.orig ] && mv mxc_vpu.ko.orig mxc_vpu.ko
    /sbin/depmod -a
'
```

Reboot the Toon and you're back to stock.

---

## Troubleshooting

| Symptom | Likely cause |
|---------|-------------|
| `insmod: cannot insert ... -1 Invalid module format` | Wrong kernel version OR `.ko` was built with `CONFIG_ARM_UNWIND=n`. Re-check both. |
| `lsmod` Oops, garbage ioctl outputs | Same as above — `struct module` ABI mismatch. The 0x148 vs 0x124 check catches this. |
| `vpu_Init` fails with "firmware open failed" | Firmware blob missing at `/lib/firmware/vpu/vpu_fw_imx27_TO2.bin`. |
| `vpu_stream` aborts after a few minutes | You're using a stale `mxc_vpu.ko` from before the futex / share-mem fix. Reinstall from `prebuilt/`. |
| Stream connects but the picture is wrong colors | Sender isn't doing `in_range=pc:out_range=tv` + `colormatrix=bt709:bt601` — use the scripts in `scripts/` as-is. |

---

For the full backstory on why each of these workarounds exists, see
[`HANDOVER.md`](HANDOVER.md).
