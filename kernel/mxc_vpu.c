/*
 * Copyright 2006-2011 Freescale Semiconductor, Inc. All Rights Reserved.
 *
 * The code contained herein is licensed under the GNU General Public
 * License. You may obtain a copy of the GNU General Public License
 * Version 2 or later at the following locations:
 *
 * http://www.opensource.org/licenses/gpl-license.html
 * http://www.gnu.org/copyleft/gpl.html
 */

/*!
 * @file mxc_vpu.c
 *
 * @brief VPU system initialization and file operation implementation
 *
 * @ingroup VPU
 */

#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/interrupt.h>
#include <linux/ioport.h>
#include <linux/stat.h>
#include <linux/platform_device.h>
#include <linux/kdev_t.h>
#include <linux/dma-mapping.h>
#include <linux/iram_alloc.h>
#include <linux/wait.h>
#include <linux/list.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/fsl_devices.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/slab.h>

#include <linux/sched.h>
#include <linux/vmalloc.h>
#include <linux/bitmap.h>
#include <linux/gfp.h>
#include <asm/sizes.h>
#include <asm/cacheflush.h>
#include <mach/clock.h>
#include <mach/hardware.h>

#include <mach/mxc_vpu.h>

struct vpu_priv {
	struct fasync_struct *async_queue;
};

/* To track the allocated memory buffer */
typedef struct memalloc_record {
	struct list_head list;
	struct vpu_mem_desc mem;
} memalloc_record;

struct iram_setting {
	u32 start;
	u32 end;
};

static DEFINE_SPINLOCK(vpu_lock);
static LIST_HEAD(head);

static int vpu_major;
static int vpu_clk_usercount;
static struct class *vpu_class;
static struct vpu_priv vpu_data;
static u8 open_count;
static struct clk *vpu_clk;
static struct vpu_mem_desc bitwork_mem = { 0 };
static struct vpu_mem_desc share_mem = { 0 };
static struct vpu_mem_desc vshare_mem = { 0 };
static struct device *vpu_dev;

static void __iomem *vpu_base;
static void __iomem *ccm_base;	/* i.MX27 CCM, for forcing VPU clock gates */
static void __iomem *sdramc_base;	/* i.MX27 SDRAM controller diagnostics */
static void __iomem *m3if_base;	/* i.MX27 M3IF diagnostics */
static void __iomem *max_base;	/* i.MX27 MAX/AHB arbitration diagnostics; unsafe to read on Toon */
static void __iomem *pp_base;	/* i.MX27 eMMA PP (post-decode YUV->RGB) */
static void __iomem *prp_base;	/* i.MX27 eMMA PrP (HW YUV->RGB + resize) */
static int pp_irq_num;
static int pp_done;
static int pp_status;
static wait_queue_head_t pp_queue;
static DEFINE_MUTEX(pp_lock);
static struct vpu_pp_csc pp_csc = {
	.c0 = 0x95, .c1 = 0xcc, .c2 = 0x32,
	.c3 = 0x68, .c4 = 0x104, .x0 = 1,
};
static int prp_irq_num;
static int prp_done;
static wait_queue_head_t prp_queue;
static DEFINE_MUTEX(prp_lock);
static int allow_prp;
static int iram_size;
static int prp_disabled_warned;
static int vpu_irq;
static u32 phy_vpu_base_addr;
static struct mxc_vpu_platform_data *vpu_plat;

/* IRAM setting */
static struct iram_setting iram;
static unsigned long iram_alloc_size;

#define MX27_SDRAMC_ESDCFG0	0x10
#define MX27_SDRAMC_ESDCFG0_LHD	(1 << 5)

/* implement the blocking ioctl */
static int codec_done;
static wait_queue_head_t vpu_queue;

#define	READ_REG(x)		__raw_readl(vpu_base + x)
#define	WRITE_REG(val, x)	__raw_writel(val, vpu_base + x)

/*
 * Force the i.MX27 VPU clock gates on (CCM_PCCR1 bit 6 = vpu_clk,
 * bit 16 = vpu_clk1). Idempotent and cheap; called before any VPU
 * register access so a gated read/write can never external-abort the
 * kernel. clk_enable() alone proved unreliable on this R10-h28 kernel.
 */
static inline void vpu_clk_force_on(void)
{
	if (ccm_base)
		__raw_writel(__raw_readl(ccm_base + 0x24) | (1 << 6) | (1 << 16),
			     ccm_base + 0x24);
}

static void vpu_dump_regs(const char *name, void __iomem *base, unsigned int bytes)
{
	unsigned int off;

	if (!base) {
		printk(KERN_WARNING "vpu: %s not mapped\n", name);
		return;
	}
	for (off = 0; off < bytes; off += 16) {
		printk(KERN_INFO "vpu: %s+0x%02x: %08x %08x %08x %08x\n",
		       name, off,
		       __raw_readl(base + off + 0x0),
		       __raw_readl(base + off + 0x4),
		       __raw_readl(base + off + 0x8),
		       __raw_readl(base + off + 0xc));
	}
}

static void vpu_dump_bus_regs(const char *tag)
{
	u32 esdcfg0;

	printk(KERN_INFO "vpu: bus diagnostics (%s)\n", tag);
	if (sdramc_base) {
		esdcfg0 = __raw_readl(sdramc_base + MX27_SDRAMC_ESDCFG0);
		printk(KERN_INFO "vpu: SDRAMC ESDCFG0=0x%08x LHD=%u "
		       "(bit5 set means latency hiding disabled)\n",
		       esdcfg0, !!(esdcfg0 & MX27_SDRAMC_ESDCFG0_LHD));
	}
	vpu_dump_regs("SDRAMC", sdramc_base, 0x40);
	vpu_dump_regs("M3IF", m3if_base, 0x40);
	/* MAX exists at MX27_MAX_BASE_ADDR, but probing it caused an external
	 * abort on Toon. Keep the mapping variable for future targeted work, but
	 * do not dump it blindly. */
}

/*
 * ---- Early-boot reserved DMA pool ----
 * dma_alloc_coherent() is capped by ARM's ~2MB consistent region, and the page
 * allocator is hopelessly fragmented once the Qt GUI is up (max free block
 * ~64KB). So at module init -- which must happen EARLY in boot, while large
 * contiguous blocks still exist -- we grab a few MAX_ORDER (4MB) page blocks
 * and serve ALL VPU buffers from them with a per-block bitmap allocator.
 * Buffers are mapped write-combine to userspace (vpu_map_dma_mem); the VPU
 * DMAs to/from the same physical RAM. We flush the cached lowmem alias once at
 * reservation so no stale dirty line can later be written back over DMA data;
 * the driver never touches the buffer contents through that alias afterwards.
 */
#define VPU_POOL_BLOCK_ORDER	10				/* 4 MB / block */
#define VPU_POOL_BLOCK_PAGES	(1U << VPU_POOL_BLOCK_ORDER)	/* 1024 pages */
#define VPU_POOL_BLOCK_SIZE	(VPU_POOL_BLOCK_PAGES << PAGE_SHIFT)
#define VPU_POOL_MAX_BLOCKS	3				/* up to 12 MB */

struct vpu_pool_block {
	struct page *page;
	unsigned long phys;
	unsigned long bitmap[BITS_TO_LONGS(VPU_POOL_BLOCK_PAGES)];
};
static struct vpu_pool_block vpu_pool[VPU_POOL_MAX_BLOCKS];
static int vpu_pool_nblocks;

static void vpu_pool_init(void)
{
	int i;

	for (i = 0; i < VPU_POOL_MAX_BLOCKS; i++) {
		struct page *pg = alloc_pages(GFP_KERNEL, VPU_POOL_BLOCK_ORDER);
		if (!pg)
			break;
		vpu_pool[i].page = pg;
		vpu_pool[i].phys = page_to_phys(pg);
		memset(vpu_pool[i].bitmap, 0, sizeof(vpu_pool[i].bitmap));
		memset(page_address(pg), 0, VPU_POOL_BLOCK_SIZE);
		vpu_pool_nblocks = i + 1;
	}
	/* clean+invalidate so the zeros hit RAM and no dirty alias survives */
	flush_cache_all();
	printk(KERN_INFO "vpu: reserved %d x %u MB DMA pool block(s) = %u MB total\n",
	       vpu_pool_nblocks, VPU_POOL_BLOCK_SIZE >> 20,
	       (unsigned)(vpu_pool_nblocks * (VPU_POOL_BLOCK_SIZE >> 20)));
}

static void vpu_pool_destroy(void)
{
	int i;

	for (i = 0; i < vpu_pool_nblocks; i++)
		__free_pages(vpu_pool[i].page, VPU_POOL_BLOCK_ORDER);
	vpu_pool_nblocks = 0;
}

/* allocate `size` bytes from the pool; returns phys addr or 0 on failure */
static unsigned long vpu_pool_alloc(unsigned long size)
{
	unsigned int npages = PAGE_ALIGN(size) >> PAGE_SHIFT;
	unsigned long idx;
	int i;

	if (!npages || npages > VPU_POOL_BLOCK_PAGES)
		return 0;

	spin_lock(&vpu_lock);
	for (i = 0; i < vpu_pool_nblocks; i++) {
		idx = bitmap_find_next_zero_area(vpu_pool[i].bitmap,
						 VPU_POOL_BLOCK_PAGES, 0, npages, 0);
		if (idx < VPU_POOL_BLOCK_PAGES) {
			bitmap_set(vpu_pool[i].bitmap, idx, npages);
			spin_unlock(&vpu_lock);
			return vpu_pool[i].phys + (idx << PAGE_SHIFT);
		}
	}
	spin_unlock(&vpu_lock);
	return 0;
}

static void vpu_pool_free(unsigned long phys, unsigned long size)
{
	unsigned int npages = PAGE_ALIGN(size) >> PAGE_SHIFT;
	int i;

	if (!phys)
		return;
	spin_lock(&vpu_lock);
	for (i = 0; i < vpu_pool_nblocks; i++) {
		if (phys >= vpu_pool[i].phys &&
		    phys < vpu_pool[i].phys + VPU_POOL_BLOCK_SIZE) {
			unsigned long idx = (phys - vpu_pool[i].phys) >> PAGE_SHIFT;
			bitmap_clear(vpu_pool[i].bitmap, idx, npages);
			break;
		}
	}
	spin_unlock(&vpu_lock);
}

/*!
 * Private function to alloc dma buffer
 * @return status  0 success.
 */
static int vpu_alloc_dma_buffer(struct vpu_mem_desc *mem)
{
	unsigned long phys = vpu_pool_alloc(mem->size);

	if (!phys) {
		printk(KERN_ERR "VPU pool: out of memory (req %u bytes, %d blocks)\n",
		       mem->size, vpu_pool_nblocks);
		return -1;
	}
	mem->phy_addr = phys;
	/* cpu_addr is only an opaque handle for free-tracking; userspace
	 * accesses the buffer via its mmap of phy_addr, never via cpu_addr. */
	mem->cpu_addr = (unsigned long)phys;
	return 0;
}

/*!
 * Private function to free dma buffer
 */
