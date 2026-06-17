#define _GNU_SOURCE
/* Live MPEG-4 stream player for the Toon: receives a raw MPEG-4 elementary
 * stream over TCP, decodes on the VPU, converts YUV->RGB565 on eMMA PP/PrP,
 * and blits to the framebuffer. Auto-recovers: loops on accept() so the
 * Orange Pi can disconnect/reconnect without restarting this.
 *
 *   Orange Pi 5:  ffmpeg rtsp:// -> scale 320x240 -> mpeg4 -> tcp://toon:PORT
 *   Toon:         vpu_stream PORT
 *
 * Usage: vpu_stream [options] [tcp-port]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/resource.h>
#include <linux/fb.h>
#include <sys/time.h>
#include <poll.h>
#include <pthread.h>
#include "vpu_lib.h"
#include "vpu_io.h"

#ifndef FIONREAD
#include <sys/ioctl.h>
#endif

static long now_ms(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec * 1000L + tv.tv_usec / 1000L;
}

#define STREAM_BUF_SIZE   (1 * 1024 * 1024)
#define DRAIN_BUF_SIZE    (256 * 1024)
#define FEED_CHUNK_SIZE   (32 * 1024)
#define BS_LOW_WATER      (128 * 1024)
#define NO_FRAME_TIMEOUT_MS 8000
#define KEYFRAME_TIMEOUT_MS 15000
#define INPUT_STARVE_TIMEOUT_MS 5000
#define H264_LATENCY_RESTART_MS 2500
#define H264_BS_LATENCY_WATER  (512 * 1024)
#define H264_SOCK_LATENCY_WATER (24 * 1024)
#define SOCK_DATA_TIMEOUT_MS    15000
#define ALIGN16(x)        (((x) + 15) & ~15)

struct vpu_prp_convert {
	unsigned int src_y, src_w, src_h, src_stride;
	unsigned int dst_rgb, dst_w, dst_h, dst_stride;
	unsigned int dst_bpp;
	unsigned int in_fmt;	/* 0=YUV420, 0x2CA00565=RGB565 */
};
#define VPU_IOC_PRP_CONVERT  _IO('V', 22)

struct vpu_pp_convert {
	unsigned int src_y, src_w, src_h, src_stride;
	unsigned int dst_rgb, dst_w, dst_h, dst_stride;
	unsigned int dst_bpp;
	unsigned int src_qp;	/* physical addr of QP buffer for deblock; 0=off */
};
#define VPU_IOC_PP_CONVERT   _IO('V', 23)

/* ---- fb1 (DISP0 FG) hardware graphic-window overlay (--overlay) ----
 * The i.MX27 LCDC composites this FG plane over the BG plane (fb0, the UI)
 * in hardware -- no fbdev cutout needed. fb1 MUST be 16bpp (mx2fb driver is
 * patched to default_bpp=16), the GW window size = fb1's var size, and the
 * on-screen position comes from xpos/ypos. Both planes must be unblanked
 * (the UI keeps fb0 on; we unblank fb1). */
struct fb_gwinfo {                 /* mach/mx2fb.h -- field order matters */
	unsigned int enabled, alpha_value, ck_enabled, ck_red, ck_green, ck_blue;
	unsigned int xpos, ypos, vs_reversed, base, xres, yres, xres_virtual;
};
#define FBIOPUT_GWINFO 0x46E1
/* Color-key for the windowed overlay: the FG plane is full-screen, the
 * video fills only its rect, and the surround is painted with this key
 * colour. The GW chroma-keys it -> those pixels are transparent and the
 * UI (fb0) shows through, giving a windowed video with no FG resize.
 * Full green (RGB565 0x07E0): the G channel is 6-bit in both 565 and the
 * GW key register, so it matches exactly (no 5->6 expansion guesswork),
 * and saturated green is essentially absent from real video (no speckle). */
#define OV_KEY565   0x07E0u
#define OV_KEY_R6   0
#define OV_KEY_G6   0x3F
#define OV_KEY_B6   0
static int g_overlay = 0;
static int ov_fd = -1;             /* /dev/fb1 */
static int ov_gw_on = 0, ov_xpos = 0, ov_ypos = 0;
static void gw_set(int on)
{
	struct fb_gwinfo gw;
	if (ov_fd < 0) return;
	/* Unblank the FG plane only when showing; the GWE bit (FBIOPUT_GWINFO
	 * enabled) is the master switch but the plane must also be unblanked
	 * to composite. Don't unblank at setup or a black (just-memset) fb1
	 * would appear over the UI before the first show. */
	/* Show: unblank the FG + set the GW-enable bit. Hide: clear the
	 * GW-enable bit only (GWE=0 removes the FG overlay; we deliberately
	 * do NOT power-blank fb1 -- blanking a plane on this LCDC can white
	 * out the whole panel). gw_set(0) at startup also clears any GWE left
	 * enabled by a previous run (the real cause of the black-at-open). */
	if (on) ioctl(ov_fd, FBIOBLANK, FB_BLANK_UNBLANK);
	memset(&gw, 0, sizeof gw);
	gw.enabled = on ? 1 : 0;
	gw.alpha_value = 255;          /* video opaque... */
	gw.ck_enabled = 1;             /* ...except key-coloured border -> UI */
	gw.ck_red = OV_KEY_R6; gw.ck_green = OV_KEY_G6; gw.ck_blue = OV_KEY_B6;
	gw.xpos = ov_xpos;
	gw.ypos = ov_ypos;
	ioctl(ov_fd, FBIOPUT_GWINFO, &gw);
	ov_gw_on = on;
}

/* Paint the whole FG plane with the color-key (transparent) colour, so
 * everywhere the video isn't drawn shows the UI through the chroma-key. */
static void ov_keyfill(unsigned char *map, int stride, int w, int h)
{
	int x, y;
	if (!map) return;
	for (y = 0; y < h; y++) {
		unsigned short *row = (unsigned short *)(map + (long)y * stride);
		for (x = 0; x < w; x++) row[x] = OV_KEY565;
	}
}

struct vpu_pp_csc {
	unsigned int c0, c1, c2, c3, c4, x0;
};
#define VPU_IOC_PP_SET_CSC   _IO('V', 24)
#define VPU_IOC_PP_GET_CSC   _IO('V', 25)

enum output_mode {
	OUTPUT_FB = 0,
	OUTPUT_DECODE_ONLY,
	OUTPUT_PP_DUMMY,
};

static int g_cpu_render = 0;
static enum output_mode g_output_mode = OUTPUT_FB;
static int g_prebuffer = 0;   /* --prebuffer KB: poll-wait when buffer below threshold */
static int g_pp_deblock = 0;  /* --pp-deblock: enable PP post-deblocking filter */

struct render_state {
	int prp_fd;
	vpu_mem_desc *fbmem;
	vpu_mem_desc *rgbmem;
	unsigned int src_w, src_h, aligned_h;
	int stride;
	int fb_bpp, fb_stride;
	unsigned long fb_phys;
	int off_x, off_y, out_w, out_h;
	unsigned char *fb_map;
	int fb_r_off, fb_g_off, fb_b_off;
	int fb_r_len, fb_g_len, fb_b_len;
	int blit_bytes;
	unsigned int *row_scratch;
	int *pp_ok;
	int dummy_rgb;
	unsigned int qp_phys;	/* PP deblock: QP buffer phys addr; 0=off */
};

static const char *output_mode_name(void)
{
	switch (g_output_mode) {
	case OUTPUT_DECODE_ONLY: return "decode-only";
	case OUTPUT_PP_DUMMY:    return "pp-dummy";
	case OUTPUT_FB:
	default:                 return "fb";
	}
}

static unsigned char clip8(int v)
{
	if (v < 0) return 0;
	if (v > 255) return 255;
	return (unsigned char)v;
}

static int render_frame_cpu(struct render_state *r, int idx,
			    long *ms_csc, long *ms_blit)
{
	unsigned char *y, *u, *v;
	int cstride, row, col;
	long t0 = now_ms(), t1;

	if (!r->fbmem[idx].virt_uaddr || r->fb_bpp != 16)
		return -EINVAL;

	y = (unsigned char *)r->fbmem[idx].virt_uaddr;
	cstride = r->stride / 2;
	u = y + r->stride * r->aligned_h;
	v = u + cstride * (r->aligned_h / 2);

	for (row = 0; row < r->out_h; row++) {
		int sy = (row * (int)r->src_h) / r->out_h;
		unsigned short *dst = (unsigned short *)
			(r->fb_map + (r->off_y + row) * r->fb_stride + r->off_x * 2);

		for (col = 0; col < r->out_w; col++) {
			int sx = (col * (int)r->src_w) / r->out_w;
			int yy = y[sy * r->stride + sx];
			int uu = u[(sy / 2) * cstride + (sx / 2)] - 128;
			int vv = v[(sy / 2) * cstride + (sx / 2)] - 128;
			int rr = yy + ((1436 * vv) >> 10);
			int gg = yy - ((352 * uu + 731 * vv) >> 10);
			int bb = yy + ((1815 * uu) >> 10);
			unsigned char r8 = clip8(rr);
			unsigned char g8 = clip8(gg);
			unsigned char b8 = clip8(bb);

			dst[col] = ((r8 & 0xf8) << 8) |
			           ((g8 & 0xfc) << 3) |
			           (b8 >> 3);
		}
	}
	t1 = now_ms();
	*ms_csc += t1 - t0;
	*ms_blit += 0;
	return 0;
}

static int render_frame(struct render_state *r, int idx, int allow_pp,
			int allow_prp, int disable_pp_on_fail,
			long *ms_csc, long *ms_blit,
			long *pp_window, long *prp_window)
{
	struct vpu_prp_convert c;
	long t0, t1, t2;
	int cvt_rc = -1;
	int used_pp = 0, used_prp = 0;
	int try_pp;

	if (g_cpu_render)
		return render_frame_cpu(r, idx, ms_csc, ms_blit);

	memset(&c, 0, sizeof(c));

	c.src_y = r->fbmem[idx].phy_addr;
	c.src_w = r->src_w;
	c.src_h = r->aligned_h;
	c.src_stride = r->stride;
	c.dst_w = r->out_w;
	c.dst_h = r->out_h;
	int fb_direct = 0;

	if (!r->dummy_rgb && r->fb_bpp == 16) {
		c.dst_bpp = 16;
		c.dst_rgb = r->fb_phys + (unsigned)r->off_y * r->fb_stride
		                      + (unsigned)r->off_x * 2;
		c.dst_stride = r->fb_stride;
		fb_direct = 1;
	} else if (!r->dummy_rgb && r->fb_bpp == 32) {
		c.dst_bpp = 32;
		c.dst_rgb = r->fb_phys + (unsigned)r->off_y * r->fb_stride
		                      + (unsigned)r->off_x * 4;
		c.dst_stride = r->fb_stride;
		fb_direct = 1;
	} else {
		c.dst_bpp = 16;
		c.dst_rgb = r->rgbmem->phy_addr;
		c.dst_stride = r->out_w * 2;
	}

	t0 = now_ms();
	try_pp = allow_pp && *r->pp_ok;

try_convert:
	if (try_pp) {
		struct vpu_pp_convert p;

		memset(&p, 0, sizeof(p));
		p.src_y = c.src_y;
		p.src_w = c.src_w;
		p.src_h = r->src_h;
		p.src_stride = c.src_stride;
		p.dst_rgb = c.dst_rgb;
		p.dst_w = c.dst_w;
		p.dst_h = c.dst_h;
		p.dst_stride = c.dst_stride;
		p.dst_bpp = c.dst_bpp;
		p.src_qp = r->qp_phys;
		cvt_rc = ioctl(r->prp_fd, VPU_IOC_PP_CONVERT, &p);
		if (cvt_rc == 0) {
			used_pp = 1;
		} else if (disable_pp_on_fail) {
			int pp_errno = errno;

			*r->pp_ok = 0;
			printf("eMMA PP unavailable for this mode (errno=%d)%s\n",
			       pp_errno,
			       allow_prp ? "; falling back to PrP" : "; no PrP fallback");
			fflush(stdout);
		}
	}

	if (cvt_rc < 0 && allow_prp) {
		cvt_rc = ioctl(r->prp_fd, VPU_IOC_PRP_CONVERT, &c);
		if (cvt_rc == 0)
			used_prp = 1;
	}

	/* If the zero-copy 32bpp path failed (e.g. upscaling), fall back to
	 * PP/PrP 16bpp conversion into the intermediate RGB565 buffer, then
	 * CPU-blit the result into the 32bpp framebuffer. */
	if (cvt_rc < 0 && fb_direct && r->fb_bpp == 32 && !r->dummy_rgb) {
		c.dst_bpp = 16;
		c.dst_rgb = r->rgbmem->phy_addr;
		c.dst_stride = r->out_w * 2;
		fb_direct = 0;
		/* PP 16bpp to intermediate buffer is known-good; always
		 * try PP first, fall back to PrP only if PP also fails. */
		try_pp = 1;
		goto try_convert;
	}

	t1 = now_ms();

	if (cvt_rc < 0)
		return -errno;

	*pp_window += used_pp;
	*prp_window += used_prp;

	if (r->dummy_rgb) {
		*ms_blit += 0;
		return 0;
	}

	if (r->fb_bpp == 32 && !fb_direct) {
		/* Try PrP second pass: RGB565 intermediate -> Toon32 fb.
		 * The PP already resized YUV->RGB565 into the intermediate
		 * buffer; PrP does 1:1 pixel-format conversion in hardware,
		 * avoiding the ~72ms CPU blit below. */
		if (r->prp_fd >= 0) {
			struct vpu_prp_convert c2;

			memcpy(&c2, &c, sizeof(c2));
			c2.src_y   = c.dst_rgb;   /* intermediate RGB565 */
			c2.src_w   = r->out_w;
			c2.src_h   = r->out_h;
			c2.src_stride = r->out_w * 2;
			c2.dst_rgb = r->fb_phys + (unsigned)r->off_y * r->fb_stride
			                       + (unsigned)r->off_x * 4;
			c2.dst_w   = r->out_w;
			c2.dst_h   = r->out_h;
			c2.dst_bpp = 32;
			c2.dst_stride = r->fb_stride;
			c2.in_fmt  = 0x2CA00565;    /* PRP_PIXIN_RGB565 */

			if (ioctl(r->prp_fd, VPU_IOC_PRP_CONVERT, &c2) == 0) {
				used_prp = 1;
				goto done_cvt;
			}
		}

		/* Software fallback: CPU blit RGB565 -> Toon32 fb */
		{
			int row, col;
			int ro = r->fb_r_off, go = r->fb_g_off, bo = r->fb_b_off;
			int rs = 8 - r->fb_r_len, gs = 8 - r->fb_g_len, bs = 8 - r->fb_b_len;
			int row_bytes = r->out_w * 4;

			for (row = 0; row < r->out_h; row++) {
				unsigned short *src = (unsigned short *)
					((unsigned char *)r->rgbmem->virt_uaddr + row * r->blit_bytes);
				for (col = 0; col < r->out_w; col++) {
					unsigned short p = src[col];
					unsigned int rv = ((p >> 11) & 0x1F) << 3;
					unsigned int gv = ((p >>  5) & 0x3F) << 2;
					unsigned int bv = ( p        & 0x1F) << 3;
					rv |= rv >> 5;  gv |= gv >> 6;  bv |= bv >> 5;
					rv >>= rs;  gv >>= gs;  bv >>= bs;
					r->row_scratch[col] = (rv << ro) | (gv << go) | (bv << bo);
				}
				memcpy(r->fb_map + (r->off_y + row) * r->fb_stride + r->off_x * 4,
				       r->row_scratch, row_bytes);
			}
		}
	}
done_cvt:
	t2 = now_ms();

	*ms_csc += t1 - t0;
	*ms_blit += t2 - t1;
	return 0;
}

