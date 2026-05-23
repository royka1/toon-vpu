# VPU firmware blobs (Freescale / NXP)

The two files in this directory are the i.MX27 Codadx6 VPU firmware images
required by the in-kernel driver:

| File | Size | SHA256 | Used on |
|------|------|--------|---------|
| `vpu_fw_imx27_TO1.bin` | 65 552 B | `f30d3a20…cad858` | i.MX27 TO1 silicon |
| `vpu_fw_imx27_TO2.bin` | 65 552 B | `48861403…0786b` | i.MX27 TO2 silicon (**the Toon**) |

Each file starts with an 8-byte `MX27TOx\0` magic followed by the VPU's
internal ARM microcode (loaded by the kernel driver into the bitwork
buffer at VPU init time).

## Provenance

These blobs originate from the **Freescale / NXP `firmware-imx`** package
(the i.MX BSP firmware bundle). They have been redistributed essentially
unchanged for over a decade by:

- The official Yocto / OpenEmbedded `meta-freescale` layer
  (`recipes-bsp/firmware-imx/`).
- The OpenEmbedded recipe that originally shipped on the Toon itself —
  the copy in this directory is bit-identical to the one already present
  on every rooted Toon at `/lib/firmware/vpu/`.
- Various i.MX27 community projects and downstream distributions.

## License

The firmware is proprietary to Freescale / NXP and is distributed under
the **Freescale Semiconductor Software License Agreement** that ships
with `firmware-imx` (look for `EULA` / `EULA.txt` in any
`firmware-imx-*` tarball, e.g.
<https://www.nxp.com/lgfiles/NMG/MAD/YOCTO/firmware-imx-8.27.bin>).

In short, the EULA grants a royalty-free, non-exclusive license to use
and redistribute the firmware **solely in connection with NXP / Freescale
i.MX hardware**. Bundling it here for use on the i.MX27-based Toon falls
squarely inside that grant.

No source code is available — this is a binary microcode image, the same
way Linux's own `linux-firmware` tree ships hundreds of vendor blobs.

If you object to the inclusion of a proprietary binary in this repo,
delete this directory after cloning and pull the same files from any
i.MX27 BSP yourself; the SHA256s above let you verify you got the right
bytes.