static void vpu_free_dma_buffer(struct vpu_mem_desc *mem)
{
	if (mem->phy_addr != 0)
		vpu_pool_free(mem->phy_addr, mem->size);
}

/*!
 * Private function to free buffers
 * @return status  0 success.
 */
static int vpu_free_buffers(void)
{
	struct memalloc_record *rec, *n;
	struct vpu_mem_desc mem;

	list_for_each_entry_safe(rec, n, &head, list) {
		mem = rec->mem;
		if (mem.cpu_addr != 0) {
			vpu_free_dma_buffer(&mem);
			pr_debug("[FREE] freed paddr=0x%08X\n", mem.phy_addr);
			/* delete from list */
			list_del(&rec->list);
			kfree(rec);
		}
	}

	return 0;
}

/* Force eMMA clock gates on (emma_clk=PCCR0 bit27, emma_clk1=PCCR1 bit18). */
static inline void emma_force_clk_on(void)
{
	if (ccm_base) {
		__raw_writel(__raw_readl(ccm_base + 0x20) | (1 << 27), ccm_base + 0x20);
		__raw_writel(__raw_readl(ccm_base + 0x24) | (1 << 18), ccm_base + 0x24);
	}
}

/*
 * ============================================================================
 * i.MX27 eMMA PP: post-decode YUV420 -> RGB565.  This is the SoC block that
 * was intended to consume decoded YUV frames before display.
 * ============================================================================
 */
#define PPW(o, v)	__raw_writel((v), pp_base + (o))
#define PPR(o)		__raw_readl(pp_base + (o))

#define PP_CNTL			0x00
#define PP_INTRCNTL		0x04
#define PP_INTRSTATUS		0x08
#define PP_SOURCE_Y_PTR		0x0c
#define PP_SOURCE_CB_PTR	0x10
#define PP_SOURCE_CR_PTR	0x14
#define PP_DEST_RGB_PTR		0x18
#define PP_QUANTIZER_PTR	0x1c
#define PP_PROCESS_PARA		0x20
#define PP_FRAME_WIDTH		0x24
#define PP_DISPLAY_WIDTH	0x28
#define PP_IMAGE_SIZE		0x2c
#define PP_DEST_FRAME_FMT_CNTL	0x30
#define PP_RESIZE_INDEX		0x34
#define PP_CSC_COEF_123		0x38
#define PP_CSC_COEF_4		0x3c
#define PP_RESIZE_COEF_TBL	0x100

#define PP_CNTL_CSC_OUT_RGB565	(2 << 10)	/* bits[11:10]=10 -> 16bpp */
#define PP_CNTL_CSC_OUT_RGB32	(3 << 10)	/* bits[11:10]=11 -> 32bpp (24<<7 per mx27_pp.c) */
#define PP_CNTL_SWRST		(1 << 8)
#define PP_CNTL_CSC_TABLE_A0	(1 << 5)
#define PP_CNTL_CSCEN		(1 << 4)
#define PP_CNTL_PP_EN		(1 << 0)

#define PP_INTR_ERR		(1 << 2)
#define PP_INTR_FRAME		(1 << 0)

#define PP_RESIZE_COEF(w, n, op)	(((w) << 3) | ((n) << 1) | (op))
#define PP_SKIP			1
#define PP_TBL_MAX		40
#define PP_BC_NXT		2
#define PP_BC_COEF		5
#define PP_SZ_COEF		(1 << PP_BC_COEF)
#define PP_SZ_NXT		(1 << PP_BC_NXT)

static irqreturn_t pp_irq_handler(int irq, void *dev_id)
{
	u32 st = PPR(PP_INTRSTATUS) & (PP_INTR_ERR | PP_INTR_FRAME);

	if (!st)
		return IRQ_HANDLED;

	PPW(PP_INTRSTATUS, st);
	pp_status = st;
	pp_done = 1;
	wake_up_interruptible(&pp_queue);
	return IRQ_HANDLED;
}

static int pp_csc_valid(const struct vpu_pp_csc *csc)
{
	if (csc->c0 > 0xff || csc->c1 > 0xff || csc->c2 > 0xff ||
	    csc->c3 > 0xff || csc->c4 > 0x1ff || csc->x0 > 1)
		return 0;
	return 1;
}

static unsigned int pp_gcd(unsigned int x, unsigned int y)
{
	unsigned int k;

	if (x < y) {
		k = x;
		x = y;
		y = k;
	}

	while ((k = x % y)) {
		x = y;
		y = k;
	}

	return y;
}

static int pp_scale_0d(u16 *tbl, int k, int coeff, int base, int nxt)
{
	if (k >= PP_TBL_MAX)
		return -ENOSPC;

	coeff = ((coeff << PP_BC_COEF) + (base >> 1)) / base;

	/*
	 * Valid PP weights are 0, 2..30 and 31. 31 is treated by the block as
	 * 32, so avoid generating raw 31 except for the forced 1:1 case.
	 */
	if (coeff >= PP_SZ_COEF - 1)
		coeff--;
	else if (coeff == 1)
		coeff++;
	coeff <<= PP_BC_NXT;

	if (nxt < PP_SZ_NXT) {
		tbl[k++] = (coeff | nxt) << 1 | 1;
	} else {
		coeff |= PP_SKIP;
		tbl[k++] = coeff << 1 | 1;
		nxt -= PP_SKIP;
		do {
			if (k >= PP_TBL_MAX)
				return -ENOSPC;
			coeff = (nxt > PP_SKIP) ? PP_SKIP : nxt;
			tbl[k++] = coeff << 1;
		} while ((nxt -= PP_SKIP) > 0);
	}

	return k;
}

/*
 * Build one PP resize coefficient sequence. This is the Freescale mx27_pp.c
 * algorithm trimmed to exact ratios; callers reduce input/output dimensions
 * before entry so common video sizes fit in the 40-entry hardware table.
 */
static int pp_scale_1d(u16 *tbl, int inv, int outv, int k)
{
	int v;
	int coeff;
	int nxt;

	if (inv == outv)
		return pp_scale_0d(tbl, k, 1, 1, 1);

	v = 0;
	if (inv < outv) {
		do {
			coeff = outv - v;
			v += inv;
			if (v >= outv) {
				v -= outv;
				nxt = 1;
			} else {
				nxt = 0;
			}
			k = pp_scale_0d(tbl, k, coeff, outv, nxt);
			if (k < 0)
				return k;
		} while (v);
	} else if (inv >= 2 * outv) {
		if ((inv != 2 * outv) && (inv != 4 * outv))
			return -EOPNOTSUPP;
		coeff = inv - 2 * outv;
		v = 0;
		do {
			v += coeff;
			nxt = 2;
			while (v >= outv) {
				v -= outv;
				nxt++;
			}
			k = pp_scale_0d(tbl, k, 1, 2, nxt);
			if (k < 0)
				return k;
		} while (v);
	} else {
		int in_pos_inc = 2 * outv;
		int out_pos = inv;
		int out_pos_inc = 2 * inv;
		int init_carry = inv - outv;
		int carry = init_carry;

		v = outv + in_pos_inc;
		do {
			coeff = v - out_pos;
			out_pos += out_pos_inc;
			carry += out_pos_inc;
			for (nxt = 0; v < out_pos; nxt++) {
				v += in_pos_inc;
				carry -= in_pos_inc;
			}
			k = pp_scale_0d(tbl, k, coeff, in_pos_inc, nxt);
			if (k < 0)
				return k;
		} while (carry != init_carry);
	}

	return k;
}

static int pp_build_resize(u16 *tbl, unsigned int src_w, unsigned int src_h,
			   unsigned int dst_w, unsigned int dst_h,
			   u32 *index_reg, int *tbl_len)
{
	unsigned int g;
	int hn, hd, vn, vd;
	int hlen;
	int vlen;

	g = pp_gcd(src_w, dst_w);
	hn = src_w / g;
	hd = dst_w / g;
	g = pp_gcd(src_h, dst_h);
	vn = src_h / g;
	vd = dst_h / g;

	/* PP hardware ratio limits (from Freescale mx27_pp.c scale_1d_smart):
	 * upscale max 4:1, downscale max 2:1 (or exact 4:1). */
	if (hd > 4 * hn || vd > 4 * vn)
		return -EOPNOTSUPP;
	if ((hn > 2 * hd && hn != 4 * hd) || (vn > 2 * vd && vn != 4 * vd))
		return -EOPNOTSUPP;

	hlen = pp_scale_1d(tbl, hn, hd, 0);
	if (hlen <= 0)
		return hlen ? hlen : -EOPNOTSUPP;

	if (hn == vn && hd == vd) {
		vlen = hlen;
		*index_reg = ((hlen - 1) << 16) | (vlen - 1);
	} else {
		vlen = pp_scale_1d(tbl, vn, vd, hlen);
		if (vlen <= hlen)
			return vlen < 0 ? vlen : -EOPNOTSUPP;
		*index_reg = ((hlen - 1) << 16) | (hlen << 8) | (vlen - 1);
	}

	*tbl_len = vlen;
	return 0;
}

/*
 * Full PP hardware reset.  Kills any in-progress DMA, restores the PP to its
 * power-on register state (PP_CNTL == 0x876).  Must be called with the eMMA
 * clock forced on.  Returns 0 on success, -ETIME on timeout, -EIO on bad
 * reset value.
 */
static int pp_hw_reset(void)
{
	int i;

	PPW(PP_CNTL, PP_CNTL_SWRST);
	for (i = 0; i < 1000; i++) {
		if (!(PPR(PP_CNTL) & PP_CNTL_SWRST))
			break;
		udelay(1);
	}
	if (i == 1000)
		return -ETIME;
	if (PPR(PP_CNTL) != 0x876) {
		printk(KERN_WARNING "vpu: PP reset value 0x%08x != 0x876\n",
		       PPR(PP_CNTL));
		return -EIO;
	}
	PPW(PP_INTRSTATUS, PP_INTR_ERR | PP_INTR_FRAME);
	return 0;
}