static int sock = -1;
static vpu_mem_desc bs, ps, slice;
static CodStd g_codec = STD_MPEG4;
static const char *g_codec_name = "mpeg4";

struct rtsp_url {
	char host[128];
	char path[256];
	char auth[192];
	char user[96];
	char pass[96];
	char realm[128];
	char nonce[128];
	int port;
};

struct rtsp_source {
	char url[512];
	int out_fd;
};

struct h264_rtp_state {
	unsigned char fu[128 * 1024];
	unsigned char au[512 * 1024];
	int fu_len;
	int fu_active;
	int au_len;
	int au_bad;
	int au_has_idr;
	int au_have_ts;
	unsigned int au_ts;
	int have_seq;
	unsigned int last_seq;
	int need_idr;
	int logs;
	int gap_logs;
	int resync_logs;
};

static int write_all(int fd, const void *buf, int len)
{
	const unsigned char *p = buf;

	while (len > 0) {
		int n = write(fd, p, len);
		if (n <= 0)
			return -1;
		p += n;
		len -= n;
	}
	return 0;
}

static int b64_value(int c)
{
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+') return 62;
	if (c == '/') return 63;
	return -1;
}

static int b64_decode(const char *in, unsigned char *out, int out_max)
{
	int val = 0, valb = -8, len = 0;

	for (; *in && *in != ',' && *in != ';' && *in != '\r' && *in != '\n'; in++) {
		int c;
		if (*in == '=')
			break;
		c = b64_value((unsigned char)*in);
		if (c < 0)
			continue;
		val = (val << 6) | c;
		valb += 6;
		if (valb >= 0) {
			if (len >= out_max)
				return -1;
			out[len++] = (unsigned char)((val >> valb) & 0xff);
			valb -= 8;
		}
	}
	return len;
}

