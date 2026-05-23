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
#include "vpu_lib.h"
#include "vpu_io.h"

#define STREAM_BUF_SIZE   (1 * 1024 * 1024)
#define ALIGN16(x)        (((x) + 15) & ~15)

struct vpu_prp_convert {
    unsigned int src_y, src_w, src_h, src_stride;
    unsigned int dst_rgb, dst_w, dst_h, dst_stride;
};
#define VPU_IOC_PRP_CONVERT  _IO('V', 22)

static int sock = -1;
static vpu_mem_desc bs;

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
 * grows without bound -> delay grows to tens of seconds.  Read all pending
 * data, find the LAST MPEG start code (00 00 01 xx), and feed only from there
 * — discarding all the stale frames before it. */
static int feed_skip_to_latest(DecHandle h)
{
    PhysicalAddress rd, wr; Uint32 space;
    unsigned char tmp[65536];
    int i, total, last_sc, off, chunk;
    int pending;

    if (ioctl(sock, FIONREAD, &pending) < 0 || pending < 16384)
        return 0;   /* not enough backlog to justify dropping frames */

    fcntl(sock, F_SETFL, O_NONBLOCK);
    total = 0;
    while (total < (int)sizeof(tmp) - 1) {
        int n = recv(sock, tmp + total, sizeof(tmp) - 1 - total, 0);
        if (n <= 0) break;
        total += n;
    }
    if (total < 4) return 0;

    /* find the most recent start-code prefix (00 00 01) */
    last_sc = -1;
    for (i = 0; i < total - 3; i++) {
        if (tmp[i] == 0 && tmp[i+1] == 0 && tmp[i+2] == 1)
            last_sc = i;
    }
    if (last_sc < 0) return -1;   /* no sync point in the drained data */

    if (vpu_DecGetBitstreamBuffer(h, &rd, &wr, &space) != RETCODE_SUCCESS)
        return -1;

    off = (int)(wr - bs.phy_addr);
    chunk = STREAM_BUF_SIZE - off;
    if (chunk > total - last_sc) chunk = total - last_sc;
    if (chunk > (int)space) chunk = (int)space;

    memcpy((void *)(bs.virt_uaddr + off), tmp + last_sc, chunk);
    if (chunk > 0)
        vpu_DecUpdateBitstreamBuffer(h, chunk);
    return chunk;
}

