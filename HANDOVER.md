# CLAUDE.md — VPU Driver for Toon (i.MX27 Codadx6)

## Goal
Integrate Freescale 2.6.35 BSP Codadx6 VPU driver into Toon's running 2.6.36-R10-h28 kernel. Get 480p/15fps video decode working.

## RENDER PATH (2026-05-22) — software CSC bottleneck + eMMA PrP plan
DeepSeek's fb player (vpu_dec_fb.c) outputs to /dev/fb0 (=DISP0 BG plane, 800x480 RGB565,
visible — confirmed via color-bar dd test). Per-phase timing (640x360 mpeg4): decode ~60ms,
YUV wc-copy ~20ms, **conv (YUV->RGB565) ~212ms = the bottleneck**, blit ~0ms. Root cause: a
64KB G_tab[256][256] thrashes ARM926's 16KB D-cache (~1 miss/px). Fix in vpu_dec_fb2.c: split
into Gu[256]+Gv[256] (4KB, cache-resident) + 2px/chroma -> conv 212->85ms, 2.7->5.0 fps. Also
nice(+15)+per-frame usleep so a CPU-heavy run can't lock up the thermostat (PREEMPT preempts it).
fb players whole-file-load the ES into a (now 3MB) ring buffer; H.264 baseline streams are ~2.4MB.
NOTE: H.264(AVC) decode segfaults at 480x270/640x360 in the player -- separate AVC-path bug, TBD.

### ✅ eMMA PrP hardware YUV->RGB IMPLEMENTED & WORKING (2026-05-22)
Driver: VPU_IOC_PRP_CONVERT (ioctl 22, struct vpu_prp_convert) in mxc_vpu.c does HW
YUV420->RGB565(+resize) via the eMMA PrP. Ported scaler (prp_scale*/prp_set_scaler), CSC
(studio BT.601), CH1 RGB16, IRQ 51 (prp_irq_handler + prp_queue, wait_event 200ms), clock
forced via PCCR0[27]+PCCR1[18]. ioremap PrP at vpu_init ("eMMA PrP ready"), freed at exit.
VERIFIED: prp_test 1:1 (640x368) and downscale (320x184) -> correct BBB frames (colors right).
Player vpu_dec_fb3.c uses it (allocates RGB pool buf, PRP_CONVERT(decoded YUV->RGB), blit to fb).
RESULT: 2.7fps(orig sw) -> 5.0(opt sw) -> **9.9 fps** (steady ~14fps/frame). Per frame @640x360:
decode ~61ms, **PrP CSC ~6ms (was 85ms sw, ~14x, CPU free during IRQ wait)**, blit ~5ms.
Video CONFIRMED on the Toon LCD (fb0/BG plane; thermostat UI composites on top). Decode is now
the bottleneck (~16fps@640x360); lower res for more. NOTE: src_h must be the PADDED height
(ALIGN16) so the chroma plane offset matches the VPU frame-buffer layout; blit only picHeight rows.
TODO/minor: (1) transient PRP_CONVERT -ETIME on the very FIRST call (cold start) -> warm eMMA clock
at init or bump timeout; (2) PrP can write straight to fb phys (skip the 5ms blit); (3) AVC decode
segfault at >=480x270 still open (doorbell is H.264). emma-ref/ has mx27_prphw.c/.h.

### eMMA PrP details (reference)
Goal: offload CSC to the i.MX27 eMMA PrP so the CPU is free + fast. Reference (Freescale BSP
mx27_prphw.c / mx27_prp.h) saved in ~/QB2-OSS/emma-ref/. Key facts:
- PrP base 0x10026400 (MX27_EMMA_PRP_BASE_ADDR), IRQ 51 (MX27_INT_EMMAPRP). PP is at 0x10026000.
- Clock: emma_clk = PCCR0 bit27 (parent emma_clk1 = PCCR1 bit18). clk_enable likely no-op on R10
  like vpu -> force via ccm_base: PCCR0(0x20)|=1<<27; PCCR1(0x24)|=1<<18.
- Input YUV420: PRP_SRC_PIXEL_FORMAT_CNTL, PRP_SOURCE_FRAME_SIZE=(w<<16)|h, PRP_SOURCE_LINE_STRIDE,
  PRP_SOURCE_Y/CB/CR_PTR (Cb=Y+stride*h, Cr=Cb+(stride*h)/4). PRP_CNTL_IN_YUV420=0.