static void b64_encode_basic(const char *in, char *out, int out_max)
{
	static const char tbl[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	int len = strlen(in), i, o = 0;

	for (i = 0; i < len && o + 4 < out_max; i += 3) {
		int rem = len - i;
		unsigned int v = ((unsigned char)in[i]) << 16;
		if (rem > 1) v |= ((unsigned char)in[i + 1]) << 8;
		if (rem > 2) v |= (unsigned char)in[i + 2];
		out[o++] = tbl[(v >> 18) & 63];
		out[o++] = tbl[(v >> 12) & 63];
		out[o++] = rem > 1 ? tbl[(v >> 6) & 63] : '=';
		out[o++] = rem > 2 ? tbl[v & 63] : '=';
	}
	out[o] = 0;
}

struct md5_ctx {
	uint32_t a, b, c, d;
	uint64_t bytes;
	unsigned char buf[64];
};

static uint32_t md5_rol(uint32_t v, int s)
{
	return (v << s) | (v >> (32 - s));
}

static void md5_block(struct md5_ctx *ctx, const unsigned char *p)
{
	static const uint32_t k[64] = {
		0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,
		0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
		0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,
		0x6b901122,0xfd987193,0xa679438e,0x49b40821,
		0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,
		0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
		0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,
		0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
		0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,
		0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
		0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,
		0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
		0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,
		0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
		0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,
		0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
	};
	static const unsigned char s[64] = {
		7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
		5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
		4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
		6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21
	};
	uint32_t a = ctx->a, b = ctx->b, c = ctx->c, d = ctx->d, x[16];
	int i;

	for (i = 0; i < 16; i++) {
		x[i] = (uint32_t)p[i * 4] | ((uint32_t)p[i * 4 + 1] << 8) |
		       ((uint32_t)p[i * 4 + 2] << 16) |
		       ((uint32_t)p[i * 4 + 3] << 24);
	}
	for (i = 0; i < 64; i++) {
		uint32_t f, g, tmp;

		if (i < 16) {
			f = (b & c) | (~b & d);
			g = i;
		} else if (i < 32) {
			f = (d & b) | (~d & c);
			g = (5 * i + 1) & 15;
		} else if (i < 48) {
			f = b ^ c ^ d;
			g = (3 * i + 5) & 15;
		} else {
			f = c ^ (b | ~d);
			g = (7 * i) & 15;
		}
		tmp = d;
		d = c;
		c = b;
		b = b + md5_rol(a + f + k[i] + x[g], s[i]);
		a = tmp;
	}
	ctx->a += a;
	ctx->b += b;
	ctx->c += c;
	ctx->d += d;
}

static void md5_init(struct md5_ctx *ctx)
{
	ctx->a = 0x67452301;
	ctx->b = 0xefcdab89;
	ctx->c = 0x98badcfe;
	ctx->d = 0x10325476;
	ctx->bytes = 0;
}

static void md5_update(struct md5_ctx *ctx, const void *data, int len)
{
	const unsigned char *p = data;
	int used = ctx->bytes & 63;

	ctx->bytes += len;
	if (used) {
		int n = 64 - used;
		if (n > len)
			n = len;
		memcpy(ctx->buf + used, p, n);
		used += n;
		p += n;
		len -= n;
		if (used == 64)
			md5_block(ctx, ctx->buf);
	}
	while (len >= 64) {
		md5_block(ctx, p);
		p += 64;
		len -= 64;
	}
	if (len > 0)
		memcpy(ctx->buf, p, len);
}

static void md5_final(struct md5_ctx *ctx, unsigned char out[16])
{
	static const unsigned char pad[64] = { 0x80 };
	unsigned char bits[8];
	uint64_t bit_len = ctx->bytes * 8;
	int used = ctx->bytes & 63;
	int pad_len = used < 56 ? 56 - used : 120 - used;
	int i;

	for (i = 0; i < 8; i++)
		bits[i] = (unsigned char)(bit_len >> (8 * i));
	md5_update(ctx, pad, pad_len);
	md5_update(ctx, bits, 8);
	for (i = 0; i < 4; i++) {
		uint32_t v = i == 0 ? ctx->a : i == 1 ? ctx->b :
		             i == 2 ? ctx->c : ctx->d;
		out[i * 4] = v & 0xff;
		out[i * 4 + 1] = (v >> 8) & 0xff;
		out[i * 4 + 2] = (v >> 16) & 0xff;
		out[i * 4 + 3] = (v >> 24) & 0xff;
	}
}

static void md5_hex(const char *s, char out[33])
{
	static const char hex[] = "0123456789abcdef";
	struct md5_ctx ctx;
	unsigned char sum[16];
	int i;

	md5_init(&ctx);
	md5_update(&ctx, s, strlen(s));
	md5_final(&ctx, sum);
	for (i = 0; i < 16; i++) {
		out[i * 2] = hex[sum[i] >> 4];
		out[i * 2 + 1] = hex[sum[i] & 15];
	}
	out[32] = 0;
}

static int parse_rtsp_url(const char *url, struct rtsp_url *u)
{
	const char *p, *slash, *at, *host;
	char hostport[160];
	int hp_len;

	memset(u, 0, sizeof(*u));
	u->port = 554;
	p = strstr(url, "rtsp://");
	if (p != url)
		return -1;
	p += 7;
	slash = strchr(p, '/');
	if (!slash)
		return -1;
	at = memchr(p, '@', slash - p);
	host = p;
	if (at) {
		char userpass[192];
		char raw_userpass[192];
		char *colon;
		int n = at - p;
		if (n >= (int)sizeof(userpass))
			return -1;
		memcpy(userpass, p, n);
		userpass[n] = 0;
		snprintf(raw_userpass, sizeof(raw_userpass), "%s", userpass);
		b64_encode_basic(raw_userpass, u->auth, sizeof(u->auth));
		colon = strchr(userpass, ':');
		if (colon) {
			int user_len = colon - userpass;
			int pass_len = strlen(colon + 1);

			if (user_len <= 0 || user_len >= (int)sizeof(u->user) ||
			    pass_len >= (int)sizeof(u->pass))
				return -1;
			*colon = 0;
			memcpy(u->user, userpass, user_len);
			u->user[user_len] = 0;
			memcpy(u->pass, colon + 1, pass_len + 1);
		}
		host = at + 1;
	}
	hp_len = slash - host;
	if (hp_len <= 0 || hp_len >= (int)sizeof(hostport))
		return -1;
	memcpy(hostport, host, hp_len);
	hostport[hp_len] = 0;
	p = strrchr(hostport, ':');
	if (p) {
		u->port = atoi(p + 1);
		hostport[p - hostport] = 0;
	}
	if (strlen(hostport) >= sizeof(u->host))
		return -1;
	strcpy(u->host, hostport);
	snprintf(u->path, sizeof(u->path), "%s", slash);
	return 0;
}

static int tcp_connect_host(const char *host, int port)
{
	struct sockaddr_in sin;
	int fd;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_port = htons(port);
	if (!inet_aton(host, &sin.sin_addr)) {
		close(fd);
		return -1;
	}
	if (connect(fd, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static int rtsp_make_digest_auth(char *dst, int dst_len, const char *url,
				 const struct rtsp_url *u,
				 const char *method)
{
	char a1[384], a2[640], resp_in[256];
	char ha1[33], ha2[33], resp[33];
	int n;

	if (!u->user[0] || !u->nonce[0] || !u->realm[0])
		return 0;
	n = snprintf(a1, sizeof(a1), "%s:%s:%s", u->user, u->realm, u->pass);
	if (n <= 0 || n >= (int)sizeof(a1))
		return -1;
	n = snprintf(a2, sizeof(a2), "%s:%s", method, url);
	if (n <= 0 || n >= (int)sizeof(a2))
		return -1;
	md5_hex(a1, ha1);
	md5_hex(a2, ha2);
	n = snprintf(resp_in, sizeof(resp_in), "%s:%s:%s", ha1, u->nonce, ha2);
	if (n <= 0 || n >= (int)sizeof(resp_in))
		return -1;
	md5_hex(resp_in, resp);
	n = snprintf(dst, dst_len,
		     "Authorization: Digest username=\"%s\", realm=\"%s\", "
		     "nonce=\"%s\", uri=\"%s\", response=\"%s\"\r\n",
		     u->user, u->realm, u->nonce, url, resp);
	return (n > 0 && n < dst_len) ? 1 : -1;
}

static int rtsp_send(int fd, const char *url, const struct rtsp_url *u,
		     int *cseq, const char *method, const char *extra)
{
	char req[3072], auth[1200];
	int n, auth_type;

	auth[0] = 0;
	auth_type = rtsp_make_digest_auth(auth, sizeof(auth), url, u, method);
	if (auth_type < 0)
		return -1;
	if (auth_type == 0 && u->auth[0]) {
		n = snprintf(auth, sizeof(auth), "Authorization: Basic %s\r\n",
			     u->auth);
		if (n <= 0 || n >= (int)sizeof(auth))
			return -1;
	}

	n = snprintf(req, sizeof(req),
		     "%s %s RTSP/1.0\r\n"
		     "CSeq: %d\r\n"
		     "User-Agent: toon-vpu-stream\r\n"
		     "%s"
		     "%s"
		     "\r\n",
		     method, url, (*cseq)++,
		     auth,
		     extra ? extra : "");
	if (n <= 0 || n >= (int)sizeof(req))
		return -1;
	return write_all(fd, req, n);
}

static int rtsp_digest_param(const char *s, const char *name,
			     char *out, int out_len)
{
	int name_len = strlen(name);
	const char *p = s;

	out[0] = 0;
	while ((p = strcasestr(p, name))) {
		const char *q = p + name_len;

		if (p != s && (isalnum((unsigned char)p[-1]) || p[-1] == '_')) {
			p = q;
			continue;
		}
		while (*q == ' ' || *q == '\t')
			q++;
		if (*q != '=') {
			p = q;
			continue;
		}
		q++;
		while (*q == ' ' || *q == '\t')
			q++;
		if (*q == '"') {
			const char *e = strchr(++q, '"');
			if (!e)
				return -1;
			snprintf(out, out_len, "%.*s", (int)(e - q), q);
			return out[0] ? 0 : -1;
		} else {
			const char *e = strpbrk(q, ", \t\r\n");
			if (!e)
				e = q + strlen(q);
			snprintf(out, out_len, "%.*s", (int)(e - q), q);
			return out[0] ? 0 : -1;
		}
	}
	return -1;
}

static int rtsp_parse_digest_challenge(const char *hdr, struct rtsp_url *u)
{
	const char *auth;

	auth = hdr ? strcasestr(hdr, "WWW-Authenticate:") : NULL;
	if (!auth || !strcasestr(auth, "Digest"))
		return -1;
	if (rtsp_digest_param(auth, "realm", u->realm, sizeof(u->realm)) < 0)
		return -1;
	if (rtsp_digest_param(auth, "nonce", u->nonce, sizeof(u->nonce)) < 0)
		return -1;
	return 0;
}

static int rtsp_read_full(int fd, unsigned char *buf, int len)
{
	int got = 0;

	while (got < len) {
		int n = read(fd, buf + got, len - got);
		if (n <= 0)
			return -1;
		got += n;
	}
	return 0;
}

static int rtsp_discard_interleaved(int fd)
{
	unsigned char h[3], buf[512];
	int len;

	if (rtsp_read_full(fd, h, 3) < 0)
		return -1;
	len = (h[1] << 8) | h[2];
	while (len > 0) {
		int chunk = len > (int)sizeof(buf) ? (int)sizeof(buf) : len;
		if (rtsp_read_full(fd, buf, chunk) < 0)
			return -1;
		len -= chunk;
	}
	return 0;
}

static int rtsp_read_response(int fd, char *hdr, int hdr_max,
			      char *body, int body_max)
{
	int n = 0, content_len = 0;
	char *p;

	hdr[0] = 0;
	body[0] = 0;
	while (n < hdr_max - 1) {
		unsigned char ch;
		int r = read(fd, &ch, 1);
		if (r <= 0)
			return -1;
		if (n == 0 && ch == '$') {
			if (rtsp_discard_interleaved(fd) < 0)
				return -1;
			continue;
		}
		if (n == 0 && ch != 'R')
			continue;
		hdr[n] = ch;
		n += r;
		hdr[n] = 0;
		if (n >= 4 && !memcmp(hdr + n - 4, "\r\n\r\n", 4))
			break;
	}
	p = strcasestr(hdr, "Content-Length:");
	if (p)
		content_len = atoi(p + 15);
	if (content_len > body_max - 1)
		return -1;
	n = 0;
	while (n < content_len) {
		int r = read(fd, body + n, content_len - n);
		if (r <= 0)
			return -1;
		n += r;
	}
	body[n] = 0;
	return strstr(hdr, "RTSP/1.0 200") ? 0 : -1;
}

static void rtsp_log_failure(const char *where, const char *hdr,
			     const char *body)
{
	char line[160];
	const char *e;
	const char *auth;

	line[0] = 0;
	if (hdr && hdr[0]) {
		e = strpbrk(hdr, "\r\n");
		snprintf(line, sizeof(line), "%.*s", e ? (int)(e - hdr) : 80, hdr);
	}
	printf("rtsp %s failed%s%s\n", where, line[0] ? ": " : "", line);
	auth = hdr ? strcasestr(hdr, "WWW-Authenticate:") : NULL;
	if (auth) {
		e = strpbrk(auth, "\r\n");
		printf("rtsp auth challenge: %.*s\n", e ? (int)(e - auth) : 80, auth);
	}
	if (body && body[0]) {
		const char *m = strstr(body, "m=video");
		const char *codec = strstr(body, "H264");
		if (!codec)
			codec = strstr(body, "H265");
		if (!codec)
			codec = strstr(body, "HEVC");
		printf("rtsp sdp: video=%s codec=%s\n",
		       m ? "yes" : "no", codec ? codec : "unknown");
	}
	fflush(stdout);
}

static void rtsp_abs_url(char *dst, int dst_len, const char *base,
			 const char *control)
{
	if (!control[0] || !strcmp(control, "*")) {
		snprintf(dst, dst_len, "%s", base);
	} else if (!strncmp(control, "rtsp://", 7)) {
		snprintf(dst, dst_len, "%s", control);
	} else {
		if ((int)(strlen(base) + 1 + strlen(control) + 1) > dst_len) {
			dst[0] = 0;
			return;
		}
		strcpy(dst, base);
		strcat(dst, "/");
		strcat(dst, control);
	}
}

static void sdp_parse_h264(const char *sdp, char *control, int control_len,
			   unsigned char *spspps, int *spspps_len)
{
	const char *p;
	const char *section = sdp;
	const char *section_end = sdp + strlen(sdp);

	control[0] = 0;
	*spspps_len = 0;
	p = strstr(sdp, "m=video");
	if (p) {
		const char *n = strstr(p + 1, "\nm=");
		section = p;
		if (n)
			section_end = n;
	}
	p = strstr(section, "a=control:");
	if (p && p < section_end) {
		const char *next = strstr(p + 1, "a=control:");
		while (next && next < section_end) {
			p = next;
			next = strstr(p + 1, "a=control:");
		}
	}
	if (p) {
		const char *e;
		p += 10;
		e = strpbrk(p, "\r\n");
		if (!e) e = p + strlen(p);
		snprintf(control, control_len, "%.*s", (int)(e - p), p);
	}
	p = strstr(sdp, "sprop-parameter-sets=");
	if (p) {
		unsigned char tmp[256];
		int n;
		p += 21;
		n = b64_decode(p, tmp, sizeof(tmp));
		if (n > 0 && *spspps_len + n + 4 < 512) {
			static const unsigned char sc[] = { 0, 0, 0, 1 };
			memcpy(spspps + *spspps_len, sc, 4);
			*spspps_len += 4;
			memcpy(spspps + *spspps_len, tmp, n);
			*spspps_len += n;
		}
		p = strchr(p, ',');
		if (p) {
			p++;
			n = b64_decode(p, tmp, sizeof(tmp));
			if (n > 0 && *spspps_len + n + 4 < 512) {
				static const unsigned char sc[] = { 0, 0, 0, 1 };
				memcpy(spspps + *spspps_len, sc, 4);
				*spspps_len += 4;
				memcpy(spspps + *spspps_len, tmp, n);
				*spspps_len += n;
			}
		}
	}
}

static int parse_h264_sprop(const char *sprop, unsigned char *spspps,
			    int *spspps_len)
{
	const char *p = sprop;
	int count = 0;

	*spspps_len = 0;
	while (*p && count < 2) {
		unsigned char tmp[256];
		const char *e = strchr(p, ',');
		int n;
		int nal_type;

		n = b64_decode(p, tmp, sizeof(tmp));
		if (n <= 0 || *spspps_len + n + 4 >= 512)
			return -1;
		nal_type = tmp[0] & 0x1f;
		if ((count == 0 && nal_type != 7) ||
		    (count == 1 && nal_type != 8))
			return -1;
		spspps[(*spspps_len)++] = 0;
		spspps[(*spspps_len)++] = 0;
		spspps[(*spspps_len)++] = 0;
		spspps[(*spspps_len)++] = 1;
		memcpy(spspps + *spspps_len, tmp, n);
		*spspps_len += n;
		count++;
		if (!e)
			break;
		p = e + 1;
	}
	return count >= 1 ? 0 : -1;
}

static void rtsp_parse_session(const char *hdr, char *session, int session_len)
{
	const char *p = strstr(hdr, "Session:");
	const char *e;

	session[0] = 0;
	if (!p)
		return;
	p += 8;
	while (*p == ' ' || *p == '\t')
		p++;
	e = strpbrk(p, ";\r\n");
	if (!e)
		e = p + strlen(p);
	snprintf(session, session_len, "%.*s", (int)(e - p), p);
}

static void rtsp_log_h264_bytes(const char *tag, const unsigned char *p, int len)
{
	int i, n = len < 32 ? len : 32;

	printf("rtsp h264 %s len=%d hex=", tag, len);
	for (i = 0; i < n; i++)
		printf("%02x", p[i]);
	printf("%s\n", len > n ? "..." : "");
	fflush(stdout);
}

static const char *h264_profile_name(int profile_idc)
{
	switch (profile_idc) {
	case 66: return "Baseline";
	case 77: return "Main";
	case 88: return "Extended";
	case 100: return "High";
	case 110: return "High 10";
	case 122: return "High 4:2:2";
	case 244: return "High 4:4:4";
	default: return "unknown";
	}
}

static void rtsp_log_h264_sps_profile(const unsigned char *p, int len)
{
	int i;

	for (i = 0; i + 7 < len; i++) {
		int sc = 0;

		if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1)
			sc = 3;
		else if (i + 4 < len && p[i] == 0 && p[i + 1] == 0 &&
			 p[i + 2] == 0 && p[i + 3] == 1)
			sc = 4;
		if (!sc || (p[i + sc] & 0x1f) != 7)
			continue;
		printf("rtsp h264 sps profile=%s(%u) constraints=0x%02x level=%u.%u\n",
		       h264_profile_name(p[i + sc + 1]), p[i + sc + 1],
		       p[i + sc + 2], p[i + sc + 3] / 10, p[i + sc + 3] % 10);
		if (p[i + sc + 1] >= 100)
			printf("rtsp h264 warning: High-profile stream; CODA DX6 is likely unable to decode this\n");
		fflush(stdout);
		return;
	}
}

static void rtp_log_h264_nal(struct h264_rtp_state *st, const char *tag,
			     int channel, int nal_type, int len)
{
	if (st->logs++ >= 40)
		return;
	printf("%s rtp ch=%d h264 nal=%d len=%d\n", tag, channel, nal_type, len);
	fflush(stdout);
}

static void rtp_au_reset(struct h264_rtp_state *st)
{
	st->au_len = 0;
	st->au_bad = 0;
	st->au_has_idr = 0;
}

static int rtp_au_append(struct h264_rtp_state *st,
			 const unsigned char *nal, int len)
{
	static const unsigned char sc[] = { 0, 0, 0, 1 };

	if (len <= 0 || st->au_len + (int)sizeof(sc) + len > (int)sizeof(st->au)) {
		st->au_bad = 1;
		return -1;
	}
	memcpy(st->au + st->au_len, sc, sizeof(sc));
	st->au_len += sizeof(sc);
	memcpy(st->au + st->au_len, nal, len);
	st->au_len += len;
	return 0;
}

static int rtp_au_append_bytes(struct h264_rtp_state *st,
			       const unsigned char *p, int len)
{
	if (len <= 0)
		return 0;
	if (st->au_len + len > (int)sizeof(st->au)) {
		st->au_bad = 1;
		return -1;
	}
	memcpy(st->au + st->au_len, p, len);
	st->au_len += len;
	return 0;
}

static int rtp_au_flush(struct h264_rtp_state *st, int out_fd, const char *tag)
{
	int rc = 0;

	if (st->au_len > 0 && !st->au_bad) {
		if (!st->need_idr || st->au_has_idr) {
			rc = write_all(out_fd, st->au, st->au_len);
			if (st->au_has_idr) {
				if (st->need_idr && st->resync_logs++ < 8) {
					printf("%s rtp resync at IDR\n", tag);
					fflush(stdout);
				}
				st->need_idr = 0;
			}
		} else {
			/* While waiting for an IDR, still pass SPS/PPS-only access
			 * units so the decoder can prime its sequence headers. */
			rc = write_all(out_fd, st->au, st->au_len);
		}
	}
	rtp_au_reset(st);
	return rc;
}

static void rtp_direct_reset(struct h264_rtp_state *st)
{
	st->au_len = 0;
	st->au_bad = 0;
	st->au_has_idr = 0;
}

static int mpeg4_payload_has_i_vop(const unsigned char *p, int len)
{
	int i;

	for (i = 0; i + 4 < len; i++) {
		if (p[i] == 0x00 && p[i + 1] == 0x00 &&
		    p[i + 2] == 0x01 && p[i + 3] == 0xb6)
			return (p[i + 4] & 0xc0) == 0x00;
	}
	return 0;
}

static int mpeg4_payload_has_vop(const unsigned char *p, int len)
{
	int i;

	for (i = 0; i + 3 < len; i++) {
		if (p[i] == 0x00 && p[i + 1] == 0x00 &&
		    p[i + 2] == 0x01 && p[i + 3] == 0xb6)
			return 1;
	}
	return 0;
}

static int rtp_direct_append(DecHandle h, struct h264_rtp_state *st,
			     const unsigned char *p, int len)
{
	PhysicalAddress rd, wr;
	Uint32 space;
	int base, pos, first;

	if (len <= 0)
		return 0;
	if (vpu_DecGetBitstreamBuffer(h, &rd, &wr, &space) != RETCODE_SUCCESS) {
		st->au_bad = 1;
		return -1;
	}
	if (st->au_len + len + 512 >= (int)space) {
		st->au_bad = 1;
		return -1;
	}
	base = (int)(wr - bs.phy_addr);
	pos = (base + st->au_len) % STREAM_BUF_SIZE;
	first = STREAM_BUF_SIZE - pos;
	if (first > len)
		first = len;
	memcpy((void *)(bs.virt_uaddr + pos), p, first);
	if (first < len)
		memcpy((void *)bs.virt_uaddr, p + first, len - first);
	st->au_len += len;
	return 0;
}

static int rtp_direct_append_nal(DecHandle h, struct h264_rtp_state *st,
				 const unsigned char *nal, int len)
{
	static const unsigned char sc[] = { 0, 0, 0, 1 };

	if (rtp_direct_append(h, st, sc, sizeof(sc)) < 0)
		return -1;
	return rtp_direct_append(h, st, nal, len);
}

static int rtp_direct_flush(DecHandle h, struct h264_rtp_state *st,
			    const char *tag)
{
	int committed = 0;

	if (st->au_len > 0 && !st->au_bad) {
		if (!st->need_idr || st->au_has_idr) {
			if (vpu_DecUpdateBitstreamBuffer(h, st->au_len) == RETCODE_SUCCESS)
				committed = st->au_len;
			if (st->au_has_idr) {
				if (st->need_idr && st->resync_logs++ < 8) {
					printf("%s rtp resync at IDR\n", tag);
					fflush(stdout);
				}
				st->need_idr = 0;
			}
		} else {
			if (vpu_DecUpdateBitstreamBuffer(h, st->au_len) == RETCODE_SUCCESS)
				committed = st->au_len;
		}
	}
	rtp_direct_reset(st);
	return committed;
}

static int rtp_h264_seq_gap(struct h264_rtp_state *st,
			    const unsigned char *pkt, int len,
			    const char *tag)
{
	unsigned int seq;
	unsigned int expect;

	if (len < 4)
		return 1;
	seq = ((unsigned int)pkt[2] << 8) | pkt[3];
	if (!st->have_seq) {
		st->have_seq = 1;
		st->last_seq = seq;
		return 0;
	}
	expect = (st->last_seq + 1) & 0xffff;
	st->last_seq = seq;
	if (seq == expect)
		return 0;
	st->fu_active = 0;
	st->fu_len = 0;
	st->need_idr = 1;
	if (st->gap_logs++ < 12) {
		printf("%s rtp sequence gap: expected=%u got=%u; waiting for IDR\n",
		       tag, expect, seq);
		fflush(stdout);
	}
	return 1;
}

static int handle_rtp_h264(struct h264_rtp_state *st, int out_fd,
			   const unsigned char *pkt, int len,
			   const unsigned char *spspps, int spspps_len,
			   int channel, const char *tag)
{
	int cc, off, nal_type, marker;
	unsigned int ts;
	int seq_gap, new_au = 0;

	if (len < 12 || (pkt[0] >> 6) != 2)
		return 0;
	marker = pkt[1] & 0x80;
	ts = ((unsigned int)pkt[4] << 24) | ((unsigned int)pkt[5] << 16) |
	     ((unsigned int)pkt[6] << 8) | pkt[7];
	seq_gap = rtp_h264_seq_gap(st, pkt, len, tag);
	if (!st->au_have_ts || st->au_ts != ts) {
		if (st->au_have_ts && st->au_len > 0) {
			st->need_idr = 1;
			rtp_au_reset(st);
		}
		st->au_have_ts = 1;
		st->au_ts = ts;
		new_au = 1;
	}
	if (seq_gap && !new_au)
		st->au_bad = 1;
	cc = pkt[0] & 0x0f;
	off = 12 + cc * 4;
	if (pkt[0] & 0x10) {
		int ext_len;
		if (len < off + 4)
			return 0;
		ext_len = (pkt[off + 2] << 8) | pkt[off + 3];
		off += 4 + ext_len * 4;
	}
	if (len <= off)
		return 0;
	pkt += off;
	len -= off;
	nal_type = pkt[0] & 0x1f;
	rtp_log_h264_nal(st, tag, channel, nal_type, len);
	if (nal_type >= 1 && nal_type <= 23) {
		if (st->need_idr && nal_type != 5 && nal_type != 7 && nal_type != 8)
			goto maybe_flush;
		if (nal_type == 5)
			st->au_has_idr = 1;
		if (nal_type == 5 && spspps_len > 0)
			rtp_au_append_bytes(st, spspps, spspps_len);
		rtp_au_append(st, pkt, len);
		goto maybe_flush;
	}
	if (nal_type == 24) {
		int p = 1;
		while (p + 2 <= len) {
			int n = (pkt[p] << 8) | pkt[p + 1];
			int stap_type;
			p += 2;
			if (n <= 0 || p + n > len)
				break;
			stap_type = pkt[p] & 0x1f;
			if (st->need_idr && stap_type != 5 &&
			    stap_type != 7 && stap_type != 8) {
				p += n;
				continue;
			}
			if (stap_type == 5)
				st->au_has_idr = 1;
			if (stap_type == 5 && spspps_len > 0)
				rtp_au_append_bytes(st, spspps, spspps_len);
			rtp_au_append(st, pkt + p, n);
			p += n;
		}
		goto maybe_flush;
	}
	if (nal_type == 28 && len >= 2) {
		int start = pkt[1] & 0x80;
		int end = pkt[1] & 0x40;
		unsigned char hdr = (pkt[0] & 0xe0) | (pkt[1] & 0x1f);
		int fu_type = hdr & 0x1f;
		if (seq_gap)
			goto maybe_flush;
		if (st->need_idr && fu_type != 5)
			goto maybe_flush;
		if (start) {
			st->fu_len = 0;
			st->fu[st->fu_len++] = hdr;
			if (fu_type == 5)
				st->au_has_idr = 1;
			if (fu_type == 5 && spspps_len > 0)
				rtp_au_append_bytes(st, spspps, spspps_len);
			st->fu_active = 1;
		}
		if (!st->fu_active || st->fu_len + len - 2 > (int)sizeof(st->fu)) {
			st->fu_active = 0;
			return 0;
		}
		memcpy(st->fu + st->fu_len, pkt + 2, len - 2);
		st->fu_len += len - 2;
		if (end) {
			rtp_au_append(st, st->fu, st->fu_len);
			st->fu_active = 0;
			st->fu_len = 0;
		}
	}
maybe_flush:
	if (marker)
		return rtp_au_flush(st, out_fd, tag);
	return 0;
}

static int handle_rtp_h264_direct(DecHandle h, struct h264_rtp_state *st,
				  const unsigned char *pkt, int len,
				  const unsigned char *spspps, int spspps_len,
				  int channel, const char *tag)
{
	int cc, off, nal_type, marker;
	unsigned int ts;
	int seq_gap, new_au = 0;

	if (len < 12 || (pkt[0] >> 6) != 2)
		return 0;
	marker = pkt[1] & 0x80;
	ts = ((unsigned int)pkt[4] << 24) | ((unsigned int)pkt[5] << 16) |
	     ((unsigned int)pkt[6] << 8) | pkt[7];
	seq_gap = rtp_h264_seq_gap(st, pkt, len, tag);
	if (!st->au_have_ts || st->au_ts != ts) {
		if (st->au_have_ts && st->au_len > 0) {
			st->need_idr = 1;
			rtp_direct_reset(st);
		}
		st->au_have_ts = 1;
		st->au_ts = ts;
		new_au = 1;
	}
	if (seq_gap && !new_au)
		st->au_bad = 1;
	cc = pkt[0] & 0x0f;
	off = 12 + cc * 4;
	if (pkt[0] & 0x10) {
		int ext_len;
		if (len < off + 4)
			return 0;
		ext_len = (pkt[off + 2] << 8) | pkt[off + 3];
		off += 4 + ext_len * 4;
	}
	if (len <= off)
		return 0;
	pkt += off;
	len -= off;
	nal_type = pkt[0] & 0x1f;
	rtp_log_h264_nal(st, tag, channel, nal_type, len);

	if (nal_type >= 1 && nal_type <= 23) {
		if (st->need_idr && nal_type != 5 && nal_type != 7 && nal_type != 8)
			goto maybe_flush;
		if (nal_type == 5)
			st->au_has_idr = 1;
		if (nal_type == 5 && spspps_len > 0)
			rtp_direct_append(h, st, spspps, spspps_len);
		rtp_direct_append_nal(h, st, pkt, len);
		goto maybe_flush;
	}
	if (nal_type == 24) {
		int p = 1;
		while (p + 2 <= len) {
			int n = (pkt[p] << 8) | pkt[p + 1];
			int stap_type;
			p += 2;
			if (n <= 0 || p + n > len)
				break;
			stap_type = pkt[p] & 0x1f;
			if (st->need_idr && stap_type != 5 &&
			    stap_type != 7 && stap_type != 8) {
				p += n;
				continue;
			}
			if (stap_type == 5)
				st->au_has_idr = 1;
			if (stap_type == 5 && spspps_len > 0)
				rtp_direct_append(h, st, spspps, spspps_len);
			rtp_direct_append_nal(h, st, pkt + p, n);
			p += n;
		}
		goto maybe_flush;
	}
	if (nal_type == 28 && len >= 2) {
		int start = pkt[1] & 0x80;
		int end = pkt[1] & 0x40;
		unsigned char hdr = (pkt[0] & 0xe0) | (pkt[1] & 0x1f);
		int fu_type = hdr & 0x1f;
		if (seq_gap)
			goto maybe_flush;
		if (st->need_idr && fu_type != 5)
			goto maybe_flush;
		if (start) {
			if (fu_type == 5)
				st->au_has_idr = 1;
			if (fu_type == 5 && spspps_len > 0)
				rtp_direct_append(h, st, spspps, spspps_len);
			rtp_direct_append_nal(h, st, &hdr, 1);
			st->fu_active = 1;
		}
		if (!st->fu_active) {
			st->au_bad = 1;
			goto maybe_flush;
		}
		rtp_direct_append(h, st, pkt + 2, len - 2);
		if (end)
			st->fu_active = 0;
	}
maybe_flush:
	if (marker)
		return rtp_direct_flush(h, st, tag);
	return 0;
}

static int handle_rtp_mpeg4_direct(DecHandle h, struct h264_rtp_state *st,
				   const unsigned char *pkt, int len,
				   int channel, const char *tag)
{
	int cc, off, marker;
	unsigned int ts;
	int seq_gap, new_au = 0;
	const unsigned char *payload;
	int payload_len;

	if (len < 12 || (pkt[0] >> 6) != 2)
		return 0;
	marker = pkt[1] & 0x80;
	ts = ((unsigned int)pkt[4] << 24) | ((unsigned int)pkt[5] << 16) |
	     ((unsigned int)pkt[6] << 8) | pkt[7];
	seq_gap = rtp_h264_seq_gap(st, pkt, len, tag);
	if (!st->au_have_ts || st->au_ts != ts) {
		if (st->au_have_ts && st->au_len > 0) {
			st->need_idr = 1;
			rtp_direct_reset(st);
		}
		st->au_have_ts = 1;
		st->au_ts = ts;
		new_au = 1;
	}
	if (seq_gap && !new_au)
		st->au_bad = 1;
	cc = pkt[0] & 0x0f;
	off = 12 + cc * 4;
	if (pkt[0] & 0x10) {
		int ext_len;
		if (len < off + 4)
			return 0;
		ext_len = (pkt[off + 2] << 8) | pkt[off + 3];
		off += 4 + ext_len * 4;
	}
	if (len <= off)
		return 0;
	payload = pkt + off;
	payload_len = len - off;
	if (st->logs++ < 40) {
		printf("%s rtp ch=%d mpeg4 payload=%d marker=%d first=%02x %02x %02x %02x\n",
		       tag, channel, payload_len, marker ? 1 : 0,
		       payload_len > 0 ? payload[0] : 0,
		       payload_len > 1 ? payload[1] : 0,
		       payload_len > 2 ? payload[2] : 0,
		       payload_len > 3 ? payload[3] : 0);
		fflush(stdout);
	}
	if (mpeg4_payload_has_i_vop(payload, payload_len)) {
		if (st->need_idr && st->resync_logs++ < 8) {
			printf("%s rtp resync at MPEG4 I-VOP\n", tag);
			fflush(stdout);
		}
		st->au_has_idr = 1;
		st->need_idr = 0;
	}
	if (st->need_idr && mpeg4_payload_has_vop(payload, payload_len))
		goto maybe_flush;
	rtp_direct_append(h, st, payload, payload_len);
maybe_flush:
	if (marker)
		return rtp_direct_flush(h, st, tag);
	return 0;
}

static void *rtsp_thread_main(void *arg)
{
	struct rtsp_source *src = arg;
	struct rtsp_url u;
	char base[512], setup_url[512], hdr[2048] = {0}, body[4096] = {0};
	char control[256];
	char session[128], play_extra[192];
	unsigned char spspps[512];
	struct h264_rtp_state rtp_state;
	int spspps_len = 0;
	int fd, cseq = 1;

	memset(&rtp_state, 0, sizeof(rtp_state));
	if (parse_rtsp_url(src->url, &u) < 0)
		goto out;
	fd = tcp_connect_host(u.host, u.port);
	if (fd < 0)
		goto out;
	snprintf(base, sizeof(base), "rtsp://%s:%d%s", u.host, u.port, u.path);
	if (rtsp_send(fd, base, &u, &cseq, "DESCRIBE",
		      "Accept: application/sdp\r\n") < 0) {
		rtsp_log_failure("DESCRIBE", hdr, body);
		goto close_fd;
	}
	if (rtsp_read_response(fd, hdr, sizeof(hdr), body, sizeof(body)) < 0) {
		if (rtsp_parse_digest_challenge(hdr, &u) == 0) {
			printf("rtsp DESCRIBE digest auth: realm='%s'\n", u.realm);
			fflush(stdout);
			if (rtsp_send(fd, base, &u, &cseq, "DESCRIBE",
				      "Accept: application/sdp\r\n") < 0 ||
			    rtsp_read_response(fd, hdr, sizeof(hdr),
					       body, sizeof(body)) < 0) {
				rtsp_log_failure("DESCRIBE", hdr, body);
				goto close_fd;
			}
		} else {
			rtsp_log_failure("DESCRIBE", hdr, body);
			goto close_fd;
		}
	}
	sdp_parse_h264(body, control, sizeof(control), spspps, &spspps_len);
	printf("rtsp DESCRIBE ok: control='%s' spspps=%d bytes\n",
	       control[0] ? control : "(none)", spspps_len);
	fflush(stdout);
	rtsp_abs_url(setup_url, sizeof(setup_url), base, control);
	if (!setup_url[0]) {
		printf("rtsp SETUP url too long\n");
		goto close_fd;
	}
	if (rtsp_send(fd, setup_url, &u, &cseq, "SETUP",
		      "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n") < 0 ||
	    rtsp_read_response(fd, hdr, sizeof(hdr), body, sizeof(body)) < 0) {
		rtsp_log_failure("SETUP", hdr, body);
		goto close_fd;
	}
	rtsp_parse_session(hdr, session, sizeof(session));
	if (!session[0]) {
		printf("rtsp SETUP failed: no Session header\n");
		fflush(stdout);
		goto close_fd;
	}
	snprintf(play_extra, sizeof(play_extra), "Session: %s\r\n", session);
	if (rtsp_send(fd, base, &u, &cseq, "PLAY", play_extra) < 0 ||
	    rtsp_read_response(fd, hdr, sizeof(hdr), body, sizeof(body)) < 0) {
		rtsp_log_failure("PLAY", hdr, body);
		goto close_fd;
	}
	if (spspps_len > 0) {
		rtsp_log_h264_bytes("spspps", spspps, spspps_len);
		rtsp_log_h264_sps_profile(spspps, spspps_len);
		if (write_all(src->out_fd, spspps, spspps_len) < 0)
			goto close_fd;
	}
	printf("rtsp streaming from %s\n", u.host);
	fflush(stdout);
	for (;;) {
		unsigned char h[4], pkt[65536];
		int len, got = 0;
		if (read(fd, h, 1) != 1)
			break;
		if (h[0] != '$')
			continue;
		if (read(fd, h + 1, 3) != 3)
			break;
		len = (h[2] << 8) | h[3];
		if (len <= 0 || len > (int)sizeof(pkt))
			break;
		while (got < len) {
			int n = read(fd, pkt + got, len - got);
			if (n <= 0)
				goto close_fd;
			got += n;
		}
		if (h[1] == 0) {
			if (handle_rtp_h264(&rtp_state, src->out_fd, pkt, len,
					    spspps, spspps_len, h[1], "rtsp") < 0)
				break;
		} else {
			static int skip_logs;

			if (skip_logs++ < 8) {
				printf("rtsp skip interleaved ch=%d len=%d\n", h[1], len);
				fflush(stdout);
			}
		}
	}
close_fd:
	close(fd);
out:
	close(src->out_fd);
	free(src);
	return NULL;
}

static int rtp_open_udp(int port)
{
	struct sockaddr_in addr;
	int fd, rcvbuf = 512 * 1024, one = 1;

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return -1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
	setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif
	setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons(port);
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("rtp bind");
		close(fd);
		return -1;
	}
	return fd;
}

static int rtp_feed_direct(DecHandle h, struct h264_rtp_state *st,
			   const unsigned char *spspps, int spspps_len,
			   int block)
{
	unsigned char pkt[65536];
	int packets = 0, committed = 0;

	fcntl(sock, F_SETFL, block ? 0 : O_NONBLOCK);
	for (;;) {
		int n, rc;

		n = recv(sock, pkt, sizeof(pkt), 0);
		if (n < 0) {
			if (packets > 0)
				return committed > 0 ? committed : -1;
			return -1;
		}
		if (n == 0)
			return 0;
		packets++;
		if (st->logs < 40) {
			printf("udp packet len=%d first=%02x %02x %02x %02x\n",
			       n, n > 0 ? pkt[0] : 0, n > 1 ? pkt[1] : 0,
			       n > 2 ? pkt[2] : 0, n > 3 ? pkt[3] : 0);
			fflush(stdout);
		}
		if (g_codec == STD_AVC)
			rc = handle_rtp_h264_direct(h, st, pkt, n,
						    spspps, spspps_len, 0, "udp");
		else
			rc = handle_rtp_mpeg4_direct(h, st, pkt, n, 0, "udp");
		if (rc > 0)
			committed += rc;

		/* Prime starts blocking for the first packet, then drains the
		 * immediately queued burst nonblocking. Normal decode uses
		 * nonblocking throughout. */
		if (block)
			fcntl(sock, F_SETFL, O_NONBLOCK);
		if (packets >= 256)
			return committed > 0 ? committed : -1;
	}
}

/* Warm mode (--warm): vpu_stream keeps the TCP connection + VPU decoder hot
 * but skips PrP+blit until SIGUSR1 fires, then returns to skip on SIGUSR2.
 * Show-to-visible latency = next I-frame (avg half a GOP).
 *
 *   g_show   set by SIGUSR1 / cleared by SIGUSR2 -- the master gate.
 *   g_armed  reset on each rising edge of g_show; set true on the first
 *            I-VOP that follows. Prevents a partial GOP from being shown
 *            (which would look like blocky garbage until the next I).
 */
static volatile sig_atomic_t g_show  = 1;     /* default: legacy "show always" */
static volatile sig_atomic_t g_armed = 1;
static volatile sig_atomic_t g_reload_csc = 1;
static int g_warm = 0;

static int pp_dims_supported(int src_w, int src_h, int dst_w, int dst_h)
{
	if (src_w < 32 || src_h < 32 || dst_w < 8 || dst_h < 8)
		return 0;
	if ((src_w & 7) || (src_h & 7))
		return 0;
	if ((dst_w & 1) || (dst_h & 1))
		return 0;
	return 1;
}

static void on_sigusr1(int s) { (void)s; g_show = 1; g_armed = 0; }
static void on_sigusr2(int s) { (void)s; g_show = 0; }
static void on_sighup(int s) { (void)s; g_reload_csc = 1; }
static void on_sigalrm(int s)
{
	static const char msg[] = "vpu_stream: libvpu call timed out; exiting\n";

	(void)s;
	write(STDERR_FILENO, msg, sizeof(msg) - 1);
	_exit(124);
}

static RetCode dec_get_initial_info_guarded(DecHandle h, DecInitialInfo *ii)
{
	RetCode rc;

	alarm(5);
	rc = vpu_DecGetInitialInfo(h, ii);
	alarm(0);
	return rc;
}

static RetCode dec_close_guarded(DecHandle h)
{
	RetCode rc;

	alarm(5);
	rc = vpu_DecClose(h);
	alarm(0);
	return rc;
}

static int feed(DecHandle h, int block)
{
	PhysicalAddress rd, wr; Uint32 space;
	int off, chunk, n;
	if (vpu_DecGetBitstreamBuffer(h, &rd, &wr, &space) != RETCODE_SUCCESS)
		return -1;
	if (space < 512) return -1;
	off = (int)(wr - bs.phy_addr);
	chunk = STREAM_BUF_SIZE - off;
	if (chunk > (int)space) chunk = space;
	if (chunk > FEED_CHUNK_SIZE) chunk = FEED_CHUNK_SIZE;
	if (block) {
		/* Never block forever: a source that stops sending but leaves the
		 * TCP connection open would otherwise wedge here, so the decode
		 * loop never reaches the stall watchdog or the hide handling and
		 * the last frame freezes on screen (only a kill -9 cleared it).
		 * Wake every 300 ms so the loop can react. Return -2 (distinct from
		 * a hard error/EOF) so callers can tell "no data yet" from "stream
		 * gone" -- the prime phase must keep waiting on -2, not give up. */
		struct pollfd pfd = { sock, POLLIN, 0 };
		if (poll(&pfd, 1, 300) <= 0) return -2;   /* timeout: no data yet */
	}
	fcntl(sock, F_SETFL, block ? 0 : O_NONBLOCK);
	n = recv(sock, (void *)(bs.virt_uaddr + off), chunk, 0);
	if (n > 0) { vpu_DecUpdateBitstreamBuffer(h, n); return n; }
	if (n == 0) return 0;        /* peer closed */
	return -1;                   /* EAGAIN */
}

static int bitstream_used(DecHandle h)
{
	PhysicalAddress rd, wr;
	Uint32 space;

	if (vpu_DecGetBitstreamBuffer(h, &rd, &wr, &space) != RETCODE_SUCCESS)
		return STREAM_BUF_SIZE;
	return STREAM_BUF_SIZE - (int)space - 1;
}

static int feed_until_watermark(DecHandle h, int target_used, int max_reads)
{
	int total = 0;
	int reads;

	for (reads = 0; reads < max_reads; reads++) {
		int used = bitstream_used(h);
		int n;

		if (used >= target_used)
			break;
		n = feed(h, 0);
		if (n > 0) {
			total += n;
			continue;
		}
		if (n == 0)
			return total > 0 ? total : 0;
		break;
	}
	return total > 0 ? total : -1;
}

/* When the VPU can't keep up with the incoming stream, the TCP socket buffer
 * grows without bound -> delay grows to tens of seconds. Drain the backlog,
 * resync to the most recent I-VOP, and feed from there. Skipping to any
 * other VOP type (P/B) would leave the decoder without its reference frame
 * -> visible blocky garbage until the next I (a whole GOP). If no I-VOP
 * is in the drained data, we push everything through unchanged -- one frame
 * of extra lag is far less visible than a smear of broken macroblocks. */
static int start_code_len(const unsigned char *p, int i, int total)
{
	if (i + 3 < total && p[i] == 0x00 && p[i+1] == 0x00 &&
	    p[i+2] == 0x00 && p[i+3] == 0x01)
		return 4;
	if (i + 2 < total && p[i] == 0x00 && p[i+1] == 0x00 &&
	    p[i+2] == 0x01)
		return 3;
	return 0;
}

static int find_resync_start(const unsigned char *p, int total)
{
	int i;

	if (g_codec == STD_AVC) {
		int param = -1, start_at = -1;

		for (i = 0; i + 4 < total; i++) {
			int sc = start_code_len(p, i, total);
			int nal;

			if (!sc || i + sc >= total)
				continue;
			nal = p[i + sc] & 0x1f;
			if (nal == 7 || nal == 8)
				param = i;
			else if (nal == 5)
				start_at = param >= 0 ? param : i;
		}
		return start_at;
	}

	/* MPEG-4 VOP start = 00 00 01 B6; vop_coding_type lives in the top two
	 * bits of the byte that follows. 00 = I-VOP (key frame). */
	for (i = total - 5; i >= 0; i--) {
		if (p[i]   == 0x00 && p[i+1] == 0x00 &&
		    p[i+2] == 0x01 && p[i+3] == 0xB6 &&
		    (p[i+4] & 0xC0) == 0x00)
			return i;
	}
	return -1;
}

/* Write a drained chunk into the VPU bitstream ring at the current write
 * pointer, wrapping at the ring end. TCP delivers a lossless byte stream;
 * dropping any part of a drained chunk splices a hole mid-GOP and corrupts
 * every frame until the next I-VOP, so the whole chunk must be committed. */
static int bs_ring_write(DecHandle h, const unsigned char *p, int len)
{
	PhysicalAddress rd, wr; Uint32 space;
	int off, written = 0;

	if (len <= 0)
		return 0;
	if (vpu_DecGetBitstreamBuffer(h, &rd, &wr, &space) != RETCODE_SUCCESS)
		return -1;
	if (len > (int)space) {
		/* The main loop keeps the ring under ~256K used and the drain
		 * buffer is 256K, so this cannot fire unless those invariants
		 * change. Truncating splices a hole, so log it loudly. */
		printf("skip refeed truncated: %d > ring space %u\n",
		       len, (unsigned)space);
		fflush(stdout);
		len = (int)space;
	}
	off = (int)(wr - bs.phy_addr);
	while (written < len) {
		int chunk = STREAM_BUF_SIZE - off;
		if (chunk > len - written)
			chunk = len - written;
		memcpy((void *)(bs.virt_uaddr + off), p + written, chunk);
		written += chunk;
		off = 0;
	}
	if (written > 0)
		vpu_DecUpdateBitstreamBuffer(h, written);
	return written;
}

static int feed_skip_to_latest(DecHandle h)
{
	static unsigned char tmp[DRAIN_BUF_SIZE];
	int total, start_at;
	int pending;
	int used;

	/* MPEG-4 can recover cleanly when we flush stale compressed bytes and
	 * restart at an I-VOP. On CODA DX6 H.264 this can leave reference/DPB
	 * state inconsistent and shows up as green/blue macroblocks. Prefer
	 * bounded latency from small socket/VPU buffers over corrupt pictures. */
	if (g_codec == STD_AVC)
		return 0;

	/* Drop the threshold (was 16k) so we re-sync to the latest frame on
	 * even a small backlog -- the goal here is *low* latency, not max fps.
	 * 2k is roughly one ~320x180 MPEG-4 P-frame at our bitrate, so any
	 * accumulated B/P-frame chain triggers a skip. */
	if (ioctl(sock, FIONREAD, &pending) < 0)
		return 0;
	used = bitstream_used(h);
	if (pending < 32768 && used < 256 * 1024)
		return 0;   /* not enough backlog to justify dropping frames */

	fcntl(sock, F_SETFL, O_NONBLOCK);
	total = 0;
	while (total < (int)sizeof(tmp)) {
		int n = recv(sock, tmp + total, sizeof(tmp) - total, 0);
		if (n <= 0) break;
		total += n;
	}
	if (total <= 0) return 0;

	start_at = find_resync_start(tmp, total);
	/* No random-access point visible in the drained window (a D1-bitrate
	 * GOP can exceed the 256K drain buffer). Feed the bytes through
	 * unchanged: dropping them would splice a mid-GOP hole into a lossless
	 * TCP stream and show blocky corruption until the next I-VOP. Latency
	 * gets trimmed at the next skip that does land on an I-VOP. */
	if (start_at < 0)
		return bs_ring_write(h, tmp, total);

	/* We found a clean random-access point in the socket backlog. Flush stale
	 * compressed bytes that were already hoarded in the VPU ring, then feed
	 * only from that point onward. */
	vpu_DecBitBufferFlush(h);
	return bs_ring_write(h, tmp + start_at, total - start_at);
}

#define CFG_PATH "/mnt/data/toonui.cfg"

static char *trim_ws(char *s)
{
	char *e;

	while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
		s++;
	e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' ||
	                 e[-1] == '\r' || e[-1] == '\n'))
		*--e = 0;
	return s;
}