/* One-shot hardware YUV420(planar) -> RGB565 with PP resize. */
static int pp_convert(struct vpu_pp_convert *c)
{
	u16 resize_tbl[PP_TBL_MAX];
	u32 ysz;
	u32 src_layout_h;
	u32 fmt;
	u32 cntl;
	u32 resize_index;
	u32 dst_bpp;
	u32 dst_bytes;
	int resize_len;
	int i;
	int rc;

	dst_bpp = c->dst_bpp ? c->dst_bpp : 16;
	/* PP mode 00 (4-byte pixel) works for 1:1 but hangs with resize
	 * (the scaler is incompatible with this mode).  Reject early so
	 * the fallback path handles upscale without hanging the PP. */
	if (dst_bpp == 32 && (c->src_w != c->dst_w || c->src_h != c->dst_h))
		return -EOPNOTSUPP;
	dst_bytes = dst_bpp / 8;
	if (!pp_base)
		return -ENODEV;
	if (c->src_w < 32 || c->src_h < 32 || c->dst_w < 8 || c->dst_h < 8) {
		printk(KERN_DEBUG "vpu: PP size check fail src=%ux%u dst=%ux%u\n",
		       c->src_w, c->src_h, c->dst_w, c->dst_h);
		return -EINVAL;
	}
	if ((c->src_w & 7) || (c->src_h & 7) || (c->src_stride & 7)) {
		printk(KERN_DEBUG "vpu: PP src align fail src=%ux%u stride=%u\n",
		       c->src_w, c->src_h, c->src_stride);
		return -EINVAL;
	}
	if ((c->dst_w & 1) || (c->dst_h & 1) || (c->dst_stride & (dst_bytes - 1))) {
		printk(KERN_DEBUG "vpu: PP dst align fail dst=%ux%u stride=%u bpp=%u\n",
		       c->dst_w, c->dst_h, c->dst_stride, dst_bpp);
		return -EINVAL;
	}
	if ((c->src_y | c->dst_rgb) & 3) {
		printk(KERN_DEBUG "vpu: PP addr align fail src=0x%x dst=0x%x\n",
		       c->src_y, c->dst_rgb);
		return -EINVAL;
	}
	if (c->src_stride < c->src_w || c->dst_stride < c->dst_w * dst_bytes) {
		printk(KERN_DEBUG "vpu: PP stride fail src_stride=%u src_w=%u dst_stride=%u dst_w=%u bpp=%u\n",
		       c->src_stride, c->src_w, c->dst_stride, c->dst_w, dst_bpp);
		return -EINVAL;
	}
	rc = pp_build_resize(resize_tbl, c->src_w, c->src_h, c->dst_w, c->dst_h,
			     &resize_index, &resize_len);
	if (rc) {
		printk(KERN_DEBUG "vpu: PP resize fail src=%ux%u dst=%ux%u rc=%d\n",
		       c->src_w, c->src_h, c->dst_w, c->dst_h, rc);
		return rc == -ENOSPC ? -EOPNOTSUPP : rc;
	}

	mutex_lock(&pp_lock);
	emma_force_clk_on();

	rc = pp_hw_reset();
	if (rc) {
		mutex_unlock(&pp_lock);
		return rc;
	}

	/*
	 * The VPU lays out Cb/Cr after the 16-line-aligned Y plane, while PP
	 * should process only the visible height. Deriving chroma addresses
	 * from c->src_h corrupts the first chroma rows for heights like 360.
	 */
	src_layout_h = (c->src_h + 15) & ~15;
	ysz = c->src_stride * src_layout_h;
	PPW(PP_SOURCE_Y_PTR, c->src_y);
	PPW(PP_SOURCE_CB_PTR, c->src_y + ysz);
	PPW(PP_SOURCE_CR_PTR, c->src_y + ysz + (ysz >> 2));
	PPW(PP_DEST_RGB_PTR, c->dst_rgb);
	PPW(PP_QUANTIZER_PTR, c->src_qp);

	PPW(PP_PROCESS_PARA, (c->src_w << 16) | c->src_h);
	PPW(PP_FRAME_WIDTH, c->src_stride);
	PPW(PP_DISPLAY_WIDTH, c->dst_stride);
	PPW(PP_IMAGE_SIZE, (c->dst_w << 16) | c->dst_h);

	if (dst_bpp == 32) {
		/* Toon fb custom 32bpp: 6-bit components at R@18 G@10 B@2.
		 * PP hardware rejects >6-bit component widths, so standard
		 * 8-bit BGR32 is not an option. */
		fmt = (18 << 26) | (10 << 21) | (2 << 16) |
		      (6 << 8) | (6 << 4) | 6;
	} else {
		fmt = (11 << 26) | (5 << 21) | (0 << 16) |
		      (5 << 8) | (6 << 4) | 5;
	}
	PPW(PP_DEST_FRAME_FMT_CNTL, fmt);

	PPW(PP_CSC_COEF_123, (pp_csc.c0 << 24) | (pp_csc.c1 << 16) |
			     (pp_csc.c2 << 8) | pp_csc.c3);
	PPW(PP_CSC_COEF_4, (pp_csc.x0 ? (1 << 9) : 0) | pp_csc.c4);

	PPW(PP_RESIZE_INDEX, resize_index);
	for (i = 0; i < resize_len; i++)
		PPW(PP_RESIZE_COEF_TBL + i * 4, resize_tbl[i]);

	PPW(PP_INTRCNTL, PP_INTR_ERR | PP_INTR_FRAME);
	pp_status = 0;
	pp_done = 0;

	/* Read-modify-write PP_CNTL matching Freescale pphw_cfg():
	 * EN_MASK=0x36 clears bits 1,2,4,5 (operation bits); bit 2 in
	 * particular conflicts with CSC when left at reset value 1. */
	cntl = PPR(PP_CNTL) & ~(c->src_qp ? 0x34 : 0x36);
	if (c->src_qp)
		cntl |= 0x02;	/* EN_DEBLOCK */
	/* auto-derive rgb_resolution (mx27_pp.c pphw_cfg lines 962-984).
	 * Mode 00 (bits[11:10]=00) is 32bpp/4-byte-per-pixel output;
	 * mode 11 (bits[11:10]=11) is 24bpp/3-byte-per-pixel output.
	 * Freescale only uses 11 but the manual says 00 is also 32bpp.
	 * We use 00 so the output stride matches the Toon 4-byte fb. */
	{
		u32 w, w2;
		int red_off = (fmt >> 26) & 0x3f;
		int grn_off = (fmt >> 21) & 0x1f;
		int blu_off = (fmt >> 16) & 0x1f;
		int red_wid = (fmt >> 8) & 0xf;
		int grn_wid = (fmt >> 4) & 0xf;
		int blu_wid = fmt & 0xf;
		w = red_off + red_wid;
		w2 = blu_off + blu_wid; if (w < w2) w = w2;
		w2 = grn_off + grn_wid; if (w < w2) w = w2;
		cntl &= ~0xC00;
		if (dst_bpp == 32) {
			/* mode 00: 32bpp, 4-byte output pixel */
		} else if (w > 16) {
			cntl |= (24 << 7);  /* mode 11: 24bpp */
		} else if (w > 8) {
			cntl |= (16 << 7);  /* mode 10: 16bpp */
		} else {
			cntl |= (8 << 7);   /* mode 01: 8bpp */
		}
	}
	cntl |= PP_CNTL_CSCEN;
	cntl |= PP_CNTL_CSC_TABLE_A0 | PP_CNTL_PP_EN;
	PPW(PP_CNTL, cntl);

	rc = wait_event_interruptible_timeout(pp_queue, pp_done != 0,
					      msecs_to_jiffies(200));
	if (rc <= 0) {
		printk(KERN_WARNING "vpu: PP timed out; resetting\n");
		pp_hw_reset();
		mutex_unlock(&pp_lock);
		return rc < 0 ? rc : -ETIME;
	}
	if (pp_status & PP_INTR_ERR) {
		printk(KERN_WARNING "vpu: PP error interrupt; resetting\n");
		pp_hw_reset();
		mutex_unlock(&pp_lock);
		return -EIO;
	}
	mutex_unlock(&pp_lock);
	return 0;
}

/*
 * ============================================================================
 * i.MX27 eMMA PrP: hardware YUV420 -> RGB565 (+ resize), offloads CPU CSC.
 * Register layout, CSC coefficients and the resize-coefficient algorithm are
 * ported from the Freescale BSP mx27_prphw.c (see ~/QB2-OSS/emma-ref/).
 * Register access goes through prp_base (ioremap of MX27_EMMA_PRP_BASE_ADDR).
 * ============================================================================
 */
#define PRPW(o, v)	__raw_writel((v), prp_base + (o))
#define PRPR(o)		__raw_readl(prp_base + (o))

/* register offsets */
#define PRP_CNTL			0x00
#define PRP_INTRCNTL			0x04
#define PRP_INTRSTATUS			0x08
#define PRP_SOURCE_Y_PTR		0x0c
#define PRP_SOURCE_CB_PTR		0x10
#define PRP_SOURCE_CR_PTR		0x14
#define PRP_DEST_RGB1_PTR		0x18
#define PRP_SOURCE_FRAME_SIZE		0x2c
#define PRP_CH1_LINE_STRIDE		0x30
#define PRP_SRC_PIXEL_FORMAT_CNTL	0x34
#define PRP_CH1_PIXEL_FORMAT_CNTL	0x38
#define PRP_CH1_OUT_IMAGE_SIZE		0x3c
#define PRP_SOURCE_LINE_STRIDE		0x44
#define PRP_CSC_COEF_012		0x48
#define PRP_CSC_COEF_345		0x4c
#define PRP_CSC_COEF_678		0x50
#define PRP_CH1_RZ_HORI_COEF1		0x54	/* +0x0c per dir, CH1 only */

/* PRP_CNTL bits */
#define PRP_CNTL_CH1EN			(1 << 0)
#define PRP_CNTL_IN_YUV420		0
#define PRP_CNTL_IN_RGB16		(1 << 4)	/* RGB565 input, bypass CSC */
#define PRP_CNTL_CH1_RGB16		(1 << 5)
#define PRP_CNTL_CH1_RGB32		(2 << 5)	/* bits [6:5] = 2 */
#define PRP_CNTL_RST			(1 << 12)
#define PRP_CNTL_RSTVAL			0x28
/* PRP_INTRCNTL / status bits */
#define PRP_INTRCNTL_RDERR		(1 << 0)
#define PRP_INTRCNTL_CH1WERR		(1 << 1)
#define PRP_INTRCNTL_CH1FC		(1 << 3)
#define PRP_INTRCNTL_LBOVF		(1 << 7)
/* pixel format codes — output (CH1) */
#define PRP_PIX1_RGB565			0x2CA00565
#define PRP_PIX1_RGB888			0x41000888
#define PRP_PIX1_TOON32			0x49420666	/* R@18/6 G@10/6 B@2/6 */
/* pixel format codes — input */
#define PRP_PIXIN_YUV420		0
#define PRP_PIXIN_RGB565		0x2CA00565	/* RGB565 input */

/* scaler (ported verbatim from mx27_prphw.c) */
#define BC_COEF		3
#define MAX_TBL		20
#define SZ_COEF		(1 << BC_COEF)
#define ALGO_AUTO	0
#define ALGO_BIL	1
#define ALGO_AVG	2
#define SCALE_RETRY	16

typedef struct {
	char tbl[20];
	char len;
	char algo;
	char ratio[20];
} scale_t;

static unsigned char prp_scale_get(scale_t *t, unsigned char *i, unsigned char *out)
{
	unsigned char c = t->tbl[*i];
	(*i)++;
	*i %= t->len;
	if (out) {
		if (t->algo == ALGO_BIL) {
			for ((*out) = 1; (*i) && ((*i) < t->len) && !t->tbl[(*i)]; (*i)++)
				(*out)++;
			if ((*i) == t->len)
				(*i) = 0;
		} else
			*out = c >> BC_COEF;
	}
	c &= SZ_COEF - 1;
	if (c == SZ_COEF - 1)
		c = SZ_COEF;
	return c;
}

static int prp_gcd(int x, int y)
{
	int k;
	if (x < y) { k = x; x = y; y = k; }
	while ((k = x % y)) { x = y; y = k; }
	return y;
}

