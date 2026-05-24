/* Live MPEG-4 stream player for the Toon: receives a raw MPEG-4 elementary
 * stream over TCP, decodes on the VPU, converts YUV->RGB565 on the eMMA PrP,
 * and blits to the framebuffer. Auto-recovers: loops on accept() so the
 * Orange Pi can disconnect/reconnect without restarting this.
 *
 *   Orange Pi 5:  ffmpeg rtsp:// -> scale 320x240 -> mpeg4 -> tcp://toon:PORT
 *   Toon:         vpu_stream PORT
 *
 * Usage: vpu_stream <tcp-port>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/resource.h>
#include <linux/fb.h>
#include <sys/time.h>
#include <pthread.h>
#include "vpu_lib.h"
#include "vpu_io.h"

static long now_ms(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec * 1000L + tv.tv_usec / 1000L;
}

#define STREAM_BUF_SIZE   (1 * 1024 * 1024)
#define ALIGN16(x)        (((x) + 15) & ~15)

struct vpu_prp_convert {
	unsigned int src_y, src_w, src_h, src_stride;
	unsigned int dst_rgb, dst_w, dst_h, dst_stride;
};
#define VPU_IOC_PRP_CONVERT  _IO('V', 22)

static int sock = -1;
static vpu_mem_desc bs;

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
static int g_warm = 0;

static void on_sigusr1(int s) { (void)s; g_show = 1; g_armed = 0; }
static void on_sigusr2(int s) { (void)s; g_show = 0; }

/* SIGALRM fires if the prime phase hasn't completed within its budget.
 * (Belt; the braces are the external heartbeat file watched by camera.c
 * which SIGKILLs us if it goes stale -- some libvpu hangs can't be
 * interrupted by a userspace signal alone.) */
static void on_sigalrm(int s)
{
	(void)s;
	const char msg[] = "prime watchdog fired; exiting for clean restart\n";
	(void)!write(STDERR_FILENO, msg, sizeof(msg) - 1);
	_exit(2);
}

/* Heartbeat: a dedicated pthread touches /tmp/vpu_stream.tick whenever
 * the main thread has made forward progress (set g_progress = 1 from
 * the main loop). If the main thread is wedged -- e.g., stuck inside a
 * libvpu call that won't return -- the flag stays 0 and the heartbeat
 * file mtime ages out. camera.c's watchdog SIGKILLs us once mtime > 10s.
 *
 * The thread (not main itself) writes the file so it keeps running even
 * if main is blocked in a long ioctl: that's important because we want
 * the watchdog to detect the wedge, not be silently bypassed. */
#define HEARTBEAT_PATH "/tmp/vpu_stream.tick"
/* Two flags so the watchdog only matters when main SHOULD be making progress:
 *   g_active   -- main is in the prime/decode path (we expect frames).
 *   g_progress -- main has made forward progress since the last tick.
 *
 * When !active (idling in accept(), client not yet connected): always
 * touch the heartbeat -- there's nothing to wedge on, the long sleep
 * inside accept() is the intended behavior.
 *
 * When active: only touch if main set g_progress. If main is wedged in
 * a libvpu call, g_progress stays 0 -> mtime ages out -> SIGKILL.
 */
static volatile sig_atomic_t g_active   = 0;
static volatile sig_atomic_t g_progress = 1;
static void heartbeat_write(void)
{
	int fd = open(HEARTBEAT_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd >= 0) close(fd);
}
static void * heartbeat_thread(void *unused)
{
	(void)unused;
	heartbeat_write();   /* fresh from the start */
	for (;;) {
		sleep(3);
		if (!g_active || __sync_fetch_and_and(&g_progress, 0))
			heartbeat_write();
		/* else (active && no progress): wedged -> let it go stale */
	}
	return NULL;
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
	fcntl(sock, F_SETFL, block ? 0 : O_NONBLOCK);
	n = recv(sock, (void *)(bs.virt_uaddr + off), chunk, 0);
	if (n > 0) { vpu_DecUpdateBitstreamBuffer(h, n); return n; }
	if (n == 0) return 0;        /* peer closed */
	return -1;                   /* EAGAIN */
}