int main(int argc, char **argv)
{
    int port = argc > 1 ? atoi(argv[1]) : 5000;
    int lsock, one = 1, i;
    struct sockaddr_in addr;
    int fb_fd, fb_stride, fb_w, fb_h, fb_bpp;
    int fb_r_off = 11, fb_g_off = 5, fb_b_off = 0;   /* RGB565 defaults */
    int fb_r_len = 5, fb_g_len = 6, fb_b_len = 5;
    struct fb_var_screeninfo vinfo;
    unsigned char *fb_map;
    DecOpenParam op; DecInitialInfo ii; DecBufInfo binfo;
    vpu_mem_desc fbmem[32] = {{0}}, rgbmem = {0};
    FrameBuffer fb[32];
    int fbcount = 0, stride = 0, ah = 0, ysize = 0, csize = 0, mvsize = 0;
    int prp_fd, fb_alloced = 0, blit_bytes = 0;

    signal(SIGPIPE, SIG_IGN);
    setpriority(PRIO_PROCESS, 0, 10);

    lsock = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET; addr.sin_addr.s_addr = INADDR_ANY; addr.sin_port = htons(port);
    if (bind(lsock, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    listen(lsock, 1);

    fb_fd = open("/dev/fb0", O_RDWR);
    ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo);
    fb_w = vinfo.xres; fb_h = vinfo.yres; fb_bpp = vinfo.bits_per_pixel;
    fb_stride = fb_w * (fb_bpp / 8);
    fb_map = mmap(NULL, fb_stride * fb_h, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    /* fb may be 16 (RGB565) or 32 (xRGB/BGRx) — Qt-gui flips this across
     * reboots. eMMA PrP always outputs RGB565; for 32bpp targets we widen
     * per-row in the blit, using the kernel-reported channel offsets so
     * any byte order works. */
    if (fb_bpp == 32) {
        fb_r_off = vinfo.red.offset;   fb_r_len = vinfo.red.length;
        fb_g_off = vinfo.green.offset; fb_g_len = vinfo.green.length;
        fb_b_off = vinfo.blue.offset;  fb_b_len = vinfo.blue.length;
    }
    printf("fb0: %dx%d %dbpp R@%d/%d G@%d/%d B@%d/%d stride=%d\n",
           fb_w, fb_h, fb_bpp, fb_r_off, fb_r_len, fb_g_off, fb_g_len,
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

    /* ---- per-connection loop ---- */
    for (;;) {
        DecHandle h;
        int inited = 0, off_x, off_y, frames = 0;

        printf("waiting for stream...\n"); fflush(stdout);
        sock = accept(lsock, NULL, NULL);
        if (sock < 0) continue;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        printf("client connected\n"); fflush(stdout);

        memset(&op, 0, sizeof(op));
        op.bitstreamFormat = STD_MPEG4;
        op.bitstreamBuffer = bs.phy_addr;
        op.bitstreamBufferSize = bs.size;
        op.reorderEnable = 1;
        op.filePlayEnable = 0;
        if (vpu_DecOpen(&h, &op) != RETCODE_SUCCESS) { close(sock); continue; }

        /* prime: feed until the sequence header parses */
        while (!inited) {
            if (feed(h, 1) <= 0) break;            /* EOF/err before header */
            vpu_DecSetEscSeqInit(h, 1);
            if (vpu_DecGetInitialInfo(h, &ii) == RETCODE_SUCCESS) inited = 1;
            vpu_DecSetEscSeqInit(h, 0);
        }
        if (!inited) { vpu_DecClose(h); close(sock); continue; }
        printf("Video: %dx%d minFB=%d\n", ii.picWidth, ii.picHeight, ii.minFrameBufferCount);
        fflush(stdout);

        off_x = (fb_w - (int)ii.picWidth) / 2;  if (off_x < 0) off_x = 0;
        off_y = (fb_h - (int)ii.picHeight) / 2; if (off_y < 0) off_y = 0;

        if (!fb_alloced) {       /* allocate frame + RGB buffers once (fixed res) */
            stride = ALIGN16(ii.picWidth); ah = ALIGN16(ii.picHeight);
            ysize = stride * ah; csize = (stride / 2) * (ah / 2);
            mvsize = ALIGN16(ii.picWidth / 16) * ALIGN16(ii.picHeight / 16) * 8;
            fbcount = ii.minFrameBufferCount + 2; if (fbcount > 32) fbcount = 32;
            blit_bytes = ii.picWidth * 2;
            for (i = 0; i < fbcount; i++) {
                fbmem[i].size = ysize + 2 * csize + mvsize;
                if (IOGetPhyMem(&fbmem[i])) { fprintf(stderr, "fb %d\n", i); return 1; }
                fb[i].strideY = stride; fb[i].strideC = stride / 2;
                fb[i].bufY = fbmem[i].phy_addr;
                fb[i].bufCb = fb[i].bufY + ysize;
                fb[i].bufCr = fb[i].bufCb + csize;
                fb[i].bufMvCol = fb[i].bufCr + csize;
            }
            rgbmem.size = ii.picWidth * ah * 2;
            if (IOGetPhyMem(&rgbmem) || IOGetVirtMem(&rgbmem) <= 0) { fprintf(stderr, "rgb\n"); return 1; }
            fb_alloced = 1;
        }
        memset(&binfo, 0, sizeof(binfo));
        if (vpu_DecRegisterFrameBuffer(h, fb, fbcount, stride, &binfo) != RETCODE_SUCCESS) {
            vpu_DecClose(h); close(sock); continue;
        }
        memset(fb_map, 0, fb_stride * fb_h);
        printf("streaming...\n"); fflush(stdout);

        /* decode/display loop */
        for (;;) {
            DecParam dp; DecOutputInfo oi;
            /* When falling behind (socket buffer > 32KB), discard stale
             * data and re-sync at the most recent MPEG start code so the
             * delay doesn't grow unbounded. */
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
            if (oi.indexFrameDisplay >= 0 && oi.indexFrameDisplay < fbcount) {
                struct vpu_prp_convert c;
                int idx = oi.indexFrameDisplay;
                c.src_y = fbmem[idx].phy_addr; c.src_w = ii.picWidth; c.src_h = ah;
                c.src_stride = stride; c.dst_rgb = rgbmem.phy_addr;
                c.dst_w = ii.picWidth; c.dst_h = ah; c.dst_stride = ii.picWidth * 2;
                ioctl(prp_fd, VPU_IOC_PRP_CONVERT, &c);
                if (fb_bpp == 16) {
                    /* RGB565 in, RGB565 out — straight memcpy per row. */
                    for (i = 0; i < (int)ii.picHeight; i++)
                        memcpy(fb_map + off_y * fb_stride + off_x * 2 + i * fb_stride,
                               (unsigned char *)rgbmem.virt_uaddr + i * blit_bytes,
                               blit_bytes);
                } else {
                    /* 32bpp target: widen RGB565 -> packed 8:8:8 using the
                     * kernel-reported channel offsets so this works on
                     * xRGB, BGRx, ARGB, ... whatever the panel driver
                     * chose this boot. */
                    int row, col;
                    int ro = fb_r_off, go = fb_g_off, bo = fb_b_off;
                    int rs = 8 - fb_r_len, gs = 8 - fb_g_len, bs = 8 - fb_b_len;
                    for (row = 0; row < (int)ii.picHeight; row++) {
                        unsigned short *src = (unsigned short *)
                            ((unsigned char *)rgbmem.virt_uaddr + row * blit_bytes);
                        unsigned int *dst = (unsigned int *)
                            (fb_map + (off_y + row) * fb_stride + off_x * 4);
                        for (col = 0; col < (int)ii.picWidth; col++) {
                            unsigned short p = src[col];
                            unsigned int r = ((p >> 11) & 0x1F) << 3;
                            unsigned int g = ((p >>  5) & 0x3F) << 2;
                            unsigned int b = ( p        & 0x1F) << 3;
                            /* replicate top bits into low ones so 0x1F -> 0xFF */
                            r |= r >> 5;  g |= g >> 6;  b |= b >> 5;
                            /* narrow each channel to its field width
                             * (6 on the Toon's BGR666-padded panel,
                             * 8 on a normal ARGB32 fb -- shift = 0). */
                            r >>= rs;  g >>= gs;  b >>= bs;
                            dst[col] = (r << ro) | (g << go) | (b << bo);
                        }
                    }
                }
                vpu_DecClrDispFlag(h, idx);
                frames++;
            }
            if (oi.indexFrameDecoded < 0 && oi.indexFrameDisplay < 0)
                if (feed(h, 1) == 0) break;
        }
disc:
        printf("client gone (%d frames); re-listening\n", frames); fflush(stdout);
        vpu_DecClose(h);
        close(sock); sock = -1;
    }
    return 0;
}