static int prp_ratio(int x, int y, int *den)
{
	int g;
	if (!x || !y)
		return 0;
	g = prp_gcd(x, y);
	*den = y / g;
	return x / g;
}

static int prp_scale_bilinear(scale_t *t, int coeff, int base, int nxt)
{
	int i;
	if (t->len >= (int)sizeof(t->tbl))
		return -1;
	coeff = ((coeff << BC_COEF) + (base >> 1)) / base;
	if (coeff >= SZ_COEF - 1)
		coeff--;
	coeff |= SZ_COEF;
	t->tbl[(int)t->len++] = (unsigned char)coeff;
	for (i = 1; i < nxt; i++) {
		if (t->len >= MAX_TBL)
			return -1;
		t->tbl[(int)t->len++] = 0;
	}
	return t->len;
}

static const unsigned char prp_c1[] = {7};
static const unsigned char prp_c2[] = {4, 4};
static const unsigned char prp_c3[] = {2, 4, 2};
static const unsigned char prp_c4[] = {2, 2, 2, 2};
static const unsigned char prp_c5[] = {1, 2, 2, 2, 1};
static const unsigned char prp_c6[] = {1, 1, 2, 2, 1, 1};
static const unsigned char prp_c7[] = {1, 1, 1, 2, 1, 1, 1};
static const unsigned char prp_c8[] = {1, 1, 1, 1, 1, 1, 1, 1};
static const unsigned char prp_c9[] = {1, 1, 1, 1, 1, 1, 1, 1, 0};
static const unsigned char prp_c10[] = {0, 1, 1, 1, 1, 1, 1, 1, 1, 0};
static const unsigned char prp_c11[] = {0, 1, 1, 1, 1, 0, 1, 1, 1, 1, 0};
static const unsigned char prp_c12[] = {0, 1, 1, 0, 1, 1, 1, 1, 0, 1, 1, 0};
static const unsigned char prp_c13[] = {0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0};
static const unsigned char prp_c14[] = {0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 0, 1, 0};
static const unsigned char prp_c15[] = {0, 1, 0, 1, 0, 1, 1, 0, 1, 1, 0, 1, 0, 1, 0};
static const unsigned char prp_c16[] = {1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0};
static const unsigned char prp_c17[] = {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0};
static const unsigned char prp_c18[] = {0, 1, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0, 1, 0};
static const unsigned char prp_c19[] = {0, 1, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0};
static const unsigned char prp_c20[] = {0, 1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0};
static const unsigned char *prp_ave_coeff[] = {
	prp_c1, prp_c2, prp_c3, prp_c4, prp_c5, prp_c6, prp_c7, prp_c8, prp_c9, prp_c10,
	prp_c11, prp_c12, prp_c13, prp_c14, prp_c15, prp_c16, prp_c17, prp_c18, prp_c19, prp_c20
};

static int prp_scale_ave(scale_t *t, unsigned char base)
{
	if (t->len + base > (int)sizeof(t->tbl))
		return -1;
	memcpy(&t->tbl[(int)t->len], prp_ave_coeff[(int)base - 1], base);
	t->len = (unsigned char)(t->len + base);
	t->tbl[t->len - 1] |= SZ_COEF;
	return t->len;
}

static int prp_ave_scale(scale_t *t, int inv, int outv)
{
	int ratio_count = 0;
	if (outv != 1) {
		unsigned char a[20];
		int v;
		for (v = 0; v < outv; v++)
			a[v] = (unsigned char)(inv / outv);
		inv %= outv;
		if (inv) {
			v = (outv - inv) >> 1;
			inv += v;
			for (; v < inv; v++)
				a[v]++;
		}
		for (v = 0; v < outv; v++) {
			if (prp_scale_ave(t, a[v]) < 0)
				return -1;
			t->ratio[ratio_count] = a[v];
			ratio_count++;
		}
	} else if (prp_scale_ave(t, inv) < 0) {
		return -1;
	} else {
		t->ratio[ratio_count++] = (char)inv;
		ratio_count++;
	}
	return t->len;
}

static int prp_scale_build(scale_t *t, int inv, int outv)
{
	int v, coeff, nxt;
	t->len = 0;
	if (t->algo == ALGO_AUTO)
		t->algo = ((outv != inv) && ((2 * outv) > inv)) ? ALGO_BIL : ALGO_AVG;
	if ((inv == outv) && (t->algo == ALGO_BIL))
		t->algo = ALGO_AVG;	/* 1:1 bilinear hangs */
	memset(t->tbl, 0, sizeof(t->tbl));
	if (t->algo == ALGO_BIL) {
		t->ratio[0] = (char)inv;
		t->ratio[1] = (char)outv;
	} else
		memset(t->ratio, 0, sizeof(t->ratio));
	if (inv == outv) {
		t->ratio[0] = 1;
		if (t->algo == ALGO_BIL)
			t->ratio[1] = 1;
		return prp_scale_ave(t, 1);
	}
	if (inv < outv)
		return -1;	/* upscaling not supported by PrP */
	if (t->algo != ALGO_BIL)
		return prp_ave_scale(t, inv, outv);
	v = 0;
	if (inv >= 2 * outv) {
		coeff = inv - 2 * outv;
		v = 0;
		do {
			v += coeff;
			nxt = 2;
			while (v >= outv) { v -= outv; nxt++; }
			if (prp_scale_bilinear(t, 1, 2, nxt) < 0)
				return -1;
		} while (v);
	} else {
		int in_pos_inc = 2 * outv;
		int out_pos = inv;
		int out_pos_inc = 2 * inv;
		int init_carry = inv - outv;
		int carry = init_carry;
		v = outv + in_pos_inc;
		do {
			coeff = v - out_pos;
			out_pos += out_pos_inc;
			carry += out_pos_inc;
			for (nxt = 0; v < out_pos; nxt++) { v += in_pos_inc; carry -= in_pos_inc; }
			if (prp_scale_bilinear(t, coeff, in_pos_inc, nxt) < 0)
				return -1;
		} while (carry != init_carry);
	}
	return t->len;
}

/* Build CH1 resize table for inv->*vout; returns 0 ok, sets *vout to real out. */
static int prp_scale(scale_t *pscale, int din, int dout, int inv,
		     unsigned short *vout, int retry)
{
	int num, den;
	unsigned short outv;
	if (din < dout)
		return -1;
lp_retry:
	num = prp_ratio(din, dout, &den);
	if (!num)
		return -1;
	if (num > MAX_TBL || prp_scale_build(pscale, num, den) < 0) {
		dout++;
		if (retry--)
			goto lp_retry;
		return -1;
	}
	if (pscale->algo == ALGO_BIL) {
		unsigned char i, j, k;
		outv = (unsigned short)(inv / pscale->ratio[0] * pscale->ratio[1]);
		inv %= pscale->ratio[0];
		for (i = j = 0; inv > 0; j++) {
			unsigned char nxt;
			k = prp_scale_get(pscale, &i, &nxt);
			if (inv == 1 && k < SZ_COEF)
				break;
			inv -= nxt;
		}
		outv = outv + j;
	} else {
		unsigned char i, tot;
		for (tot = i = 0; pscale->ratio[i]; i++)
			tot = tot + pscale->ratio[i];
		outv = (unsigned short)(inv / tot) * i;
		inv %= tot;
		for (i = 0; inv > 0; i++, outv++)
			inv -= pscale->ratio[i];
	}
	if (!(*vout) || ((*vout) > outv))
		*vout = outv;
	return 0;
}

/* Write CH1 resize coefficients (dir 0=horizontal/width, 1=vertical/height). */
static void prp_set_scaler(int dir, scale_t *scale)
{
	unsigned long off = PRP_CH1_RZ_HORI_COEF1 + dir * 0xc;
	unsigned int coeff[2], valid;
	int i, j;
	for (coeff[0] = coeff[1] = valid = 0, i = 19; i >= 0; i--) {
		j = i > 9 ? 1 : 0;
		coeff[j] = (coeff[j] << BC_COEF) | (scale->tbl[i] & (SZ_COEF - 1));
		if (i == 5 || i == 15)
			coeff[j] <<= 1;
		valid = (valid << 1) | (scale->tbl[i] >> BC_COEF);
	}
	valid |= (scale->len << 24) | ((2 - scale->algo) << 31);
	PRPW(off + 0, coeff[0]);
	PRPW(off + 4, coeff[1]);
	PRPW(off + 8, valid);
}

static inline void prp_force_clk_on(void)
{
	emma_force_clk_on();
}

static irqreturn_t prp_irq_handler(int irq, void *dev_id)
{
	u32 st = PRPR(PRP_INTRSTATUS) & 0x1ff;
	/* silicon bug: CH1EN doesn't self-clear */
	PRPW(PRP_CNTL, PRPR(PRP_CNTL) & ~PRP_CNTL_CH1EN);
	PRPW(PRP_INTRSTATUS, st);	/* clear */
	prp_done = 1;
	wake_up_interruptible(&prp_queue);
	return IRQ_HANDLED;
}