/* When the VPU can't keep up with the incoming stream, the TCP socket buffer
 * grows without bound -> delay grows to tens of seconds. Drain the backlog,
 * resync to the most recent I-VOP, and feed from there. Skipping to any
 * other VOP type (P/B) would leave the decoder without its reference frame
 * -> visible blocky garbage until the next I (a whole GOP). If no I-VOP
 * is in the drained data, we push everything through unchanged -- one frame
 * of extra lag is far less visible than a smear of broken macroblocks. */
static int feed_skip_to_latest(DecHandle h)
{
	PhysicalAddress rd, wr; Uint32 space;
	unsigned char tmp[65536];
	int i, total, last_ivop, off, chunk, start_at;
	int pending;

	/* Drop the threshold (was 16k) so we re-sync to the latest frame on
	 * even a small backlog -- the goal here is *low* latency, not max fps.
	 * 2k is roughly one ~320x180 MPEG-4 P-frame at our bitrate, so any
	 * accumulated B/P-frame chain triggers a skip. */
	if (ioctl(sock, FIONREAD, &pending) < 0 || pending < 2048)
		return 0;   /* not enough backlog to justify dropping frames */

	fcntl(sock, F_SETFL, O_NONBLOCK);
	total = 0;
	while (total < (int)sizeof(tmp)) {
		int n = recv(sock, tmp + total, sizeof(tmp) - total, 0);
		if (n <= 0) break;
		total += n;
	}
	if (total <= 0) return 0;

	/* MPEG-4 VOP start = 00 00 01 B6; vop_coding_type lives in the top two
	 * bits of the byte that follows. 00 = I-VOP (key frame). Find the last
	 * I-VOP in the drained data and skip to it. */
	last_ivop = -1;
	for (i = 0; i + 4 < total; i++) {
		if (tmp[i]   == 0x00 && tmp[i+1] == 0x00 &&
			tmp[i+2] == 0x01 && tmp[i+3] == 0xB6 &&
			(tmp[i+4] & 0xC0) == 0x00) {
			last_ivop = i;
		}
	}
	/* No I-VOP visible: push the data through as-is rather than risk a
	 * P/B resync. Better one frame late than corrupt output. */
	start_at = (last_ivop > 0) ? last_ivop : 0;

	if (vpu_DecGetBitstreamBuffer(h, &rd, &wr, &space) != RETCODE_SUCCESS)
		return -1;

	off = (int)(wr - bs.phy_addr);
	chunk = STREAM_BUF_SIZE - off;
	if (chunk > total - start_at) chunk = total - start_at;
	if (chunk > (int)space) chunk = (int)space;
	if (chunk <= 0) return 0;

	memcpy((void *)(bs.virt_uaddr + off), tmp + start_at, chunk);
	vpu_DecUpdateBitstreamBuffer(h, chunk);
	return chunk;
}