- CSC (studio-range BT.601 = index1 _y2r {0x95,0xcc,0x32,0x68,0x104}, offset bit set):
  PRP_CSC_COEF_012(0x48)=0x12A66032, _345(0x4c)=0x0D082000, _678(0x50)=0x80000000.
  encoding: COEF_012=(c0<<21)|(c1<<11)|c2; _345=(c3<<21)|(c4<<11)|c5; _678=(c6<<21)|(c7<<11)|c8|(off<<31).
  (full-range index0 _y2r={0x80,0xb4,0x2c,0x5b,0xe4}, offset bit 0.)
- CH1 RGB565 out: PRP_CNTL_CH1_RGB16=bit5; PRP_CH1_PIXEL_FORMAT_CNTL, PRP_CH1_OUT_IMAGE_SIZE=(w<<16)|h,
  PRP_CH1_LINE_STRIDE=stride*2, PRP_DEST_RGB1_PTR=dst phys.
- Resize: prp_scale()/scale()/prp_scale_bilinear/ave + prp_set_scaler (PRP_CH1_RZ_HORI/VERT_*). 1:1
  must use AVG algo (BIL hangs at 1:1). Port verbatim from emma-ref/mx27_prphw.c.
- Start: PRP_CNTL = inbits|PRP_CNTL_CH1_RGB16|PRP_CNTL_CH1EN(bit0). IRQ: PRP_INTRCNTL CH1FC; ISR
  reads PRP_INTRSTATUS, clears CH1EN (silicon bug: enable doesn't self-clear), writes status back.
- New ioctl VPU_IOC_PRP_CONVERT (num 22) + struct vpu_prp_convert added to mxc_vpu.h.
- Plan: per frame decode -> PRP_CONVERT(decoded YUV pool buf -> RGB565 -> fb) -> 0 CPU in pixel path.

## ✅✅✅ HARDWARE VIDEO DECODE WORKS (2026-05-22)
`vpu_dec mpeg4 bbb.m4v`: "DECODE DONE: 300 frames decoded, 300 displayed". Decoded YUV dumped,
converted to PNG = correct Big Buck Bunny frames (keyframe + P-frames w/ motion). VPU_CODEC_IRQ
fires per frame; CUR_PC running. The i.MX27 Codadx6 VPU is decoding 640x360 MPEG-4 SP on the Toon.
Test program: /tmp/vpu_dec.c (mpeg4|avc + ES file [+out.yuv]); loads whole ES into a 2MB ring
buffer, signals EOS, decode loop with frame-buf registration from the reserved pool.
NOTE on H.264: the user's clip is H.264 HIGH profile -> Codadx6 only does H.264 BASELINE, so it
won't decode as-is. Need a Baseline-profile ES (host ffmpeg here lacks libx264; transcode elsewhere
or use the AVC path in vpu_dec with a baseline stream + psSaveBuffer/sliceSaveBuffer already wired).

## ✅✅ vpu_Init() SUCCEEDS (2026-05-22) — full VPU bring-up works
`vpu_test7` (calls `vpu_Init(NULL)` directly): "vpu_Init returned 0 SUCCESS / VPU is RUNNING".
Firmware (TO2) downloaded to the VPU via the kernel register ioctls, buffers served from the
reserved DMA pool (work/code/para at phys 0xa68xxxxx). All the blockers below are solved:
ABI (ARM_UNWIND), DMA addrs (NULL dev), clock (CCM PCCR1), register-bus privilege (REG ioctls),
firmware id (forced TO2), contiguous memory (early-boot pool), share-mem futex (vmalloc), and
the last one: `ResetVpu()` in vpu_io.c did a direct *reg_map(BIT_CODE_RESET=0x14) write ->
external abort; fixed to use VpuReadReg/VpuWriteReg (kernel ioctls).
NEXT: actual decode — open a decoder instance + frame buffers (from the pool) + feed a stream.
Housekeeping TODO: the CLKGATE handler still spams dmesg with PCCR1/CUR_PC debug prints (remove).