/* One-shot hardware YUV420(planar) -> RGB565/888 (+resize). */
static int prp_convert(struct vpu_prp_convert *c)
{
	scale_t sw, sh;
	unsigned short outw = 0, outh = 0;
	u32 sz;
	u32 dst_bpp;
	u32 ch1_pix;
	u32 ch1_rgb;
	u32 in_bits;
	int in_rgb;
	int i, rc;

	in_rgb = (c->in_fmt != 0);

	if (!prp_base)
		return -ENODEV;
	if (c->src_w < 32 || c->src_h < 32 || c->dst_w < 8 || c->dst_h < 8)
		return -EINVAL;

	dst_bpp = c->dst_bpp ? c->dst_bpp : 16;

	memset(&sw, 0, sizeof(sw)); sw.algo = ALGO_AUTO;
	memset(&sh, 0, sizeof(sh)); sh.algo = ALGO_AUTO;
	if (prp_scale(&sw, c->src_w, c->dst_w, c->src_w, &outw, SCALE_RETRY) < 0)
		return -EINVAL;
	if (prp_scale(&sh, c->src_h, c->dst_h, c->src_h, &outh, SCALE_RETRY) < 0)
		return -EINVAL;
	outw &= ~1;	/* width must be even for both 16bpp and 32bpp */

	mutex_lock(&prp_lock);
	prp_force_clk_on();

	/* soft reset */
	PRPW(PRP_CNTL, PRP_CNTL_RST);
	for (i = 0; i < 1000; i++) {
		if (PRPR(PRP_CNTL) == PRP_CNTL_RSTVAL)
			break;
		udelay(1);
	}

	/* CSC coefficients always written (Freescale reference does it for
	 * both YUV and RGB input; the IN_RGB bit in CNTL tells the block
	 * whether to treat them as YUV2RGB or RGB2YUV). */
	PRPW(PRP_CSC_COEF_012, 0x12A66032);
	PRPW(PRP_CSC_COEF_345, 0x0D082000);
	PRPW(PRP_CSC_COEF_678, 0x80000000);

	/* input setup: YUV420 planar or RGB single-plane */
	PRPW(PRP_SRC_PIXEL_FORMAT_CNTL, in_rgb ? c->in_fmt : PRP_PIXIN_YUV420);
	PRPW(PRP_SOURCE_FRAME_SIZE, (c->src_w << 16) | c->src_h);
	PRPW(PRP_SOURCE_LINE_STRIDE, c->src_stride);
	PRPW(PRP_SOURCE_Y_PTR, c->src_y);
	if (in_rgb) {
		PRPW(PRP_SOURCE_CB_PTR, c->src_y);
		PRPW(PRP_SOURCE_CR_PTR, c->src_y);
	} else {
		sz = c->src_stride * c->src_h;
		PRPW(PRP_SOURCE_CB_PTR, c->src_y + sz);
		PRPW(PRP_SOURCE_CR_PTR, c->src_y + sz + (sz >> 2));
	}

	/* output CH1 — 16bpp or Toon 32bpp */
	if (dst_bpp == 32) {
		ch1_pix = PRP_PIX1_TOON32;
		ch1_rgb = PRP_CNTL_CH1_RGB32;
	} else {
		ch1_pix = PRP_PIX1_RGB565;
		ch1_rgb = PRP_CNTL_CH1_RGB16;
	}
	PRPW(PRP_CH1_PIXEL_FORMAT_CNTL, ch1_pix);
	PRPW(PRP_CH1_OUT_IMAGE_SIZE, (outw << 16) | outh);
	PRPW(PRP_CH1_LINE_STRIDE, c->dst_stride);
	PRPW(PRP_DEST_RGB1_PTR, c->dst_rgb);

	prp_set_scaler(0, &sw);	/* width */
	prp_set_scaler(1, &sh);	/* height */

	PRPW(PRP_INTRCNTL, PRP_INTRCNTL_CH1FC | PRP_INTRCNTL_RDERR |
	     PRP_INTRCNTL_CH1WERR | PRP_INTRCNTL_LBOVF);

	prp_done = 0;
	in_bits = in_rgb ? PRP_CNTL_IN_RGB16 : PRP_CNTL_IN_YUV420;
	PRPW(PRP_CNTL, in_bits | ch1_rgb | PRP_CNTL_CH1EN);

	rc = wait_event_interruptible_timeout(prp_queue, prp_done != 0,
					      msecs_to_jiffies(200));
	if (rc <= 0) {
		PRPW(PRP_CNTL, PRPR(PRP_CNTL) & ~PRP_CNTL_CH1EN);
		mutex_unlock(&prp_lock);
		if (in_rgb)
			printk(KERN_WARNING "vpu: PrP RGB->RGB timed out "
			       "src=%ux%u dst=%ux%u in_fmt=0x%x\n",
			       c->src_w, c->src_h, c->dst_w, c->dst_h, c->in_fmt);
		return -ETIME;
	}
	mutex_unlock(&prp_lock);
	return 0;
}

/*
 * VPU interrupt handler — wakes waiters directly (no workqueue).
 * The deferred workqueue was adding latency and causing 50ms timeouts.
 * Clock stays on while device is open (userspace controls via CLKGATE_SETTING).
 */
static irqreturn_t vpu_irq_handler(int irq, void *dev_id)
{
	READ_REG(BIT_INT_STATUS);
	WRITE_REG(0x1, BIT_INT_CLEAR);

	codec_done = 1;
	wake_up_interruptible(&vpu_queue);

	return IRQ_HANDLED;
}

/*!
 * @brief open function for vpu file operation
 *
 * @return  0 on success or negative error code on error
 */
static int vpu_open(struct inode *inode, struct file *filp)
{
	spin_lock(&vpu_lock);
	open_count++;
	filp->private_data = (void *)(&vpu_data);
	spin_unlock(&vpu_lock);
	return 0;
}

/*!
 * @brief IO ctrl function for vpu file operation
 * @param cmd IO ctrl command
 * @return  0 on success or negative error code on error
 */
static long vpu_ioctl(struct file *filp, unsigned int cmd,
		     unsigned long arg)
{
	int ret = 0;

	switch (cmd) {
	case VPU_IOC_PHYMEM_ALLOC:
		{
			struct memalloc_record *rec;

			rec = kzalloc(sizeof(*rec), GFP_KERNEL);
			if (!rec)
				return -ENOMEM;

			ret = copy_from_user(&(rec->mem),
					     (struct vpu_mem_desc *)arg,
					     sizeof(struct vpu_mem_desc));
			if (ret) {
				kfree(rec);
				return -EFAULT;
			}

			pr_debug("[ALLOC] mem alloc size = 0x%x\n",
				 rec->mem.size);

			ret = vpu_alloc_dma_buffer(&(rec->mem));
			if (ret == -1) {
				kfree(rec);
				printk(KERN_ERR
				       "Physical memory allocation error!\n");
				break;
			}
			ret = copy_to_user((void __user *)arg, &(rec->mem),
					   sizeof(struct vpu_mem_desc));
			if (ret) {
				kfree(rec);
				ret = -EFAULT;
				break;
			}

			spin_lock(&vpu_lock);
			list_add(&rec->list, &head);
			spin_unlock(&vpu_lock);

			break;
		}
	case VPU_IOC_PHYMEM_FREE:
		{
			struct memalloc_record *rec, *n;
			struct vpu_mem_desc vpu_mem;

			ret = copy_from_user(&vpu_mem,
					     (struct vpu_mem_desc *)arg,
					     sizeof(struct vpu_mem_desc));
			if (ret)
				return -EACCES;

			pr_debug("[FREE] mem freed cpu_addr = 0x%x\n",
				 vpu_mem.cpu_addr);
			if ((void *)vpu_mem.cpu_addr != NULL) {
				vpu_free_dma_buffer(&vpu_mem);
			}

			spin_lock(&vpu_lock);
			list_for_each_entry_safe(rec, n, &head, list) {
				if (rec->mem.cpu_addr == vpu_mem.cpu_addr) {
					/* delete from list */
					list_del(&rec->list);
					kfree(rec);
					break;
				}
			}
			spin_unlock(&vpu_lock);

			break;
		}
	case VPU_IOC_WAIT4INT:
		{
			u_long timeout = (u_long) arg;
			if (!wait_event_interruptible_timeout
			    (vpu_queue, codec_done != 0,
			     msecs_to_jiffies(timeout))) {
				ret = -ETIME;
			} else if (signal_pending(current)) {
				printk(KERN_WARNING
				       "VPU interrupt received.\n");
				ret = -ERESTARTSYS;
			} else
				codec_done = 0;
			break;
		}
	case VPU_IOC_VL2CC_FLUSH:
		flush_cache_all();
		break;
	case VPU_IOC_REG_READ:
		{
			struct vpu_reg_rw rw;

			if (copy_from_user(&rw, (void __user *)arg, sizeof(rw)))
				return -EFAULT;
			if (rw.offset >= SZ_4K || (rw.offset & 3))
				return -EINVAL;
			vpu_clk_force_on();
			rw.value = READ_REG(rw.offset);
			if (copy_to_user((void __user *)arg, &rw, sizeof(rw)))
				return -EFAULT;
			break;
		}
	case VPU_IOC_REG_WRITE:
		{
			struct vpu_reg_rw rw;

			if (copy_from_user(&rw, (void __user *)arg, sizeof(rw)))
				return -EFAULT;
			if (rw.offset >= SZ_4K || (rw.offset & 3))
				return -EINVAL;
			vpu_clk_force_on();
			WRITE_REG(rw.value, rw.offset);
			break;
		}
	case VPU_IOC_PRP_CONVERT:
		{
			struct vpu_prp_convert cvt;

			if (!allow_prp) {
				if (!prp_disabled_warned) {
					prp_disabled_warned = 1;
					printk(KERN_WARNING
					       "vpu: PrP convert disabled; load mxc_vpu with allow_prp=1 to enable\n");
				}
				ret = -EOPNOTSUPP;
				break;
			}
			if (copy_from_user(&cvt, (void __user *)arg, sizeof(cvt)))
				return -EFAULT;
			ret = prp_convert(&cvt);
			break;
		}
	case VPU_IOC_PP_CONVERT:
		{
			struct vpu_pp_convert cvt;

			if (copy_from_user(&cvt, (void __user *)arg, sizeof(cvt)))
				return -EFAULT;
			ret = pp_convert(&cvt);
			break;
		}
	case VPU_IOC_PP_SET_CSC:
		{
			struct vpu_pp_csc csc;

			if (copy_from_user(&csc, (void __user *)arg, sizeof(csc)))
				return -EFAULT;
			if (!pp_csc_valid(&csc))
				return -EINVAL;
			mutex_lock(&pp_lock);
			pp_csc = csc;
			mutex_unlock(&pp_lock);
			break;
		}
	case VPU_IOC_PP_GET_CSC:
		{
			struct vpu_pp_csc csc;

			mutex_lock(&pp_lock);
			csc = pp_csc;
			mutex_unlock(&pp_lock);
			if (copy_to_user((void __user *)arg, &csc, sizeof(csc)))
				return -EFAULT;
			break;
		}
	case VPU_IOC_IRAM_SETTING:
		{
			ret = copy_to_user((void __user *)arg, &iram,
					   sizeof(struct iram_setting));
			if (ret)
				ret = -EFAULT;

			break;
		}
	case VPU_IOC_CLKGATE_SETTING:
		/* The clock is forced on at init and before every register
		 * access via vpu_clk_force_on() (direct CCM PCCR1 write).
		 * The clock-framework enable/disable counting in the
		 * library drifts on error paths and causes refcount
		 * underflow WARNINGs at __clk_disable.  Ignore the ioctl
		 * entirely -- the hardware clock stays on regardless. */
		break;
		case VPU_IOC_GET_SHARE_MEM:
			{
				/*
				 * The share mem holds the library's host-side
				 * PROCESS_SHARED pthread mutexes + codec instance
				 * pool -- the VPU never DMAs it. It MUST be
				 * page-backed cacheable memory, or glibc's futex
				 * fails ("futex facility returned an unexpected
				 * error") on a VM_PFNMAP/write-combine mapping.
				 * So use vmalloc_user (like the vshare path), not
				 * the DMA pool.
				 */
				spin_lock(&vpu_lock);
				if (share_mem.cpu_addr != 0) {
					ret = copy_to_user((void __user *)arg,
							   &share_mem,
							   sizeof(struct vpu_mem_desc));
					spin_unlock(&vpu_lock);
					break;
				}
				if (copy_from_user(&share_mem,
						   (struct vpu_mem_desc *)arg,
						   sizeof(struct vpu_mem_desc))) {
					spin_unlock(&vpu_lock);
					return -EFAULT;
				}
				share_mem.cpu_addr =
				    (unsigned long)vmalloc_user(share_mem.size);
				if (!share_mem.cpu_addr) {
					spin_unlock(&vpu_lock);
					ret = -ENOMEM;
					break;
				}
				/* phy_addr is the mmap token (vmalloc addr); the
				 * mmap handler routes it to remap_vmalloc_range. */
				share_mem.phy_addr = share_mem.cpu_addr;
				if (copy_to_user((void __user *)arg, &share_mem,
						 sizeof(struct vpu_mem_desc)))
					ret = -EFAULT;
				spin_unlock(&vpu_lock);
				break;
			}
	case VPU_IOC_REQ_VSHARE_MEM:
		{
			spin_lock(&vpu_lock);
			if (vshare_mem.cpu_addr != 0) {
				ret = copy_to_user((void __user *)arg,
						   &vshare_mem,
						   sizeof(struct vpu_mem_desc));
				spin_unlock(&vpu_lock);
				break;
			} else {
				if (copy_from_user(&vshare_mem,
						   (struct vpu_mem_desc *)arg,
						   sizeof(struct
							  vpu_mem_desc))) {
					spin_unlock(&vpu_lock);
					return -EFAULT;
				}
				/* vmalloc shared memory if not allocated */
				if (!vshare_mem.cpu_addr)
					vshare_mem.cpu_addr =
					    (unsigned long)
					    vmalloc_user(vshare_mem.size);
				if (copy_to_user
				     ((void __user *)arg, &vshare_mem,
				     sizeof(struct vpu_mem_desc)))
					ret = -EFAULT;
			}
			spin_unlock(&vpu_lock);
			break;
		}
	case VPU_IOC_GET_WORK_ADDR:
		{
			if (bitwork_mem.cpu_addr != 0) {
				ret =
				    copy_to_user((void __user *)arg,
						 &bitwork_mem,
						 sizeof(struct vpu_mem_desc));
				break;
			} else {
				if (copy_from_user(&bitwork_mem,
						   (struct vpu_mem_desc *)arg,
						   sizeof(struct vpu_mem_desc)))
					return -EFAULT;

				if (vpu_alloc_dma_buffer(&bitwork_mem) == -1)
					ret = -EFAULT;
				else if (copy_to_user((void __user *)arg,
						      &bitwork_mem,
						      sizeof(struct
							     vpu_mem_desc)))
					ret = -EFAULT;
			}
			break;
		}
	/*
	 * The following two ioctl is used when user allocates working buffer
	 * and register it to vpu driver.
	 */
	case VPU_IOC_QUERY_BITWORK_MEM:
		{
			if (copy_to_user((void __user *)arg,
					 &bitwork_mem,
					 sizeof(struct vpu_mem_desc)))
				ret = -EFAULT;
			break;
		}
	case VPU_IOC_SET_BITWORK_MEM:
		{
			if (copy_from_user(&bitwork_mem,
					   (struct vpu_mem_desc *)arg,
					   sizeof(struct vpu_mem_desc)))
				ret = -EFAULT;
			break;
		}
	case VPU_IOC_SYS_SW_RESET:
		{
			/* Hardware reset of the Codadx6.  Without this between
			 * sessions, libvpu's vpu_DecGetInitialInfo on the next
			 * decoder open can hang inside the wait-for-IRQ -- the
			 * chip is stuck in whatever mid-decode state the prior
			 * session left it in. (vpu_plat->reset is the original
			 * BSP path, which doesn't exist on Toon since we bypass
			 * platform_device; we fall through to the direct reg
			 * write below.) */
			if (vpu_plat && vpu_plat->reset) {
				vpu_plat->reset();
			} else if (vpu_base) {
				vpu_clk_force_on();
				/* Halt the BIT processor, pulse software reset,
				 * clear pending IRQ.  Codadx6 latches reset on
				 * a write of 1; a few cycles later the chip is
				 * idle and ready for a fresh code download. */
				WRITE_REG(0, BIT_CODE_RUN);
				WRITE_REG(1, BIT_CODE_RESET);
				udelay(10);
				WRITE_REG(0, BIT_CODE_RESET);
				WRITE_REG(1, BIT_INT_CLEAR);
				/* Drop the codec_done flag -- a stale "decode
				 * finished" from before the reset must not
				 * satisfy the next wait. */
				codec_done = 0;
				printk(KERN_INFO "mxc_vpu: hardware reset issued\n");
			}
			break;
		}
	case VPU_IOC_REG_DUMP:
		break;
	case VPU_IOC_PHYMEM_DUMP:
		break;
	default:
		{
			printk(KERN_ERR "No such IOCTL, cmd is %d\n", cmd);
			break;
		}
	}
	return ret;
}