int main(int argc, char **argv)
{
	int port = 5000;
	/* Optional --rect X Y W H: render at (X,Y) sized W x H instead of the
	 * decoded-and-centred default. eMMA PrP does the resize bilinearly in
	 * hardware, so picking a smaller W/H is essentially free CPU-wise.
	 * Used by toonui to embed live video in a card on the home screen. */
	int rect_x = -1, rect_y = -1, rect_w = -1, rect_h = -1;
	for (int ai = 1; ai < argc; ai++) {
		if (!strcmp(argv[ai], "--rect") && ai + 4 < argc) {
			rect_x = atoi(argv[ai+1]); rect_y = atoi(argv[ai+2]);
			rect_w = atoi(argv[ai+3]); rect_h = atoi(argv[ai+4]);
			ai += 4;
		} else if (!strcmp(argv[ai], "--warm")) {
			/* Start hidden: decode normally, but skip PrP+blit until
			 * SIGUSR1 fires. Used by toonui so the doorbell tile opens
			 * with zero startup latency -- on signal we wait for the next
			 * I-VOP then start displaying. SIGUSR2 returns to hidden. */
			g_warm  = 1;
			g_show  = 0;
			g_armed = 0;
		} else if (argv[ai][0] != '-') {
			port = atoi(argv[ai]);
		}
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
	unsigned int *row_scratch = NULL;   /* one row of 32bpp, cached DRAM */

	signal(SIGPIPE, SIG_IGN);
	signal(SIGALRM, on_sigalrm);

	/* Start the heartbeat thread BEFORE anything that can hang. The
	 * watchdog in camera.c only kills us if heartbeat goes stale, so
	 * the thread needs to be alive even when main is wedged. */
	{
		pthread_t hb;
		if (pthread_create(&hb, NULL, heartbeat_thread, NULL) == 0)
			pthread_detach(hb);
		else
			fprintf(stderr, "heartbeat thread failed; watchdog will misfire\n");
	}
	if (g_warm) {
		signal(SIGUSR1, on_sigusr1);   /* show:  unmask blits at next I */
		signal(SIGUSR2, on_sigusr2);   /* hide:  back to warm/no-blit */
	}
	setpriority(PRIO_PROCESS, 0, 10);

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

	if (vpu_Init(NULL) != RETCODE_SUCCESS) { fprintf(stderr, "vpu_Init fail\n"); return 1; }
	bs.size = STREAM_BUF_SIZE;
	if (IOGetPhyMem(&bs) || IOGetVirtMem(&bs) <= 0) { fprintf(stderr, "bs alloc\n"); return 1; }
	prp_fd = open("/dev/mxc_vpu", O_RDWR);
	printf("vpu_stream ready; listening tcp/%d\n", port); fflush(stdout);

	/* ---- per-connection loop ----
	 * Long-lived: keeps vpu_Init's single VPU init across reconnects (a
	 * fresh vpu_Init in a respawned process turned out to corrupt the
	 * kernel's clock refcount + wedge the eMMA after a few cycles).
	 * Reconnect handling: vpu_DecClose, accept again, vpu_DecOpen, prime.
	 * The prime phase is watchdogged via SIGALRM (5 s) so a hung
	 * vpu_DecGetInitialInfo (which can sit inside libvpu forever after
	 * a poorly-closed prior stream) kills the process cleanly instead
	 * of pegging the CPU + flooding the kernel with VPU timeouts.
	 * camera.c's watchdog respawns us with a delay so the TCP port
	 * has time to clear TIME_WAIT. */
	for (;;) {
		DecHandle h;
		int inited = 0, off_x, off_y, frames = 0;

		printf("waiting for stream...\n"); fflush(stdout);
		g_active = 0;   /* idle in accept -- watchdog should not gate on progress */
		sock = accept(lsock, NULL, NULL);
		g_active = 1;   /* got a connection -- now we must make progress */
		g_progress = 1;
		if (sock < 0) continue;
		/* Stale-socket guard. If the OPi rapidly reconnects (kill+
		 * restart of its ffmpeg, or a TCP RST that didn't deliver
		 * its FIN cleanly), our accept queue can have a leftover
		 * mid-stream socket sitting in front of the fresh one. Take
		 * the freshest by draining everything else still queued -- the
		 * mid-stream one has no sequence header so the prime phase
		 * would hang on it forever. Done with a NONBLOCK toggle on
		 * lsock so any queued sockets pop out as accept()=-1+EAGAIN. */
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
		/* A fresh decoder needs a fresh I-frame before its output is
		 * trustworthy. If the prior connection was being shown, we'd
		 * happily blit the new decoder's first P-frame against an empty
		 * reference -> garbage. Re-arm on the next I after reconnect. */
		if (g_show) g_armed = 0;
		setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
		/* Cap the kernel's TCP receive buffer so it can't hoard frames on
		 * us. Linux's default is ~87 KB which holds several seconds of
		 * MPEG-4 at our bitrate. With ~32 KB the kernel itself bounds the
		 * latency to a fraction of a second; feed_skip_to_latest mops up
		 * the rest. */
		int rcvbuf = 32 * 1024;
		setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
		/* 5s recv timeout: if the stream wedges (peer silently dies, or
		 * sends partial-frame garbage forever), blocking feed(h,1) in
		 * the prime phase or block-fallback in decode would otherwise
		 * pin the decoder. EAGAIN/EWOULDBLOCK -> feed returns -1 and
		 * the loop progresses; 0 (EOF) breaks back to accept. */
		struct timeval rcvto = { .tv_sec = 5, .tv_usec = 0 };
		setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &rcvto, sizeof rcvto);
		printf("client connected\n"); fflush(stdout);

		memset(&op, 0, sizeof(op));
		op.bitstreamFormat = STD_MPEG4;
		op.bitstreamBuffer = bs.phy_addr;
		op.bitstreamBufferSize = bs.size;
		op.reorderEnable = 1;
		op.filePlayEnable = 0;
		if (vpu_DecOpen(&h, &op) != RETCODE_SUCCESS) { close(sock); continue; }

		/* Prime: feed until the sequence header parses.
		 * Three safety nets, because vpu_DecGetInitialInfo can sit
		 * inside libvpu indefinitely waiting on a VPU IRQ that never
		 * fires after a poorly-closed prior stream:
		 *   1. Try-count cap (64): only counts our loop iterations.
		 *   2. SO_RCVTIMEO=5s on sock: recv times out -> feed=-1 -> break.
		 *   3. SIGALRM(5s): kills the process if the library itself
		 *      hangs (the only thing that breaks an in-library wait).
		 * camera.c respawns us either way; the user sees ~2-7s of
		 * reconnect latency in the rare degraded case. */
		alarm(5);
		int prime_tries = 0;
		while (!inited && prime_tries++ < 64) {
			if (feed(h, 1) <= 0) break;            /* EOF/err/timeout */
			g_progress = 1;   /* fed some bytes -- main is alive */
			vpu_DecSetEscSeqInit(h, 1);
			if (vpu_DecGetInitialInfo(h, &ii) == RETCODE_SUCCESS) inited = 1;
			vpu_DecSetEscSeqInit(h, 0);
		}
		alarm(0);
		if (!inited) {
			printf("prime failed (%d tries, no sequence header); reconnecting\n",
			       prime_tries);
			fflush(stdout);
			vpu_DecClose(h); close(sock); continue;
		}
		printf("Video: %dx%d minFB=%d\n", ii.picWidth, ii.picHeight, ii.minFrameBufferCount);
		fflush(stdout);

		/* Output rect (where the video lands, and how big the PrP should
		 * resize each decoded frame to). The eMMA PrP only *downsamples*
		 * cleanly; asking it to upscale produces garbage (the classic
		 * "tiled tiny copies of the source on a black background" pattern).
		 * So in rect-mode cap the dst to the actual decoded source dims —
		 * a requested 640x480 against a 640x360 source becomes 640x360
		 * at the requested (rect_x, rect_y). Remember the original rect
		 * so we can blackfill the rest of the cutout once, otherwise the
		 * UI pixels that were there before vpu_stream started show through. */
		int orig_w = has_rect ? rect_w : 0;
		int orig_h = has_rect ? rect_h : 0;
		int out_w = has_rect ? rect_w : (int)ii.picWidth;
		int out_h = has_rect ? rect_h : (int)ii.picHeight;
		if (has_rect) {
			if (out_w > (int)ii.picWidth)  out_w = ii.picWidth;
			if (out_h > (int)ii.picHeight) out_h = ii.picHeight;
		}
		off_x = has_rect ? rect_x : (fb_w  - out_w) / 2;
		off_y = has_rect ? rect_y : (fb_h  - out_h) / 2;
		if (off_x < 0) off_x = 0;
		if (off_y < 0) off_y = 0;

		/* One-time blackfill of the *original* cutout area so any UI pixels
		 * left behind when the cutout was set don't show through where the
		 * (possibly smaller) video doesn't reach. Only runs when the rect
		 * was actually clamped. */
		if (has_rect && (out_w < orig_w || out_h < orig_h)) {
			int bpp = fb_bpp / 8;
			for (int yy = 0; yy < orig_h && (rect_y + yy) < fb_h; yy++) {
				int w_clip = orig_w;
				if (rect_x + w_clip > fb_w) w_clip = fb_w - rect_x;
				if (w_clip > 0) {
					memset(fb_map + (rect_y + yy) * fb_stride + rect_x * bpp,
						   0, (size_t)w_clip * bpp);
				}
			}
			printf("[rect] requested %dx%d but source is %dx%d -> capping to %dx%d (no upscale)\n",
				   orig_w, orig_h, ii.picWidth, ii.picHeight, out_w, out_h);
			fflush(stdout);
		}

		if (!fb_alloced) {       /* allocate frame + RGB buffers once (fixed res) */
			stride = ALIGN16(ii.picWidth); ah = ALIGN16(ii.picHeight);
			ysize = stride * ah; csize = (stride / 2) * (ah / 2);
			mvsize = ALIGN16(ii.picWidth / 16) * ALIGN16(ii.picHeight / 16) * 8;
			/* minFB+4 (was +2). Gives the decoder more headroom; if it
			 * ever stalls waiting for a buffer release, this hides it.
			 * Cheap (~2 extra MB at 720x480 YUV420). */
			fbcount = ii.minFrameBufferCount + 4; if (fbcount > 32) fbcount = 32;
			blit_bytes = out_w * 2;
			for (i = 0; i < fbcount; i++) {
				fbmem[i].size = ysize + 2 * csize + mvsize;
				if (IOGetPhyMem(&fbmem[i])) { fprintf(stderr, "fb %d\n", i); return 1; }
				fb[i].strideY = stride; fb[i].strideC = stride / 2;
				fb[i].bufY = fbmem[i].phy_addr;
				fb[i].bufCb = fb[i].bufY + ysize;
				fb[i].bufCr = fb[i].bufCb + csize;
				fb[i].bufMvCol = fb[i].bufCr + csize;
			}
			/* RGB565 bounce buffer is only needed for the 32bpp fb path
			 * (where we have to widen on the CPU). On 16bpp we point the
			 * PrP straight at fb_phys + offset and skip the bounce + memcpy. */
			if (fb_bpp == 32) {
				rgbmem.size = out_w * out_h * 2;
				if (IOGetPhyMem(&rgbmem) || IOGetVirtMem(&rgbmem) <= 0) { fprintf(stderr, "rgb\n"); return 1; }
				row_scratch = malloc(out_w * 4);
				if (!row_scratch) { fprintf(stderr, "row_scratch\n"); return 1; }
			}
			fb_alloced = 1;
			printf("output rect: %dx%d at (%d,%d)%s\n", out_w, out_h, off_x, off_y,
				   has_rect ? " (hw-resized)" : " (1:1, centred)");
			fflush(stdout);
		}
		memset(&binfo, 0, sizeof(binfo));
		if (vpu_DecRegisterFrameBuffer(h, fb, fbcount, stride, &binfo) != RETCODE_SUCCESS) {
			vpu_DecClose(h); close(sock); continue;
		}
		memset(fb_map, 0, fb_stride * fb_h);
		printf("streaming...\n"); fflush(stdout);

		long t_window = now_ms(), f_window = 0;
		long ms_dec = 0, ms_prp = 0, ms_blit = 0;

		/* Sequential decode/display loop.
		 *
		 * Note: an earlier attempt to overlap the PrP of frame N with the
		 * decode of N+1 deadlocked on the *very first* concurrent ioctl
		 * -- the eMMA PrP's wait_event in the kernel never returns while
		 * the VPU's vpu_lock is held by an in-flight DecStartOneFrame.
		 * The two IP blocks have separate DMA, separate IRQs, and CAN
		 * physically run in parallel; the bottleneck is in the driver
		 * serialisation. Lifting it requires a kernel patch to allow
		 * VPU_IOC_PRP_CONVERT to enter without holding the VPU mutex.
		 * Until then we stick with sequential PrP-after-VPU and keep
		 * the (already-shipped) PrP-to-fb-phys win.
		 */
		for (;;) {
			DecParam dp; DecOutputInfo oi;
			/* When falling behind (socket buffer > 32KB), discard stale
			 * data and re-sync at the most recent I-VOP so the delay
			 * doesn't grow unbounded. */
			long t0 = now_ms();
			feed_skip_to_latest(h);
			if (feed(h, 0) == 0) break;
			memset(&dp, 0, sizeof(dp));
			if (vpu_DecStartOneFrame(h, &dp) != RETCODE_SUCCESS) break;
			i = 0;
			while (vpu_IsBusy()) {
				if (vpu_WaitForInt(20) != 0) { if (feed(h, 0) == 0) goto disc; }
				if (++i > 50) break;
			}
			memset(&oi, 0, sizeof(oi));
			if (vpu_DecGetOutputInfo(h, &oi) != RETCODE_SUCCESS) break;
			long t1 = now_ms();
			if (oi.indexFrameDisplay >= 0 && oi.indexFrameDisplay < fbcount) {
				int idx = oi.indexFrameDisplay;
				long t2 = t1, t3 = t1;
				if (g_show && !g_armed && oi.picType == 0)
					g_armed = 1;

				if (g_show && g_armed) {
					struct vpu_prp_convert c;
					/* PrP: YUV->RGB565 + bilinear resize in one hw pass.
					 * 16bpp: dst is fb_phys directly (cutout offset baked
					 * in, stride = fb_stride so PrP skips the right margin
					 * per row) -- no CPU memcpy needed. 32bpp: PrP writes
					 * to the RGB565 bounce buffer; CPU widens below. */
					c.src_y = fbmem[idx].phy_addr; c.src_w = ii.picWidth; c.src_h = ah;
					c.src_stride = stride;
					c.dst_w = out_w; c.dst_h = out_h;
					if (fb_bpp == 16) {
						c.dst_rgb    = fb_phys + (unsigned)off_y * fb_stride
						                       + (unsigned)off_x * 2;
						c.dst_stride = fb_stride;
					} else {
						c.dst_rgb    = rgbmem.phy_addr;
						c.dst_stride = out_w * 2;
					}
					ioctl(prp_fd, VPU_IOC_PRP_CONVERT, &c);
					t2 = now_ms();
					if (fb_bpp == 16) {
						/* No blit -- PrP wrote into the framebuffer directly. */
					} else {
						/* 32bpp target: widen RGB565 -> packed 8:8:8 using the
						 * kernel-reported channel offsets so this works on
						 * xRGB, BGRx, ARGB, ... whatever the panel driver
						 * chose this boot. */
						int row, col;
						int ro = fb_r_off, go = fb_g_off, bo = fb_b_off;
						int rs = 8 - fb_r_len, gs = 8 - fb_g_len, bs = 8 - fb_b_len;
						int row_bytes = out_w * 4;
						for (row = 0; row < out_h; row++) {
							unsigned short *src = (unsigned short *)
								((unsigned char *)rgbmem.virt_uaddr + row * blit_bytes);
							for (col = 0; col < out_w; col++) {
								unsigned short p = src[col];
								unsigned int r = ((p >> 11) & 0x1F) << 3;
								unsigned int g = ((p >>  5) & 0x3F) << 2;
								unsigned int b = ( p        & 0x1F) << 3;
								r |= r >> 5;  g |= g >> 6;  b |= b >> 5;
								r >>= rs;  g >>= gs;  b >>= bs;
								row_scratch[col] = (r << ro) | (g << go) | (b << bo);
							}
							memcpy(fb_map + (off_y + row) * fb_stride + off_x * 4,
								   row_scratch, row_bytes);
						}
					}
					t3 = now_ms();
				}
				/* Always release the frame to the decoder pool. */
				vpu_DecClrDispFlag(h, idx);
				ms_dec += (t1 - t0); ms_prp += (t2 - t1); ms_blit += (t3 - t2);
				frames++; f_window++;
				/* Every successful frame is progress. (Setting once per
				 * frame is cheap and ensures even slow streams keep the
				 * heartbeat fresh.) */
				g_progress = 1;
				if (f_window >= 60) {
					long el = now_ms() - t_window;
					printf("%.1f fps over %ld frames%s | per-frame avg: dec %ldms prp %ldms blit %ldms\n",
						   (f_window * 1000.0) / (el ? el : 1), f_window,
						   g_show ? (g_armed ? "" : " (waiting for I)") : " (warm/hidden)",
						   ms_dec / f_window, ms_prp / f_window, ms_blit / f_window);
					fflush(stdout);
					t_window = now_ms(); f_window = 0;
					ms_dec = ms_prp = ms_blit = 0;
				}
			}
			if (oi.indexFrameDecoded < 0 && oi.indexFrameDisplay < 0)
				if (feed(h, 1) == 0) break;
		}
disc:
		printf("client gone (%d frames); re-listening\n", frames); fflush(stdout);
		g_active = 0;   /* back to accept idle */
		vpu_DecClose(h);
		close(sock); sock = -1;
		/* NOTE: do NOT call IOSysSWReset here. The kernel reset halts
		 * the BIT processor + clears registers; libvpu's in-process
		 * state still believes the firmware is loaded and the next
		 * vpu_DecOpen writes to a dead chip -> no decode, no IRQ.
		 * The kernel does a last-close reset (vpu_release) so a fresh
		 * process always gets a clean VPU; for in-process reconnects
		 * we rely on the heartbeat watchdog (camera.c) to SIGKILL us
		 * if a reconnect-prime hangs, which triggers vpu_release ->
		 * reset -> respawn with clean state. */
	}
	return 0;
}