static int clamp_int(int v, int lo, int hi)
{
	if (v < lo) return lo;
	if (v > hi) return hi;
	return v;
}

static int cfg_get_int(const char *name, int def, int lo, int hi)
{
	FILE *f;
	char line[256];
	int val = def;

	f = fopen(CFG_PATH, "r");
	if (!f)
		return def;

	while (fgets(line, sizeof(line), f)) {
		char *k = trim_ws(line);
		char *eq, *v;

		if (!*k || *k == '#')
			continue;
		eq = strchr(k, '=');
		if (!eq)
			continue;
		*eq = 0;
		v = trim_ws(eq + 1);
		k = trim_ws(k);
		if (!strcmp(k, name)) {
			val = atoi(v);
			break;
		}
	}
	fclose(f);
	return clamp_int(val, lo, hi);
}

static int set_codec_name(const char *name)
{
	if (!strcmp(name, "mpeg4") || !strcmp(name, "mp4") ||
	    !strcmp(name, "m4v")) {
		g_codec = STD_MPEG4;
		g_codec_name = "mpeg4";
		return 0;
	}
	if (!strcmp(name, "h264") || !strcmp(name, "avc")) {
		g_codec = STD_AVC;
		g_codec_name = "h264";
		return 0;
	}
	return -1;
}