/*!
 * @brief Release function for vpu file operation
 * @return  0 on success or negative error code on error
 */
static int vpu_release(struct inode *inode, struct file *filp)
{
	spin_lock(&vpu_lock);
	if (open_count > 0 && !(--open_count)) {
		/* Last close -- hard-reset the VPU so the next process that
		 * opens /dev/mxc_vpu and runs vpu_Init starts from a clean
		 * state. Without this the chip is left in whatever (possibly
		 * mid-decode) state the prior session ended in, and the
		 * next vpu_DecGetInitialInfo can hang forever waiting on a
		 * BIT-processor IRQ that will never fire. */
		if (vpu_base) {
			vpu_clk_force_on();
			WRITE_REG(0, BIT_CODE_RUN);
			WRITE_REG(1, BIT_CODE_RESET);
			udelay(10);
			WRITE_REG(0, BIT_CODE_RESET);
			WRITE_REG(1, BIT_INT_CLEAR);
			codec_done = 0;
		}
		vpu_free_buffers();

		/* Free shared memory when vpu device is idle (share_mem is now
		 * vmalloc'd host-side semaphore memory, so vfree it). */
		vfree((void *)share_mem.cpu_addr);
		share_mem.cpu_addr = 0;
		share_mem.phy_addr = 0;
		vfree((void *)vshare_mem.cpu_addr);
		vshare_mem.cpu_addr = 0;
	}
	spin_unlock(&vpu_lock);

	return 0;
}

/*!
 * @brief fasync function for vpu file operation
 * @return  0 on success or negative error code on error
 */
static int vpu_fasync(int fd, struct file *filp, int mode)
{
	struct vpu_priv *dev = (struct vpu_priv *)filp->private_data;
	return fasync_helper(fd, filp, mode, &dev->async_queue);
}

/*!
 * @brief memory map function of harware registers for vpu file operation
 * @return  0 on success or negative error code on error
 */
static int vpu_map_hwregs(struct file *fp, struct vm_area_struct *vm)
{
	unsigned long pfn;

	vm->vm_flags |= VM_IO | VM_RESERVED;
	vm->vm_page_prot = pgprot_noncached(vm->vm_page_prot);
	pfn = phy_vpu_base_addr >> PAGE_SHIFT;
	pr_debug("size=0x%x,  page no.=0x%x\n",
		 (int)(vm->vm_end - vm->vm_start), (int)pfn);
	return remap_pfn_range(vm, vm->vm_start, pfn, vm->vm_end - vm->vm_start,
			       vm->vm_page_prot) ? -EAGAIN : 0;
}

/*!
 * @brief memory map function of memory for vpu file operation
 * @return  0 on success or negative error code on error
 */
static int vpu_map_dma_mem(struct file *fp, struct vm_area_struct *vm)
{
	int request_size;
	request_size = vm->vm_end - vm->vm_start;

	pr_debug(" start=0x%x, pgoff=0x%x, size=0x%x\n",
		 (unsigned int)(vm->vm_start), (unsigned int)(vm->vm_pgoff),
		 request_size);

	vm->vm_flags |= VM_RESERVED;
	/* Write-back cacheable — userspace must invalidate cache after VPU DMA
	 * writes (vl2cc_flush / VPU_IOC_VL2CC_FLUSH). */
	vm->vm_page_prot = __pgprot_modify(vm->vm_page_prot,
					   L_PTE_MT_MASK, L_PTE_MT_BUFFERABLE);

	return remap_pfn_range(vm, vm->vm_start, vm->vm_pgoff,
			       request_size, vm->vm_page_prot) ? -EAGAIN : 0;

}

/* !
 * @brief memory map function of vmalloced share memory
 * @return  0 on success or negative error code on error
 */
static int vpu_map_vshare_mem(struct file *fp, struct vm_area_struct *vm)
{
	int ret = -EINVAL;

	spin_lock(&vpu_lock);
	/*
	 * Do NOT set VM_IO here. This vmalloc-backed mapping holds the library's
	 * PROCESS_SHARED pthread mutex; futex needs to pin the page via GUP, which
	 * a VM_IO VMA forbids -> futex EFAULT -> glibc "futex facility returned an
	 * unexpected error" abort under continuous (contended) streaming. vmalloc
	 * pages have struct page, so a plain mapping is GUP/futex-safe.
	 */
	ret = remap_vmalloc_range(vm, (void *)(vm->vm_pgoff << PAGE_SHIFT), 0);
	spin_unlock(&vpu_lock);

	return ret;
}
/*!
 * @brief memory map interface for vpu file operation
 * @return  0 on success or negative error code on error
 */
static int vpu_mmap(struct file *fp, struct vm_area_struct *vm)
{
	unsigned long offset;

	offset = vshare_mem.cpu_addr >> PAGE_SHIFT;

	if (vm->vm_pgoff && (vm->vm_pgoff == offset))
		return vpu_map_vshare_mem(fp, vm);
	/* share mem is vmalloc'd too -> map it the same (cacheable, page-backed) */
	else if (vm->vm_pgoff && share_mem.cpu_addr &&
		 vm->vm_pgoff == (share_mem.cpu_addr >> PAGE_SHIFT))
		return vpu_map_vshare_mem(fp, vm);
	else if (vm->vm_pgoff)
		return vpu_map_dma_mem(fp, vm);
	else
		return vpu_map_hwregs(fp, vm);
}

struct file_operations vpu_fops = {
	.owner = THIS_MODULE,
	.open = vpu_open,
	.unlocked_ioctl = vpu_ioctl,
	.release = vpu_release,
	.fasync = vpu_fasync,
	.mmap = vpu_mmap,
};

/*!
 * This function is called by the driver framework to initialize the vpu device.
 * @param   dev The device structure for the vpu passed in by the framework.
 * @return   0 on success or negative error code on error
 */
