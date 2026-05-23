/* VPU decode to i.MX27 framebuffer — optimized for real-time playback.
 * Usage: vpu_dec_fb <mpeg4|avc> <es-file>
 * Switches fb0 to RGB565 (16bpp) to halve SDRAM traffic vs RGB666/32bpp.
 * Uses word-aligned 32-bit reads from write-combine DMA buffer to avoid
 * the byte-read penalty of uncached memory on ARM926.
 * Target use: live doorbell video via Orange Pi 5 → network → Toon VPU decode. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <linux/fb.h>
#include "vpu_lib.h"
#include "vpu_io.h"

#define STREAM_BUF_SIZE   (3 * 1024 * 1024)
#define PS_SAVE_SIZE      (512 * 1024)
#define SLICE_SAVE_SIZE   (512 * 1024)
#define ALIGN16(x)        (((x) + 15) & ~15)

/* eMMA PrP hardware YUV420->RGB565(+resize), provided by the kernel driver. */
struct vpu_prp_convert {
    unsigned int src_y, src_w, src_h, src_stride;
    unsigned int dst_rgb, dst_w, dst_h, dst_stride;
};
#define VPU_IOC_PRP_CONVERT  _IO('V', 22)

/* BT.601 YUV->RGB565 offset tables.
 * Key fix vs the old G_tab[256][256] (64KB -> thrashes ARM926's 16KB D-cache,
 * ~1 miss/pixel, ~920ns/pixel): split the G offset into two 1-D tables
 * Gu[u]+Gv[v]. All four tables = 4KB and stay cache-resident. Offsets are full
 * ints; clamping happens after adding Y via a 768-entry LUT (branch-free). */
static int R_tab[256];   /* V -> R offset */
static int B_tab[256];   /* U -> B offset */
static int Gu_tab[256];  /* U -> G offset */
static int Gv_tab[256];  /* V -> G offset */
static unsigned char clamp_lut[256 + 512]; /* [-256..511] -> [0..255] */

static inline int clamp8(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return v;
}

static void build_tables(void)
{
    int i;
    for (i = 0; i < 256; i++) {
        R_tab[i]  =  (1402 * (i - 128)) >> 10;
        B_tab[i]  =  (1772 * (i - 128)) >> 10;
        Gu_tab[i] = -((352 * (i - 128)) >> 10);
        Gv_tab[i] = -((731 * (i - 128)) >> 10);
    }
    for (i = -256; i < 512; i++)
        clamp_lut[i + 256] = (unsigned char)(i < 0 ? 0 : (i > 255 ? 255 : i));
}
#define CLAMP(x) clamp_lut[(x) + 256]

/*
 * Copy from write-combine DMA buffer to cached staging using 32-bit reads.
 * On ARM926, LDR from bufferable memory triggers burst AHB transfers;
 * LDRB from bufferable memory is a single-byte bus transaction (very slow).
 */
static void wc_copy32(void *dst, const void *src, int bytes)
{
    unsigned int *d = (unsigned int *)dst;
    const unsigned int *s = (const unsigned int *)src;
    int n = bytes >> 2;  /* /4 */
    while (n--)
        *d++ = *s++;
}

/*
 * YUV420→RGB565 into cached buffer.  dst stride = w * 2 bytes.
 * Reads from cached y/cb/cr pointers (caller staged via wc_copy32).
 */
static void yuv420_to_rgb565(unsigned char *y, unsigned char *cb, unsigned char *cr,
                              int y_stride, int uv_stride,
                              unsigned short *dst, int w, int h)
{
    int i, j;
    for (i = 0; i < h; i++) {
        unsigned char *y_row  = y  + i * y_stride;
        unsigned char *cb_row = cb + (i / 2) * uv_stride;
        unsigned char *cr_row = cr + (i / 2) * uv_stride;
        unsigned short *d_row = dst + i * w;

        /* 2 luma pixels share one chroma sample (4:2:0) -> compute the R/G/B
         * offsets once per pair, only the Y term varies. */
        for (j = 0; j < w; j += 2) {
            int c = j >> 1;
            int u = cb_row[c], v = cr_row[c];
            int ro = R_tab[v];
            int go = Gu_tab[u] + Gv_tab[v];
            int bo = B_tab[u];
            int Y0 = y_row[j], Y1 = y_row[j + 1];
            d_row[j]     = (unsigned short)
                ((CLAMP(Y0 + ro) >> 3 << 11) | (CLAMP(Y0 + go) >> 2 << 5) | (CLAMP(Y0 + bo) >> 3));
            d_row[j + 1] = (unsigned short)
                ((CLAMP(Y1 + ro) >> 3 << 11) | (CLAMP(Y1 + go) >> 2 << 5) | (CLAMP(Y1 + bo) >> 3));
        }
    }
}