static void print_help(const char *argv0)
{
	printf("Usage: %s [options] [tcp-port]\n", argv0);
	printf("\n");
	printf("Inputs:\n");
	printf("  [tcp-port]              Listen for raw elementary stream over TCP (default 5000)\n");
	printf("  --rtp PORT              Listen for RTP/H.264 or RTP/MPEG-4 over UDP\n");
	printf("  --rtsp URL              Pull H.264 from an RTSP source\n");
	printf("  --sprop VALUE           H.264 RTP sprop-parameter-sets value\n");
	printf("\n");
	printf("Codec/options:\n");
	printf("  --codec mpeg4|h264      Select decoder (default from camera_codec or mpeg4)\n");
	printf("  --rect X Y W H          Output rectangle on fb0 (default: centred 1:1)\n");
	printf("  --warm                  Decode continuously, show only after SIGUSR1\n");
	printf("  --prebuffer KB          Poll-wait for data when bitstream buffer below threshold\n");
	printf("  --pp-deblock            Enable PP post-deblocking filter (needs QP from decoder)\n");
	printf("  --cpu-render            CPU YUV420->RGB565 path for comparison\n");
	printf("  --dec-iram              Experimental: enable i.MX27 decoder IRAM registers\n");
	printf("  --no-dec-iram           Disable decoder IRAM even if configured\n");
	printf("\n");
	printf("Benchmark output modes:\n");
	printf("  --decode-only           Decode and release frames; no PP/PrP, no fb writes\n");
	printf("  --pp-dummy              Run PP/PrP to an off-screen RGB565 DMA buffer; no fb writes\n");
	printf("  --fb-output             Default: PP/PrP output to framebuffer\n");
	printf("\n");
	printf("Runtime:\n");
	printf("  SIGHUP                  Reload PP CSC values from %s\n", CFG_PATH);
	printf("  SIGUSR1/SIGUSR2         Show/hide when --warm is active\n");
}

static void load_codec_config(void)
{
	FILE *f;
	char line[160];

	f = fopen(CFG_PATH, "r");
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		char *k = trim_ws(line);
		char *eq, *v;

		if (!*k || *k == '#')
			continue;
		eq = strchr(k, '=');
		if (!eq)
			continue;
		*eq = 0;
		v = trim_ws(eq + 1);
		k = trim_ws(k);
		if (!strcmp(k, "camera_codec")) {
			if (set_codec_name(v) < 0)
				printf("unknown camera_codec '%s'; using %s\n",
				       v, g_codec_name);
			break;
		}
	}
	fclose(f);
}

static unsigned int scale_coeff(unsigned int base, int pct, unsigned int max)
{
	unsigned int v;

	pct = clamp_int(pct, 0, 200);
	v = (base * (unsigned int)pct + 50) / 100;
	return v > max ? max : v;
}

static int parse_pp_csc_csv(const char *v, struct vpu_pp_csc *csc)
{
	int c0, c1, c2, c3, c4, x0;

	if (sscanf(v, "%i,%i,%i,%i,%i,%i", &c0, &c1, &c2, &c3, &c4, &x0) != 6)
		return -1;
	csc->c0 = (unsigned int)c0; csc->c1 = (unsigned int)c1;
	csc->c2 = (unsigned int)c2; csc->c3 = (unsigned int)c3;
	csc->c4 = (unsigned int)c4; csc->x0 = (unsigned int)x0;
	return 0;
}

static void load_pp_csc_config(struct vpu_pp_csc *csc)
{
	FILE *f;
	char line[160];
	int luma = 100, sat = 100, red = 100, green = 100, blue = 100;
	int raw[6];
	int i;

	csc->c0 = 0x95; csc->c1 = 0xcc; csc->c2 = 0x32;
	csc->c3 = 0x68; csc->c4 = 0x104; csc->x0 = 1;
	for (i = 0; i < 6; i++)
		raw[i] = -1;

	f = fopen(CFG_PATH, "r");
	if (!f)
		return;

	while (fgets(line, sizeof(line), f)) {
		char *k = trim_ws(line);
		char *eq;
		char *v;

		if (!*k || *k == '#')
			continue;
		eq = strchr(k, '=');
		if (!eq)
			continue;
		*eq = 0;
		v = trim_ws(eq + 1);
		k = trim_ws(k);

		if (!strcmp(k, "camera_pp_luma_pct")) luma = atoi(v);
		else if (!strcmp(k, "camera_pp_sat_pct")) sat = atoi(v);
		else if (!strcmp(k, "camera_pp_red_pct")) red = atoi(v);
		else if (!strcmp(k, "camera_pp_green_pct")) green = atoi(v);
		else if (!strcmp(k, "camera_pp_blue_pct")) blue = atoi(v);
		else if (!strcmp(k, "camera_pp_c0")) raw[0] = (int)strtoul(v, NULL, 0);
		else if (!strcmp(k, "camera_pp_c1")) raw[1] = (int)strtoul(v, NULL, 0);
		else if (!strcmp(k, "camera_pp_c2")) raw[2] = (int)strtoul(v, NULL, 0);
		else if (!strcmp(k, "camera_pp_c3")) raw[3] = (int)strtoul(v, NULL, 0);
		else if (!strcmp(k, "camera_pp_c4")) raw[4] = (int)strtoul(v, NULL, 0);
		else if (!strcmp(k, "camera_pp_x0")) raw[5] = (int)strtoul(v, NULL, 0);
		else if (!strcmp(k, "camera_pp_csc")) parse_pp_csc_csv(v, csc);
	}
	fclose(f);

	luma = clamp_int(luma, 0, 200);
	sat = clamp_int(sat, 0, 200);
	red = clamp_int((sat * clamp_int(red, 0, 200) + 50) / 100, 0, 200);
	green = clamp_int((sat * clamp_int(green, 0, 200) + 50) / 100, 0, 200);
	blue = clamp_int((sat * clamp_int(blue, 0, 200) + 50) / 100, 0, 200);

	csc->c0 = scale_coeff(0x95, luma, 0xff);
	csc->c1 = scale_coeff(0xcc, red, 0xff);
	csc->c2 = scale_coeff(0x32, green, 0xff);
	csc->c3 = scale_coeff(0x68, green, 0xff);
	csc->c4 = scale_coeff(0x104, blue, 0x1ff);
	csc->x0 = 1;

	if (raw[0] >= 0) csc->c0 = (unsigned int)raw[0];
	if (raw[1] >= 0) csc->c1 = (unsigned int)raw[1];
	if (raw[2] >= 0) csc->c2 = (unsigned int)raw[2];
	if (raw[3] >= 0) csc->c3 = (unsigned int)raw[3];
	if (raw[4] >= 0) csc->c4 = (unsigned int)raw[4];
	if (raw[5] >= 0) csc->x0 = (unsigned int)raw[5];
}

static void apply_pp_csc_config(int fd)
{
	struct vpu_pp_csc csc;

	load_pp_csc_config(&csc);
	if (ioctl(fd, VPU_IOC_PP_SET_CSC, &csc) == 0) {
		printf("PP CSC: c0=0x%x c1=0x%x c2=0x%x c3=0x%x c4=0x%x x0=%u\n",
		       csc.c0, csc.c1, csc.c2, csc.c3, csc.c4, csc.x0);
	} else {
		printf("PP CSC set failed (errno=%d); using kernel default\n", errno);
	}
	fflush(stdout);
}