static int vpu_dev_probe(struct platform_device *pdev)
{
	int err = 0;
	struct device *temp_class;
	struct resource *res;
	unsigned long addr = 0;
	unsigned long requested_iram = 0;

	vpu_plat = pdev->dev.platform_data;

	if (iram_size > 0)
		requested_iram = iram_size;
	else if (vpu_plat && vpu_plat->iram_enable && vpu_plat->iram_size)
		requested_iram = vpu_plat->iram_size;

	iram_alloc_size = 0;
	if (requested_iram)
		iram_alloc(requested_iram, &addr);
	if (!addr && requested_iram && requested_iram <= 0xb000) {
		addr = MX27_IRAM_BASE_ADDR;
		printk(KERN_INFO "vpu: using fixed i.MX27 IRAM base because "
		       "iram_alloc() is stubbed\n");
	}
	if (addr == 0)
		iram.start = iram.end = 0;
	else {
		iram.start = addr;
		iram.end = addr + requested_iram - 1;
		iram_alloc_size = requested_iram;
	}
	printk(KERN_INFO "vpu: IRAM %s start=0x%08x end=0x%08x size=0x%lx\n",
	       iram_alloc_size ? "enabled" : "disabled",
	       iram.start, iram.end, iram_alloc_size);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		printk(KERN_ERR "vpu: unable to get vpu base addr\n");
		return -ENODEV;
	}
	phy_vpu_base_addr = res->start;
	vpu_base = ioremap(res->start, res->end - res->start);

	vpu_major = register_chrdev(vpu_major, "mxc_vpu", &vpu_fops);
	if (vpu_major < 0) {
		printk(KERN_ERR "vpu: unable to get a major for VPU\n");
		err = -EBUSY;
		goto error;
	}

	vpu_class = class_create(THIS_MODULE, "mxc_vpu");
	if (IS_ERR(vpu_class)) {
		err = PTR_ERR(vpu_class);
		goto err_out_chrdev;
	}

	temp_class = device_create(vpu_class, NULL, MKDEV(vpu_major, 0),
				   NULL, "mxc_vpu");
	if (IS_ERR(temp_class)) {
		err = PTR_ERR(temp_class);
		goto err_out_class;
	}

	vpu_clk = clk_get(&pdev->dev, "vpu");
	if (IS_ERR(vpu_clk)) {
		err = -ENOENT;
		goto err_out_class;
	}

	res = platform_get_resource(pdev, IORESOURCE_IRQ, 0);
	if (!res) {
		printk(KERN_ERR "vpu: unable to get vpu interrupt\n");
		err = -ENXIO;
		goto err_out_class;
	}
	vpu_irq = res->start;

	err = request_irq(vpu_irq, vpu_irq_handler, 0, "VPU_CODEC_IRQ",
			  (void *)(&vpu_data));
	if (err)
		goto err_out_class;

	printk(KERN_INFO "VPU initialized\n");
	goto out;

      err_out_class:
	device_destroy(vpu_class, MKDEV(vpu_major, 0));
	class_destroy(vpu_class);
      err_out_chrdev:
	unregister_chrdev(vpu_major, "mxc_vpu");
      error:
	iounmap(vpu_base);
      out:
	return err;
}

static int vpu_dev_remove(struct platform_device *pdev)
{
	free_irq(vpu_irq, &vpu_data);

	iounmap(vpu_base);
	if (iram_alloc_size)
		iram_free(iram.start, iram_alloc_size);

	return 0;
}

#ifdef CONFIG_PM
static int vpu_suspend(struct platform_device *pdev, pm_message_t state)
{
	int i;
	unsigned long timeout;

	/* Wait for vpu go to idle state */
	if (open_count > 0) {
		timeout = jiffies + HZ;
		clk_enable(vpu_clk);
		while (READ_REG(BIT_BUSY_FLAG)) {
			msleep(1);
			if (time_after(jiffies, timeout))
				goto out;
		}
		clk_disable(vpu_clk);
	}

	/* Make sure clock is disabled before suspend */
	vpu_clk_usercount = vpu_clk->usecount;
	for (i = 0; i < vpu_clk_usercount; i++)
		clk_disable(vpu_clk);

	return 0;

out:
	clk_disable(vpu_clk);
	return -EAGAIN;

}

static int vpu_resume(struct platform_device *pdev)
{
	int i;

	/* Recover vpu clock state */
	for (i = 0; i < vpu_clk_usercount; i++)
		clk_enable(vpu_clk);

	return 0;
}
#else
#define	vpu_suspend	NULL
#define	vpu_resume	NULL
#endif				/* !CONFIG_PM */

/*! Driver definition
 *
 */
static struct platform_driver mxcvpu_driver = {
	.driver = {
		   .name = "mxc_vpu",
		   },
	.probe = vpu_dev_probe,
	.remove = vpu_dev_remove,
	.suspend = vpu_suspend,
	.resume = vpu_resume,
};

/* Toon ships with HCLK (= AHB = VPU clock) at ~103 MHz (CSCR.AHB_PDF = 2,
 * divide-by-3 of mpll_main2 = 2*MPLL/3 = 310 MHz). The i.MX27 spec max is
 * 133 MHz, so the VPU is at ~77 % of spec by default.
 *
 * Setting hclk_max=1 at insmod time programs CSCR.AHB_PDF = 1 (divide-by-2)
 * which puts HCLK at ~155 MHz -- a bit over spec, but in practice the VPU,
 * eMMA and SDRAM controller all run fine at this rate on Toon silicon
 * (we tested this from the same module). NAND/LCDC also bump by ~50 %;
 * we have not seen corruption but it is not Freescale-blessed.
 *
 * 2.6.36's clk_set_rate(ahb_clk, ...) is a no-op on this BSP (ahb_clk has
 * no .set_rate callback, only .get_rate), so a direct CSCR write is the
 * only path. Reverts on reboot (no persistent change). */
static int hclk_max = 0;
module_param(hclk_max, int, 0644);
MODULE_PARM_DESC(hclk_max,
    "If 1, bump HCLK/VPU from ~103 MHz to ~155 MHz at module load "
    "(CSCR.AHB_PDF 2->1). Slightly above i.MX27 spec; revert via reboot.");

static int clear_lhd = 0;
module_param(clear_lhd, int, 0644);
MODULE_PARM_DESC(clear_lhd,
    "If 1, clear SDRAMC ESDCFG0 bit5 (LHD / Latency Hiding Disable) at module "
    "load. This removes the old MPEG-4 FIFO-overrun workaround and may improve "
    "H.264/VPU memory throughput. Reverts on reboot.");

module_param(iram_size, int, 0644);
MODULE_PARM_DESC(iram_size,
    "Override platform IRAM allocation size for VPU. Use 0xb000 for CODA DX6 "
    "experiments on i.MX27; default 0 keeps platform data behavior.");

module_param(allow_prp, int, 0644);
MODULE_PARM_DESC(allow_prp,
    "If 1, enable legacy eMMA PrP conversion ioctl. Default 0 disables PrP "
    "because PrP can wedge/crash when used near VPU decode; PP remains enabled.");

