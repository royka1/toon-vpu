/*
 * fb1_overlay_test.c — light up the i.MX27 LCDC graphic-window overlay (fb1).
 *
 * fb1 is NOT scanned out until the graphic window is enabled via the
 * FBIOPUT_GWINFO ioctl. This fills fb1 with a recognizable pattern, enables
 * the overlay opaquely over fb0, holds until Ctrl-C, then disables cleanly.
 *
 * Build (with the Toon toolchain):
 *   /tmp/qt_rebuild/toon1-toolchain/bin/arm-linux-gcc \
 *     --sysroot=/tmp/qt_rebuild/toon1-toolchain/arm-buildroot-linux-gnueabi/sysroot \
 *     -march=armv5te -marm -mfloat-abi=soft -O2 -static \
 *     -o fb1_overlay_test fb1_overlay_test.c
 *
 * Run on Toon:
 *   ./fb1_overlay_test            # full-screen overlay, alpha 255
 *   ./fb1_overlay_test 200 100 128  # xpos=200 ypos=100 alpha=128 (half-transparent)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>

/* From arch/arm/plat-mxc/include/mach/mx2fb.h — defined here so we don't
 * need the kernel headers to build. Field order MUST match the kernel. */
struct fb_gwinfo {
	uint32_t enabled;
	uint32_t alpha_value;
	uint32_t ck_enabled;
	uint32_t ck_red, ck_green, ck_blue;
	uint32_t xpos, ypos;
	uint32_t vs_reversed;
	uint32_t base;          /* ignored by FBIOPUT_GWINFO (driver fills it) */
	uint32_t xres, yres;    /* ignored by FBIOPUT_GWINFO (driver fills it) */
	uint32_t xres_virtual;  /* ignored by FBIOPUT_GWINFO (driver fills it) */
};
#define FBIOGET_GWINFO  0x46E0
#define FBIOPUT_GWINFO  0x46E1

static int fbfd = -1;

static void disable_and_exit(int sig)
{
	(void)sig;
	if (fbfd >= 0) {
		struct fb_gwinfo gw;
		memset(&gw, 0, sizeof gw);
		gw.enabled = 0;
		ioctl(fbfd, FBIOPUT_GWINFO, &gw);
	}
	printf("\noverlay disabled; bye\n");
	_exit(0);
}