## ✅ SOLVED (2026-05-21) — root cause was a SINGLE kernel config option: CONFIG_ARM_UNWIND
The long-standing blocker was **NOT** in `mxc_vpu.c` and **NOT** the DMA mask. It was a
`struct module` ABI mismatch between our R07-built module and the running R10-h28 kernel,
caused entirely by **`CONFIG_ARM_UNWIND` being OFF in our `.config` while the R10-h28 kernel
has it ON**. `ARM_UNWIND` adds 9 pointers (36 bytes) to `struct mod_arch_specific` (the `arch`
field of `struct module`), shifting every following field. On load the kernel wrote module
metadata (refptr/source_list) at offsets past where our too-small struct ended → corruption,
`m_show`/`lsmod` Oops, and garbage ioctl output (`phy=0xbf13bfbc` was a pointer into the
module's own image, not a DMA address).

**How it was found:** `.gnu.linkonce.this_module` section size == `sizeof(struct module)`.
Official R10-h28 module (pulled from the Eneco feed) = **0x148**; our R07 build = **0x124**;
delta = **36 bytes = exactly ARM_UNWIND's 9 pointers**. Confirmed: hello-world built with
`CONFIG_ARM_UNWIND=y` loads cleanly, `lsmod` works, `rmmod` works. NO downgrade, NO R10
source, NO kernel rebuild needed — device stays on R10-h28 as-is.

### THE FIX (two parts, both applied):
1. **Kernel `.config`: `CONFIG_ARM_UNWIND=y`** (and `# CONFIG_FRAME_POINTER is not set` —
   they are mutually exclusive). After editing `.config`, you MUST regenerate the generated
   headers or the `M=` build won't see it:
   `make ARCH=arm CROSS_COMPILE=… silentoldconfig` (writes `include/generated/autoconf.h`
   + `include/config/auto.conf` with `CONFIG_ARM_UNWIND=1`), then rebuild the module.
   Verify: `objdump -h mxc_vpu.ko | grep this_module` → size must be **0x148**, and
   `.ARM.exidx` sections must be present.
2. **`dma_alloc_coherent(NULL, …)`** (not `vpu_dev`). `struct device` ALSO differs in layout
   between R07 and R10-h28, so writing `vpu_dev->coherent_dma_mask` lands at the wrong offset
   (kernel still saw 0 → "coherent DMA mask is unset"). The original NXP driver used `NULL`,
   which makes `get_coherent_dma_mask(NULL)` return a non-zero default. Reverted to `NULL` in
   `vpu_alloc_dma_buffer`/`vpu_free_dma_buffer`; removed the vpu_dev mask write.

### CURRENT STATUS — vpu_test4 steps 1-5 PASS:
- open, mmap regs, CLKGATE ok.
- ioctl 12 GET_SHARE_MEM: `phy=0xa6fde000 cpu=0xffb8a000 size=4096 OK` — REAL i.MX27 phys
  RAM addr (base 0xA0000000) + DMA-consistent CPU addr. The garbage-address bug is GONE.
- mmap share mem ok.
- ioctl 8 GET_WORK_ADDR (256 KB) FAILS: `Physical memory allocation error!` + page-allocator
  dump. This is the NEXT problem, not ABI: `dma_alloc_coherent` can't get a large physically
  CONTIGUOUS block (order-6+) from the fragmented page allocator (128 MB device, ~22 MB free
  but fragmented; fails even right after boot because rt5370sta wifi fragments early).

### SOLVED #2 (2026-05-22) — userspace can't reach the i.MX27 peripheral bus
After the clock was on, vpu_test5 still aborted reading a VPU register. /dev/mem probe proved
it: userspace reads of ANY peripheral addr (CCM, VPU, AIPI1/2) external-abort, while RAM and
all KERNEL accesses work => non-privileged accesses to the i.MX27 periph bus are blocked. The
whole VPU lib is built around mmap'ing registers into userspace, which is incompatible here.
Clock note: DeepSeek's `0xf4002724` was a wrong CCM addr (-> misleading PCCR1=0); the clock was
actually fine via clk_enable. Real blocker was always this bus-access privilege.

FIX (Path A, implemented + verified): proxy register access through the kernel.
- Driver: `VPU_IOC_REG_READ`/`REG_WRITE` (num 20/21, struct vpu_reg_rw{offset,value}), bounds-
  checked + word-aligned, each calls vpu_clk_force_on() (sets CCM_PCCR1 bits 6+16 via ccm_base)
  so a gated access can't oops the kernel. Clock also forced on at init and left on.
- Library `imx-lib/vpu/vpu_io.c`: VpuReadReg/VpuWriteReg now ioctl instead of *reg_map().
- Verified by vpu_test6: REG_WRITE 0x104=0xDEADBEEF, REG_READ back = 0xdeadbeef MATCH, no abort.
- vpu_test3 validation: with fresh-boot memory, IOSystemInit OK + vpu_Init reaches firmware
  download via the ioctls. Build lib: `make CROSS_COMPILE=arm-linux-gnueabi- PLATFORM=IMX27ADS
  libvpu.a` then `arm-linux-gnueabi-gcc -static -DIMX27ADS -I. -o vpu_test3 vpu_test3.c libvpu.a -lpthread`.
- IMPORTANT: the device AUTO-LOADS /lib/modules/2.6.36-R10-h28/kernel/drivers/mxc/vpu/mxc_vpu.ko
  at boot. Keep it in sync with the build (cp + depmod -a) or reboots run a stale module
  ("No such IOCTL, cmd is 22036/22037" = stale REG_READ/WRITE; "insmod: File exists").

### SOLVED #3 (2026-05-22) — firmware selection (i.MX27 TO2)
vpu_Init couldn't open the firmware: the Quby kernel reports `/proc/cpuinfo` "Revision : 0000",
so system_rev=0 -> mxc_cpu()!=0x27 -> cpu_is_mx27_rev() false -> bogus fw filename. Driver now
reads SYSCTRL CHIP_ID (CCM+0x800): `0x2882101d` part=0x8821 silicon_rev=2 => genuine i.MX27 TO2.
Lib `get_system_rev()` now forces system_rev=0x27020 (TO2) when not mx27 -> loads
/lib/firmware/vpu/vpu_fw_imx27_TO2.bin (present, 65552 bytes). (Switch to 0x27010 for TO1.)

### NEXT PHASE — contiguous DMA memory (THE remaining blocker):
buddyinfo proves the constraint: at ~75s uptime there are 13x order-10 (4 MB) free blocks; after
~12 min of Qt GUI it's fragmented to max order-4 (64 KB). So VPU buffers (work 128K, code 64K,
para 12K, and later MB-scale frame buffers) only allocate shortly after boot. vpu_Init succeeds
or fails purely on this timing. Two paths:
  (a) DRIVER-SIDE EARLY POOL (no env risk): reserve ~8-12 MB contiguous at module init (grab a
      few order-10 blocks) + simple allocator for PHYMEM_ALLOC/GET_WORK_ADDR; REQUIRES the module
      to load EARLY in boot (before the GUI fragments RAM). Module already auto-loads; ensure it's
      ordered before qt-gui. Feasible per buddyinfo. More driver code, zero device risk.
  (b) mem= CARVEOUT via u-boot env: guaranteed regardless of timing, but env (mtd0) is `ro`,
      no fw_setenv -> needs serial console (ttymxc0) to `setenv mem 112M; saveenv`, or risky
      raw write. DECISION/serial-access question still open.
