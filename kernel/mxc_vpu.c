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
static void __iomem *prp_base;	/* i.MX27 eMMA PrP (HW YUV->RGB + resize) */
static int prp_irq_num;
static int prp_done;
static wait_queue_head_t prp_queue;
static DEFINE_MUTEX(prp_lock);
static int vpu_irq;
static u32 phy_vpu_base_addr;
static struct mxc_vpu_platform_data *vpu_plat;

/* IRAM setting */
static struct iram_setting iram;

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
#define PRP_CNTL_CH1_RGB16		(1 << 5)
#define PRP_CNTL_RST			(1 << 12)
#define PRP_CNTL_RSTVAL			0x28
/* PRP_INTRCNTL / status bits */
#define PRP_INTRCNTL_RDERR		(1 << 0)
#define PRP_INTRCNTL_CH1WERR		(1 << 1)
#define PRP_INTRCNTL_CH1FC		(1 << 3)
#define PRP_INTRCNTL_LBOVF		(1 << 7)
/* pixel format codes */
#define PRP_PIXIN_YUV420		0
#define PRP_PIX1_RGB565			0x2CA00565

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

/* Force the eMMA clock gates on (emma_clk=PCCR0 bit27, emma_clk1=PCCR1 bit18). */
static inline void prp_force_clk_on(void)
{
	if (ccm_base) {
		__raw_writel(__raw_readl(ccm_base + 0x20) | (1 << 27), ccm_base + 0x20);
		__raw_writel(__raw_readl(ccm_base + 0x24) | (1 << 18), ccm_base + 0x24);
	}
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

/* One-shot hardware YUV420(planar) -> RGB565 (+resize). */
static int prp_convert(struct vpu_prp_convert *c)
{
	scale_t sw, sh;
	unsigned short outw = 0, outh = 0;
	u32 sz;
	int i, rc;

	if (!prp_base)
		return -ENODEV;
	if (c->src_w < 32 || c->src_h < 32 || c->dst_w < 8 || c->dst_h < 8)
		return -EINVAL;

	memset(&sw, 0, sizeof(sw)); sw.algo = ALGO_AUTO;
	memset(&sh, 0, sizeof(sh)); sh.algo = ALGO_AUTO;
	if (prp_scale(&sw, c->src_w, c->dst_w, c->src_w, &outw, SCALE_RETRY) < 0)
		return -EINVAL;
	if (prp_scale(&sh, c->src_h, c->dst_h, c->src_h, &outh, SCALE_RETRY) < 0)
		return -EINVAL;
	outw &= ~1;	/* RGB16 width must be even */

	mutex_lock(&prp_lock);
	prp_force_clk_on();

	/* soft reset */
	PRPW(PRP_CNTL, PRP_CNTL_RST);
	for (i = 0; i < 1000; i++) {
		if (PRPR(PRP_CNTL) == PRP_CNTL_RSTVAL)
			break;
		udelay(1);
	}

	/* CSC: studio-range BT.601 YUV -> RGB */
	PRPW(PRP_CSC_COEF_012, 0x12A66032);
	PRPW(PRP_CSC_COEF_345, 0x0D082000);
	PRPW(PRP_CSC_COEF_678, 0x80000000);

	/* input: YUV420 planar */
	PRPW(PRP_SRC_PIXEL_FORMAT_CNTL, PRP_PIXIN_YUV420);
	PRPW(PRP_SOURCE_FRAME_SIZE, (c->src_w << 16) | c->src_h);
	PRPW(PRP_SOURCE_LINE_STRIDE, c->src_stride);
	PRPW(PRP_SOURCE_Y_PTR, c->src_y);
	sz = c->src_stride * c->src_h;
	PRPW(PRP_SOURCE_CB_PTR, c->src_y + sz);
	PRPW(PRP_SOURCE_CR_PTR, c->src_y + sz + (sz >> 2));

	/* output CH1: RGB565 */
	PRPW(PRP_CH1_PIXEL_FORMAT_CNTL, PRP_PIX1_RGB565);
	PRPW(PRP_CH1_OUT_IMAGE_SIZE, (outw << 16) | outh);
	PRPW(PRP_CH1_LINE_STRIDE, c->dst_stride);
	PRPW(PRP_DEST_RGB1_PTR, c->dst_rgb);

	prp_set_scaler(0, &sw);	/* width */
	prp_set_scaler(1, &sh);	/* height */

	PRPW(PRP_INTRCNTL, PRP_INTRCNTL_CH1FC | PRP_INTRCNTL_RDERR |
	     PRP_INTRCNTL_CH1WERR | PRP_INTRCNTL_LBOVF);

	prp_done = 0;
	PRPW(PRP_CNTL, PRP_CNTL_IN_YUV420 | PRP_CNTL_CH1_RGB16 | PRP_CNTL_CH1EN);

	rc = wait_event_interruptible_timeout(prp_queue, prp_done != 0,
					      msecs_to_jiffies(200));
	if (rc <= 0) {
		PRPW(PRP_CNTL, PRPR(PRP_CNTL) & ~PRP_CNTL_CH1EN);
		mutex_unlock(&prp_lock);
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
				printk(KERN_WARNING "VPU blocking: timeout.\n");
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

			if (copy_from_user(&cvt, (void __user *)arg, sizeof(cvt)))
				return -EFAULT;
			ret = prp_convert(&cvt);
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
		{
			u32 clkgate_en;

			if (get_user(clkgate_en, (u32 __user *) arg))
				return -EFAULT;

			if (vpu_clk) {
				if (clkgate_en) {
					clk_enable(vpu_clk);
					/* clk_enable doesn't set PCCR1 on this kernel */
					__raw_writel(__raw_readl(ccm_base + 0x24)
						     | (1 << 6) | (1 << 16),
						     ccm_base + 0x24);
				} else {
					clk_disable(vpu_clk);
				}
			}

			break;
		}
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
			if (vpu_plat->reset)
				vpu_plat->reset();

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

	vpu_plat = pdev->dev.platform_data;

	if (vpu_plat && vpu_plat->iram_enable && vpu_plat->iram_size)
		iram_alloc(vpu_plat->iram_size, &addr);
	if (addr == 0)
		iram.start = iram.end = 0;
	else {
		iram.start = addr;
		iram.end = addr +  vpu_plat->iram_size - 1;
	}

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
	if (vpu_plat && vpu_plat->iram_enable && vpu_plat->iram_size)
		iram_free(iram.start,  vpu_plat->iram_size);

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
			printk(KERN_INFO "vpu: eMMA PrP ready (HW YUV->RGB)\n");
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
	iounmap(vpu_base);
	return err;
}

static void __exit vpu_exit(void)
{
	free_irq(vpu_irq, (void *)(&vpu_data));
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
	if (ccm_base)
		iounmap(ccm_base);
	iounmap(vpu_base);
	vpu_major = 0;
}

MODULE_AUTHOR("Freescale Semiconductor, Inc.");
MODULE_DESCRIPTION("Linux VPU driver for Freescale i.MX/MXC");
MODULE_LICENSE("GPL");

module_init(vpu_init);
module_exit(vpu_exit);