int main(int argc, char **argv)
{
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	int xpos = 0, ypos = 0, alpha = 255;

	/* Unbuffered: this program then blocks in pause() forever, so any
	 * block-buffered stdout (ssh/pipe) would never flush and you'd see
	 * no output at all even while it runs. */
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	/* Args, any order: "sweep" (alpha ramp), "16"/"32" (force fb1 bpp),
	 * "WxH" (resize fb1, e.g. 768x432), and up to 3 bare numbers =
	 * xpos ypos alpha. */
	int want_bpp = 0, do_sweep = 0, npos = 0, ai;
	int force_w = 0, force_h = 0;
	for (ai = 1; ai < argc; ai++) {
		int tw, th;
		if (!strcmp(argv[ai], "sweep")) { do_sweep = 1; continue; }
		if (!strcmp(argv[ai], "16"))    { want_bpp = 16; continue; }
		if (!strcmp(argv[ai], "32"))    { want_bpp = 32; continue; }
		if (sscanf(argv[ai], "%dx%d", &tw, &th) == 2) {
			force_w = tw; force_h = th; continue;
		}
		if (npos == 0) xpos = atoi(argv[ai]);
		else if (npos == 1) ypos = atoi(argv[ai]);
		else if (npos == 2) alpha = atoi(argv[ai]);
		npos++;
	}

	fbfd = open("/dev/fb1", O_RDWR);
	if (fbfd < 0) { perror("open /dev/fb1"); return 1; }

	if (ioctl(fbfd, FBIOGET_VSCREENINFO, &var) < 0) { perror("GET_VSCREENINFO"); return 1; }

	/* Force a pixel depth and/or size if asked. The mx2fb driver was
	 * patched to default_bpp=16, so the graphic window fetches 16bpp;
	 * the buffer must match. RGB565 bitfields for 16bpp. */
	if (want_bpp || force_w) {
		if (!want_bpp) want_bpp = var.bits_per_pixel;
		if (force_w) { var.xres = var.xres_virtual = force_w;
		               var.yres = var.yres_virtual = force_h; }
		var.bits_per_pixel = want_bpp;
		if (want_bpp == 16) {
			var.red.offset   = 11; var.red.length   = 5;
			var.green.offset = 5;  var.green.length = 6;
			var.blue.offset  = 0;  var.blue.length  = 5;
			var.transp.offset = 0; var.transp.length = 0;
		} else { /* 32bpp xRGB8888 */
			var.red.offset   = 16; var.red.length   = 8;
			var.green.offset = 8;  var.green.length = 8;
			var.blue.offset  = 0;  var.blue.length  = 8;
			var.transp.offset = 24; var.transp.length = 8;
		}
		var.activate = FB_ACTIVATE_NOW;
		if (ioctl(fbfd, FBIOPUT_VSCREENINFO, &var) < 0)
			perror("FBIOPUT_VSCREENINFO (set bpp, non-fatal)");
		/* re-read so line_length / smem reflect the new depth */
		ioctl(fbfd, FBIOGET_VSCREENINFO, &var);
	}

	if (ioctl(fbfd, FBIOGET_FSCREENINFO, &fix) < 0) { perror("GET_FSCREENINFO"); return 1; }

	printf("fb1: %ux%u (virt %ux%u) %u bpp | smem_start=0x%lx smem_len=%u line=%u\n",
	       var.xres, var.yres, var.xres_virtual, var.yres_virtual,
	       var.bits_per_pixel, (unsigned long)fix.smem_start,
	       fix.smem_len, fix.line_length);

	if (fix.smem_len == 0) {
		fprintf(stderr, "fb1 has no backing memory (smem_len=0). The overlay "
		        "buffer isn't allocated -- try FBIOPUT_VSCREENINFO first, or "
		        "the driver needs an alloc patch.\n");
		return 1;
	}

	unsigned char *p = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE,
	                        MAP_SHARED, fbfd, 0);
	if (p == MAP_FAILED) { perror("mmap"); return 1; }

	/* Fill with a recognizable pattern: magenta field + white diagonal,
	 * so it's unmistakable vs garbage or the UI underneath. */
	int bpp = var.bits_per_pixel;
	for (unsigned y = 0; y < var.yres; y++) {
		unsigned char *row = p + (size_t)y * fix.line_length;
		for (unsigned x = 0; x < var.xres; x++) {
			int diag = (x == y) || (x == y + 1) || (x + 1 == y);
			if (bpp == 32) {
				uint32_t px = diag ? 0x00FFFFFF : 0x00FF00FF; /* white : magenta */
				((uint32_t *)row)[x] = px;
			} else if (bpp == 16) {
				uint16_t px = diag ? 0xFFFF : 0xF81F;          /* white : magenta */
				((uint16_t *)row)[x] = px;
			}
		}
	}

	signal(SIGINT, disable_and_exit);
	signal(SIGTERM, disable_and_exit);

	/* Enable the graphic window. base/xres/yres are filled by the driver
	 * from fb1's own fix.smem_start + var, so we leave them 0 here. */
	struct fb_gwinfo gw;
	memset(&gw, 0, sizeof gw);
	gw.enabled     = 1;
	gw.alpha_value = (uint32_t)alpha;   /* 255 = opaque over fb0 */
	gw.ck_enabled  = 0;                 /* no color key */
	gw.xpos        = (uint32_t)xpos;
	gw.ypos        = (uint32_t)ypos;
	gw.vs_reversed = 0;

	if (ioctl(fbfd, FBIOPUT_GWINFO, &gw) < 0) {
		perror("FBIOPUT_GWINFO (enable)");
		return 1;
	}
	/* Some BG/FG drivers don't start the FG DMA until a pan/flip. Kick it. */
	var.activate = FB_ACTIVATE_NOW;
	var.yoffset = 0;
	if (ioctl(fbfd, FBIOPAN_DISPLAY, &var) < 0)
		perror("FBIOPAN_DISPLAY (non-fatal)");

	printf("overlay ENABLED at (%d,%d) alpha=%d, size from fb1 var %ux%u.\n"
	       "You should see a magenta field with a white diagonal over the UI.\n"
	       "Ctrl-C to disable.\n", xpos, ypos, alpha, var.xres, var.yres);

	/* Sweep mode: ramp alpha 255->0->255 so you can SEE whether the FG
	 * plane composites at all. If the screen changes during the sweep,
	 * the overlay works and it's just a value/position issue. If it never
	 * changes, the GW DMA isn't scanning (deeper: Z-order/cache/refresh). */
	if (do_sweep) {
		int a, dir = -1;
		printf("alpha sweep (watch the screen)...\n");
		for (a = 255; ; a += dir * 15) {
			if (a <= 0)   { a = 0;   dir = 1; }
			if (a >= 255) { a = 255; dir = -1; }
			gw.alpha_value = (uint32_t)a;
			ioctl(fbfd, FBIOPUT_GWINFO, &gw);
			printf("alpha=%d\r", a);
			usleep(120000);
		}
	}

	for (;;) pause();
	return 0;
}