NOTE: a transient bus error at offset 0x14 of a buffer mapping was seen when IOSystemInit passed
but a later (re-init) buffer alloc was bad -> likely also memory; revisit once memory is solid.

### NEXT PHASE — contiguous DMA memory for VPU work/frame buffers (LAST blocker):
480p decode needs MANY MB of contiguous DMA RAM (a 480p YUV420 frame ≈ 0.5 MB, ×several).
The page allocator can't supply that (buddyinfo: max free block = order-5/128 KB; even the
128 KB bitwork pre-alloc fails when fragmented). Standard i.MX27 solution = **boot-time
carveout**: shrink `mem=` (now 128M) to reserve a top region of 0xA0000000 RAM, driver hands
out chunks via ioremap + a simple allocator (NOT dma_declare_coherent_memory — that writes
dev->dma_mem and struct device's layout is mismatched).
BLOCKER for the carveout: the u-boot env (mtd0) is `ro` and there is no fw_setenv on the
device, so changing `mem=` from Linux is brick-risky. Options: (a) install u-boot-fw-utils
from the feed and find a way past the ro; (b) edit env via u-boot serial console (ttymxc0,
console is enabled); (c) raw mtd0 rewrite (risky). Decision pending.

### Notes:
- ABI is fixed, so the module is a NORMAL module now: `rmmod mxc_vpu` works, `lsmod` is fine,
  no more `[permanent]` / reboot-per-iteration. Iterate freely.
- Eneco VPN was started via `update-rooted.sh -o` for feed access; it closes on reboot.
- Feed facts (qb2/uni): firmware purged below 4.13.7; oldest available kernel = R10-h27;
  R07 is NOT obtainable from the feed (this is why downgrade was abandoned).

## Target Device
- **Toon thermostat** running Linux 2.6.36-R10-h28 ARMv5 PREEMPT
- i.MX27 SoC: VPU at physical 0x10023000, IRQ 53 (MX27_INT_VPU)
- Reachable at `root@toon` (SSH)
- **`/sbin/insmod`** (not in default PATH)
- **Device node must be created manually** (no udev): `mknod /dev/mxc_vpu c <major> 0`

## Repo Layout
- **Kernel source**: `/home/roy/QB2-OSS/oe/homeautomationeurope/recipes/linux/linux-quby2/linux-r07-vpu/linux_r07/`
- **Driver**: `drivers/mxc/vpu/mxc_vpu.c`
- **Kernel header with ioctl defs**: `arch/arm/plat-mxc/include/mach/mxc_vpu.h`
- **Userspace library** (i.MX27 Codadx6 — CORRECT): `/home/roy/QB2-OSS/imx-lib/vpu/`
  - `vpu_io.c/h` — ioctl wrappers, IOSystemInit, IOSystemShutdown
  - `vpu_lib.c/h` — vpu_Init/vpu_UnInit, encode/decode API
  - `vpu_util.c/h` — semaphore ops (pthread_mutex_timedlock)
- **DO NOT USE** `/home/roy/QB2-OSS/libimxvpuapi/` — i.MX6 V4L2, incompatible

## Build
- **Toolchain**: GCC 4.5.3 at `/home/roy/toolchain-build/install/bin/arm-linux-gnueabi-`
- **Host workaround**: `include/linux/compiler-gcc14.h -> compiler-gcc4.h` symlink (kernel's make prepares with host GCC 14)
- **Build** (from linux_r07/):
  ```
  make ARCH=arm CROSS_COMPILE=/home/roy/toolchain-build/install/bin/arm-linux-gnueabi- -j$(nproc) M=drivers/mxc/vpu modules
  ```
- **Userspace tests**: system GCC 14, static link against Toon glibc 2.21 sysroot at `/tmp/toon-sysroot/`:
  ```
  arm-linux-gnueabi-gcc -static -I/home/roy/QB2-OSS/imx-lib/vpu --sysroot=/tmp/toon-sysroot -o /tmp/vpu_test4 /tmp/vpu_test4.c -lpthread
  ```

## Deploy
```
scp drivers/mxc/vpu/mxc_vpu.ko root@toon:/lib/modules/2.6.36-R10-h28/kernel/drivers/mxc/vpu/mxc_vpu.ko
ssh root@toon "/sbin/depmod -a"
```
Module is NOT auto-loaded on boot — `/sbin/insmod` it manually.

## Current Blocker: ioctl 12 (GET_SHARE_MEM) returns garbage DMA addresses

### Symptom (vpu_test4 output — same after spinlock fix applied):
```
phy=0xbf13bfbc  cpu=0xc3819640  size=-1089224772  (size = 0xBF13BFBC again!)
```

### What's happening:
- `cpu_addr` (0xC3XXXXXX) looks PLAUSIBLE — kernel direct-mapped region, correct for DMA coherent CPU addr
- `phy_addr` (0xBFXXXXXX) is a **kernel stack/vmalloc address**, NOT a DMA bus address
- `size` gets overwritten to the same garbage as phy_addr → struct field corruption

### Tried fix: removed `spin_lock(&vpu_lock)` from `VPU_IOC_GET_SHARE_MEM` handler (line ~317)
- **It did NOT help** — same exact corruption pattern after reboot + fresh module load
- The fix is deployed and active (verified by dmesg showing the spinlock-less module loaded)

### New theory: `vpu_dev` from `device_create()` has NO DMA mask set
The driver uses `vpu_dev` (saved from `device_create()`) as the device pointer for `dma_alloc_coherent()`. But `device_create()` creates a device on a virtual bus with:

```
vpu_dev->dma_mask = NULL   (pointer is NULL, not just *dma_mask = 0)
vpu_dev->coherent_dma_mask = 0
```

On ARM 2.6.36's `dma_alloc_coherent`, this may trigger a fallback path that returns wrong physical addresses, or the DMA mask determines which memory zone is used. The physical address computation in `arch/arm/mm/dma-mapping.c:__dma_alloc()` uses `dev->coherent_dma_mask` to decide between DMA zones (NORMAL vs CONSISTENT).

**Fix to try**: after `device_create()`, set up the DMA mask:
```c
vpu_dev->dma_mask = &vpu_dev->coherent_dma_mask;
vpu_dev->coherent_dma_mask = DMA_BIT_MASK(32);
```
And pass `vpu_dev` (not `&vpu_dummy_pdev.dev`) — we already do this correctly since the platform_device revert.

### Alternative theory: `copy_from_user(&share_mem, ...)` picks up garbage from userspace stack
The test4 program does `memset(&share_mem_buf, 0, sizeof(...)); share_mem_buf.size = 4096;` then passes it to ioctl. But if the kernel's `copy_from_user` reads MORE than 16 bytes, or if there's a padding mismatch, the size field could be corrupted. However, given that cpu_addr looks correct, this is unlikely — the allocation itself succeeds.

### Strongest signal: cpu_addr is always correct, phy_addr is always a vmalloc pointer
This points to `dma_alloc_coherent`'s `dma_handle` output being wrong. On ARM, the `dma_handle` (physical address) is computed from the allocated page's PFN. If `dev->coherent_dma_mask` is 0/unset, the PFN-to-bus-address conversion may return a virtual address instead of a physical one.

### Side issue: lsmod crashes (NULL deref in m_show)
After the module loads, `lsmod` causes a kernel Oops in `m_show+0x80` (module procfs reader). This is triggered by `class_create(THIS_MODULE, ...)` creating bad module sysfs entries. While it doesn't affect VPU operation, it's a sign the module's device model integration is fragile. Could also be related to the missing DMA mask causing the device to be in an inconsistent state.

## What already works
1. open("/dev/mxc_vpu") → OK
2. mmap registers (offset 0) → OK (can read VPU regs)
3. ioctl 7 (CLKGATE_SETTING) → OK (NULL check for missing vpu_clk works)
4. ioctl 8 (GET_WORK_ADDR) — NOT YET TESTED but doesn't hold spinlock
5. ioctl 0 (PHYMEM_ALLOC) — NOT YET TESTED but doesn't hold spinlock
6. ioctl 5 (VL2CC_FLUSH) → implemented, flushes cache

## Struct Layout (verified — no mismatch)
- **Kernel** `vpu_mem_desc`: `{u32 size; dma_addr_t phy_addr; u32 cpu_addr; u32 virt_uaddr;}` — 16 bytes
- **dma_addr_t on ARM**: `typedef u32 dma_addr_t` in `arch/arm/include/asm/types.h`
- **Userspace**: `{int size; unsigned long phy_addr; unsigned long cpu_addr; unsigned long virt_uaddr;}` — 16 bytes
- Same field order, same sizes, no padding on ARM32.

## IOCTL Map
| Num | Name | Notes |
|-----|------|-------|
| 0 | PHYMEM_ALLOC | No spinlock, untested |
| 1 | PHYMEM_FREE | |
| 5 | VL2CC_FLUSH | Added, flush_cache_all() |
| 7 | CLKGATE_SETTING | Fixed NULL clk check |
| 8 | GET_WORK_ADDR | No spinlock, untested |
| 9 | REQ_VSHARE_MEM | vmalloc-based, no DMA issue |
| 10 | GET_PIC_PARA_ADDR | NOT in kernel |
| 12 | GET_SHARE_MEM | **BUG: garbage phy_addr+size** |

## Kernel/Hardware Notes
- **No vpu_clk on Toon** — expected warning in dmesg, safe to continue
- **i.MX27 has NO IRAM** — library checks `cpu_is_mx37()` before IRAM usage
- **Firmware** (later): `vpu_fw_imx27_TO{1,2}.bin` (65552 bytes) in `/lib/firmware/vpu/`
- **Module is [permanent]** — `class_create(THIS_MODULE, ...)` holds module ref. Can't rmmod. Every fix needs reboot.

## Drivers Changed (mxc_vpu.c diff from original BSP)
1. Platform bus completely bypassed — `vpu_init()` directly ioremap's MX27_VPU_BASE_ADDR, register_chrdev, class_create, device_create, request_irq
2. `clk_get("vpu_clk")` made optional (NULL check before clk_enable/disable)
3. `VPU_IOC_VL2CC_FLUSH` (ioctl 5) added with `flush_cache_all()`
4. DMA device: `vpu_dev` from `device_create()` stored globally, passed to dma_alloc/free_coherent
5. `VPU_IOC_GET_SHARE_MEM`: spinlock removed (committed but didn't fix the bug)
6. Duplicate `request_irq` removed from vpu_init
7. `platform_device_register(NULL)` crash: reverted — caused `platform_device_add+0x98` NULL deref

## Verification After Fix
1. insmod → dmesg shows "VPU initialized (major=%d)" → mknod /dev/mxc_vpu
2. Run vpu_test4 → all 7 steps pass (especially step 4 returns valid phy_addr, step 5 mmap succeeds)
3. Run vpu_test3 (full vpu_Init/vpu_UnInit via library)
4. Place firmware, test decode