static long msec_since(struct timeval *start)
{
    struct timeval now;
    gettimeofday(&now, NULL);
    return (now.tv_sec  - start->tv_sec)  * 1000L +
           (now.tv_usec - start->tv_usec) / 1000L;
}

int main(int argc, char **argv)
{
    CodStd std;
    const char *path;
    FILE *fp;
    long flen;
    unsigned char *fbuf;
    vpu_mem_desc bs = {0}, ps = {0}, slice = {0};
    vpu_mem_desc fbmem[32] = {{0}};
    FrameBuffer fb[32];
    DecHandle h;
    DecOpenParam op;
    DecInitialInfo ii;
    DecBufInfo binfo;
    RetCode r;
    int i, fbcount, stride, ah, ysize, csize, mvsize;
    int decoded = 0, displayed = 0, loops = 0;

    /* Framebuffer */
    int fb_fd;
    struct fb_var_screeninfo vinfo, vinfo_orig, vinfo_new;
    unsigned char *fb_map;
    size_t fb_size;
    int fb_stride, off_x, off_y, fb_w, fb_h;
    int fb_bpp;

    /* Cached staging buffers */
    unsigned short *cvt_buf = NULL;
    unsigned char *yuv_cache = NULL;
    int cvt_w = 0, cvt_h = 0;

    if (argc < 3) {
        fprintf(stderr, "usage: %s <mpeg4|avc> <es-file>\n", argv[0]);
        return 1;
    }
    std = strcmp(argv[1], "avc") == 0 ? STD_AVC : STD_MPEG4;
    path = argv[2];

    /* Run at low priority so the thermostat (normal priority) always preempts
     * us on this single-core ARM926 PREEMPT kernel -- a CPU-heavy software
     * decode/convert run must never lock up the device. */
    setpriority(PRIO_PROCESS, 0, 15);

    /* ---- Open & configure framebuffer for RGB565 ---- */
    fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0) { perror("/dev/fb0"); return 1; }
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
        perror("FBIOGET_VSCREENINFO"); return 1;
    }
    vinfo_orig = vinfo;
    printf("FB orig: %dx%d %dbpp\n", vinfo.xres, vinfo.yres, vinfo.bits_per_pixel);

    /* Switch to 16bpp RGB565 */
    if (vinfo.bits_per_pixel != 16) {
        vinfo_new = vinfo;
        vinfo_new.bits_per_pixel = 16;
        vinfo_new.red.offset   = 11; vinfo_new.red.length   = 5;
        vinfo_new.green.offset = 5;  vinfo_new.green.length = 6;
        vinfo_new.blue.offset  = 0;  vinfo_new.blue.length  = 5;
        vinfo_new.transp.offset = 0; vinfo_new.transp.length = 0;
        if (ioctl(fb_fd, FBIOPUT_VSCREENINFO, &vinfo_new) < 0)
            fprintf(stderr, "Warning: can't switch to 16bpp, staying at %d\n", vinfo.bits_per_pixel);
        else
            ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo);
    }

    fb_w = vinfo.xres; fb_h = vinfo.yres;
    fb_bpp = vinfo.bits_per_pixel;
    fb_stride = vinfo.xres * (fb_bpp / 8);
    fb_size = fb_stride * vinfo.yres;
    fb_map = mmap(NULL, fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (fb_map == MAP_FAILED) { perror("mmap fb"); return 1; }
    printf("FB: %dx%d %dbpp stride=%d\n", fb_w, fb_h, fb_bpp, fb_stride);

    /* ---- Read elementary stream ---- */
    fp = fopen(path, "rb");
    if (!fp) { perror("open es"); return 1; }
    fseek(fp, 0, SEEK_END); flen = ftell(fp); fseek(fp, 0, SEEK_SET);
    if (flen > STREAM_BUF_SIZE) {
        fprintf(stderr, "ES too big (%ld > %d)\n", flen, STREAM_BUF_SIZE);
        return 1;
    }
    fbuf = malloc(flen);
    if (fread(fbuf, 1, flen, fp) != (size_t)flen) { perror("read"); return 1; }
    fclose(fp);
    printf("ES %s: %ld bytes, codec=%s\n", path, flen, std == STD_AVC ? "AVC" : "MPEG4");

    /* ---- Init VPU ---- */
    if (vpu_Init(NULL) != RETCODE_SUCCESS) { fprintf(stderr, "vpu_Init FAILED\n"); return 1; }
    printf("vpu_Init OK\n"); fflush(stdout);

    bs.size = STREAM_BUF_SIZE;
    if (IOGetPhyMem(&bs) || IOGetVirtMem(&bs) <= 0) { fprintf(stderr, "bs alloc fail\n"); return 1; }
    memcpy((void *)bs.virt_uaddr, fbuf, flen);

    memset(&op, 0, sizeof(op));
    op.bitstreamFormat = std;
    op.bitstreamBuffer = bs.phy_addr;
    op.bitstreamBufferSize = bs.size;
    op.reorderEnable = 1;
    op.mp4Class = 0;
    op.chromaInterleave = 0;
    op.filePlayEnable = 0;
    if (std == STD_AVC) {
        ps.size = PS_SAVE_SIZE;
        if (IOGetPhyMem(&ps)) { fprintf(stderr, "ps alloc fail\n"); return 1; }
        op.psSaveBuffer = ps.phy_addr;
        op.psSaveBufferSize = ps.size;
    }
    r = vpu_DecOpen(&h, &op);
    if (r != RETCODE_SUCCESS) { fprintf(stderr, "vpu_DecOpen FAILED: %d\n", r); return 1; }

    vpu_DecUpdateBitstreamBuffer(h, flen);
    vpu_DecSetEscSeqInit(h, 1);
    r = vpu_DecGetInitialInfo(h, &ii);
    vpu_DecSetEscSeqInit(h, 0);
    if (r != RETCODE_SUCCESS) {
        fprintf(stderr, "InitialInfo FAILED: %d\n", r, ii.errorcode);
        vpu_DecClose(h); vpu_UnInit(); return 1;
    }
    printf("Video: %dx%d minFB=%d\n", ii.picWidth, ii.picHeight, ii.minFrameBufferCount);

    off_x = (fb_w - (int)ii.picWidth)  / 2;
    off_y = (fb_h - (int)ii.picHeight) / 2;
    if (off_x < 0) off_x = 0;
    if (off_y < 0) off_y = 0;

    /* Frame buffers */
    stride = ALIGN16(ii.picWidth);
    ah     = ALIGN16(ii.picHeight);
    ysize  = stride * ah;
    csize  = (stride / 2) * (ah / 2);
    mvsize = ALIGN16(ii.picWidth / 16) * ALIGN16(ii.picHeight / 16) * 8;
    fbcount = ii.minFrameBufferCount + 2;
    if (fbcount > 32) fbcount = 32;

    for (i = 0; i < fbcount; i++) {
        fbmem[i].size = ysize + 2 * csize + mvsize;
        if (IOGetPhyMem(&fbmem[i])) { fprintf(stderr, "framebuf %d alloc fail\n", i); return 1; }
        fb[i].strideY  = stride;
        fb[i].strideC  = stride / 2;
        fb[i].bufY     = fbmem[i].phy_addr;
        fb[i].bufCb    = fb[i].bufY + ysize;
        fb[i].bufCr    = fb[i].bufCb + csize;
        fb[i].bufMvCol = fb[i].bufCr + csize;
    }

    memset(&binfo, 0, sizeof(binfo));
    if (std == STD_AVC) {
        slice.size = SLICE_SAVE_SIZE;
        if (IOGetPhyMem(&slice)) { fprintf(stderr, "slice alloc fail\n"); return 1; }
        binfo.avcSliceBufInfo.sliceSaveBuffer = slice.phy_addr;
        binfo.avcSliceBufInfo.sliceSaveBufferSize = slice.size;
    }
    r = vpu_DecRegisterFrameBuffer(h, fb, fbcount, stride, &binfo);
    if (r != RETCODE_SUCCESS) { fprintf(stderr, "RegFB FAILED: %d\n", r); return 1; }

    vpu_DecUpdateBitstreamBuffer(h, 0);

    cvt_w = ii.picWidth;
    cvt_h = ii.picHeight;

    /* RGB565 output buffer for the eMMA PrP (DMA pool, phys + write-combine
     * virt). The PrP converts the decoded YUV here in hardware; we then blit. */
    vpu_mem_desc rgbmem = {0};
    rgbmem.size = ii.picWidth * ah * 2;
    if (IOGetPhyMem(&rgbmem) || IOGetVirtMem(&rgbmem) <= 0) {
        fprintf(stderr, "rgb buffer alloc fail\n"); return 1;
    }
    int prp_fd = open("/dev/mxc_vpu", O_RDWR);
    if (prp_fd < 0) { perror("open prp"); return 1; }

    /* Clear border to black once */
    if (off_x > 0 || off_y > 0) {
        if (off_y > 0) memset(fb_map, 0, off_y * fb_stride);
        if (off_y + (int)ii.picHeight < fb_h)
            memset(fb_map + (off_y + ii.picHeight) * fb_stride, 0,
                   (fb_h - off_y - ii.picHeight) * fb_stride);
        for (i = off_y; i < off_y + (int)ii.picHeight; i++) {
            if (off_x > 0) memset(fb_map + i * fb_stride, 0, off_x * 2);
            if (off_x + (int)ii.picWidth < fb_w)
                memset(fb_map + i * fb_stride + (off_x + ii.picWidth) * 2,
                       0, (fb_w - off_x - ii.picWidth) * 2);
        }
    }

    /* Decode loop with per-phase timing */
    int empty_rounds = 0;
    struct timeval tframe, tafter_decode, tafter_copy, tafter_conv, tstart;
    long decode_ms, copy_ms, conv_ms, blit_ms;
    long acc_decode = 0, acc_copy = 0, acc_conv = 0, acc_blit = 0;
    int blit_bytes = cvt_w * 2;

    printf("Decoding...\n"); fflush(stdout);
    gettimeofday(&tstart, NULL);

    while (loops++ < 800) {
        DecParam dp;
        DecOutputInfo oi;

        gettimeofday(&tframe, NULL);

        memset(&dp, 0, sizeof(dp));
        r = vpu_DecStartOneFrame(h, &dp);
        if (r != RETCODE_SUCCESS) { fprintf(stderr, "StartOneFrame ret %d\n", r); break; }

        i = 0;
        while (vpu_IsBusy()) { vpu_WaitForInt(50); if (++i > 30) break; }

        memset(&oi, 0, sizeof(oi));
        r = vpu_DecGetOutputInfo(h, &oi);
        if (r != RETCODE_SUCCESS) { fprintf(stderr, "GetOutputInfo ret %d\n", r); break; }

        gettimeofday(&tafter_decode, NULL);

        if (oi.indexFrameDecoded >= 0) decoded++;
        if (oi.indexFrameDisplay >= 0) {
            displayed++;

            int idx = oi.indexFrameDisplay;

            /* Stage A: hardware YUV420->RGB565 via eMMA PrP (zero CPU).
             * src_h = padded height so the chroma plane offset matches the VPU
             * frame-buffer layout; we blit only the visible picHeight rows. */
            struct vpu_prp_convert cvt;
            cvt.src_y      = fbmem[idx].phy_addr;
            cvt.src_w      = ii.picWidth;
            cvt.src_h      = ah;
            cvt.src_stride = stride;
            cvt.dst_rgb    = rgbmem.phy_addr;
            cvt.dst_w      = ii.picWidth;
            cvt.dst_h      = ah;
            cvt.dst_stride = ii.picWidth * 2;
            if (ioctl(prp_fd, VPU_IOC_PRP_CONVERT, &cvt) < 0)
                fprintf(stderr, "PRP_CONVERT failed\n");

            gettimeofday(&tafter_copy, NULL);  /* after HW CSC */

            /* Stage B: blit the visible rows from the RGB buffer to the fb */
            for (i = 0; i < cvt_h; i++)
                memcpy(fb_map + off_y * fb_stride + off_x * (fb_bpp / 8) + i * fb_stride,
                       (unsigned char *)rgbmem.virt_uaddr + i * blit_bytes,
                       blit_bytes);

            gettimeofday(&tafter_conv, NULL);
            vpu_DecClrDispFlag(h, idx);

            decode_ms = msec_since(&tframe);
            copy_ms   = msec_since(&tafter_decode);
            conv_ms   = msec_since(&tafter_copy);
            blit_ms   = msec_since(&tafter_conv);
            acc_decode += decode_ms;
            acc_copy   += copy_ms;
            acc_conv   += conv_ms;
            acc_blit   += blit_ms;

            if (displayed <= 5 || displayed % 50 == 0)
                printf("  frame %3d: decode=%ld  copy=%ld  conv=%ld  blit=%ld ms\n",
                       displayed, decode_ms, copy_ms, conv_ms, blit_ms);
        }
        if (oi.indexFrameDecoded < 0 && oi.indexFrameDisplay < 0) {
            if (++empty_rounds > 3) break;
        } else {
            empty_rounds = 0;
        }
        usleep(2000);  /* explicit yield point so other tasks always run */
    }

    if (displayed > 0) {
        printf("avg decode: %ld ms  copy(YUV): %ld ms  conv: %ld ms  blit: %ld ms\n",
               acc_decode / displayed, acc_copy / displayed,
               acc_conv / displayed, acc_blit / displayed);
    }
    {
        long elapsed = msec_since(&tstart);
        printf("DECODE DONE: %d decoded, %d displayed, %ld ms (%.1f fps)\n",
               decoded, displayed, elapsed,
               elapsed > 0 ? (1000.0 * displayed) / elapsed : 0);
    }

    /* Leave the last decoded frame on screen (no clear) for visual confirm. */
    munmap(fb_map, fb_size);
    close(fb_fd);
    free(cvt_buf);
    free(yuv_cache);
    vpu_DecClose(h);
    vpu_UnInit();
    return 0;
}