int main(int argc, char **argv)
{
	/* Install signal handlers FIRST. The parent (camera.c) may send
	 * SIGUSR1 immediately after fork+exec (post-respawn re-arm if the
	 * user had the camera tile open when the prior child died); if
	 * that signal arrives before we install the handler, the default
	 * action is to terminate. on_sigusr1/2 are pure flag-setters --
	 * harmless to install before we know whether --warm was passed. */
	signal(SIGUSR1, on_sigusr1);
	signal(SIGUSR2, on_sigusr2);
	signal(SIGHUP, on_sighup);
	signal(SIGALRM, on_sigalrm);

	int port = 5000;
	int codec_from_arg = 0;
	char *rtsp_source_url = NULL;
	int rtp_listen_port = 0;
	int dec_iram_arg = -1;
	unsigned char rtp_spspps[512];
	int rtp_spspps_len = 0;
	/* Optional --rect X Y W H: render at (X,Y) sized W x H instead of the
	 * decoded-and-centred default. eMMA PrP does the resize bilinearly in
	 * hardware, so picking a smaller W/H is essentially free CPU-wise.
	 * Used by toonui to embed live video in a card on the home screen. */
	int rect_x = -1, rect_y = -1, rect_w = -1, rect_h = -1;
	for (int ai = 1; ai < argc; ai++) {
		if (!strcmp(argv[ai], "--help") || !strcmp(argv[ai], "-h")) {
			print_help(argv[0]);
			return 0;
		} else if (!strcmp(argv[ai], "--rect") && ai + 4 < argc) {
			rect_x = atoi(argv[ai+1]); rect_y = atoi(argv[ai+2]);
			rect_w = atoi(argv[ai+3]); rect_h = atoi(argv[ai+4]);
			ai += 4;
		} else if (!strcmp(argv[ai], "--codec") && ai + 1 < argc) {
			if (set_codec_name(argv[ai+1]) < 0) {
				fprintf(stderr, "unknown codec '%s' (use mpeg4 or h264)\n",
				        argv[ai+1]);
				return 1;
			}
			codec_from_arg = 1;
			ai++;
		} else if (!strcmp(argv[ai], "--rtsp") && ai + 1 < argc) {
			rtsp_source_url = argv[ai + 1];
			ai++;
		} else if (!strcmp(argv[ai], "--rtp") && ai + 1 < argc) {
			rtp_listen_port = atoi(argv[ai + 1]);
			ai++;
		} else if (!strcmp(argv[ai], "--cpu-render")) {
			g_cpu_render = 1;
		} else if (!strcmp(argv[ai], "--decode-only")) {
			g_output_mode = OUTPUT_DECODE_ONLY;
		} else if (!strcmp(argv[ai], "--pp-dummy")) {
			g_output_mode = OUTPUT_PP_DUMMY;
		} else if (!strcmp(argv[ai], "--fb-output")) {
			g_output_mode = OUTPUT_FB;
		} else if (!strcmp(argv[ai], "--dec-iram")) {
			dec_iram_arg = 1;
		} else if (!strcmp(argv[ai], "--no-dec-iram")) {
			dec_iram_arg = 0;
		} else if (!strcmp(argv[ai], "--sprop") && ai + 1 < argc) {
			if (parse_h264_sprop(argv[ai + 1], rtp_spspps,
					     &rtp_spspps_len) < 0) {
				fprintf(stderr, "invalid --sprop '%s'\n", argv[ai + 1]);
				return 1;
			}
			ai++;
		} else if (!strcmp(argv[ai], "--prebuffer") && ai + 1 < argc) {
				g_prebuffer = atoi(argv[ai + 1]) * 1024;
				ai++;
			} else if (!strcmp(argv[ai], "--pp-deblock")) {
				g_pp_deblock = 1;
			} else if (!strcmp(argv[ai], "--warm")) {
			/* Start hidden: decode normally, but skip PrP+blit until
			 * SIGUSR1 fires. Used by toonui so the doorbell tile opens
			 * with zero startup latency -- on signal we wait for the next
			 * I-VOP then start displaying. SIGUSR2 returns to hidden. */
			g_warm  = 1;
			g_show  = 0;
			g_armed = 0;
		} else if (!strcmp(argv[ai], "--overlay")) {
			g_overlay = 1;
		} else if (argv[ai][0] != '-') {
			port = atoi(argv[ai]);
		} else {
			fprintf(stderr, "unknown option '%s'\n", argv[ai]);
			fprintf(stderr, "run '%s --help' for usage\n", argv[0]);
			return 1;
		}
	}
	if (!codec_from_arg)
		load_codec_config();
	if (!g_pp_deblock)
		g_pp_deblock = cfg_get_int("camera_pp_deblock", 0, 0, 1);
	if (dec_iram_arg < 0)
		dec_iram_arg = cfg_get_int("camera_vpu_dec_iram", -1, -1, 1);
	if (dec_iram_arg >= 0)
		setenv("VPU_DEC_IRAM", dec_iram_arg ? "1" : "0", 1);
	if (g_cpu_render && g_output_mode != OUTPUT_FB) {
		fprintf(stderr, "--cpu-render only makes sense with --fb-output\n");
		return 1;
	}
	if (rtsp_source_url && !codec_from_arg)
		set_codec_name("h264");
	if (rtsp_source_url && rtp_listen_port) {
		fprintf(stderr, "use only one of --rtsp or --rtp\n");
		return 1;
	}
	if (rtsp_source_url && g_codec != STD_AVC) {
		fprintf(stderr, "--rtsp currently supports only --codec h264\n");
		return 1;
	}
	if (rtp_listen_port && g_codec != STD_AVC && g_codec != STD_MPEG4) {
		fprintf(stderr, "--rtp currently supports only --codec h264 or mpeg4\n");
		return 1;
	}
	int has_rect = (rect_w > 0 && rect_h > 0);

	int lsock, one = 1, i;
	struct sockaddr_in addr;
	int fb_fd, fb_stride, fb_w, fb_h, fb_bpp;
	int fb_r_off = 11, fb_g_off = 5, fb_b_off = 0;   /* RGB565 defaults */
	int fb_r_len = 5, fb_g_len = 6, fb_b_len = 5;
	struct fb_var_screeninfo vinfo;
	struct fb_fix_screeninfo finfo;
	unsigned long fb_phys = 0;     /* /dev/fb0's smem_start; PrP writes here directly on 16bpp */
	unsigned char *fb_map;
	DecOpenParam op; DecInitialInfo ii; DecBufInfo binfo;
	vpu_mem_desc fbmem[32] = {{0}}, rgbmem = {0};
	FrameBuffer fb[32];
	int fbcount = 0, stride = 0, ah = 0, ysize = 0, csize = 0, mvsize = 0;
	int prp_fd, fb_alloced = 0, blit_bytes = 0;
	int pp_ok = 1;
	unsigned int *row_scratch = NULL;   /* one row of 32bpp, cached DRAM */

	signal(SIGPIPE, SIG_IGN);

	setpriority(PRIO_PROCESS, 0, 10);

	lsock = -1;
	if (!rtsp_source_url && !rtp_listen_port) {
		lsock = socket(AF_INET, SOCK_STREAM, 0);
		setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
		/* If a previous vpu_stream is still in TIME_WAIT, SO_REUSEADDR
		 * alone is sometimes insufficient on this old kernel. SO_REUSEPORT
		 * lets the new bind go through regardless. */
		setsockopt(lsock, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif
		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET; addr.sin_addr.s_addr = INADDR_ANY; addr.sin_port = htons(port);
		if (bind(lsock, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
		listen(lsock, 1);
	}

	if (g_overlay) {
		/* Overlay mode: drive the fb1 (DISP0 FG) plane. Force 16bpp (the
		 * FG always fetches 16bpp; 32bpp shows wrong colors) but DO NOT
		 * resize it -- the mx2fb driver mangles FG resizes (768->752),
		 * which corrupts the stride and breaks the picture. Use the
		 * plane's NATIVE size and position the video inside it (same as
		 * the fb0 cutout). GW is full-screen; the surround is black. */
		struct fb_var_screeninfo ov;
		struct fb_fix_screeninfo of;
		ov_fd = open("/dev/fb1", O_RDWR);
		if (ov_fd < 0) { perror("open /dev/fb1"); return 1; }
		fb_fd = ov_fd;                 /* cleanup paths close fb_fd */
		ioctl(ov_fd, FBIOGET_VSCREENINFO, &ov);
		/* Prime the FG with a 32->16 bpp toggle. fb1 boots at 32bpp; going
		 * straight to 16bpp updates the buffer (line=1600) but the graphic
		 * window keeps fetching with the 32bpp line stride (3200 = 2x), so
		 * the 16bpp image is squashed into the upper half. Passing through
		 * 32bpp first forces the GW stride latch to reprogram to 16bpp
		 * (verified on-device: 16 squashed, 32 full, then 16 full). */
		ov.bits_per_pixel = 32;
		ov.activate = FB_ACTIVATE_NOW;
		if (ioctl(ov_fd, FBIOPUT_VSCREENINFO, &ov) < 0)
			perror("fb1 prime 32bpp");
		ioctl(ov_fd, FBIOGET_VSCREENINFO, &ov);
		ov.bits_per_pixel = 16;
		ov.red.offset = 11;  ov.red.length = 5;
		ov.green.offset = 5; ov.green.length = 6;
		ov.blue.offset = 0;  ov.blue.length = 5;
		ov.transp.offset = 0; ov.transp.length = 0;
		ov.activate = FB_ACTIVATE_NOW;   /* bpp only -- keep xres/yres */
		if (ioctl(ov_fd, FBIOPUT_VSCREENINFO, &ov) < 0)
			perror("fb1 set 16bpp");
		ioctl(ov_fd, FBIOGET_VSCREENINFO, &ov);
		ioctl(ov_fd, FBIOGET_FSCREENINFO, &of);
		fb_w = ov.xres; fb_h = ov.yres; fb_bpp = 16;
		fb_phys = of.smem_start;
	/* The kernel's mx2_gw_set() hardcodes default_bpp=32 for LGWVPWR,
	 * so the GW reads 800*32/8/4 = 800 words = 3200 bytes per line
	 * regardless of fb1's actual bpp.  Match that stride or the image
	 * is vertically squashed into the upper half of the screen. */
	fb_stride = fb_w * 4;                /* match GW LGWVPWR (3200) */
		fb_map = mmap(NULL, of.smem_len, PROT_READ | PROT_WRITE,
			      MAP_SHARED, ov_fd, 0);
		if (fb_map == MAP_FAILED) { perror("mmap fb1"); return 1; }
		ov_keyfill(fb_map, fb_stride, fb_w, fb_h);  /* border = transparent key */
		ov_xpos = 0; ov_ypos = 0;     /* GW full-screen; video positioned inside */
		gw_set(0);                    /* start hidden (GW off) */
		printf("overlay: fb1 native %dx%d 16bpp phys=0x%lx stride=%d (GW full-screen)\n",
		       fb_w, fb_h, (unsigned long)fb_phys, fb_stride);
	} else {
	fb_fd = open("/dev/fb0", O_RDWR);
	ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo);
	/* finfo.smem_start = fb's physical address. On 16bpp targets we point
	 * the eMMA PrP straight at it (cutout offset baked in) so the YUV->
	 * RGB565 conversion lands in /dev/fb0 with NO CPU memcpy at all --
	 * saves the per-frame blit (~5-10 ms at 720x480). 32bpp targets still
	 * need a CPU widening pass since PrP only outputs RGB565. */
	ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo);
	fb_phys = finfo.smem_start;
	fb_w = vinfo.xres; fb_h = vinfo.yres; fb_bpp = vinfo.bits_per_pixel;
	fb_stride = fb_w * (fb_bpp / 8);
	fb_map = mmap(NULL, fb_stride * fb_h, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
	if (fb_bpp == 32) {
		fb_r_off = vinfo.red.offset;   fb_r_len = vinfo.red.length;
		fb_g_off = vinfo.green.offset; fb_g_len = vinfo.green.length;
		fb_b_off = vinfo.blue.offset;  fb_b_len = vinfo.blue.length;
	}
	printf("fb0: %dx%d %dbpp phys=0x%lx R@%d/%d G@%d/%d B@%d/%d stride=%d\n",
		   fb_w, fb_h, fb_bpp, fb_phys, fb_r_off, fb_r_len, fb_g_off, fb_g_len,
		   fb_b_off, fb_b_len, fb_stride);
	if (fb_bpp != 16 && fb_bpp != 32) {
		fprintf(stderr, "fb0 bpp %d not supported (need 16 or 32)\n", fb_bpp);
		return 1;
	}
	}

	if (vpu_Init(NULL) != RETCODE_SUCCESS) { fprintf(stderr, "vpu_Init fail\n"); return 1; }
	bs.size = STREAM_BUF_SIZE;
	if (IOGetPhyMem(&bs) || IOGetVirtMem(&bs) <= 0) { fprintf(stderr, "bs alloc\n"); return 1; }
	prp_fd = open("/dev/mxc_vpu", O_RDWR);
	apply_pp_csc_config(prp_fd);
	g_reload_csc = 0;
	if (rtsp_source_url)
		printf("vpu_stream ready; codec=%s mode=%s dec_iram=%s rtsp source\n",
		       g_codec_name, output_mode_name(),
		       getenv("VPU_DEC_IRAM") && strcmp(getenv("VPU_DEC_IRAM"), "0") ? "on" : "off");
	else if (rtp_listen_port)
		printf("vpu_stream ready; codec=%s mode=%s dec_iram=%s rtp udp/%d\n",
		       g_codec_name, output_mode_name(),
		       getenv("VPU_DEC_IRAM") && strcmp(getenv("VPU_DEC_IRAM"), "0") ? "on" : "off",
		       rtp_listen_port);
	else
		printf("vpu_stream ready; codec=%s mode=%s dec_iram=%s deblock=%s prebuf=%dK listening tcp/%d\n",
		       g_codec_name, output_mode_name(),
		       getenv("VPU_DEC_IRAM") && strcmp(getenv("VPU_DEC_IRAM"), "0") ? "on" : "off",
		       g_pp_deblock ? "on" : "off",
		       g_prebuffer / 1024, port);
	fflush(stdout);

	/* ---- per-connection loop ----
	 * Long-lived: keeps vpu_Init's single VPU init across reconnects (a
	 * fresh vpu_Init in a respawned process turned out to corrupt the
	 * kernel's clock refcount + wedge the eMMA after a few cycles).
	 * Reconnect handling: vpu_DecClose, accept again, vpu_DecOpen, prime. */
	for (;;) {
		DecHandle h;
		int inited = 0, off_x, off_y, frames = 0;
		long last_data_ms;
		struct h264_rtp_state rtp_direct_state;

		memset(&rtp_direct_state, 0, sizeof(rtp_direct_state));
		last_data_ms = now_ms();

		if (rtsp_source_url) {
			int sv[2];
			pthread_t tid;
			struct rtsp_source *src;

			printf("connecting rtsp source...\n"); fflush(stdout);
			if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
				perror("socketpair");
				break;
			}
			src = calloc(1, sizeof(*src));
			if (!src) {
				close(sv[0]); close(sv[1]);
				break;
			}
			snprintf(src->url, sizeof(src->url), "%s", rtsp_source_url);
			src->out_fd = sv[1];
			if (pthread_create(&tid, NULL, rtsp_thread_main, src) != 0) {
				close(sv[0]); close(sv[1]); free(src);
				continue;
			}
			pthread_detach(tid);
			sock = sv[0];
		} else if (rtp_listen_port) {
			printf("rtp direct listening udp/%d\n", rtp_listen_port);
			fflush(stdout);
			sock = rtp_open_udp(rtp_listen_port);
			if (sock < 0)
				break;
			if (rtp_spspps_len > 0) {
				rtsp_log_h264_bytes("sprop", rtp_spspps, rtp_spspps_len);
				rtsp_log_h264_sps_profile(rtp_spspps, rtp_spspps_len);
			}
			printf("rtp waiting for packets\n");
			fflush(stdout);
		} else {
			printf("waiting for stream...\n"); fflush(stdout);
			sock = accept(lsock, NULL, NULL);
			if (sock < 0) continue;
		}
		/* Stale-socket guard. If the OPi rapidly reconnects (kill+
		 * restart of its ffmpeg, or a TCP RST that didn't deliver
		 * its FIN cleanly), our accept queue can have a leftover
		 * mid-stream socket sitting in front of the fresh one. Take
		 * the freshest by draining everything else still queued -- the
		 * mid-stream one has no sequence header so the prime phase
		 * would hang on it forever. Done with a NONBLOCK toggle on
		 * lsock so any queued sockets pop out as accept()=-1+EAGAIN. */
		if (!rtsp_source_url && !rtp_listen_port) {
			fcntl(lsock, F_SETFL, fcntl(lsock, F_GETFL, 0) | O_NONBLOCK);
			for (;;) {
				int next = accept(lsock, NULL, NULL);
				if (next < 0) break;
				printf("discarding stale queued socket; taking newer one\n");
				fflush(stdout);
				close(sock);
				sock = next;
			}
			fcntl(lsock, F_SETFL, fcntl(lsock, F_GETFL, 0) & ~O_NONBLOCK);
		}
		/* A fresh decoder needs a fresh I-frame before its output is
		 * trustworthy. If the prior connection was being shown, we'd
		 * happily blit the new decoder's first P-frame against an empty
		 * reference -> garbage. Re-arm on the next I after reconnect. */
		if (g_show) g_armed = 0;
		if (!rtsp_source_url && !rtp_listen_port)
			setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
		/* Cap the kernel's TCP receive buffer so it can't hoard frames on
		 * us. Linux's default is ~87 KB which holds several seconds of
		 * MPEG-4 at our bitrate. With ~32 KB the kernel itself bounds the
		 * latency to a fraction of a second; feed_skip_to_latest mops up
		 * the rest. */
		int rcvbuf = 32 * 1024;
		setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
		if (!rtsp_source_url && !rtp_listen_port) {
			/* 5s recv timeout: if the raw TCP stream wedges (peer
			 * silently dies, or sends partial-frame garbage forever),
			 * blocking feed(h,1) in the prime phase or block-fallback
			 * in decode would otherwise pin the decoder. Do not apply
			 * this to the RTSP/RTP socketpair: their source thread owns
			 * network liveness, and RTP must be allowed to sit idle
			 * before the first packet arrives. */
			struct timeval rcvto = { .tv_sec = 5, .tv_usec = 0 };
			setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &rcvto, sizeof rcvto);
		}
		last_data_ms = now_ms();
		if (rtp_listen_port)
			printf("rtp source ready\n");
		else
			printf("client connected\n");
		fflush(stdout);

		memset(&op, 0, sizeof(op));
		op.bitstreamFormat = g_codec;
		if (g_pp_deblock)
			op.qpReport = 1;
		op.bitstreamBuffer = bs.phy_addr;
		op.bitstreamBufferSize = bs.size;
		op.reorderEnable = 1;
		op.filePlayEnable = 0;
		if (g_codec == STD_AVC && !ps.phy_addr) {
			ps.size = 512 * 1024;
			if (IOGetPhyMem(&ps)) { fprintf(stderr, "ps alloc\n"); close(sock); continue; }
		}
		if (g_codec == STD_AVC) {
			op.psSaveBuffer = ps.phy_addr;
			op.psSaveBufferSize = ps.size;
		}
		if (vpu_DecOpen(&h, &op) != RETCODE_SUCCESS) { close(sock); continue; }

		/* Prime: feed until the sequence header parses. Bounded by a try
		 * cap (64 iters; the 300 ms feed() poll means a silent source can't
		 * spin faster than that, so this is also a ~19 s wall-clock ceiling).
		 * A no-data tick (feed == -2) does NOT abort -- a slow encoder may
		 * take a moment to send its first frame after connecting, and giving
		 * up there would close the socket under it (ffmpeg "Broken pipe").
		 * Only a real EOF/error closes + reconnects. */
		int prime_tries = 0;
		while (!inited && prime_tries++ < (rtp_listen_port ? 512 : 64)) {
			if (rtp_listen_port) {
				int fr = rtp_feed_direct(h, &rtp_direct_state,
							 rtp_spspps, rtp_spspps_len, 1);
				if (fr == 0)
					break;
				if (fr < 0)
					continue;
			} else {
				int fr = feed(h, 1);
				if (fr == 0 || fr == -1) break;  /* EOF / hard error -> reconnect */
				if (fr == -2) continue;          /* no data yet (slow stream
				                                  * start): keep waiting, don't
				                                  * close the connection */
			}
			vpu_DecSetEscSeqInit(h, 1);
			if (dec_get_initial_info_guarded(h, &ii) == RETCODE_SUCCESS) inited = 1;
			vpu_DecSetEscSeqInit(h, 0);
		}
		if (!inited) {
			printf("prime failed (%d tries, no sequence header); reconnecting\n",
			       prime_tries);
			fflush(stdout);
			dec_close_guarded(h); close(sock);
			if (rtp_listen_port && g_codec == STD_AVC) {
				printf("rtp prime failed; restarting process to release UDP receiver\n");
				fflush(stdout);
				close(prp_fd);
				vpu_UnInit();
				munmap(fb_map, fb_stride * fb_h);
				close(fb_fd);
				close(lsock);
				execv(argv[0], argv);
				execv("/proc/self/exe", argv);
				perror("execv");
				return 1;
			}
			continue;
		}
		printf("Video: %dx%d minFB=%d\n", ii.picWidth, ii.picHeight, ii.minFrameBufferCount);
		if (g_codec == STD_AVC) {
			printf("h264 crop: left=%lu top=%lu right=%lu bottom=%lu\n",
			       (unsigned long)ii.picCropRect.left,
			       (unsigned long)ii.picCropRect.top,
			       (unsigned long)ii.picCropRect.right,
			       (unsigned long)ii.picCropRect.bottom);
		}
		fflush(stdout);

		/* Output rect (where the video lands, and how big PP should resize
		 * each decoded frame to). PrP used to require downscale-only output,
		 * but PrP is now disabled by default in the kernel. PP handles the
		 * normal up/down resize cases, including 640x360 -> 800x450. */
		int src_visible_w = ii.picWidth;
		int src_visible_h = ii.picHeight;
		if (g_codec == STD_AVC &&
		    ii.picCropRect.left == 0 && ii.picCropRect.top == 0 &&
		    ii.picCropRect.right > 0 && ii.picCropRect.bottom > 0 &&
		    ii.picCropRect.right <= (int)ii.picWidth &&
		    ii.picCropRect.bottom <= (int)ii.picHeight) {
			src_visible_w = ii.picCropRect.right;
			src_visible_h = ii.picCropRect.bottom;
		}
	int orig_w = has_rect ? rect_w : 0;
		int orig_h = has_rect ? rect_h : 0;
		int out_w = has_rect ? rect_w : src_visible_w;
		int out_h = has_rect ? rect_h : src_visible_h;
		off_x = has_rect ? rect_x : (fb_w  - out_w) / 2;
		off_y = has_rect ? rect_y : (fb_h  - out_h) / 2;
		if (off_x < 0) off_x = 0;
		if (off_y < 0) off_y = 0;
		if (off_x + out_w > fb_w) out_w = fb_w - off_x;
		if (off_y + out_h > fb_h) out_h = fb_h - off_y;
		if (out_w < 2 || out_h < 2) {
			fprintf(stderr, "invalid output rect after clipping\n");
			dec_close_guarded(h); close(sock); continue;
		}

		/* Overlay mode keeps off_x/off_y as the video's position *inside*
		 * the full-screen FG plane (GW is full-screen at 0,0). The plane
		 * is black-filled below so the area around the video is a black
		 * border over the UI. */

		/* One-time blackfill of the *original* cutout area so any UI pixels
		 * left behind when the cutout was set don't show through where the
		 * (possibly smaller) video doesn't reach. Only runs when the rect
		 * was actually clamped. (Not in overlay mode: separate plane, and
		 * fb_map isn't mapped yet here.) */
		if (!g_overlay && g_output_mode == OUTPUT_FB &&
		    has_rect && (out_w < orig_w || out_h < orig_h)) {
			int bpp = fb_bpp / 8;
			for (int yy = 0; yy < orig_h && (rect_y + yy) < fb_h; yy++) {
				int w_clip = orig_w;
				if (rect_x + w_clip > fb_w) w_clip = fb_w - rect_x;
				if (w_clip > 0) {
					memset(fb_map + (rect_y + yy) * fb_stride + rect_x * bpp,
						   0, (size_t)w_clip * bpp);
				}
			}
			printf("[rect] requested %dx%d clipped to %dx%d\n",
				   orig_w, orig_h, out_w, out_h);
			fflush(stdout);
		}

		if (g_output_mode != OUTPUT_DECODE_ONLY &&
		    g_codec == STD_AVC &&
		    !pp_dims_supported(src_visible_w, src_visible_h, out_w, out_h)) {
			printf("unsupported H.264 PP geometry: src=%dx%d dst=%dx%d; reconnecting\n",
			       src_visible_w, src_visible_h, out_w, out_h);
			fflush(stdout);
			dec_close_guarded(h);
			close(sock);
			if (g_codec == STD_AVC) {
				close(prp_fd);
				vpu_UnInit();
				munmap(fb_map, fb_stride * fb_h);
				close(fb_fd);
				close(lsock);
				execv(argv[0], argv);
				execv("/proc/self/exe", argv);
				perror("execv");
				return 1;
			}
			continue;
		}

		pp_ok = 1;

		if (!fb_alloced) {       /* allocate frame + RGB buffers once (fixed res) */
			int fb_extra;

			stride = ALIGN16(ii.picWidth); ah = ALIGN16(ii.picHeight);
			ysize = stride * ah; csize = (stride / 2) * (ah / 2);
			mvsize = ALIGN16(ii.picWidth / 16) * ALIGN16(ii.picHeight / 16) * 8;
			fb_extra = cfg_get_int("camera_fb_extra", 4, 0, 24);
			/* minFB+N. Gives the decoder more headroom; if it
			 * ever stalls waiting for a buffer release, this hides it.
			 * Cheap (~2 extra MB at 720x480 YUV420). */
			fbcount = ii.minFrameBufferCount + fb_extra; if (fbcount > 32) fbcount = 32;
			blit_bytes = out_w * 2;
			for (i = 0; i < fbcount; i++) {
				fbmem[i].size = ysize + 2 * csize + mvsize;
				if (IOGetPhyMem(&fbmem[i])) { fprintf(stderr, "fb %d\n", i); return 1; }
				if (g_cpu_render && IOGetVirtMem(&fbmem[i]) <= 0) {
					fprintf(stderr, "fb virt %d\n", i);
					return 1;
				}
				fb[i].strideY = stride; fb[i].strideC = stride / 2;
				fb[i].bufY = fbmem[i].phy_addr;
				fb[i].bufCb = fb[i].bufY + ysize;
				fb[i].bufCr = fb[i].bufCb + csize;
				fb[i].bufMvCol = fb[i].bufCr + csize;
			}
			/* RGB565 bounce buffer is needed for the 32bpp fb path
			 * (where we have to widen on the CPU), and for --pp-dummy
			 * where PP/PrP writes off-screen so LCD/fb writes are excluded. */
			if (fb_bpp == 32 || g_output_mode == OUTPUT_PP_DUMMY) {
				rgbmem.size = out_w * out_h * 2;
				if (IOGetPhyMem(&rgbmem)) { fprintf(stderr, "rgb\n"); return 1; }
				if (fb_bpp == 32) {
					if (IOGetVirtMem(&rgbmem) <= 0) { fprintf(stderr, "rgb virt\n"); return 1; }
					row_scratch = malloc(out_w * 4);
					if (!row_scratch) { fprintf(stderr, "row_scratch\n"); return 1; }
				}
			}
			/* (overlay fb1 is set up once at startup, not resized here) */
			fb_alloced = 1;
			printf("buffers: fbcount=%d fbextra=%d fbsize=%dK streambuf=%dK workbuf=0x%lx\n",
			       fbcount, fb_extra, fbmem[0].size / 1024,
			       STREAM_BUF_SIZE / 1024, (unsigned long)bit_work_addr.size);
			printf("output mode: %s%s\n", output_mode_name(),
			       g_cpu_render ? " (cpu-render)" : "");
			printf("output rect: %dx%d at (%d,%d)%s\n", out_w, out_h, off_x, off_y,
			       g_output_mode == OUTPUT_DECODE_ONLY ? " (not rendered)" :
			       g_output_mode == OUTPUT_PP_DUMMY ? " (off-screen RGB565)" :
			       has_rect ? " (hw-resized)" : " (1:1, centred)");
			fflush(stdout);
		}
		memset(&binfo, 0, sizeof(binfo));
		if (g_codec == STD_AVC && !slice.phy_addr) {
			slice.size = 512 * 1024;
			if (IOGetPhyMem(&slice)) { fprintf(stderr, "slice alloc\n"); dec_close_guarded(h); close(sock); continue; }
		}
		if (g_codec == STD_AVC) {
			binfo.avcSliceBufInfo.sliceSaveBuffer = slice.phy_addr;
			binfo.avcSliceBufInfo.sliceSaveBufferSize = slice.size;
		}
		if (vpu_DecRegisterFrameBuffer(h, fb, fbcount, stride, &binfo) != RETCODE_SUCCESS) {
			dec_close_guarded(h); close(sock); continue;
		}
		if (g_output_mode == OUTPUT_FB && !g_overlay)
			memset(fb_map, 0, fb_stride * fb_h);
		else if (g_overlay)
			/* keep the key-colour border (don't black it); just make
			 * sure the video rect is clean before the first frame. */
			ov_keyfill(fb_map, fb_stride, fb_w, fb_h);
		printf("streaming...\n"); fflush(stdout);

		long t_window = now_ms(), f_window = 0;
		long ms_dec = 0, ms_prp = 0, ms_blit = 0;
		long ms_feed = 0, ms_vpu = 0, ms_wait = 0;
		long pp_window = 0, prp_window = 0, pipe_window = 0;
		long bs_used_sum = 0, bs_used_max = 0, bs_samples = 0;
		long sock_pending_max = 0;
			long h264_err_mb_sum = 0, h264_err_mb_max = 0;
		long h264_dec_fail = 0, h264_ps_short = 0, h264_slice_short = 0;
		long h264_samples = 0;
		int pending_idx = -1, pending_display = 0, pp_pipeline_ok = 1;
		unsigned int pending_qp_phys = 0;
		int pp_pipeline_cooldown = 0;
		long pending_dec_ms = 0;
		long pending_feed_ms = 0;
		long pending_vpu_ms = 0;
		long pending_wait_ms = 0;
		long last_frame_ms = now_ms();
		long waiting_i_since = 0;
		long input_starve_since = 0;
		long h264_latency_since = 0;
		struct render_state rs;

		memset(&rs, 0, sizeof(rs));
		rs.prp_fd = prp_fd;
		rs.fbmem = fbmem;
		rs.rgbmem = &rgbmem;
		rs.src_w = src_visible_w;
		rs.src_h = src_visible_h;
		rs.aligned_h = ah;
		rs.stride = stride;
		rs.fb_bpp = fb_bpp;
		rs.fb_stride = fb_stride;
		rs.fb_phys = fb_phys;
		rs.off_x = off_x;
		rs.off_y = off_y;
		rs.out_w = out_w;
		rs.out_h = out_h;
		rs.fb_map = fb_map;
		rs.fb_r_off = fb_r_off;
		rs.fb_g_off = fb_g_off;
		rs.fb_b_off = fb_b_off;
		rs.fb_r_len = fb_r_len;
		rs.fb_g_len = fb_g_len;
		rs.fb_b_len = fb_b_len;
		rs.blit_bytes = blit_bytes;
		rs.row_scratch = row_scratch;
		rs.pp_ok = &pp_ok;
		rs.dummy_rgb = (g_output_mode == OUTPUT_PP_DUMMY);

		/* Decode/display loop.
		 *
		 * Note: an earlier attempt to overlap the PrP of frame N with the
		 * decode of N+1 deadlocked on the *very first* concurrent ioctl
		 * -- the eMMA PrP IRQ never arrived while the VPU was bursting.
		 * PP is the post-decode block, so MPEG-4 uses a guarded
		 * one-frame-late PP pipeline. H.264 keeps decoded pictures live as
		 * references for longer, and overlapping PP reads with the next
		 * decode can corrupt the decoder state on CODA DX6. Keep H.264
		 * sequential until proven otherwise.
		 */
		for (;;) {
			DecParam dp; DecOutputInfo oi;
			int rendered_pending = 0;
			/* When falling behind (socket buffer > 32KB), discard stale
			 * data and re-sync at the most recent key frame so the delay
			 * doesn't grow unbounded. */
			long t0 = now_ms();
			int bs_now = bitstream_used(h);
			int sock_pending = 0;
			int feed_rc;
			long feed_ms_this = 0;
			long vpu_ms_this = 0;
			long wait_ms_this = 0;
			long tf0, tf1, td0, td1, tw0, tw1, ti0, ti1;

			if (bs_now >= 0) {
				bs_used_sum += bs_now;
				if (bs_now > bs_used_max)
					bs_used_max = bs_now;
				bs_samples++;
			}
			if (ioctl(sock, FIONREAD, &sock_pending) == 0 &&
			    sock_pending > sock_pending_max)
				sock_pending_max = sock_pending;
			if (g_reload_csc) {
				g_reload_csc = 0;
				apply_pp_csc_config(prp_fd);
			}
			if (!pp_pipeline_ok && pp_pipeline_cooldown > 0) {
				pp_pipeline_cooldown--;
				if (pp_pipeline_cooldown == 0) {
					pp_pipeline_ok = 1;
					printf("eMMA PP pipeline retrying\n");
					fflush(stdout);
				}
			}
			tf0 = now_ms();
			if (rtp_listen_port) {
				feed_rc = rtp_feed_direct(h, &rtp_direct_state,
							  rtp_spspps, rtp_spspps_len, 0);
			} else {
				feed_skip_to_latest(h);
				feed_rc = feed_until_watermark(h, BS_LOW_WATER * 2, 16);
			}
			/* When the bitstream buffer is below the prebuffer
			 * threshold and non-blocking reads can't fill it, poll-wait
			 * up to 50ms for more data. This avoids starving the decoder
			 * with bursty sources. Works for TCP (feed_until_watermark)
			 * and RTP/UDP (rtp_feed_direct). */
			if (g_prebuffer > 0 && feed_rc <= 0) {
				int bs_after = bitstream_used(h);
				if (bs_after >= 0 && bs_after < g_prebuffer) {
					struct pollfd pfd;
					pfd.fd = sock;
					pfd.events = POLLIN;
					pfd.revents = 0;
					if (poll(&pfd, 1, 50) > 0) {
						int fr;
						if (rtp_listen_port)
							fr = rtp_feed_direct(h, &rtp_direct_state,
									     rtp_spspps, rtp_spspps_len, 0);
						else
							fr = feed_until_watermark(h, BS_LOW_WATER * 2, 16);
						if (fr > 0)
							feed_rc = fr;
					}
				}
			}
			tf1 = now_ms();
			feed_ms_this = tf1 - tf0;
			if (feed_rc == 0) break;
			if (feed_rc > 0) {
				last_data_ms = now_ms();
				input_starve_since = 0;
			} else if (bs_now > 4096 || sock_pending > 0) {
				input_starve_since = 0;
			} else {
				long now = now_ms();
				if (!input_starve_since)
					input_starve_since = now;
				else if (now - input_starve_since > INPUT_STARVE_TIMEOUT_MS) {
					printf("decoder watchdog: input starved for %ld ms (bs=%d sock=%d); reconnecting\n",
					       now - input_starve_since, bs_now, sock_pending);
					fflush(stdout);
					break;
				}
			}
			if (g_codec == STD_AVC &&
			    (bs_now > H264_BS_LATENCY_WATER ||
			     sock_pending > H264_SOCK_LATENCY_WATER)) {
				long now = now_ms();
				if (!h264_latency_since)
					h264_latency_since = now;
				else if (now - h264_latency_since > H264_LATENCY_RESTART_MS) {
					printf("decoder watchdog: h264 backlog for %ld ms (bs=%d sock=%d); reconnecting\n",
					       now - h264_latency_since, bs_now, sock_pending);
					fflush(stdout);
					break;
				}
			} else {
				h264_latency_since = 0;
			}
			/* When the source silently dies (half-open socket, no
			 * data but not EOF), the bitstream ring buffer keeps the
			 * decoder producing stale frames, and the existing
			 * input_starve_since watchdog won't fire because
			 * bs_now > 4096. Track actual recv() success
			 * independently and reconnect when data stops arriving. */
			if (now_ms() - last_data_ms > SOCK_DATA_TIMEOUT_MS) {
				printf("decoder watchdog: no data from socket for %ld ms; reconnecting\n",
				       now_ms() - last_data_ms);
				fflush(stdout);
				break;
			}
			memset(&dp, 0, sizeof(dp));
			td0 = now_ms();
			if (vpu_DecStartOneFrame(h, &dp) != RETCODE_SUCCESS) break;
			td1 = now_ms();

			if (g_output_mode != OUTPUT_DECODE_ONLY &&
			    g_codec != STD_AVC &&
			    pending_idx >= 0 && pending_display && pp_ok && pp_pipeline_ok) {
				rs.qp_phys = pending_qp_phys;
				int prc = render_frame(&rs, pending_idx, 1, 0, 0,
						       &ms_prp, &ms_blit,
						       &pp_window, &prp_window);
				if (prc == 0) {
					rendered_pending = 1;
					pipe_window++;
				} else {
					pp_pipeline_ok = 0;
					pp_pipeline_cooldown = 120;
					printf("eMMA PP pipeline failed (rc=%d); continuing sequential\n",
					       prc);
					fflush(stdout);
				}
			}

			i = 0;
			tw0 = now_ms();
			while (vpu_IsBusy()) {
				if (!rtp_listen_port && bitstream_used(h) < BS_LOW_WATER) {
					int fw = feed_until_watermark(h, BS_LOW_WATER * 2, 16);
					if (fw > 0) last_data_ms = now_ms();
				}
				if (vpu_WaitForInt(40) != 0) {
					int fr = -1;
					if (bitstream_used(h) < BS_LOW_WATER) {
						if (rtp_listen_port)
							fr = rtp_feed_direct(h, &rtp_direct_state,
									     rtp_spspps, rtp_spspps_len, 0);
						else
							fr = feed(h, 0);
					}
					if (fr == 0)
						goto disc;
					if (fr > 0) last_data_ms = now_ms();
				}
				if (++i > 30) break;
			}
			tw1 = now_ms();
			memset(&oi, 0, sizeof(oi));
			ti0 = now_ms();
			if (vpu_DecGetOutputInfo(h, &oi) != RETCODE_SUCCESS) break;
			ti1 = now_ms();
			vpu_ms_this = (td1 - td0) + (tw1 - tw0) + (ti1 - ti0);
			wait_ms_this = tw1 - tw0;
			long t1 = now_ms();
			if (g_codec == STD_AVC) {
				h264_samples++;
				h264_err_mb_sum += oi.numOfErrMBs;
				if (oi.numOfErrMBs > h264_err_mb_max)
					h264_err_mb_max = oi.numOfErrMBs;
				if (!oi.decodingSuccess)
					h264_dec_fail++;
				h264_ps_short += oi.notSufficientPsBuffer;
				h264_slice_short += oi.notSufficientSliceBuffer;
			}
			/* PP deblock: zero-pad the QP buffer for the extra
			 * macroblock row when picHeight isn't a multiple of 16.
			 * The VPU only writes picHeight/16 rows, while the PP
			 * reads (picHeight+15)>>4 rows. */
			if (g_pp_deblock && oi.qpInfo) {
				int mb_w = ii.picWidth / 16;
				int pp_stride = (((ii.picWidth + 15) >> 4) + 3) & ~3;
				int pp_rows = (ii.picHeight + 15) >> 4;
				int vpu_written = (ii.picHeight / 16) * ((mb_w + 3) & ~3);
				int pp_expect = pp_stride * pp_rows;
				if (pp_expect > vpu_written && pp_expect <= 2048)
					memset((unsigned char *)oi.qpInfo + vpu_written, 0,
					       pp_expect - vpu_written);
			}

			if (pending_idx >= 0) {
				if (g_output_mode != OUTPUT_DECODE_ONLY &&
				    pending_display && !rendered_pending)
					render_frame(&rs, pending_idx, pp_ok,
						     g_codec != STD_AVC, 1,
						     &ms_prp, &ms_blit,
						     &pp_window, &prp_window);
				vpu_DecClrDispFlag(h, pending_idx);
				ms_dec += pending_dec_ms;
				ms_feed += pending_feed_ms;
				ms_vpu += pending_vpu_ms;
				ms_wait += pending_wait_ms;
				frames++; f_window++;
				if (f_window >= 60) {
					long el = now_ms() - t_window;
					printf("%.1f fps over %ld frames%s | mode %s | per-frame avg: dec %ldms feed %ldms vpu %ldms wait %ldms csc %ldms blit %ldms | pp %ld prp %ld pipe %ld | bs avg %ldK max %ldK sock max %ldK",
						   (f_window * 1000.0) / (el ? el : 1), f_window,
						   g_show ? (g_armed ? "" : " (waiting for I)") : " (warm/hidden)",
						   output_mode_name(),
						   ms_dec / f_window, ms_feed / f_window,
						   ms_vpu / f_window, ms_wait / f_window,
						   ms_prp / f_window, ms_blit / f_window,
						   pp_window, prp_window, pipe_window,
						   bs_samples ? (bs_used_sum / bs_samples) / 1024 : 0,
						   bs_used_max / 1024, sock_pending_max / 1024);
					if (g_codec == STD_AVC)
						printf(" | h264 err avg %ld max %ld fail %ld ps %ld slice %ld",
						       h264_samples ? h264_err_mb_sum / h264_samples : 0,
						       h264_err_mb_max, h264_dec_fail,
						       h264_ps_short, h264_slice_short);
					printf("\n");
					fflush(stdout);
					t_window = now_ms(); f_window = 0;
					pp_window = prp_window = pipe_window = 0;
					bs_used_sum = bs_used_max = bs_samples = 0;
					sock_pending_max = 0;
					h264_err_mb_sum = h264_err_mb_max = 0;
					h264_dec_fail = h264_ps_short = h264_slice_short = 0;
					h264_samples = 0;
					ms_dec = ms_feed = ms_vpu = ms_wait = ms_prp = ms_blit = 0;
				}
				pending_idx = -1;
				pending_display = 0;
				pending_dec_ms = 0;
				pending_feed_ms = 0;
				pending_vpu_ms = 0;
				pending_wait_ms = 0;
			}

			if (oi.indexFrameDisplay >= 0 && oi.indexFrameDisplay < fbcount) {
				int idx = oi.indexFrameDisplay;
				last_frame_ms = now_ms();
				if (g_show && !g_armed && oi.picType == 0)
					g_armed = 1;

				/* Overlay: enable the FG graphic window once we're armed
				 * and shown; disable it when hidden so the UI shows fully. */
				if (g_overlay) {
					if (g_show && g_armed && !ov_gw_on) gw_set(1);
					else if (!g_show && ov_gw_on)       gw_set(0);
				}

				if (g_output_mode == OUTPUT_DECODE_ONLY) {
					pending_idx = idx;
					pending_display = 0;
					pending_dec_ms = t1 - t0;
					pending_feed_ms = feed_ms_this;
					pending_vpu_ms = vpu_ms_this;
					pending_wait_ms = wait_ms_this;
				} else if (g_show && g_armed) {
					pending_idx = idx;
					pending_display = 1;
					pending_qp_phys = g_pp_deblock ? para_buf.phy_addr : 0;
					pending_dec_ms = t1 - t0;
					pending_feed_ms = feed_ms_this;
					pending_vpu_ms = vpu_ms_this;
					pending_wait_ms = wait_ms_this;
				} else {
					vpu_DecClrDispFlag(h, idx);
					ms_dec += t1 - t0;
					ms_feed += feed_ms_this;
					ms_vpu += vpu_ms_this;
					ms_wait += wait_ms_this;
					frames++; f_window++;
				}
			}
			if (oi.indexFrameDecoded < 0 && oi.indexFrameDisplay < 0) {
				int fb = feed(h, 1);
				if (fb == 0) break;
				if (fb > 0) last_data_ms = now_ms();
				/* No data right now: make a pending hide take effect even
				 * while the source is stalled, so the frozen last frame is
				 * cleared instead of lingering until a kill -9. The stall
				 * watchdog above then reconnects once it times out. */
				else if (g_overlay && !g_show && ov_gw_on) gw_set(0);
			}
			if (g_show && !g_armed) {
				long now = now_ms();
				if (!waiting_i_since)
					waiting_i_since = now;
				else if (now - waiting_i_since > KEYFRAME_TIMEOUT_MS) {
					printf("decoder watchdog: no keyframe for %ld ms; reconnecting\n",
					       now - waiting_i_since);
					fflush(stdout);
					break;
				}
			} else {
				waiting_i_since = 0;
			}
			if (now_ms() - last_frame_ms > NO_FRAME_TIMEOUT_MS) {
				printf("decoder watchdog: no display frame for %ld ms; reconnecting\n",
				       now_ms() - last_frame_ms);
				fflush(stdout);
				break;
			}
		}
	disc:
		if (pending_idx >= 0) {
			vpu_DecClrDispFlag(h, pending_idx);
			pending_idx = -1;
			pending_feed_ms = 0;
			pending_vpu_ms = 0;
			pending_wait_ms = 0;
		}
		printf("client gone (%d frames); re-listening\n", frames); fflush(stdout);
		if (g_overlay) gw_set(0);   /* drop the overlay so the UI shows fully */
		dec_close_guarded(h);
		close(sock); sock = -1;
		if (g_codec == STD_AVC) {
			printf("h264 session ended; restarting process for clean VPU state\n");
			fflush(stdout);
			close(prp_fd);
			vpu_UnInit();
			munmap(fb_map, fb_stride * fb_h);
			close(fb_fd);
			close(lsock);
			execv(argv[0], argv);
			execv("/proc/self/exe", argv);
			perror("execv");
			return 1;
		}
		/* NOTE: do NOT call IOSysSWReset here. The kernel reset halts
		 * the BIT processor + clears registers; libvpu's in-process
		 * state still believes the firmware is loaded and the next
		 * vpu_DecOpen writes to a dead chip -> no decode, no IRQ.
		 * The kernel's last-close reset (vpu_release) guarantees a
		 * fresh process always gets a clean VPU. */
	}
	return 0;
}