static int __init vpu_init(void)
{
	int err;

	init_waitqueue_head(&vpu_queue);

	/* Direct hardware init -- bypass platform bus to avoid
	 * struct layout incompatibility with running kernel. */
	phy_vpu_base_addr = MX27_VPU_BASE_ADDR;
	vpu_base = ioremap(MX27_VPU_BASE_ADDR, SZ_4K);
	if (!vpu_base) {
		printk(KERN_ERR "vpu: ioremap failed\n");
		return -ENOMEM;
	}

	iram.start = iram.end = 0;
	iram_alloc_size = 0;
	if (iram_size > 0) {
		unsigned long addr = 0;

		iram_alloc(iram_size, &addr);
		if (!addr && iram_size <= 0xb000) {
			addr = MX27_IRAM_BASE_ADDR;
			printk(KERN_INFO "vpu: using fixed i.MX27 IRAM base because "
			       "iram_alloc() is stubbed\n");
		}
		if (addr) {
			iram.start = addr;
			iram.end = addr + iram_size - 1;
			iram_alloc_size = iram_size;
		}
	}
	printk(KERN_INFO "vpu: IRAM %s start=0x%08x end=0x%08x size=0x%lx\n",
	       iram_alloc_size ? "enabled" : "disabled",
	       iram.start, iram.end, iram_alloc_size);

	/* Map the CCM so the CLKGATE ioctl can force-ungate the VPU clock
	 * (clk_enable() doesn't actually set PCCR1 on this R10-h28 kernel). */
	ccm_base = ioremap(MX27_CCM_BASE_ADDR, SZ_4K);
	if (!ccm_base)
		printk(KERN_WARNING "vpu: CCM ioremap failed; clock gating may not work\n");
	else {
		/* SYSCTRL CHIP_ID @ CCM+0x800: part should be 0x8821, rev = val>>28
		 * (0 = i.MX27 TO1/1.0, 1 = TO2/2.0). Userspace can't read this, so
		 * log it to pick the right vpu_fw_imx27_TOx.bin. */
		u32 cid = __raw_readl(ccm_base + 0x800);
		printk(KERN_INFO "vpu: SYSCTRL CHIP_ID=0x%08x part=0x%04x silicon_rev=%d\n",
		       cid, (cid >> 12) & 0xFFFF, cid >> 28);

		/* CCM clock dump — answers "is the VPU underclocked?" without
		 * needing /dev/mem or any userspace peripheral access.
		 * VPU on i.MX27 runs on HCLK (AHB); HCLK = SPLL / (BCLKDIV+1).
		 * CKIH = 26 MHz external crystal.  MPCTL0 / SPCTL0 layout:
		 *   bits  0..9  MFN  (signed via bit 29)
		 *   bits 10..13 MFI
		 *   bits 16..25 MFD
		 *   bits 26..29 PD
		 * PLL = 2 * CKIH * (MFI + MFN/(MFD+1)) / (PD+1) when MFN>=0. */
		{
			/* CCM clock dump — answers "is the VPU underclocked?".
			 * VPU runs on AHB (HCLK). For i.MX27 TO2+:
			 *   ahb_pdf  = (CSCR >> 8)  & 0x3
			 *   mpll_main2 = (2 * MPLL) / 3    (the standard ARM path)
			 *   ahb_clk  = mpll_main2 / (ahb_pdf + 1)
			 * ARM source is selected by CSCR bit 15 (ARM_SRC): 0 -> main2
			 * (= 2*MPLL/3), 1 -> main1 (= MPLL). cpu_pdf = (CSCR>>12)&0x3.
			 * PLL formula is the standard mxc_decode_pll from
			 * arch/arm/plat-mxc/clock.c. CKIH = 26 MHz crystal. */
			u32 mpctl0 = __raw_readl(ccm_base + 0x04);
			u32 spctl0 = __raw_readl(ccm_base + 0x0C);
			u32 cscr   = __raw_readl(ccm_base + 0x00);
			u32 pcdr0  = __raw_readl(ccm_base + 0x18);
			u32 pcdr1  = __raw_readl(ccm_base + 0x1C);
			u32 pccr0  = __raw_readl(ccm_base + 0x20);
			u32 pccr1  = __raw_readl(ccm_base + 0x24);
			u32 ckih_khz = 26000;
			int i;
			u32 plls_khz[2] = {0, 0};
			for (i = 0; i < 2; i++) {
				u32 r   = (i == 0) ? mpctl0 : spctl0;
				int mfi = (r >> 10) & 0xF;
				int mfn =  r        & 0x3FF;
				int mfd = (r >> 16) & 0x3FF;
				int pd  = (r >> 26) & 0xF;
				int mfn_abs = mfn;
				u32 freq = 2 * ckih_khz / (pd + 1);
				if (mfi < 5) mfi = 5;
				if (mfn >= 0x200) mfn_abs = 0x400 - mfn;
				/* PLL = freq * mfi + (freq * mfn_abs) / (mfd+1) */
				{
					u32 frac = (freq * (u32)mfn_abs) / (mfd + 1);
					plls_khz[i] = freq * (u32)mfi +
					              (mfn >= 0x200 ? -frac : frac);
				}
			}
			{
				u32 mpll_khz  = plls_khz[0];
				u32 spll_khz  = plls_khz[1];
				/* main paths: main1 = 2*MPLL/2 = MPLL, main2 = 2*MPLL/3 */
				u32 main1_khz = mpll_khz;
				u32 main2_khz = (2u * mpll_khz) / 3u;
				u32 cpu_pdf   = (cscr >> 12) & 0x3;
				u32 ahb_pdf   = (cscr >> 8)  & 0x3;
				u32 arm_src   = (cscr >> 15) & 1;
				u32 arm_parent = arm_src ? main1_khz : main2_khz;
				u32 arm_khz   = arm_parent / (cpu_pdf + 1);
				u32 ahb_khz   = main2_khz  / (ahb_pdf + 1);
				printk(KERN_INFO "vpu: CCM CSCR=0x%08x MPCTL0=0x%08x SPCTL0=0x%08x\n",
				       cscr, mpctl0, spctl0);
				printk(KERN_INFO "vpu: CCM PCDR0=0x%08x PCDR1=0x%08x PCCR0=0x%08x PCCR1=0x%08x\n",
				       pcdr0, pcdr1, pccr0, pccr1);
				printk(KERN_INFO "vpu: PLLs MPLL=%u kHz SPLL=%u kHz main1=%u main2=%u\n",
				       mpll_khz, spll_khz, main1_khz, main2_khz);
				printk(KERN_INFO "vpu: clocks ARM=%u kHz HCLK/VPU=%u kHz "
				       "(arm_src=%s cpu_pdf=%u ahb_pdf=%u)\n",
				       arm_khz, ahb_khz,
				       arm_src ? "main1" : "main2", cpu_pdf, ahb_pdf);
				printk(KERN_INFO "vpu: spec max HCLK/VPU = 133 MHz; we are at %u%% of spec\n",
				       (ahb_khz * 100u) / 133000u);
			}

			/* Program AHB_PDF directly from hclk_max so insmod with a
			 * fresh parameter always lands on the requested rate (the
			 * CSCR change is not auto-reverted by rmmod — only a reboot
			 * resets it). hclk_max=1 -> AHB_PDF=1 (~155 MHz), anything
			 * else -> AHB_PDF=2 (~103 MHz, Toon's shipped default). */
			{
				u32 want_pdf = hclk_max ? 1u : 2u;
				u32 cur_pdf  = (cscr >> 8) & 0x3;
				if (cur_pdf != want_pdf) {
					u32 new_cscr = (cscr & ~(0x3u << 8)) | (want_pdf << 8);
					printk(KERN_WARNING "vpu: %s HCLK: CSCR 0x%08x -> 0x%08x "
					       "(AHB_PDF %u -> %u)\n",
					       want_pdf == 1 ? "bumping" : "restoring",
					       cscr, new_cscr, cur_pdf, want_pdf);
					__raw_writel(new_cscr, ccm_base + 0x00);
					new_cscr = __raw_readl(ccm_base + 0x00);
					{
						u32 main2 = (2u * plls_khz[0]) / 3u;
						u32 new_ahb = main2 / (((new_cscr >> 8) & 0x3) + 1);
						printk(KERN_WARNING "vpu: HCLK/VPU now %u kHz "
						       "(CSCR=0x%08x)\n", new_ahb, new_cscr);
					}
				}
			}
		}
	}

	sdramc_base = ioremap(MX27_SDRAMC_BASE_ADDR, SZ_4K);
	m3if_base = ioremap(MX27_M3IF_BASE_ADDR, SZ_4K);
	max_base = NULL;
	if (!sdramc_base)
		printk(KERN_WARNING "vpu: SDRAMC ioremap failed; no LHD diagnostics\n");
	if (!m3if_base)
		printk(KERN_WARNING "vpu: M3IF ioremap failed; no M3IF diagnostics\n");
	vpu_dump_bus_regs("before tuning");
	if (clear_lhd && sdramc_base) {
		u32 old = __raw_readl(sdramc_base + MX27_SDRAMC_ESDCFG0);
		u32 new = old & ~MX27_SDRAMC_ESDCFG0_LHD;

		if (old != new) {
			printk(KERN_WARNING "vpu: clearing SDRAMC ESDCFG0.LHD: "
			       "0x%08x -> 0x%08x\n", old, new);
			__raw_writel(new, sdramc_base + MX27_SDRAMC_ESDCFG0);
		} else {
			printk(KERN_INFO "vpu: SDRAMC ESDCFG0.LHD already clear\n");
		}
		vpu_dump_bus_regs("after clear_lhd");
	}

	vpu_major = register_chrdev(0, "mxc_vpu", &vpu_fops);
	if (vpu_major < 0) {
		printk(KERN_ERR "vpu: unable to get a major for VPU\n");
		err = -EBUSY;
		goto err_iounmap;
	}

	vpu_class = class_create(THIS_MODULE, "mxc_vpu");
	if (IS_ERR(vpu_class)) {
		err = PTR_ERR(vpu_class);
		goto err_chrdev;
	}

	vpu_dev = device_create(vpu_class, NULL, MKDEV(vpu_major, 0),
				 NULL, "mxc_vpu");
	if (IS_ERR(vpu_dev)) {
		err = PTR_ERR(vpu_dev);
		goto err_class;
	}

	/*
	 * NOTE: do NOT write vpu_dev->coherent_dma_mask here. struct device's
	 * layout differs between the R07 headers this module is built from and
	 * the running R10-h28 kernel, so the write lands at the wrong offset.
	 * vpu_alloc_dma_buffer() uses dma_alloc_coherent(NULL, ...) instead.
	 */

	vpu_clk = clk_get(NULL, "vpu");
	if (IS_ERR(vpu_clk)) {
		printk(KERN_WARNING "vpu: unable to get vpu clock, continuing without\n");
		vpu_clk = NULL;
	} else {
		clk_enable(vpu_clk);
		printk(KERN_INFO "vpu: clock acquired\n");
	}

	/* Keep the VPU clock ungated for the life of the module so register
	 * access (incl. the REG_READ/REG_WRITE ioctls) never aborts. */
	vpu_clk_force_on();

	vpu_irq = MX27_INT_VPU;
	err = request_irq(vpu_irq, vpu_irq_handler, 0, "VPU_CODEC_IRQ",
			  (void *)(&vpu_data));
	if (err) {
		printk(KERN_ERR "vpu: unable to request IRQ %d\n", vpu_irq);
		goto err_clk;
	}


	/* Reserve the contiguous DMA pool now (must be early in boot). All VPU
	 * buffers are served from it, so allocations no longer depend on the
	 * fragmented page allocator at decode time. */
	vpu_pool_init();
	if (vpu_pool_nblocks == 0)
		printk(KERN_WARNING "vpu: DMA pool empty -- module loaded too late "
		       "in boot? VPU buffer allocation will fail.\n");

	/* eMMA PP: post-decode YUV->RGB color conversion for the display path. */
	init_waitqueue_head(&pp_queue);
	pp_base = ioremap(MX27_EMMA_PP_BASE_ADDR, SZ_4K);
	if (!pp_base) {
		printk(KERN_WARNING "vpu: eMMA PP ioremap failed; no PP CSC\n");
	} else {
		pp_irq_num = MX27_INT_EMMAPP;
		if (request_irq(pp_irq_num, pp_irq_handler, 0, "VPU_PP_IRQ", NULL)) {
			printk(KERN_WARNING "vpu: eMMA PP IRQ %d request failed\n",
			       pp_irq_num);
			iounmap(pp_base);
			pp_base = NULL;
		} else {
			emma_force_clk_on();
			if (pp_hw_reset() == 0)
				printk(KERN_INFO "vpu: eMMA PP ready (post-decode HW YUV->RGB)\n");
			else
				printk(KERN_WARNING "vpu: eMMA PP reset failed; PP may be unusable\n");
		}
	}

	/* eMMA PrP: hardware YUV->RGB color conversion for the display path. */
	init_waitqueue_head(&prp_queue);
	prp_base = ioremap(MX27_EMMA_PRP_BASE_ADDR, SZ_4K);
	if (!prp_base) {
		printk(KERN_WARNING "vpu: eMMA PrP ioremap failed; no HW CSC\n");
	} else {
		prp_irq_num = MX27_INT_EMMAPRP;
		if (request_irq(prp_irq_num, prp_irq_handler, 0, "VPU_PRP_IRQ", NULL)) {
			printk(KERN_WARNING "vpu: eMMA PrP IRQ %d request failed\n",
			       prp_irq_num);
			iounmap(prp_base);
			prp_base = NULL;
		} else {
			/* Reset PrP to clean power-on state before first use. */
			int prp_i;
			prp_force_clk_on();
			PRPW(PRP_CNTL, PRP_CNTL_RST);
			for (prp_i = 0; prp_i < 1000; prp_i++) {
				if (PRPR(PRP_CNTL) == PRP_CNTL_RSTVAL)
					break;
				udelay(1);
			}
			if (prp_i < 1000)
				printk(KERN_INFO "vpu: eMMA PrP ready (HW YUV->RGB)\n");
			else
				printk(KERN_WARNING "vpu: eMMA PrP reset timeout; PrP may be unusable\n");
		}
	}

	printk(KERN_INFO "VPU initialized (major=%d)\n", vpu_major);
	return 0;

err_clk:
	if (vpu_clk)
		clk_put(vpu_clk);
	device_destroy(vpu_class, MKDEV(vpu_major, 0));
err_class:
	class_destroy(vpu_class);
err_chrdev:
	unregister_chrdev(vpu_major, "mxc_vpu");
err_iounmap:
	if (iram_alloc_size)
		iram_free(iram.start, iram_alloc_size);
	if (ccm_base)
		iounmap(ccm_base);
	if (sdramc_base)
		iounmap(sdramc_base);
	if (m3if_base)
		iounmap(m3if_base);
	if (max_base)
		iounmap(max_base);
	iounmap(vpu_base);
	return err;
}

static void __exit vpu_exit(void)
{
	free_irq(vpu_irq, (void *)(&vpu_data));
	if (pp_base) {
		free_irq(pp_irq_num, NULL);
		iounmap(pp_base);
	}
	if (prp_base) {
		free_irq(prp_irq_num, NULL);
		iounmap(prp_base);
	}
	if (vpu_clk)
		clk_put(vpu_clk);
	device_destroy(vpu_class, MKDEV(vpu_major, 0));
	class_destroy(vpu_class);
	unregister_chrdev(vpu_major, "mxc_vpu");
	vpu_free_dma_buffer(&bitwork_mem);
	vpu_pool_destroy();
	if (iram_alloc_size)
		iram_free(iram.start, iram_alloc_size);
	if (ccm_base)
		iounmap(ccm_base);
	if (sdramc_base)
		iounmap(sdramc_base);
	if (m3if_base)
		iounmap(m3if_base);
	if (max_base)
		iounmap(max_base);
	iounmap(vpu_base);
	vpu_major = 0;
}

MODULE_AUTHOR("Freescale Semiconductor, Inc.");
MODULE_DESCRIPTION("Linux VPU driver for Freescale i.MX/MXC");
MODULE_LICENSE("GPL");

module_init(vpu_init);
module_exit(vpu_exit);
