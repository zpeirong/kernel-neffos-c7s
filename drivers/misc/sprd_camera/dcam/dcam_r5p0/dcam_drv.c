/*
 * Copyright (C) 2015-2016 Spreadtrum Communications Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/mfd/syscon/sprd-glb.h>
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>
#include <linux/semaphore.h>
#include <linux/completion.h>
#include <linux/slab.h>
#include <linux/sprd_ion.h>
#include <linux/videodev2.h>
#include <linux/wakelock.h>
#include <video/sprd_mm.h>
#include "dcam_drv.h"
#include "dcam2isp.h"
#include "isp2dcam.h"
#include "gen_scale_coef.h"
#include "getyuvinput.h"
#include "cam_pw_domain.h"
#include "csi_driver.h"
#include "isp_drv.h"

/* Macro Definitions */
#ifdef pr_fmt
#undef pr_fmt
#endif
#define pr_fmt(fmt) "DCAM_DRV: %d: %d " fmt, current->pid, __LINE__

#define DCAM_DRV_DEBUG
#define DCAM_LOWEST_ADDR			0x800
#define DCAM_ADDR_INVALID(addr) \
	((unsigned long)(addr) < DCAM_LOWEST_ADDR)
#define DCAM_YUV_ADDR_INVALID(y, u, v) \
	(DCAM_ADDR_INVALID(y) && \
	 DCAM_ADDR_INVALID(u) && \
	 DCAM_ADDR_INVALID(v))

#define DCAM_SC1_H_TAB_OFFSET			0x400
#define DCAM_SC1_V_TAB_OFFSET			0x4F0
#define DCAM_SC1_V_CHROMA_TAB_OFFSET		0x8F0

#define DCAM_SC2_H_TAB_OFFSET			0x1400
#define DCAM_SC2_V_TAB_OFFSET			0x14F0
#define DCAM_SC2_V_CHROMA_TAB_OFFSET		0x18F0

#define DCAM_SC3_H_TAB_OFFSET			0x2400
#define DCAM_SC3_V_TAB_OFFSET			0x24F0
#define DCAM_SC3_V_CHROMA_TAB_OFFSET		0x28F0

#define DCAM_SC_COEFF_BUF_SIZE			(24 << 10)
#define DCAM_SC_COEFF_COEF_SIZE			(1 << 10)
#define DCAM_SC_COEFF_TMP_SIZE			(21 << 10)
#define DCAM_SC_COEFF_BUF_COUNT			3

#define DCAM_SC_H_COEF_SIZE			0xC0
#define DCAM_SC_V_COEF_SIZE			0x210
#define DCAM_SC_V_CHROM_COEF_SIZE		0x210

#define DCAM_SC_COEFF_H_NUM			(DCAM_SC_H_COEF_SIZE/4)
#define DCAM_SC_COEFF_V_NUM			(DCAM_SC_V_COEF_SIZE/4)
#define DCAM_SC_COEFF_V_CHROMA_NUM		(DCAM_SC_V_CHROM_COEF_SIZE/4)

#define DCAM_AXI_STOP_TIMEOUT			1000

#ifdef CONFIG_SC_FPGA
#define DCAM_PATH_TIMEOUT			msecs_to_jiffies(500*10)
#else
#define DCAM_PATH_TIMEOUT			msecs_to_jiffies(500)
#endif

#define DCAM_FRM_QUEUE_LENGTH			4
#define DCAM_STATE_QUICKQUIT			0x01
#define DCAM_MAX_COUNT				2

#define DCAM_IRQ_ERR_MASK \
	((1 << DCAM_PATH0_OV) | (1 << DCAM_PATH1_OV) | \
	(1 << DCAM_PATH2_OV) | (1 << DCAM_PATH3_OV) | \
	(1 << DCAM_SN_LINE_ERR) | (1 << DCAM_SN_FRAME_ERR) | \
	(1 << DCAM_JPEG_BUF_OV) | (1 << DCAM_ISP_OV) | \
	(1 << DCAM_MIPI_OV))

#define DCAM_IRQ_LINE_MASK \
	((1 << DCAM_CAP_SOF) | (1 << DCAM_PATH0_SOF) | \
	(1 << DCAM_PATH1_SOF) | (1 << DCAM_PATH2_SOF) | \
	(1 << DCAM_PATH3_SOF) | (1 << DCAM_SN_EOF) | \
	(1 << DCAM_CAP_EOF) | (1 << DCAM_PATH0_DONE) | \
	(1 << DCAM_PATH1_DONE) | (1 << DCAM_PATH2_DONE) | \
	(1 << DCAM_PATH3_DONE) | DCAM_IRQ_ERR_MASK)


/* Structure Definitions */

typedef void (*dcam_isr) (enum dcam_id idx);

enum {
	DCAM_FRM_UNLOCK = 0,
	DCAM_FRM_LOCK_WRITE = 0x10011001,
	DCAM_FRM_LOCK_READ = 0x01100110
};

enum {
	DCAM_ST_STOP = 0,
	DCAM_ST_START,
};

struct dcam_cap_desc {
	enum dcam_cap_if_mode cam_if;
	enum dcam_cap_sensor_mode input_format;
	enum dcam_capture_mode cap_mode;
	struct dcam_rect cap_rect;
};

struct dcam_path_valid {
	unsigned int input_size:1;
	unsigned int input_rect:1;
	unsigned int output_size:1;
	unsigned int output_format:1;
	unsigned int src_sel:1;
	unsigned int data_endian:1;
	unsigned int frame_deci:1;
	unsigned int scale_tap:1;
	unsigned int v_deci:1;
	unsigned int rot_mode:1;
	unsigned int shrink:1;
	unsigned int jpegls:1;
	unsigned int pdaf_ctrl:1;
};

struct dcam_frm_queue {
	struct dcam_frame frm_array[DCAM_FRM_QUEUE_LENGTH];
	unsigned int valid_cnt;
};

struct dcam_buf_queue {
	struct dcam_frame frame[DCAM_FRM_CNT_MAX];
	struct dcam_frame *write;
	struct dcam_frame *read;
	int w_index;
	int r_index;
	spinlock_t lock;
};

struct dcam_cowork_desc {
	struct dcam_rect trim1;
	unsigned int pitch;
	unsigned int scl_ip_int;
	unsigned int scl_ip_rmd;
	unsigned int scl_cip_int;
	unsigned int scl_cip_rmd;
	unsigned int scl_factor_in;
	unsigned int scl_factor_out;
};

struct dcam_path_desc {
	enum dcam_path_id id;
	struct dcam_size input_size;
	struct dcam_rect input_rect;
	struct dcam_size sc_input_size;
	struct dcam_size output_size;
	struct dcam_frm_queue frame_queue;
	struct dcam_buf_queue buf_queue;
	struct dcam_endian_sel data_endian;
	struct dcam_sc_tap scale_tap;
	struct dcam_deci deci_val;
	struct dcam_regular_desc regular_desc;
	struct sprd_pdaf_control pdaf_ctrl;
	struct dcam_jpegls_desc jpegls_desc;
	struct dcam_cowork_desc cowork_desc;
	struct dcam_path_valid valid_param;
	unsigned int frame_base_id;
	unsigned int output_frame_count;
	unsigned int output_format;
	enum dcam_path_src_sel src_sel;
	unsigned int rot_mode;
	unsigned int frame_deci;
	unsigned int valid;
	unsigned int status;
	struct completion tx_done_com;
	struct completion sof_com;
	unsigned int wait_for_done;
	unsigned int is_update;
	unsigned int wait_for_sof;
	unsigned int need_stop;
	unsigned int need_wait;
	int sof_cnt;
	int done_cnt;
};

struct dcam_module {
	unsigned char jpegls_enable;
	unsigned char cowork_enable;
	struct DCAMINFO *cowork_info;
	struct dcam_cap_desc dcam_cap;

	struct dcam_path_desc dcam_path[DCAM_PATH_MAX];
	struct dcam_frame path_reserved_frame[DCAM_PATH_MAX];
	struct dcam_frame path_frame[DCAM_PATH_MAX];
	unsigned int path_framecnt[DCAM_PATH_MAX];

	unsigned int wait_resize_done;
	unsigned int wait_rotation_done;
	unsigned int err_happened;
	struct semaphore scale_coeff_mem_sema;
	unsigned int state;
};

struct dcam_sc_coeff {
	unsigned int buf[DCAM_SC_COEFF_BUF_SIZE];
	unsigned int flag;
	struct dcam_path_desc dcam_path1;
};

struct dcam_sc_array {
	struct dcam_sc_coeff scaling_coeff[DCAM_SC_COEFF_BUF_COUNT];
	struct dcam_sc_coeff *scaling_coeff_queue[DCAM_SC_COEFF_BUF_COUNT];
	unsigned int valid_cnt;
	unsigned int is_smooth_zoom;
};

struct dcam_init_flag {
	unsigned char dcam0_sof;
	unsigned char dcam0_eof;
	unsigned char dcam1_sof;
	unsigned char dcam1_eof;
};

/* Static Variables Declaration */

static struct platform_device *s_pdev;
static atomic_t s_dcam_users[DCAM_MAX_COUNT];
static struct clk *s_dcam_clk[DCAM_MAX_COUNT];
static struct dcam_module *s_p_dcam_mod[DCAM_MAX_COUNT];
static int s_dcam_irq[DCAM_MAX_COUNT];
static dcam_isr_func s_user_func[DCAM_MAX_COUNT][DCAM_IRQ_NUMBER];
static void *s_user_data[DCAM_MAX_COUNT][DCAM_IRQ_NUMBER];
static struct dcam_sc_array *s_dcam_sc_array[DCAM_MAX_COUNT];
static unsigned long s_dcam_regbase[DCAM_MAX_COUNT];

static struct mutex dcam_sem[DCAM_MAX_COUNT];
static struct mutex dcam_module_sema[DCAM_MAX_COUNT];

static spinlock_t dcam_lock;
static spinlock_t dcam_glb_reg_cfg_lock[DCAM_MAX_COUNT];
static spinlock_t dcam_glb_reg_control_lock[DCAM_MAX_COUNT];
static spinlock_t dcam_glb_reg_mask_lock[DCAM_MAX_COUNT];
static spinlock_t dcam_glb_reg_clr_lock[DCAM_MAX_COUNT];
static spinlock_t dcam_glb_reg_ahbm_sts_lock[DCAM_MAX_COUNT];
static spinlock_t dcam_glb_reg_endian_lock[DCAM_MAX_COUNT];

static struct clk *dcam0_clk;
static struct clk *dcam0_clk_default;
static struct clk *dcam0_clk_parent;
static struct clk *dcam1_clk;
static struct clk *dcam1_clk_default;
static struct clk *dcam1_clk_parent;
static struct clk *dcam0_if_clk;
static struct clk *dcam0_if_clk_default;
static struct clk *dcam0_if_clk_parent;

static struct clk *dcam0_eb;
static struct clk *dcam0_axi_eb;
static struct clk *dcam1_eb;
static struct clk *dcam1_axi_eb;
static struct clk *d0if_in_d_eb;
static struct clk *d1if_in_d_eb;
static struct regmap *cam_ahb_gpr;
struct dcam_init_flag glb_dcam_init_flag = {0};

const unsigned int s_irq_vect[] = {
	DCAM_SN_EOF,
	DCAM_CAP_EOF,
	DCAM_PATH0_DONE,
	DCAM_PATH1_DONE,
	DCAM_PATH2_DONE,
	DCAM_PATH3_DONE,
	DCAM_CAP_SOF,
	DCAM_PATH0_SOF,
	DCAM_PATH1_SOF,
	DCAM_PATH2_SOF,
	DCAM_PATH3_SOF,
#if 0
	DCAM_PATH0_OV,
	DCAM_PATH1_OV,
	DCAM_PATH2_OV,
	DCAM_PATH3_OV,
	DCAM_SN_LINE_ERR,
	DCAM_SN_FRAME_ERR,
	DCAM_JPEG_BUF_OV,
	DCAM_ISP_OV,
	DCAM_MIPI_OV,
	DCAM_RAW_SLICE_DONE
	DCAM_I2P_OV,
	DCAM_JPEGLS_DONE,
	DCAM_JPEGLS_OV,
	DCAM_PATH0_END,
	DCAM_PATH1_END,
	DCAM_PATH2_END,
	DCAM_PATH3_END
	DCAM_ROT_DONE,
	DCAM_PATH1_SLICE_DONE,
	DCAM_PATH2_SLICE_DONE
#endif
};

/* Internal Function Implementation */

int sprd_dcam_cowork_enable(enum dcam_id idx)
{
	if (DCAM_ADDR_INVALID(s_p_dcam_mod[idx])) {
		pr_err("zero pointer\n");
		return -EFAULT;
	}

	if (!isp_match_stripe_mode())
		return 0;

	s_p_dcam_mod[idx]->cowork_enable = 1;
	if (DCAM_ID_0 == idx && NULL == s_p_dcam_mod[idx]->cowork_info) {
		s_p_dcam_mod[idx]->cowork_info =
			vzalloc(sizeof(struct DCAMINFO));
		if (DCAM_ADDR_INVALID(s_p_dcam_mod[idx]->cowork_info)) {
			pr_err("zero pointer\n");
			return -EFAULT;
		}
		pr_info("alloc cowork_info:0x%p\n",
			s_p_dcam_mod[0]->cowork_info);
	}
	return 0;
}

void sprd_dcam_cowork_disable(enum dcam_id idx)
{
	if (DCAM_ADDR_INVALID(s_p_dcam_mod[idx])) {
		pr_err("zero pointer\n");
		return;
	}

	if (!isp_match_stripe_mode())
		return;

	s_p_dcam_mod[idx]->cowork_enable = 0;
	if (idx == DCAM_ID_0 && s_p_dcam_mod[idx]->cowork_info) {
		pr_info("release cowork_info:0x%p\n",
			s_p_dcam_mod[0]->cowork_info);
		vfree(s_p_dcam_mod[idx]->cowork_info);
		s_p_dcam_mod[idx]->cowork_info = NULL;
	}
}

static int is_dcam_cowork(void)
{
	if (s_p_dcam_mod[DCAM_ID_0] && s_p_dcam_mod[DCAM_ID_1]) {
		if (s_p_dcam_mod[0]->cowork_enable &&
		    s_p_dcam_mod[1]->cowork_enable)
			return 1;
	}

	return 0;
}

/* Internal Function Implementation */

static int dcam_enable_clk(enum dcam_id idx)
{
	pr_info("clk enable! %d %d", atomic_read(&s_dcam_users[0]),
		atomic_read(&s_dcam_users[1]));
	if ((atomic_read(&s_dcam_users[0]) == 1
		&& atomic_read(&s_dcam_users[1]) == 0)
		|| (atomic_read(&s_dcam_users[0]) == 0
		&& atomic_read(&s_dcam_users[1]) == 1)) {
		clk_prepare_enable(dcam0_eb);
		clk_prepare_enable(dcam0_axi_eb);
		clk_prepare_enable(dcam1_eb);
		clk_prepare_enable(dcam1_axi_eb);
		clk_prepare_enable(d0if_in_d_eb);
		clk_prepare_enable(d1if_in_d_eb);

		clk_set_parent(dcam0_clk, dcam0_clk_parent);
		clk_prepare_enable(dcam0_clk);

		clk_set_parent(dcam1_clk, dcam1_clk_parent);
		clk_prepare_enable(dcam1_clk);

		clk_set_parent(dcam0_if_clk, dcam0_if_clk_parent);
		clk_prepare_enable(dcam0_if_clk);
	}
	return 0;
}

static int dcam_disable_clk(enum dcam_id idx)
{
	pr_info("clk disable! %d %d", atomic_read(&s_dcam_users[0]),
		atomic_read(&s_dcam_users[1]));
	if (atomic_read(&s_dcam_users[0]) == 0
		&& atomic_read(&s_dcam_users[1]) == 0) {
		clk_disable_unprepare(dcam0_eb);
		clk_disable_unprepare(dcam0_axi_eb);
		clk_disable_unprepare(dcam1_eb);
		clk_disable_unprepare(dcam1_axi_eb);
		clk_disable_unprepare(d0if_in_d_eb);
		clk_disable_unprepare(d1if_in_d_eb);

		clk_set_parent(dcam0_clk, dcam0_clk_default);
		clk_disable_unprepare(dcam0_clk);

		clk_set_parent(dcam1_clk, dcam1_clk_default);
		clk_disable_unprepare(dcam1_clk);

		clk_set_parent(dcam0_if_clk, dcam0_if_clk_default);
		clk_disable_unprepare(dcam0_if_clk);
	}
	return 0;
}

static unsigned int *dcam_get_scale_coeff_addr(enum dcam_id idx,
					       unsigned int *index)
{
	unsigned int i;

	if (DCAM_ADDR_INVALID(s_dcam_sc_array[idx])) {
		pr_err("DCAM%d: scale addr, invalid parm %p\n", idx,
		       s_dcam_sc_array[idx]);
		return NULL;
	}

	for (i = 0; i < DCAM_SC_COEFF_BUF_COUNT; i++) {
		if (s_dcam_sc_array[idx]->scaling_coeff[i].flag == 0) {
			*index = i;
			DCAM_TRACE("get buf index %d\n", i);
			return s_dcam_sc_array[idx]->scaling_coeff[i].buf;
		}
	}
	pr_info("DCAM%d: get buf index %d\n", idx, i);

	return NULL;
}

static void dcam_path_set(enum dcam_id idx, enum dcam_path_index path_index)
{
	unsigned int reg_val = 0;
	unsigned long addr = 0;
	struct dcam_path_desc *path = NULL;
	struct dcam_rect *rect;
	struct dcam_size *size;
	union dcam_regular_value *v;
	enum dcam_regular_mode mode;

	if (DCAM_ADDR_INVALID(s_p_dcam_mod[idx])) {
		pr_err("zero pointer\n");
		return;
	}

	if (DCAM_PATH_IDX_0 & path_index) {
		path = &s_p_dcam_mod[idx]->dcam_path[DCAM_PATH0];
		if (path->valid_param.output_format) {
			unsigned int format = path->output_format - 0x10;

			if (path->pdaf_ctrl.mode == 1)
				DCAM_REG_WR(idx, DCAM_PATH0_CFG, 0 << 2);
			else
				DCAM_REG_WR(idx, DCAM_PATH0_CFG, format << 2);
			DCAM_TRACE("DCAM: path 0, output format 0x%x\n",
				   format);
		}

		if (path->valid_param.frame_deci) {
			sprd_dcam_glb_reg_mwr(idx, DCAM_PATH0_CFG,
					      BIT_1 | BIT_0,
					      path->frame_deci << 0,
					      DCAM_REG_MAX);
		}

		if (path->valid_param.data_endian) {
			sprd_dcam_glb_reg_mwr(idx, DCAM_ENDIAN_SEL,
					      BIT_5 | BIT_4,
					      path->data_endian.y_endian << 4,
					      DCAM_ENDIAN_REG);
			sprd_dcam_glb_reg_mwr(idx, DCAM_ENDIAN_SEL,
					      BIT_18, BIT_18,
					      DCAM_ENDIAN_REG);
			DCAM_TRACE("path 0: data_endian y=0x%x\n",
				   path->data_endian.y_endian);
		}

		if (path->valid_param.input_rect) {
			DCAM_REG_MWR(idx, CAP_MIPI_CTRL, BIT_9, BIT_9);
			pr_info("%s, set path0 crop_eb\n", __func__);

			addr = PACK_CROP_START;

			rect = &path->input_rect;
			reg_val = rect->x | (rect->y << 16);
			DCAM_REG_WR(idx, addr, reg_val);

			addr = PACK_CROP_END;

			rect = &path->input_rect;
			reg_val = (rect->x + rect->w - 1) |
				((rect->y + rect->h - 1) << 16);
			DCAM_REG_WR(idx, addr, reg_val);
			pr_info("path%d set: rect {%d %d %d %d}\n",
				   ffs(path_index) - 1,
				   path->input_rect.x,
				   path->input_rect.y,
				   path->input_rect.w, path->input_rect.h);
		}

		if (path->valid_param.pdaf_ctrl) {
			sprd_dcam_glb_reg_mwr(idx, DCAM_PDAF_CTRL,
					      BIT_0,
					      path->pdaf_ctrl.mode << 0,
					      DCAM_REG_MAX);
			sprd_dcam_glb_reg_mwr(idx, DCAM_PDAF_CTRL,
					      BIT_13 | BIT_12 | BIT_11 |
					      BIT_10 | BIT_9 | BIT_8,
					      path->pdaf_ctrl.phase_data_type
					      << 8,
					      DCAM_REG_MAX);
		}
	} else if ((DCAM_PATH_IDX_1 |
		   DCAM_PATH_IDX_2 |
		   DCAM_PATH_IDX_3) &
		   path_index) {
		if (DCAM_PATH_IDX_1 & path_index)
			path = &s_p_dcam_mod[idx]->dcam_path[DCAM_PATH1];
		else if (DCAM_PATH_IDX_2 & path_index)
			path = &s_p_dcam_mod[idx]->dcam_path[DCAM_PATH2];
		else if (DCAM_PATH_IDX_3 & path_index)
			path = &s_p_dcam_mod[idx]->dcam_path[DCAM_PATH3];

		if (path->valid_param.input_size) {
			if (DCAM_PATH_IDX_1 & path_index)
				addr = DCAM_PATH1_SRC_SIZE;
			else if (DCAM_PATH_IDX_2 & path_index)
				addr = DCAM_PATH2_SRC_SIZE;
			else if (DCAM_PATH_IDX_3 & path_index)
				addr = DCAM_PATH3_SRC_SIZE;

			size = &path->input_size;
			reg_val = size->w | (size->h << 16);
			DCAM_REG_WR(idx, addr, reg_val);
			DCAM_TRACE("path%d set: src {%d %d}\n",
				   ffs(path_index) - 1, path->input_size.w,
				   path->input_size.h);
		}

		if (path->valid_param.input_rect) {
			if (DCAM_PATH_IDX_1 & path_index)
				addr = DCAM_PATH1_TRIM0_START;
			else if (DCAM_PATH_IDX_2 & path_index)
				addr = DCAM_PATH2_TRIM0_START;
			else if (DCAM_PATH_IDX_3 & path_index)
				addr = DCAM_PATH3_TRIM0_START;

			rect = &path->input_rect;
			reg_val = rect->x | (rect->y << 16);
			DCAM_REG_WR(idx, addr, reg_val);

			if (DCAM_PATH_IDX_1 & path_index)
				addr = DCAM_PATH1_TRIM0_SIZE;
			else if (DCAM_PATH_IDX_2 & path_index)
				addr = DCAM_PATH2_TRIM0_SIZE;
			else if (DCAM_PATH_IDX_3 & path_index)
				addr = DCAM_PATH3_TRIM0_SIZE;

			rect = &path->input_rect;
			reg_val = rect->w | (rect->h << 16);
			DCAM_REG_WR(idx, addr, reg_val);
			DCAM_TRACE("path%d set: rect {%d %d %d %d}\n",
				   ffs(path_index) - 1,
				   path->input_rect.x,
				   path->input_rect.y,
				   path->input_rect.w, path->input_rect.h);
		}

		if (path->valid_param.output_size) {
			if (DCAM_PATH_IDX_1 & path_index)
				addr = DCAM_PATH1_DST_SIZE;
			else if (DCAM_PATH_IDX_2 & path_index)
				addr = DCAM_PATH2_DST_SIZE;
			else if (DCAM_PATH_IDX_3 & path_index)
				addr = DCAM_PATH3_DST_SIZE;

			size = &path->output_size;
			reg_val = size->w | (size->h << 16);
			DCAM_REG_WR(idx, addr, reg_val);
			DCAM_TRACE("path%d set: dst {%d %d}\n",
				   ffs(path_index) - 1, path->output_size.w,
				   path->output_size.h);
		}

		if (path->valid_param.src_sel) {
			if (DCAM_PATH_IDX_1 & path_index)
				DCAM_REG_MWR(idx, DCAM_CFG, BIT_12 | BIT_11,
					path->src_sel << 11);
			else if (DCAM_PATH_IDX_2 & path_index)
				DCAM_REG_MWR(idx, DCAM_CFG, BIT_13 | BIT_14,
					path->src_sel << 13);
			else if (DCAM_PATH_IDX_3 & path_index)
				DCAM_REG_MWR(idx, DCAM_CFG, BIT_17 | BIT_18,
					path->src_sel << 17);
			DCAM_TRACE("path %d, src_sel=0x%x\n",
				   ffs(path_index) - 1,
				   path->src_sel);
		}

		if (path->valid_param.data_endian) {
			if (DCAM_PATH_IDX_1 & path_index) {
				sprd_dcam_glb_reg_mwr(idx, DCAM_ENDIAN_SEL,
						      BIT_7 | BIT_6,
						      path->data_endian.
						      y_endian << 6,
						      DCAM_ENDIAN_REG);
				sprd_dcam_glb_reg_mwr(idx, DCAM_ENDIAN_SEL,
						      BIT_9 | BIT_8,
						      path->data_endian.
						      uv_endian << 8,
						      DCAM_ENDIAN_REG);
			} else if (DCAM_PATH_IDX_2 & path_index) {
				sprd_dcam_glb_reg_mwr(idx, DCAM_ENDIAN_SEL,
						      BIT_11 | BIT_10,
						      path->data_endian.
						      y_endian << 10,
						      DCAM_ENDIAN_REG);
				sprd_dcam_glb_reg_mwr(idx, DCAM_ENDIAN_SEL,
						      BIT_13 | BIT_12,
						      path->data_endian.
						      uv_endian << 12,
						      DCAM_ENDIAN_REG);
			} else if (DCAM_PATH_IDX_3 & path_index) {
				sprd_dcam_glb_reg_mwr(idx, DCAM_ENDIAN_SEL,
						      BIT_22 | BIT_21,
						      path->data_endian.
						      y_endian << 21,
						      DCAM_ENDIAN_REG);
				sprd_dcam_glb_reg_mwr(idx, DCAM_ENDIAN_SEL,
						      BIT_24 | BIT_23,
						      path->data_endian.
						      uv_endian << 23,
						      DCAM_ENDIAN_REG);
			}

			sprd_dcam_glb_reg_mwr(idx, DCAM_ENDIAN_SEL,
					      BIT_18, BIT_18,
					      DCAM_ENDIAN_REG);
			DCAM_TRACE("path %d: data_endian y=0x%x, uv=0x%x\n",
				   ffs(path_index) - 1,
				   path->data_endian.y_endian,
				   path->data_endian.uv_endian);
		}

		/* begin to set DCAM_PATH_CFG */
		if (DCAM_PATH_IDX_1 & path_index)
			addr = DCAM_PATH1_CFG;
		else if (DCAM_PATH_IDX_2 & path_index)
			addr = DCAM_PATH2_CFG;
		else if (DCAM_PATH_IDX_3 & path_index)
			addr = DCAM_PATH3_CFG;

		if (path->valid_param.output_format) {
			if (path->output_format == DCAM_YUV422 ||
			    path->output_format == DCAM_YUV420 ||
			    path->output_format == DCAM_YUV420_3FRAME) {
				DCAM_REG_MWR(idx, addr, BIT_7 | BIT_6,
					path->output_format << 6);
				DCAM_TRACE("path%d: output_format=0x%x\n",
					   ffs(path_index) - 1,
					   path->output_format);
			} else
				pr_info("invalid path%d output format %d\n",
					ffs(path_index) - 1,
					path->output_format);
		}

		if (path->valid_param.frame_deci) {
			if (path->frame_deci >= 0 && path->frame_deci <= 3) {
				DCAM_REG_MWR(idx, addr, BIT_24 | BIT_23,
					path->frame_deci << 23);
				DCAM_TRACE("path %d: frame_deci=0x%x\n",
					   ffs(path_index) - 1,
					   path->frame_deci);
			} else {
				pr_info("invalid path%d frame_deci %d\n",
					ffs(path_index) - 1,
					path->frame_deci);
			}

		}

		if (path->valid_param.scale_tap) {
			path->valid_param.scale_tap = 0;
			DCAM_REG_MWR(idx, addr,
				BIT_19 | BIT_18 | BIT_17 | BIT_16,
				(path->scale_tap.y_tap & 0x0F) << 16);
			DCAM_REG_MWR(idx, addr,
				BIT_15 | BIT_14 | BIT_13 | BIT_12 | BIT_11,
				(path->scale_tap.uv_tap & 0x1F) << 11);
			DCAM_TRACE("path %d: scale_tap, y=0x%x, uv=0x%x\n",
				   ffs(path_index) - 1, path->scale_tap.y_tap,
				   path->scale_tap.uv_tap);
		}

		if (path->valid_param.v_deci) {
			path->valid_param.v_deci = 0;
			DCAM_REG_MWR(idx, addr, BIT_2,
				path->deci_val.deci_x_en << 2);
			DCAM_REG_MWR(idx, addr, BIT_1 | BIT_0,
				path->deci_val.deci_x);
			DCAM_REG_MWR(idx, addr, BIT_5,
				path->deci_val.deci_y_en << 5);
			DCAM_REG_MWR(idx, addr, BIT_4 | BIT_3,
				path->deci_val.deci_y << 3);
			DCAM_TRACE("path %d: deci x_en=%d x=%d y_en=%d y=%d\n",
				   ffs(path_index) - 1,
				   path->deci_val.deci_x_en,
				   path->deci_val.deci_x,
				   path->deci_val.deci_y_en,
				   path->deci_val.deci_y);
		}

		if (path->valid_param.rot_mode) {
			path->valid_param.rot_mode = 0;
			DCAM_REG_MWR(idx, addr, BIT_10 | BIT_9,
				path->rot_mode << 9);
			DCAM_TRACE("dcam_path%d_set rot_mod :%d reg:%x\n",
				   ffs(path_index) - 1,
				   path->rot_mode, DCAM_REG_RD(idx, addr));
		}

		if (path->valid_param.shrink) {
			path->valid_param.shrink = 0;
			/* current shrinfvk range, y: (16, 235) uv: (16, 240) */
			if (path->regular_desc.regular_mode ==
			    DCAM_REGULAR_SHRINK) {
				reg_val = SHRINK_Y_UP_TH << 0 |
					SHRINK_Y_DN_TH << 8 |
					SHRINK_UV_UP_TH << 16 |
					SHRINK_UV_DN_TH << 24;
				DCAM_REG_WR(idx, DCAM_YUV_SHRINK, reg_val);
				reg_val = (SHRINK_Y_OFFSET & 0x1f) << 0 |
					(SHRINK_Y_RANGE & 0xf) << 8 |
					(SHRINK_C_OFFSET & 0x1f) << 16 |
					(SHRINK_C_RANGE & 0xf) << 24;
				DCAM_REG_WR(idx, DCAM_YUV_REGULAR, reg_val);
			} else if (path->regular_desc.regular_mode ==
				   DCAM_REGULAR_CUT) {
				v = &path->regular_desc.regular_value;
				reg_val =
					v->shrink_val.y_up_threshold << 0 |
					v->shrink_val.y_dn_threshold << 8 |
					v->shrink_val.uv_up_threshold << 16 |
					v->shrink_val.uv_dn_threshold << 24;
				DCAM_REG_WR(idx, DCAM_YUV_SHRINK, reg_val);
			} else if (path->regular_desc.regular_mode ==
				   DCAM_REGULAR_EFFECT) {
				v = &path->regular_desc.regular_value;
				reg_val =
					v->effect_val.y_special_threshold << 0 |
					v->effect_val.u_special_threshold << 8 |
					v->effect_val.v_special_threshold << 16;
				DCAM_REG_WR(idx, DCAM_YUV_EFFECT, reg_val);
			}

			mode = path->regular_desc.regular_mode;
			sprd_dcam_glb_reg_mwr(idx, addr, BIT_25 | BIT_26,
					      mode << 25, DCAM_REG_MAX);

			DCAM_TRACE("path %d: shrink, %d\n", ffs(path_index) - 1,
				   path->regular_desc.regular_mode);
		}

		if (path->valid_param.jpegls) {
			if (DCAM_PATH_IDX_2 & path_index) {
				DCAM_REG_WR(idx, JPEGLS_BSM_SIZE_THD_Y,
				       path->jpegls_desc.jpegls_thd[0]);
				DCAM_REG_WR(idx, JPEGLS_BSM_SIZE_THD_U,
				       path->jpegls_desc.jpegls_thd[1]);
				DCAM_REG_WR(idx, JPEGLS_BSM_SIZE_THD_V,
				       path->jpegls_desc.jpegls_thd[2]);

				path->valid_param.jpegls = 0;
				DCAM_TRACE("path %d: jpegls thd:\n",
					   ffs(path_index) - 1);
				DCAM_TRACE("0x%.8x 0x%.8x 0x%.8x\n",
					   path->jpegls_desc.jpegls_thd[0],
					   path->jpegls_desc.jpegls_thd[1],
					   path->jpegls_desc.jpegls_thd[2]);
			} else {
				pr_err("Only path2 support jpegls!\n");
			}
		}
	}
}

static void dcam_cowork_para_init(enum dcam_path_index path_index,
	struct DCAMINFO *dcam_info,
	unsigned char over_lap)
{
	unsigned char path_id = 0;
	struct YUV_PATH_INFO_tag *yuv_path = NULL;
	struct YUV_PATH_INFO_tag *input_info = NULL;
	struct dcam_path_desc *path = NULL;

	switch (path_index) {
	case DCAM_PATH_IDX_1:
		path_id = DCAM_PATH1;
		break;
	case DCAM_PATH_IDX_2:
		path_id = DCAM_PATH2;
		break;
	case DCAM_PATH_IDX_3:
		path_id = DCAM_PATH3;
		break;
	default:
		pr_err("%s, path_index: %d, not support!\n",
			__func__, path_index);
		return;
	}
	path = &s_p_dcam_mod[DCAM_ID_0]->dcam_path[path_id];
	yuv_path = &dcam_info->dcam_yuv_path[path_id - 1];
	yuv_path->path_en = 1;
	yuv_path->src_size_x = path->input_size.w;
	yuv_path->src_size_y = path->input_size.h;
	yuv_path->trim0_info.trim_start_x = path->input_rect.x;
	yuv_path->trim0_info.trim_start_y = path->input_rect.y;
	yuv_path->trim0_info.trim_size_x = path->input_rect.w;
	yuv_path->trim0_info.trim_size_y = path->input_rect.h;
	yuv_path->scaler_info.scaler_out_width = path->output_size.w;
	yuv_path->scaler_info.scaler_out_height = path->output_size.h;
	if (path->deci_val.deci_x_en) {
		yuv_path->deci_info.deci_x_en = 1;
		/*0-2 1-4 2-8 3-16*/
		yuv_path->deci_info.deci_x = 1 << (path->deci_val.deci_x + 1);
	} else {
		yuv_path->deci_info.deci_x_en = 0;
		yuv_path->deci_info.deci_x = 1;
	}
	if (path->deci_val.deci_y_en) {
		yuv_path->deci_info.deci_y_en = 1;
		yuv_path->deci_info.deci_y = 1 << (path->deci_val.deci_y + 1);
	} else {
		yuv_path->deci_info.deci_y_en = 0;
		yuv_path->deci_info.deci_y = 1;
	}
	yuv_path->outdata_format = 1;
	yuv_path->outdata_mode = 1;
	yuv_path->scaler_info.scaler_en = 1;
	DCAM_TRACE("src: %d %d, trim: %d, %d, %d, %d, out: %d, %d\n",
		path->input_size.w, path->input_size.h,
		path->input_rect.x, path->input_rect.y, path->input_rect.w,
		path->input_rect.h, path->output_size.w,
		path->output_size.h);

	InitDcamInfo(dcam_info);

	input_info = &dcam_info->InputInfo[DCAM_ID_0][path_id - 1];
	path = &s_p_dcam_mod[DCAM_ID_0]->dcam_path[path_id];
	if (over_lap == ISP_OVERLAP_ALIGN_16) {
		path->input_size.w = input_info->src_size_x;
		path->input_size.h = input_info->src_size_y;
		path->input_rect.x = input_info->trim0_info.trim_start_x;
		path->input_rect.y = input_info->trim0_info.trim_start_y;
		path->input_rect.w = input_info->trim0_info.trim_size_x;
		path->input_rect.h = input_info->trim0_info.trim_size_y;
	} else {
		path->input_size.w = input_info->src_size_x + 4;
		path->input_size.h = input_info->src_size_y;
		path->input_rect.x = input_info->trim0_info.trim_start_x;
		path->input_rect.y = input_info->trim0_info.trim_start_y;
		path->input_rect.w = input_info->trim0_info.trim_size_x;
		path->input_rect.h = input_info->trim0_info.trim_size_y;
	}

	path->output_size.w = input_info->scaler_info.scaler_out_width;
	path->output_size.h = input_info->scaler_info.scaler_out_height;
	path->cowork_desc.trim1.x = input_info->trim1_info.trim_start_x;
	path->cowork_desc.trim1.y = input_info->trim1_info.trim_start_y;
	path->cowork_desc.trim1.w = input_info->trim1_info.trim_size_x;
	path->cowork_desc.trim1.h = input_info->trim1_info.trim_size_y;
	path->cowork_desc.scl_ip_int =
		input_info->scaler_info.scaler_init_phase_int;
	path->cowork_desc.scl_ip_rmd =
		input_info->scaler_info.scaler_init_phase_rmd;
	path->cowork_desc.scl_cip_int =
		input_info->scaler_info.scaler_chroma_init_phase_int;
	path->cowork_desc.scl_cip_rmd =
		input_info->scaler_info.scaler_chroma_init_phase_rmd;
	path->cowork_desc.scl_factor_in =
		input_info->scaler_info.scaler_factor_in_hor;
	path->cowork_desc.scl_factor_out =
		input_info->scaler_info.scaler_factor_out_hor;
	path->cowork_desc.pitch =
		input_info->scaler_info.scaler_factor_out_hor;

	input_info = &dcam_info->InputInfo[DCAM_ID_1][path_id - 1];
	path = &s_p_dcam_mod[DCAM_ID_1]->dcam_path[path_id];
	if (over_lap == ISP_OVERLAP_ALIGN_16) {
		path->input_size.w = input_info->src_size_x;
		path->input_size.h = input_info->src_size_y;
		path->input_rect.x = input_info->trim0_info.trim_start_x;
		path->input_rect.y = input_info->trim0_info.trim_start_y;
		path->input_rect.w = input_info->trim0_info.trim_size_x;
		path->input_rect.h = input_info->trim0_info.trim_size_y;
	} else {
		path->input_size.w = input_info->src_size_x + 4;
		path->input_size.h = input_info->src_size_y;
		path->input_rect.x = input_info->trim0_info.trim_start_x + 4;
		path->input_rect.y = input_info->trim0_info.trim_start_y;
		path->input_rect.w = input_info->trim0_info.trim_size_x;
		path->input_rect.h = input_info->trim0_info.trim_size_y;
	}
	path->output_size.w = input_info->scaler_info.scaler_out_width;
	path->output_size.h = input_info->scaler_info.scaler_out_height;

	path->cowork_desc.trim1.x = input_info->trim1_info.trim_start_x;
	path->cowork_desc.trim1.y = input_info->trim1_info.trim_start_y;
	path->cowork_desc.trim1.w = input_info->trim1_info.trim_size_x;
	path->cowork_desc.trim1.h = input_info->trim1_info.trim_size_y;
	path->cowork_desc.scl_ip_int =
		input_info->scaler_info.scaler_init_phase_int;
	path->cowork_desc.scl_ip_rmd =
		input_info->scaler_info.scaler_init_phase_rmd;
	path->cowork_desc.scl_cip_int =
		input_info->scaler_info.scaler_chroma_init_phase_int;
	path->cowork_desc.scl_cip_rmd =
		input_info->scaler_info.scaler_chroma_init_phase_rmd;
	path->cowork_desc.scl_factor_in =
		input_info->scaler_info.scaler_factor_in_hor;
	path->cowork_desc.scl_factor_out =
		input_info->scaler_info.scaler_factor_out_hor;
	path->cowork_desc.pitch =
		input_info->scaler_info.scaler_factor_out_hor;
}

static int32_t dcam_cowork_para_cfg(enum dcam_path_index path_index)
{
	unsigned long cfg_reg[6] = {0};
	unsigned char over_lap = 0;
	struct DCAMINFO *dcam_info = NULL;
	enum dcam_id idx = DCAM_ID_0;
	struct dcam_path_desc   *path = NULL;
	struct dcam_cap_desc   *cap_desc = NULL;
	enum dcam_drv_rtn       rtn = DCAM_RTN_SUCCESS;

	pr_info("%s, cfg path_index:%d\n", __func__, path_index);

	if (!((DCAM_PATH_IDX_1 |
		DCAM_PATH_IDX_2 |
		DCAM_PATH_IDX_3) & path_index)) {
		pr_err("path_index error: %d\n", path_index);
		rtn = -EINVAL;
		goto exit;
	}

	dcam_info = s_p_dcam_mod[DCAM_ID_0]->cowork_info;
	memset(dcam_info, 0, sizeof(struct DCAMINFO));

	cap_desc = &s_p_dcam_mod[DCAM_ID_0]->dcam_cap;

	if (0 == (cap_desc->cap_rect.w % 16)) {
		over_lap = ISP_OVERLAP_ALIGN_16;
	} else if (8 == (cap_desc->cap_rect.w % 16)) {
		over_lap = ISP_OVERLAP_ALIGN_16 + 4;
	} else {
		pr_err("invalid width: %d\n",
			cap_desc->cap_rect.w);
		rtn = -EINVAL;
		goto exit;
	}

	dcam_info->cowork_mode = 1;
	dcam_info->cowork_overlap = ISP_OVERLAP_ALIGN_16;
	dcam_info->dcam_in_width = cap_desc->cap_rect.w;
	dcam_info->dcam_in_height = cap_desc->cap_rect.h;
	DCAM_TRACE("overlap: %d, dcam: %d, %d\n",
		over_lap, cap_desc->cap_rect.w, cap_desc->cap_rect.h);

	dcam_cowork_para_init(path_index, dcam_info, over_lap);

	for (idx = DCAM_ID_0; idx < DCAM_ID_MAX; idx++) {
		if (DCAM_ADDR_INVALID(s_p_dcam_mod[idx])) {
			pr_err("zero pointer\n");
			return -EFAULT;
		}
		if (0 == ((DCAM_REG_RD(idx, DCAM_CFG)) & BIT_19)) {
			pr_err("%s,cowork mode disable!\n", __func__);
			rtn = -EINVAL;
			goto exit;
		}
		if (path_index == DCAM_PATH_IDX_1) {
			path = &s_p_dcam_mod[idx]->dcam_path[DCAM_PATH1];
			cfg_reg[0] = DCAM_PATH1_PITCH;
			cfg_reg[1] = DCAM_PATH1_TRIM1_START;
			cfg_reg[2] = DCAM_PATH1_TRIM1_SIZE;
			cfg_reg[3] = DCAM_PATH1_SCL_IP;
			cfg_reg[4] = DCAM_PATH1_SCL_CIP;
			cfg_reg[5] = DCAM_PATH1_SCL_FACTOR;
		} else if (path_index == DCAM_PATH_IDX_2) {
			path = &s_p_dcam_mod[idx]->dcam_path[DCAM_PATH2];
			cfg_reg[0] = DCAM_PATH2_PITCH;
			cfg_reg[1] = DCAM_PATH2_TRIM1_START;
			cfg_reg[2] = DCAM_PATH2_TRIM1_SIZE;
			cfg_reg[3] = DCAM_PATH2_SCL_IP;
			cfg_reg[4] = DCAM_PATH2_SCL_CIP;
			cfg_reg[5] = DCAM_PATH2_SCL_FACTOR;
		} else if (path_index == DCAM_PATH_IDX_3) {
			path = &s_p_dcam_mod[idx]->dcam_path[DCAM_PATH3];
			cfg_reg[0] = DCAM_PATH3_PITCH;
			cfg_reg[1] = DCAM_PATH3_TRIM1_START;
			cfg_reg[2] = DCAM_PATH3_TRIM1_SIZE;
			cfg_reg[3] = DCAM_PATH3_SCL_IP;
			cfg_reg[4] = DCAM_PATH3_SCL_CIP;
			cfg_reg[5] = DCAM_PATH3_SCL_FACTOR;
		}

		DCAM_REG_WR(idx, cfg_reg[0],
			(path->cowork_desc.pitch & 0x1fff) | BIT_16);
		DCAM_REG_WR(idx, cfg_reg[1],
			(path->cowork_desc.trim1.x & 0x1fff) |
			((path->cowork_desc.trim1.y & 0x1fff) << 16));
		DCAM_REG_WR(idx, cfg_reg[2],
			(path->cowork_desc.trim1.w & 0x1fff) |
			((path->cowork_desc.trim1.h & 0x1fff) << 16));
		DCAM_REG_WR(idx, cfg_reg[3],
			(path->cowork_desc.scl_ip_rmd & 0x1fff) |
			((path->cowork_desc.scl_ip_int & 0x1fff) << 16));
		DCAM_REG_WR(idx, cfg_reg[4],
			(path->cowork_desc.scl_cip_rmd & 0x1fff) |
			((path->cowork_desc.scl_cip_int & 0x1fff) << 16));
		DCAM_REG_WR(idx, cfg_reg[5],
			(path->cowork_desc.scl_factor_out & 0x1fff) |
			((path->cowork_desc.scl_factor_in & 0x1fff) << 16));
	}
exit:
	return rtn;
}

static void dcam_buf_queue_init(struct dcam_buf_queue *queue)
{
	if (DCAM_ADDR_INVALID(queue)) {
		pr_err("invalid heap %p\n", queue);
		return;
	}

	memset((void *)queue, 0, sizeof(struct dcam_buf_queue));
	queue->write = &queue->frame[0];
	queue->read = &queue->frame[0];
	spin_lock_init(&queue->lock);
}

static int dcam_buf_queue_write(struct dcam_buf_queue *queue,
				struct dcam_frame *frame)
{
	int ret = DCAM_RTN_SUCCESS;
	struct dcam_frame *ori_frame;
	unsigned long flags;

	if (DCAM_ADDR_INVALID(queue) || DCAM_ADDR_INVALID(frame)) {
		pr_err("enq, invalid parm %p, %p\n", queue, frame);
		return -EINVAL;
	}

	DCAM_TRACE("write buf queue\n");
	spin_lock_irqsave(&queue->lock, flags);

	ori_frame = queue->write;
	*queue->write++ = *frame;
	queue->w_index++;
	if (queue->write > &queue->frame[DCAM_FRM_CNT_MAX - 1]) {
		queue->write = &queue->frame[0];
		queue->w_index = 0;
		DCAM_TRACE("warning, queue write rewind\n");
	}

	if (queue->write == queue->read) {
		queue->write = ori_frame;
		DCAM_TRACE("warning, queue is full, can't write 0x%x\n",
			frame->yaddr);
		ret = -EAGAIN;
	}
	spin_unlock_irqrestore(&queue->lock, flags);

	DCAM_TRACE("write buf queue type %d index %x\n",
		   frame->type, queue->w_index);

	return ret;
}

static int dcam_buf_queue_read(struct dcam_buf_queue *queue,
			       struct dcam_frame *frame)
{
	int ret = DCAM_RTN_SUCCESS;
	unsigned long flags;

	if (DCAM_ADDR_INVALID(queue) || DCAM_ADDR_INVALID(frame)) {
		pr_err("deq, invalid parm %p, %p\n", queue, frame);
		return -EINVAL;
	}

	DCAM_TRACE("read buf queue\n");

	spin_lock_irqsave(&queue->lock, flags);
	if (queue->read != queue->write) {
		*frame = *queue->read++;
		queue->r_index++;
		if (queue->read > &queue->frame[DCAM_FRM_CNT_MAX - 1]) {
			queue->read = &queue->frame[0];
			queue->r_index = 0;
		}
	} else {
		ret = -EAGAIN;
		DCAM_TRACE("warning, read wait new node write\n");
	}
	spin_unlock_irqrestore(&queue->lock, flags);

	DCAM_TRACE("read buf queue %d index %x\n",
		   frame->type, queue->r_index);

	return ret;
}

static void dcam_frm_queue_clear(struct dcam_frm_queue *queue)
{
	if (DCAM_ADDR_INVALID(queue)) {
		pr_err("invalid heap %p\n", queue);
		return;
	}

	memset((void *)queue, 0, sizeof(struct dcam_frm_queue));
}

static int dcam_frame_enqueue(struct dcam_frm_queue *queue,
			      struct dcam_frame *frame)
{
	if (DCAM_ADDR_INVALID(queue) || DCAM_ADDR_INVALID(frame)) {
		pr_err("enq, invalid parm %p, %p\n", queue, frame);
		return -1;
	}

	if (queue->valid_cnt >= DCAM_FRM_QUEUE_LENGTH) {
		pr_info("q over flow\n");
		return -1;
	}

	memcpy(&queue->frm_array[queue->valid_cnt], frame,
	       sizeof(struct dcam_frame));
	queue->valid_cnt++;
	DCAM_TRACE("en queue, %d, %d, 0x%x, 0x%x\n",
		   (0xF & frame->fid),
		   queue->valid_cnt, frame->yaddr, frame->uaddr);

	return 0;
}

static int dcam_frame_dequeue(struct dcam_frm_queue *queue,
			      struct dcam_frame *frame)
{
	unsigned int i = 0;

	if (DCAM_ADDR_INVALID(queue) || DCAM_ADDR_INVALID(frame)) {
		pr_err("deq, invalid parm %p, %p\n", queue, frame);
		return -1;
	}

	if (queue->valid_cnt == 0) {
		DCAM_TRACE("q under flow\n");
		return -1;
	}

	memcpy(frame, &queue->frm_array[0], sizeof(struct dcam_frame));
	queue->valid_cnt--;
	for (i = 0; i < queue->valid_cnt; i++) {
		memcpy(&queue->frm_array[i], &queue->frm_array[i + 1],
		       sizeof(struct dcam_frame));
	}
	DCAM_TRACE("de queue, %d, %d\n",
		   (0xF & (frame)->fid), queue->valid_cnt);

	return 0;
}

static int dcam_path_set_next_frm(enum dcam_id idx,
				  enum dcam_path_index path_index)
{
	enum dcam_drv_rtn rtn = DCAM_RTN_SUCCESS;
	struct dcam_frame frame;
	struct dcam_frame *reserved_frame = NULL;
	struct dcam_path_desc *path = NULL;
	unsigned long yuv_reg[3] = { 0 };
	unsigned int yuv_addr[3] = {0};
	unsigned int path_max_frm_cnt;
	struct dcam_frm_queue *p_heap = NULL;
	struct dcam_buf_queue *p_buf_queue = NULL;
	unsigned int output_frame_count = 0;
	int use_reserve_frame = 0;
	struct dcam_module *module;

	if (DCAM_ADDR_INVALID(s_p_dcam_mod[idx])) {
		pr_err("zero pointer\n");
		return -EFAULT;
	}

	module = s_p_dcam_mod[idx];
	if (path_index == DCAM_PATH_IDX_0) {
		reserved_frame = &module->path_reserved_frame[DCAM_PATH0];
		path = &module->dcam_path[DCAM_PATH0];
		yuv_reg[0] = DCAM_FRM_ADDR0;
		path_max_frm_cnt = DCAM_PATH_0_FRM_CNT_MAX;
		p_heap = &path->frame_queue;
		p_buf_queue = &path->buf_queue;
		output_frame_count = path->output_frame_count;
	} else if (path_index == DCAM_PATH_IDX_1 ||
		   path_index == DCAM_PATH_IDX_2 ||
		   path_index == DCAM_PATH_IDX_3) {
		if (path_index == DCAM_PATH_IDX_1) {
			reserved_frame =
				&module->path_reserved_frame[DCAM_PATH1];
			path = &module->dcam_path[DCAM_PATH1];
			yuv_reg[0] = DCAM_FRM_ADDR1;
			yuv_reg[1] = DCAM_FRM_ADDR2;
			yuv_reg[2] = DCAM_FRM_ADDR3;
			path_max_frm_cnt = DCAM_PATH_1_FRM_CNT_MAX;
		} else if (path_index == DCAM_PATH_IDX_2) {
			reserved_frame =
				&module->path_reserved_frame[DCAM_PATH2];
			path = &module->dcam_path[DCAM_PATH2];
			if (path->jpegls_desc.is_jpegls) {
				yuv_reg[0] = DCAM_FRM_ADDR16;
				yuv_reg[1] = DCAM_FRM_ADDR17;
				yuv_reg[2] = DCAM_FRM_ADDR18;
			} else {
				yuv_reg[0] = DCAM_FRM_ADDR4;
				yuv_reg[1] = DCAM_FRM_ADDR5;
				yuv_reg[2] = DCAM_FRM_ADDR6;
			}
			path_max_frm_cnt = DCAM_PATH_2_FRM_CNT_MAX;
		} else if (path_index == DCAM_PATH_IDX_3) {
			reserved_frame =
				&module->path_reserved_frame[DCAM_PATH3];
			path = &module->dcam_path[DCAM_PATH3];
			yuv_reg[0] = DCAM_FRM_ADDR13;
			yuv_reg[1] = DCAM_FRM_ADDR14;
			yuv_reg[2] = DCAM_FRM_ADDR15;
			path_max_frm_cnt = DCAM_PATH_3_FRM_CNT_MAX;
		}
		p_heap = &path->frame_queue;
		p_buf_queue = &path->buf_queue;
		output_frame_count = path->output_frame_count;
	}

	if (dcam_buf_queue_read(p_buf_queue, &frame) == 0 &&
	    (frame.pfinfo.mfd[0] != 0)) {
		path->output_frame_count--;
	} else {
		DCAM_TRACE("DCAM%d: No freed frame path_index %d cnt %d\n",
			idx, path_index, path->output_frame_count);
		if (reserved_frame->pfinfo.mfd[0] == 0) {
			pr_info("DCAM%d: No need to cfg frame buffer", idx);
			return -1;
		}
		memcpy(&frame, reserved_frame, sizeof(struct dcam_frame));
		use_reserve_frame = 1;
	}

	DCAM_TRACE("DCAM%d: reserved %d y 0x%x mfd 0x%x path_index %d\n",
		   idx, use_reserve_frame, frame.yaddr,
		   frame.pfinfo.mfd[0], path_index);

	if (pfiommu_check_addr(&frame.pfinfo)) {
		pr_err("the frame has been broken!\n");
		return -1;
	}
	if (frame.pfinfo.dev == NULL)
		pr_info("DCAM%d next dev NULL %p\n", idx, frame.pfinfo.dev);
	if (pfiommu_get_addr(&frame.pfinfo) || 0 == frame.pfinfo.iova[0]) {
		pr_err("get frame address failed!\n");
		return -1;
	}
	yuv_addr[0] = frame.pfinfo.iova[0];
	yuv_addr[1] = frame.pfinfo.iova[1];
	yuv_addr[2] = frame.pfinfo.iova[2];

	if (use_reserve_frame)
		memcpy(reserved_frame, &frame, sizeof(struct dcam_frame));

	DCAM_REG_WR(idx, yuv_reg[0], yuv_addr[0]);
	if (path_index != DCAM_PATH_IDX_0 &&
	    path->output_format < DCAM_YUV400) {
		DCAM_REG_WR(idx, yuv_reg[1], yuv_addr[1]);
		if (path->output_format == DCAM_YUV420_3FRAME)
			DCAM_REG_WR(idx, yuv_reg[2], yuv_addr[2]);
	}

	if (!(DCAM_PATH_IDX_0 & path_index) && is_dcam_cowork()) {
		DCAM_REG_WR(DCAM_ID_1, yuv_reg[0],
			yuv_addr[0] + path->cowork_desc.trim1.w);
		if (path_index != DCAM_PATH_IDX_0 &&
		    path->output_format < DCAM_YUV400) {
			DCAM_REG_WR(DCAM_ID_1, yuv_reg[1],
				yuv_addr[1] + path->cowork_desc.trim1.w);
		}
	}

	if (dcam_frame_enqueue(p_heap, &frame) == 0)
		DCAM_TRACE("success to enq frame buf\n");
	else
		rtn = DCAM_RTN_PATH_FRAME_LOCKED;

	return -rtn;
}

static unsigned int dcam_get_path_deci_factor(unsigned int src_size,
					      unsigned int dst_size)
{
	unsigned int factor = 0;

	if (0 == src_size || 0 == dst_size)
		return factor;

	/* factor: 0 - 1/2, 1 - 1/4, 2 - 1/8, 3 - 1/16 */
	for (factor = 0; factor < DCAM_PATH_DECI_FAC_MAX; factor++) {
		if (src_size < (unsigned int) (dst_size * (1 << (factor + 1))))
			break;
	}

	return factor;
}

static void dcam_force_copy_ext(enum dcam_id idx,
				enum dcam_path_index path_index,
				unsigned int path_copy, unsigned int coef_copy)
{
	unsigned int reg_val = 0;

	if ((DCAM_PATH_IDX_0|DCAM_PATH_IDX_1|DCAM_PATH_IDX_2|DCAM_PATH_IDX_3) &
	    path_index) {
		if (path_index == DCAM_PATH_IDX_0) {
			if (path_copy)
				reg_val |= BIT_8;
		}
		if (path_index == DCAM_PATH_IDX_1) {
			if (path_copy)
				reg_val |= BIT_10;
			if (coef_copy)
				reg_val |= BIT_14;
		} else if (path_index == DCAM_PATH_IDX_2) {
			if (path_copy)
				reg_val |= BIT_12;
			if (coef_copy)
				reg_val |= BIT_16;
		} else if (path_index == DCAM_PATH_IDX_3) {
			if (path_copy)
				reg_val |= BIT_18;
			if (coef_copy)
				reg_val |= BIT_20;
		}
		sprd_dcam_glb_reg_mwr(idx, DCAM_CONTROL, reg_val, reg_val,
				      DCAM_CONTROL_REG);
	} else {
		DCAM_TRACE("invalid path index: %d\n", path_index);
	}
}

static void dcam_auto_copy_ext(enum dcam_id idx,
			       enum dcam_path_index path_index,
			       unsigned int path_copy, unsigned int coef_copy)
{
	unsigned int reg_val = 0;

	if ((DCAM_PATH_IDX_0|DCAM_PATH_IDX_1|DCAM_PATH_IDX_2|DCAM_PATH_IDX_3) &
	    path_index) {
		if (path_index == DCAM_PATH_IDX_0) {
			if (path_copy)
				reg_val |= BIT_9;
		}
		if (path_index == DCAM_PATH_IDX_1) {
			if (path_copy)
				reg_val |= BIT_11;
			if (coef_copy)
				reg_val |= BIT_15;
		} else if (path_index == DCAM_PATH_IDX_2) {
			if (path_copy)
				reg_val |= BIT_13;
			if (coef_copy)
				reg_val |= BIT_17;
		} else if (path_index == DCAM_PATH_IDX_3) {
			if (path_copy)
				reg_val |= BIT_19;
			if (coef_copy)
				reg_val |= BIT_21;
		}
		sprd_dcam_glb_reg_mwr(idx, DCAM_CONTROL, reg_val, reg_val,
				      DCAM_CONTROL_REG);
	} else {
		DCAM_TRACE("invalid path index: %d\n", path_index);
	}
}

static void dcam_force_copy(enum dcam_id idx, enum dcam_path_index path_index)
{
	unsigned int reg_val = 0;

	if (path_index >= DCAM_PATH_IDX_0 && path_index <= DCAM_PATH_IDX_ALL) {
		if (path_index == DCAM_PATH_IDX_0)
			reg_val |= BIT_8;
		else if (path_index == DCAM_PATH_IDX_1)
			reg_val |= BIT_10;
		else if (path_index == DCAM_PATH_IDX_2)
			reg_val |= BIT_12;
		else if (path_index == DCAM_PATH_IDX_3)
			reg_val |= BIT_18;

		sprd_dcam_glb_reg_mwr(idx, DCAM_CONTROL, reg_val, reg_val,
				      DCAM_CONTROL_REG);
	} else {
		DCAM_TRACE("invalid path index: %d\n", path_index);
	}
}

static void dcam_auto_copy(enum dcam_id idx, enum dcam_path_index path_index)
{
	unsigned int reg_val = 0;

	if (path_index >= DCAM_PATH_IDX_0 && path_index <= DCAM_PATH_IDX_ALL) {
		if (path_index == DCAM_PATH_IDX_0)
			reg_val |= BIT_9;
		else if (path_index == DCAM_PATH_IDX_1)
			reg_val |= BIT_11;
		else if (path_index == DCAM_PATH_IDX_2)
			reg_val |= BIT_13;
		else if (path_index == DCAM_PATH_IDX_3)
			reg_val |= BIT_19;

		sprd_dcam_glb_reg_mwr(idx, DCAM_CONTROL, reg_val, reg_val,
				      DCAM_CONTROL_REG);
	} else {
		DCAM_TRACE("invalid path index: %d\n", path_index);
	}
}

static void dcam_reg_trace(enum dcam_id idx)
{
#ifdef DCAM_DRV_DEBUG
	unsigned long addr = 0;

	pr_info("DCAM%d: Register list", idx);
	for (addr = DCAM_CFG; addr <= DCAM_PDAF_CTRL; addr += 16) {
		pr_info("0x%lx: 0x%x 0x%x 0x%x 0x%x\n",
			addr,
			DCAM_REG_RD(idx, addr),
			DCAM_REG_RD(idx, addr + 4),
			DCAM_REG_RD(idx, addr + 8),
			DCAM_REG_RD(idx, addr + 12));
	}
#endif
}

static void dcam_path_done_notice(enum dcam_id idx,
				  enum dcam_path_index path_index)
{
	struct dcam_path_desc *p_path = NULL;

	if (DCAM_ADDR_INVALID(s_p_dcam_mod[idx])) {
		pr_err("zero pointer\n");
		return;
	}

	if (path_index >= DCAM_PATH_IDX_0 && path_index <= DCAM_PATH_IDX_3) {
		if (path_index == DCAM_PATH_IDX_0)
			p_path = &s_p_dcam_mod[idx]->dcam_path[DCAM_PATH0];
		else if (path_index == DCAM_PATH_IDX_1)
			p_path = &s_p_dcam_mod[idx]->dcam_path[DCAM_PATH1];
		else if (path_index == DCAM_PATH_IDX_2)
			p_path = &s_p_dcam_mod[idx]->dcam_path[DCAM_PATH2];
		else if (path_index == DCAM_PATH_IDX_3)
			p_path = &s_p_dcam_mod[idx]->dcam_path[DCAM_PATH3];

		DCAM_TRACE("DCAM%d: path done notice %d, %d\n", idx,
			   p_path->wait_for_done, p_path->tx_done_com.done);
		if (p_path->wait_for_done) {
			complete(&p_path->tx_done_com);
			pr_info("release tx_done_com: %d\n",
				p_path->tx_done_com.done);
			p_path->wait_for_done = 0;
		}
	} else {
		pr_info("DCAM%d: wrong index 0x%x\n", idx, path_index);
		return;
	}
}

static void dcam_path_updated_notice(enum dcam_id idx,
				     enum dcam_path_index path_index)
{
	struct dcam_path_desc *p_path = NULL;

	if (DCAM_ADDR_INVALID(s_p_dcam_mod[idx])) {
		pr_err("zero pointer\n");
		return;
	}

	if (path_index >= DCAM_PATH_IDX_0 && path_index <= DCAM_PATH_IDX_3) {
		if (path_index == DCAM_PATH_IDX_0)
			p_path = &s_p_dcam_mod[idx]->dcam_path[DCAM_PATH0];
		else if (path_index == DCAM_PATH_IDX_1)
			p_path = &s_p_dcam_mod[idx]->dcam_path[DCAM_PATH1];
		else if (path_index == DCAM_PATH_IDX_2)
			p_path = &s_p_dcam_mod[idx]->dcam_path[DCAM_PATH2];
		else if (path_index == DCAM_PATH_IDX_3)
			p_path = &s_p_dcam_mod[idx]->dcam_path[DCAM_PATH3];

		DCAM_TRACE("DCAM%d: update notice %d, %d\n", idx,
			   p_path->wait_for_sof, p_path->sof_com.done);
		if (p_path->wait_for_sof) {
			complete(&p_path->sof_com);
			p_path->wait_for_sof = 0;
		}
	} else {
		pr_info("DCAM%d: wrong index 0x%x\n", idx, path_index);
		return;
	}
}

static int dcam_get_valid_sc_coeff(struct dcam_sc_array *sc,
				   struct dcam_sc_coeff **sc_coeff)
{
	if (DCAM_ADDR_INVALID(sc) || DCAM_ADDR_INVALID(sc_coeff)) {
		pr_err("DCAM: get valid sc, invalid parm %p, %p\n",
		       sc, sc_coeff);
		return -1;
	}

	if (sc->valid_cnt == 0) {
		pr_err("valid cnt 0\n");
		return -1;
	}

	*sc_coeff = sc->scaling_coeff_queue[0];
	DCAM_TRACE("get valid sc, %d\n", sc->valid_cnt);

	return 0;
}

static int dcam_push_sc_buf(struct dcam_sc_array *sc, unsigned int index)
{
	if (DCAM_ADDR_INVALID(sc)) {
		pr_err("push sc, invalid parm %p\n", sc);
		return -1;
	}

	if (sc->valid_cnt >= DCAM_SC_COEFF_BUF_COUNT) {
		pr_err("valid cnt %d\n", sc->valid_cnt);
		return -1;
	}

	sc->scaling_coeff[index].flag = 1;
	sc->scaling_coeff_queue[sc->valid_cnt] = &sc->scaling_coeff[index];
	sc->valid_cnt++;

	DCAM_TRACE("push sc, %d\n", sc->valid_cnt);

	return 0;
}

static int dcam_pop_sc_buf(struct dcam_sc_array *sc,
			   struct dcam_sc_coeff **sc_coeff)
{
	unsigned int i = 0;

	if (DCAM_ADDR_INVALID(sc) || DCAM_ADDR_INVALID(sc_coeff)) {
		pr_err("pop sc, invalid parm %p, %p\n", sc, sc_coeff);
		return -1;
	}

	if (sc->valid_cnt == 0) {
		pr_err("valid cnt 0\n");
		return -1;
	}

	sc->scaling_coeff_queue[0]->flag = 0;
	*sc_coeff = sc->scaling_coeff_queue[0];
	sc->valid_cnt--;
	for (i = 0; i < sc->valid_cnt; i++)
		sc->scaling_coeff_queue[i] = sc->scaling_coeff_queue[i + 1];

	DCAM_TRACE("pop sc, %d\n", sc->valid_cnt);

	return 0;
}

static int dcam_write_sc_coeff(enum dcam_id idx,
			       enum dcam_path_index path_index)
{
	int ret = 0;
	struct dcam_path_desc *path = NULL;
	unsigned int i = 0;
	unsigned long h_coeff_addr = 0;
	unsigned long v_coeff_addr = 0;
	unsigned long v_chroma_coeff_addr = 0;
	unsigned int *tmp_buf = NULL;
	unsigned int *h_coeff = NULL;
	unsigned int *v_coeff = NULL;
	unsigned int *v_chroma_coeff = NULL;
	unsigned int scale2yuv420 = 0;
	struct dcam_sc_coeff *sc_coeff;

	if (DCAM_ADDR_INVALID(s_p_dcam_mod[idx])) {
		pr_err("zero pointer\n");
		return -EFAULT;
	}
	if (DCAM_ADDR_INVALID(s_dcam_sc_array[idx])) {
		pr_err("zero pointer\n");
		return -EFAULT;
	}
	if (DCAM_PATH_IDX_1 != path_index && DCAM_PATH_IDX_2 != path_index)
		return -DCAM_RTN_PARA_ERR;

	ret = dcam_get_valid_sc_coeff(s_dcam_sc_array[idx], &sc_coeff);
	if (ret)
		return -DCAM_RTN_PATH_NO_MEM;

	tmp_buf = sc_coeff->buf;
	if (tmp_buf == NULL)
		return -DCAM_RTN_PATH_NO_MEM;

	h_coeff = tmp_buf;
	v_coeff = tmp_buf + (DCAM_SC_COEFF_COEF_SIZE / 4);
	v_chroma_coeff = v_coeff + (DCAM_SC_COEFF_COEF_SIZE / 4);

	if (path_index == DCAM_PATH_IDX_1) {
		path = &sc_coeff->dcam_path1;
		h_coeff_addr += DCAM_SC1_H_TAB_OFFSET;
		v_coeff_addr += DCAM_SC1_V_TAB_OFFSET;
		v_chroma_coeff_addr += DCAM_SC1_V_CHROMA_TAB_OFFSET;
	} else if (path_index == DCAM_PATH_IDX_2) {
		path = &s_p_dcam_mod[idx]->dcam_path[DCAM_PATH2];
		h_coeff_addr += DCAM_SC2_H_TAB_OFFSET;
		v_coeff_addr += DCAM_SC2_V_TAB_OFFSET;
		v_chroma_coeff_addr += DCAM_SC2_V_CHROMA_TAB_OFFSET;
	} else if (path_index == DCAM_PATH_IDX_3) {
		path = &s_p_dcam_mod[idx]->dcam_path[DCAM_PATH3];
		h_coeff_addr += DCAM_SC3_H_TAB_OFFSET;
		v_coeff_addr += DCAM_SC3_V_TAB_OFFSET;
		v_chroma_coeff_addr += DCAM_SC3_V_CHROMA_TAB_OFFSET;
	}

	if (path->output_format == DCAM_YUV420)
		scale2yuv420 = 1;

	DCAM_TRACE("write sc coeff {%d %d %d %d}, 420=%d\n",
		   path->sc_input_size.w,
		   path->sc_input_size.h,
		   path->output_size.w, path->output_size.h, scale2yuv420);

	for (i = 0; i < DCAM_SC_COEFF_H_NUM; i++) {
		DCAM_REG_WR(idx, h_coeff_addr, *h_coeff);
		h_coeff_addr += 4;
		h_coeff++;
	}

	for (i = 0; i < DCAM_SC_COEFF_V_NUM; i++) {
		DCAM_REG_WR(idx, v_coeff_addr, *v_coeff);
		v_coeff_addr += 4;
		v_coeff++;
	}

	for (i = 0; i < DCAM_SC_COEFF_V_CHROMA_NUM; i++) {
		DCAM_REG_WR(idx, v_chroma_coeff_addr, *v_chroma_coeff);
		v_chroma_coeff_addr += 4;
		v_chroma_coeff++;
	}

	return ret;
}

static int dcam_calc_sc_coeff(enum dcam_id idx,
			      enum dcam_path_index path_index)
{
	unsigned long flag;
	struct dcam_path_desc *path = NULL;
	unsigned int *tmp_buf = NULL;
	unsigned int *h_coeff = NULL;
	unsigned int *v_coeff = NULL;
	unsigned int *v_chroma_coeff = NULL;
	unsigned int scale2yuv420 = 0;
	unsigned char y_tap = 0;
	unsigned char uv_tap = 0;
	unsigned int index = 0;
	struct dcam_sc_coeff *sc_coeff;

	if (DCAM_ADDR_INVALID(s_p_dcam_mod[idx])) {
		pr_err("zero pointer\n");
		return -EFAULT;
	}
	if (DCAM_ADDR_INVALID(s_dcam_sc_array[idx])) {
		pr_err("zero pointer\n");
		return -EFAULT;
	}
	if (!((DCAM_PATH_IDX_1 | DCAM_PATH_IDX_2 | DCAM_PATH_IDX_3) &
	      path_index))
		return -DCAM_RTN_PARA_ERR;

	if (path_index == DCAM_PATH_IDX_1)
		path = &s_p_dcam_mod[idx]->dcam_path[DCAM_PATH1];
	else if (path_index == DCAM_PATH_IDX_2)
		path = &s_p_dcam_mod[idx]->dcam_path[DCAM_PATH2];
	else if (path_index == DCAM_PATH_IDX_3)
		path = &s_p_dcam_mod[idx]->dcam_path[DCAM_PATH3];

	if (path->output_format == DCAM_YUV420)
		scale2yuv420 = 1;

	DCAM_TRACE("calc sc coeff {%d %d %d %d}, 420=%d\n",
		   path->sc_input_size.w,
		   path->sc_input_size.h,
		   path->output_size.w, path->output_size.h, scale2yuv420);

	/* tang_question: why not move this sema to path? */
	down(&s_p_dcam_mod[idx]->scale_coeff_mem_sema);

	spin_lock_irqsave(&dcam_lock, flag);
	tmp_buf = dcam_get_scale_coeff_addr(idx, &index);
	if (tmp_buf == NULL) {
		dcam_pop_sc_buf(s_dcam_sc_array[idx], &sc_coeff);
		tmp_buf = dcam_get_scale_coeff_addr(idx, &index);
	}
	spin_unlock_irqrestore(&dcam_lock, flag);

	if (tmp_buf == NULL) {
		up(&s_p_dcam_mod[idx]->scale_coeff_mem_sema);
		return -DCAM_RTN_PATH_NO_MEM;
	}

	h_coeff = tmp_buf;
	v_coeff = tmp_buf + (DCAM_SC_COEFF_COEF_SIZE / 4);
	v_chroma_coeff = v_coeff + (DCAM_SC_COEFF_COEF_SIZE / 4);

	if (!(dcam_gen_scale_coeff((short)path->sc_input_size.w,
				   (short)path->sc_input_size.h,
				   (short)path->output_size.w,
				   (short)path->output_size.h,
				   h_coeff,
				   v_coeff,
				   v_chroma_coeff,
				   scale2yuv420,
				   &y_tap,
				   &uv_tap,
				   tmp_buf + (DCAM_SC_COEFF_COEF_SIZE * 3 / 4),
				   DCAM_SC_COEFF_TMP_SIZE))) {
		pr_err("failed to call dcam_gen_scale_coeff\n");
		up(&s_p_dcam_mod[idx]->scale_coeff_mem_sema);
		return -DCAM_RTN_PATH_GEN_COEFF_ERR;
	}
	path->scale_tap.y_tap = y_tap;
	path->scale_tap.uv_tap = uv_tap;
	path->valid_param.scale_tap = 1;
	memcpy(&s_dcam_sc_array[idx]->scaling_coeff[index].dcam_path1, path,
	       sizeof(struct dcam_path_desc));
	spin_lock_irqsave(&dcam_lock, flag);
	dcam_push_sc_buf(s_dcam_sc_array[idx], index);
	spin_unlock_irqrestore(&dcam_lock, flag);

	up(&s_p_dcam_mod[idx]->scale_coeff_mem_sema);

	return DCAM_RTN_SUCCESS;
}

static int dcam_calc_sc_size(struct dcam_path_desc *path)
{
	enum dcam_drv_rtn rtn = DCAM_RTN_SUCCESS;
	unsigned int tmp_dstsize = 0;
	unsigned int align_size = 0;
	struct dcam_rect *in_rect;
	struct dcam_size *out_size;
	unsigned int d_max = DCAM_SC_COEFF_DOWN_MAX;
	unsigned int u_max = DCAM_SC_COEFF_UP_MAX;
	unsigned int f_max = DCAM_PATH_DECI_FAC_MAX;

	if (DCAM_ADDR_INVALID(path)) {
		pr_err("zero pointer\n");
		return -EFAULT;
	}

	in_rect = &path->input_rect;
	out_size = &path->output_size;
	if (in_rect->w > (out_size->w * d_max * (1 << f_max)) ||
	    in_rect->h > (out_size->h * d_max * (1 << f_max)) ||
	    in_rect->w * u_max < out_size->w ||
	    in_rect->h * u_max < out_size->h) {
		rtn = DCAM_RTN_PATH_SC_ERR;
	} else {
		path->sc_input_size.w = in_rect->w;
		path->sc_input_size.h = in_rect->h;
		if (in_rect->w > out_size->w * d_max) {
			tmp_dstsize =
				out_size->w * d_max;
			path->deci_val.deci_x =
				dcam_get_path_deci_factor(in_rect->w,
							  tmp_dstsize);
			path->deci_val.deci_x_en = 1;
			path->valid_param.v_deci = 1;
			align_size =
				(1 << (path->deci_val.deci_x + 1)) *
				DCAM_PIXEL_ALIGN_WIDTH;
			in_rect->w = (in_rect->w) & ~(align_size - 1);
			in_rect->x = (in_rect->x) & ~(align_size - 1);
			path->sc_input_size.w =
				in_rect->w >> (path->deci_val.deci_x + 1);
		} else {
			path->deci_val.deci_x = 0;
			path->deci_val.deci_x_en = 0;
			path->valid_param.v_deci = 1;
		}

		if (in_rect->h > out_size->h * d_max) {
			tmp_dstsize = out_size->h * d_max;
			path->deci_val.deci_y =
				dcam_get_path_deci_factor(in_rect->h,
							  tmp_dstsize);
			path->deci_val.deci_y_en = 1;
			path->valid_param.v_deci = 1;
			align_size =
				(1 << (path->deci_val.deci_y + 1)) *
				DCAM_PIXEL_ALIGN_HEIGHT;
			in_rect->h = (in_rect->h) & ~(align_size - 1);
			in_rect->y = (in_rect->y) & ~(align_size - 1);
			path->sc_input_size.h =
				in_rect->h >> (path->deci_val.deci_y + 1);
		} else {
			path->deci_val.deci_y = 0;
			path->deci_val.deci_y_en = 0;
			path->valid_param.v_deci = 1;
		}
	}

	DCAM_TRACE("cal sc, path=%x, x_en=%d, deci_x=%d, y_en=%d, deci_y=%d\n",
		   path->id, path->deci_val.deci_x_en, path->deci_val.deci_x,
		   path->deci_val.deci_y_en, path->deci_val.deci_y);

	return -rtn;
}

static int dcam_set_sc_coeff(enum dcam_id idx,
			     enum dcam_path_index path_index)
{
	struct dcam_path_desc *path = NULL;
	unsigned int i = 0;
	unsigned long h_coeff_addr = 0;
	unsigned long v_coeff_addr = 0;
	unsigned long v_chroma_coeff_addr = 0;
	unsigned int *tmp_buf = NULL;
	unsigned int *h_coeff = NULL;
	unsigned int *v_coeff = NULL;
	unsigned int *v_chroma_coeff = NULL;
	unsigned int scale2yuv420 = 0;
	unsigned char y_tap = 0;
	unsigned char uv_tap = 0;
	unsigned int index = 0;

	if (DCAM_ADDR_INVALID(s_p_dcam_mod[idx])) {
		pr_err("zero pointer\n");
		return -EFAULT;
	}

	if (!((DCAM_PATH_IDX_1 | DCAM_PATH_IDX_2 | DCAM_PATH_IDX_3) &
	      path_index))
		return -DCAM_RTN_PARA_ERR;

	if (path_index == DCAM_PATH_IDX_1) {
		path = &s_p_dcam_mod[idx]->dcam_path[DCAM_PATH1];
		h_coeff_addr += DCAM_SC1_H_TAB_OFFSET;
		v_coeff_addr += DCAM_SC1_V_TAB_OFFSET;
		v_chroma_coeff_addr += DCAM_SC1_V_CHROMA_TAB_OFFSET;
	} else if (path_index == DCAM_PATH_IDX_2) {
		path = &s_p_dcam_mod[idx]->dcam_path[DCAM_PATH2];
		h_coeff_addr += DCAM_SC2_H_TAB_OFFSET;
		v_coeff_addr += DCAM_SC2_V_TAB_OFFSET;
		v_chroma_coeff_addr += DCAM_SC2_V_CHROMA_TAB_OFFSET;
	} else if (path_index == DCAM_PATH_IDX_3) {
		path = &s_p_dcam_mod[idx]->dcam_path[DCAM_PATH3];
		h_coeff_addr += DCAM_SC3_H_TAB_OFFSET;
		v_coeff_addr += DCAM_SC3_V_TAB_OFFSET;
		v_chroma_coeff_addr += DCAM_SC3_V_CHROMA_TAB_OFFSET;
	}

	if (path->output_format == DCAM_YUV420)
		scale2yuv420 = 1;

	DCAM_TRACE("DCAM%d: set sc coeff {%d %d %d %d}, 420=%d\n",
		   idx,
		   path->sc_input_size.w,
		   path->sc_input_size.h,
		   path->output_size.w, path->output_size.h, scale2yuv420);

	tmp_buf = dcam_get_scale_coeff_addr(idx, &index);
	if (tmp_buf == NULL)
		return -DCAM_RTN_PATH_NO_MEM;

	h_coeff = tmp_buf;
	v_coeff = tmp_buf + (DCAM_SC_COEFF_COEF_SIZE / 4);
	v_chroma_coeff = v_coeff + (DCAM_SC_COEFF_COEF_SIZE / 4);

	down(&s_p_dcam_mod[idx]->scale_coeff_mem_sema);

	if (!(dcam_gen_scale_coeff(
		(int16_t)path->sc_input_size.w,
		(int16_t)path->sc_input_size.h,
		(int16_t)path->output_size.w,
		(int16_t)path->output_size.h,
		h_coeff,
		v_coeff,
		v_chroma_coeff,
		scale2yuv420,
		&y_tap,
		&uv_tap,
		tmp_buf + (DCAM_SC_COEFF_COEF_SIZE*3/4),
		DCAM_SC_COEFF_TMP_SIZE))) {
		pr_err("DCAM: _dcam_set_sc_coeff dcam_gen_scale_coeff error!\n");
		up(&s_p_dcam_mod[idx]->scale_coeff_mem_sema);
		return -DCAM_RTN_PATH_GEN_COEFF_ERR;
	}

	for (i = 0; i < DCAM_SC_COEFF_H_NUM; i++) {
		DCAM_REG_WR(idx, h_coeff_addr, *h_coeff);
		h_coeff_addr += 4;
		h_coeff++;
	}

	for (i = 0; i < DCAM_SC_COEFF_V_NUM; i++) {
		DCAM_REG_WR(idx, v_coeff_addr, *v_coeff);
		v_coeff_addr += 4;
		v_coeff++;
	}

	for (i = 0; i < DCAM_SC_COEFF_V_CHROMA_NUM; i++) {
		DCAM_REG_WR(idx, v_chroma_coeff_addr, *v_chroma_coeff);
		v_chroma_coeff_addr += 4;
		v_chroma_coeff++;
	}

	path->scale_tap.y_tap = y_tap;
	path->scale_tap.uv_tap = uv_tap;
	path->valid_param.scale_tap = 1;

	up(&s_p_dcam_mod[idx]->scale_coeff_mem_sema);

	return DCAM_RTN_SUCCESS;
}

static int dcam_path_scaler(enum dcam_id idx,
			    enum dcam_path_index path_index)
{
	enum dcam_drv_rtn rtn = DCAM_RTN_SUCCESS;
	struct dcam_path_desc *path = NULL;
	unsigned long cfg_reg = 0;

	if (DCAM_ADDR_INVALID(s_p_dcam_mod[idx])) {
		pr_err("zero pointer\n");
		return -EFAULT;
	}
	if (DCAM_ADDR_INVALID(s_dcam_sc_array[idx])) {
		pr_err("zero pointer\n");
		return -EFAULT;
	}
	if (!((DCAM_PATH_IDX_1 | DCAM_PATH_IDX_2 | DCAM_PATH_IDX_3) &
	      path_index)) {
		pr_err("path_index error: %d\n", path_index);
		return -1;
	}

	if (path_index == DCAM_PATH_IDX_1) {
		path = &s_p_dcam_mod[idx]->dcam_path[DCAM_PATH1];
		cfg_reg = DCAM_PATH1_CFG;
	} else if (path_index == DCAM_PATH_IDX_2) {
		path = &s_p_dcam_mod[idx]->dcam_path[DCAM_PATH2];
		cfg_reg = DCAM_PATH2_CFG;
	} else if (path_index == DCAM_PATH_IDX_3) {
		path = &s_p_dcam_mod[idx]->dcam_path[DCAM_PATH3];
		cfg_reg = DCAM_PATH3_CFG;
	}

	if (DCAM_ADDR_INVALID(path)) {
		pr_err("zero pointer\n");
		return -EFAULT;
	}
	if (path->output_format == DCAM_RAWRGB ||
	    path->output_format == DCAM_JPEG) {
		DCAM_TRACE("out format is %d, no need scaler\n",
			   path->output_format);
		return DCAM_RTN_SUCCESS;
	}

	rtn = dcam_calc_sc_size(path);
	if (rtn != DCAM_RTN_SUCCESS)
		return rtn;

	if (path->sc_input_size.w == path->output_size.w &&
	    path->sc_input_size.h == path->output_size.h &&
	    path->output_format == DCAM_YUV422) {
		/* bypass scaler if the output size equals input size
		 * for YUV422 format
		 */
		DCAM_REG_MWR(idx, cfg_reg, BIT_20, 1 << 20);
	} else {
		DCAM_REG_MWR(idx, cfg_reg, BIT_20, 0 << 20);
		if (s_dcam_sc_array[idx]->is_smooth_zoom &&
		    ((DCAM_PATH_IDX_1 | DCAM_PATH_IDX_2 | DCAM_PATH_IDX_3) &
		     path_index) &&
		    path->status == DCAM_ST_START)
			rtn = dcam_calc_sc_coeff(idx, path_index);
		else
			rtn = dcam_set_sc_coeff(idx, path_index);
	}

	return rtn;
}

static void dcam_sensor_eof(enum dcam_id idx)
{
	dcam_isr_func user_func = s_user_func[idx][DCAM_SN_EOF];
	void *data = s_user_data[idx][DCAM_SN_EOF];

	if (DCAM_ADDR_INVALID(s_p_dcam_mod[idx])) {
		pr_err("zero pointer\n");
		return;
	}

	if (user_func)
		(*user_func) (NULL, data);
}

static void dcam_cap_sof(enum dcam_id idx)
{
	dcam_isr_func user_func = s_user_func[idx][DCAM_CAP_SOF];
	void *data = s_user_data[idx][DCAM_CAP_SOF];

	if (DCAM_ADDR_INVALID(s_p_dcam_mod[idx])) {
		pr_err("zero pointer\n");
		return;
	}

	if (user_func)
		(*user_func) (NULL, data);
}

static void dcam_cap_eof(enum dcam_id idx)
{
	dcam_isr_func user_func = s_user_func[idx][DCAM_CAP_EOF];
	void *data = s_user_data[idx][DCAM_CAP_EOF];

	return;
	if (DCAM_ADDR_INVALID(s_p_dcam_mod[idx])) {
		pr_err("zero pointer\n");
		return;
	}
	if (user_func)
		(*user_func) (NULL, data);
}

static void dcam_path_sof(enum dcam_id idx, enum dcam_path_id path_id)
{
	enum dcam_drv_rtn rtn = DCAM_RTN_SUCCESS;
	enum dcam_irq_id irq_id;
	dcam_isr_func user_func;
	void *data;
	struct dcam_path_desc *path = NULL;
	enum dcam_path_index path_index = 1 << path_id;

	if ((path_id != DCAM_PATH0) && is_dcam_cowork()) {
		if (idx == DCAM_ID_0)
			glb_dcam_init_flag.dcam0_sof |= path_index;
		else
			glb_dcam_init_flag.dcam1_sof |= path_index;
		if ((0 == (glb_dcam_init_flag.dcam0_sof & path_index)) ||
			(0 == (glb_dcam_init_flag.dcam1_sof & path_index)))
			return;
		glb_dcam_init_flag.dcam0_sof &= ~path_index;
		glb_dcam_init_flag.dcam1_sof &= ~path_index;

		idx = DCAM_ID_0;
	}

	if (DCAM_ADDR_INVALID(s_p_dcam_mod[idx]))
		return;

	if (path_id == DCAM_PATH0)
		irq_id = DCAM_PATH0_SOF;
	else if (path_id == DCAM_PATH1)
		irq_id = DCAM_PATH1_SOF;
	else if (path_id == DCAM_PATH2)
		irq_id = DCAM_PATH2_SOF;
	else if (path_id == DCAM_PATH3)
		irq_id = DCAM_PATH3_SOF;
	else
		return;

	user_func = s_user_func[idx][irq_id];
	data = s_user_data[idx][irq_id];

	if (s_p_dcam_mod[idx]->dcam_path[path_id].status == DCAM_ST_START) {
		path = &s_p_dcam_mod[idx]->dcam_path[path_id];
		if (path->valid == 0) {
			pr_info("DCAM%d: path%d not valid\n", idx, path_id);
			return;
		}

		if (path->sof_cnt == 1 + path->frame_deci)
			path->sof_cnt = 0;

		path->sof_cnt++;
		if (path->sof_cnt == 1) {
			DCAM_TRACE("DCAM%d: path%d sof %d frame_deci %d\n",
				idx, path_id, path->sof_cnt, path->frame_deci);
		} else {
			DCAM_TRACE("DCAM%d: path%d invalid sof, cnt %d\n",
				idx, path_id, path->sof_cnt);
			return;
		}

		if (path->is_update) {
			if (s_dcam_sc_array[idx]->is_smooth_zoom) {
				struct dcam_sc_coeff *sc_coeff;

				dcam_get_valid_sc_coeff(s_dcam_sc_array[idx],
							&sc_coeff);
				dcam_write_sc_coeff(idx, path_index);
				dcam_path_set(idx, path_index);
				dcam_pop_sc_buf(s_dcam_sc_array[idx],
						&sc_coeff);
			} else {
				if ((path_id != DCAM_PATH0) &&
				    is_dcam_cowork()) {
					dcam_cowork_para_cfg(path_index);
					dcam_path_set(DCAM_ID_0,
						path_index);
					dcam_path_set(DCAM_ID_1,
						path_index);
				} else
					dcam_path_set(idx, path_index);
			}
			path->is_update = 0;
			DCAM_TRACE("DCAM%d: path%d updated\n", idx, path_id);
			rtn = dcam_path_set_next_frm(idx, path_index);
			if (!rtn) {
				if ((path_id != DCAM_PATH0) &&
				    is_dcam_cowork()) {
					dcam_auto_copy_ext(DCAM_ID_0,
							path_index,
							true,
							true);
					dcam_auto_copy_ext(DCAM_ID_1,
							path_index,
							true,
							true);
				} else
					dcam_auto_copy_ext(idx,
							path_index,
							true,
							true);
			}
		} else {
			rtn = dcam_path_set_next_frm(idx, path_index);
			if (!rtn) {
				if ((path_id != DCAM_PATH0) &&
				    is_dcam_cowork()) {
					dcam_auto_copy(DCAM_ID_0, path_index);
					dcam_auto_copy(DCAM_ID_1, path_index);
				} else
					dcam_auto_copy(idx, path_index);
			}
		}

		dcam_path_updated_notice(idx, path_index);

		if (rtn) {
			path->need_wait = 1;
			pr_info("DCAM%d: path%d w\n", idx, path_id);
			return;
		}
		if (user_func)
			(*user_func) (NULL, data);
	}
}

static void dcam_path_done(enum dcam_id idx, enum dcam_path_id path_id)
{
	enum dcam_drv_rtn rtn = DCAM_RTN_SUCCESS;
	enum dcam_irq_id irq_id;
	dcam_isr_func user_func;
	void *data;
	struct dcam_path_desc *path;
	enum dcam_path_index path_index = 1 << path_id;
	struct dcam_module *module;

	if (DCAM_ADDR_INVALID(s_p_dcam_mod[idx])) {
		pr_err("zero pointer\n");
		return;
	}

	if ((path_id != DCAM_PATH0) && is_dcam_cowork()) {
		if (idx == DCAM_ID_0)
			glb_dcam_init_flag.dcam0_eof |= path_index;
		else
			glb_dcam_init_flag.dcam1_eof |= path_index;
		if ((0 == (glb_dcam_init_flag.dcam0_eof & path_index)) ||
			(0 == (glb_dcam_init_flag.dcam1_eof & path_index)))
			return;
		glb_dcam_init_flag.dcam0_eof &= ~path_index;
		glb_dcam_init_flag.dcam1_eof &= ~path_index;

		idx = DCAM_ID_0;
	}
	module = s_p_dcam_mod[idx];
	path = &module->dcam_path[path_id];
	if (path->valid == 0) {
		pr_info("DCAM%d: path%d not valid\n", idx, path_id);
		return;
	}

	if (path->frame_deci > 1) {
		if (path->done_cnt == 0) {
			path->done_cnt = 1;
		} else {
			path->done_cnt = 0;
			DCAM_TRACE("DCAM%d: path%d dummy done, drop\n",
					idx, path_id);
			return;
		}
	} else {
		path->done_cnt = 1;
	}
	if (path_id == DCAM_PATH0)
		irq_id = DCAM_PATH0_DONE;
	else if (path_id == DCAM_PATH1)
		irq_id = DCAM_PATH1_DONE;
	else if (path_id == DCAM_PATH2)
		irq_id = DCAM_PATH2_DONE;
	else if (path_id == DCAM_PATH3)
		irq_id = DCAM_PATH3_DONE;
	else
		return;

	user_func = s_user_func[idx][irq_id];
	data = s_user_data[idx][irq_id];
	DCAM_TRACE("dcam%d %d %p\n", idx, path_id, data);

	if (path->need_stop) {
		if ((path_id != DCAM_PATH0) && is_dcam_cowork()) {
			sprd_dcam_glb_reg_awr(DCAM_ID_0,
				DCAM_CFG,
				~(1 << path_id),
				DCAM_CFG_REG);
			sprd_dcam_glb_reg_awr(DCAM_ID_1,
				DCAM_CFG,
				~(1<<path_id),
				DCAM_CFG_REG);
		} else {
			sprd_dcam_glb_reg_awr(idx, DCAM_CFG, ~(1 << path_id),
					      DCAM_CFG_REG);
		}
		path->need_stop = 0;
	}
	dcam_path_done_notice(idx, path_index);

	if (path_id != DCAM_PATH0) {
		if (path->done_cnt) {
			if (path->sof_cnt == 1) {
				DCAM_TRACE("DCAM%d: path%d valid done\n",
						idx, path_id);
				if (path->frame_deci == 0)
					path->sof_cnt = 0;
			} else {
				path->need_wait = 0;
				DCAM_TRACE("DCAM%d: path%d need wait\n",
						idx, path_id);
				return;
			}
		}
	}

	if (path->need_wait) {
		path->need_wait = 0;
	} else {
		struct dcam_frame frame;

		rtn = dcam_frame_dequeue(&path->frame_queue, &frame);
		if (rtn)
			return;
		if (pfiommu_check_addr(&frame.pfinfo)) {
			pr_err("the frame has been broken!\n");
			return;
		}
		if (frame.pfinfo.dev == NULL)
			pr_info("DCAM%d done dev NULL %p\n",
				idx, frame.pfinfo.dev);

		pfiommu_free_addr(&frame.pfinfo);

		if (frame.pfinfo.mfd[0] !=
		    module->path_reserved_frame[path_id].pfinfo.mfd[0]) {
			frame.width = path->output_size.w;
			frame.height = path->output_size.h;

			DCAM_TRACE("DCAM%d: path%d frame %p\n",
				   idx, path_id, &frame);
			DCAM_TRACE("y uv, 0x%x 0x%x, mfd = 0x%x,0x%x\n",
				   frame.yaddr, frame.uaddr,
				   frame.pfinfo.mfd[0], frame.pfinfo.mfd[1]);
			if (user_func)
				(*user_func) (&frame, data);
		} else {
			DCAM_TRACE("DCAM%d: use reserved [%d]\n", idx, path_id);
			module->path_reserved_frame[path_id].pfinfo.iova[0] = 0;
			module->path_reserved_frame[path_id].pfinfo.iova[1] = 0;
		}
	}
}

static void dcam_path_ov(enum dcam_id idx, enum dcam_path_id path_id)
{
	enum dcam_irq_id irq_id;
	dcam_isr_func user_func;
	void *data;
	struct dcam_path_desc *path;
	struct dcam_frame frame;

	if (DCAM_ADDR_INVALID(s_p_dcam_mod[idx])) {
		pr_err("zero pointer\n");
		return;
	}

	pr_info("DCAM%d: path%d overflow\n", idx, path_id);

	if (path_id == DCAM_PATH0)
		irq_id = DCAM_PATH0_OV;
	else if (path_id == DCAM_PATH1)
		irq_id = DCAM_PATH1_OV;
	else if (path_id == DCAM_PATH2)
		irq_id = DCAM_PATH2_OV;
	else if (path_id == DCAM_PATH3)
		irq_id = DCAM_PATH3_OV;
	else
		return;

	user_func = s_user_func[idx][irq_id];
	data = s_user_data[idx][irq_id];

	path = &s_p_dcam_mod[idx]->dcam_path[path_id];
	dcam_frame_dequeue(&path->frame_queue, &frame);

	if (user_func)
		(*user_func) (&frame, data);
}

static void dcam_path0_done(enum dcam_id idx)
{
	dcam_path_done(idx, DCAM_PATH0);
}

static void dcam_path0_overflow(enum dcam_id idx)
{
	dcam_path_ov(idx, DCAM_PATH0);
}

static void dcam_path1_done(enum dcam_id idx)
{
	dcam_path_done(idx, DCAM_PATH1);
}

static void dcam_path1_overflow(enum dcam_id idx)
{
	dcam_path_ov(idx, DCAM_PATH1);
}

static void dcam_jpegls_done(enum dcam_id idx)
{
	struct dcam_frame frame = { 0 };
	struct dcam_path_desc *path = NULL;
	void *data = s_user_data[idx][DCAM_JPEGLS_DONE];
	enum dcam_drv_rtn rtn = DCAM_RTN_SUCCESS;
	dcam_isr_func user_func = s_user_func[idx][DCAM_JPEGLS_DONE];

	if (DCAM_ADDR_INVALID(s_p_dcam_mod[idx])) {
		pr_err("zero pointer\n");
		return;
	}

	path = &s_p_dcam_mod[idx]->dcam_path[DCAM_PATH2];
	if (path->valid == 0) {
		pr_info("DCAM%d: path2 not valid\n", idx);
		return;
	}

	DCAM_TRACE("DCAM%d: 2\n", idx);

	if (path->need_stop) {
		sprd_dcam_glb_reg_awr(idx, DCAM_CFG, ~BIT_2, DCAM_CFG_REG);
		path->need_stop = 0;
	}
	dcam_path_done_notice(idx, DCAM_PATH_IDX_2);

	if (path->sof_cnt < 1) {
		pr_info("DCAM%d: jpegls done cnt %d\n", idx, path->sof_cnt);
		path->need_wait = 0;
		return;
	}
	path->sof_cnt = 0;

	if (path->need_wait) {
		path->need_wait = 0;
	} else {
		rtn = dcam_frame_dequeue(&path->frame_queue, &frame);
		if (rtn == 0
		    && frame.yaddr !=
		    s_p_dcam_mod[idx]->path_reserved_frame[DCAM_PATH2].yaddr) {
			frame.width = path->output_size.w;
			frame.height = path->output_size.h;
			DCAM_TRACE("DCAM%d: jpegls frame %p, y uv, 0x%x 0x%x\n",
				   idx, &frame, frame.yaddr, frame.uaddr);
			if (user_func)
				(*user_func) (&frame, data);
		} else {
			DCAM_TRACE("DCAM%d: path2_reserved_frame\n", idx);
		}
	}
}

static void dcam_jpegls_ov(enum dcam_id idx)
{
	struct dcam_frame frame = { 0 };
	struct dcam_path_desc *path = NULL;
	void *data = s_user_data[idx][DCAM_JPEGLS_OV];
	dcam_isr_func user_func = s_user_func[idx][DCAM_JPEGLS_OV];

	if (DCAM_ADDR_INVALID(s_p_dcam_mod[idx])) {
		pr_err("zero pointer\n");
		return;
	}

	pr_info("DCAM%d: jpegls overflow\n", idx);
	path = &s_p_dcam_mod[idx]->dcam_path[DCAM_PATH2];
	dcam_frame_dequeue(&path->frame_queue, &frame);

	if (user_func)
		(*user_func) (&frame, data);
}

static void dcam_sensor_line_err(enum dcam_id idx)
{
	dcam_isr_func user_func = s_user_func[idx][DCAM_SN_LINE_ERR];
	void *data = s_user_data[idx][DCAM_SN_LINE_ERR];

	if (DCAM_ADDR_INVALID(s_p_dcam_mod[idx])) {
		pr_err("zero pointer\n");
		return;
	}

	pr_info("DCAM%d: line err\n", idx);

	if (user_func)
		(*user_func) (NULL, data);
}

static void dcam_sensor_frame_err(enum dcam_id idx)
{
	dcam_isr_func user_func = s_user_func[idx][DCAM_SN_FRAME_ERR];
	void *data = s_user_data[idx][DCAM_SN_FRAME_ERR];

	if (DCAM_ADDR_INVALID(s_p_dcam_mod[idx])) {
		pr_err("zero pointer\n");
		return;
	}

	pr_info("DCAM%d: frame err\n", idx);

	if (user_func)
		(*user_func) (NULL, data);
}

static void dcam_jpeg_buf_ov(enum dcam_id idx)
{
	dcam_isr_func user_func = s_user_func[idx][DCAM_JPEG_BUF_OV];
	void *data = s_user_data[idx][DCAM_JPEG_BUF_OV];
	struct dcam_path_desc *path;
	struct dcam_frame frame;

	if (DCAM_ADDR_INVALID(s_p_dcam_mod[idx])) {
		pr_err("zero pointer\n");
		return;
	}

	pr_info("DCAM%d: jpeg overflow\n", idx);
	path = &s_p_dcam_mod[idx]->dcam_path[DCAM_PATH0];
	dcam_frame_dequeue(&path->frame_queue, &frame);

	if (user_func)
		(*user_func) (&frame, data);
}

static void dcam_path2_done(enum dcam_id idx)
{
	dcam_path_done(idx, DCAM_PATH2);
}

static void dcam_path2_ov(enum dcam_id idx)
{
	dcam_path_ov(idx, DCAM_PATH2);
}

static void dcam_isp_ov(enum dcam_id idx)
{
	dcam_isr_func user_func = s_user_func[idx][DCAM_ISP_OV];
	void *data = s_user_data[idx][DCAM_ISP_OV];

	if (DCAM_ADDR_INVALID(s_p_dcam_mod[idx])) {
		pr_err("zero pointer\n");
		return;
	}

	pr_info("DCAM%d: isp overflow\n", idx);

	if (user_func)
		(*user_func) (NULL, data);
}

static void dcam_mipi_ov(enum dcam_id idx)
{
	dcam_isr_func user_func = s_user_func[idx][DCAM_MIPI_OV];
	void *data = s_user_data[idx][DCAM_MIPI_OV];

	if (DCAM_ADDR_INVALID(s_p_dcam_mod[idx])) {
		pr_err("zero pointer\n");
		return;
	}

	pr_info("DCAM%d: mipi overflow\n", idx);

	if (user_func)
		(*user_func) (NULL, data);
}

static void dcam_raw_slice_done(enum dcam_id idx)
{
	dcam_isr_func user_func = s_user_func[idx][DCAM_RAW_SLICE_DONE];
	void *data = s_user_data[idx][DCAM_RAW_SLICE_DONE];

	DCAM_TRACE("DCAM%d: 0 slice done\n", idx);

	if (user_func)
		(*user_func) (NULL, data);
}

static void dcam_path0_sof(enum dcam_id idx)
{
	dcam_path_sof(idx, DCAM_PATH0);
}

static void dcam_path1_sof(enum dcam_id idx)
{
	dcam_path_sof(idx, DCAM_PATH1);
}

static void dcam_path2_sof(enum dcam_id idx)
{
	dcam_path_sof(idx, DCAM_PATH2);
}

static void dcam_path3_sof(enum dcam_id idx)
{
	dcam_path_sof(idx, DCAM_PATH3);
}

static void dcam_path3_done(enum dcam_id idx)
{
	dcam_path_done(idx, DCAM_PATH3);
}

static void dcam_path3_ov(enum dcam_id idx)
{
	dcam_path_ov(idx, DCAM_PATH3);
}

static void dcam_stopped(enum dcam_id idx)
{
	int i = 0;

	if (DCAM_ADDR_INVALID(s_p_dcam_mod[idx])) {
		pr_err("zero pointer\n");
		return;
	}

	for (i = 0; i < DCAM_PATH_MAX; i++)
		dcam_path_done_notice(idx, 1 << i);
}

static void dcam_wait_for_channel_stop(enum dcam_id idx,
				       enum dcam_path_index path_index)
{
	unsigned int ret = 0;
	int time_out = 5000;
	struct dcam_path_desc *path = NULL;
	unsigned int mask;

	if (DCAM_ADDR_INVALID(s_p_dcam_mod[idx])) {
		pr_err("zero pointer\n");
		return;
	}

	if (DCAM_PATH_IDX_1 & path_index) {
		path = &s_p_dcam_mod[idx]->dcam_path[DCAM_PATH1];
		mask = BIT_17;
	} else if (DCAM_PATH_IDX_2 & path_index) {
		path = &s_p_dcam_mod[idx]->dcam_path[DCAM_PATH2];
		mask = BIT_18;
	} else if (DCAM_PATH_IDX_0 & path_index) {
		path = &s_p_dcam_mod[idx]->dcam_path[DCAM_PATH0];
		mask = BIT_19;
	} else if (DCAM_PATH_IDX_3 & path_index) {
		path = &s_p_dcam_mod[idx]->dcam_path[DCAM_PATH3];
		mask = BIT_21;
	} else {
		pr_err("path index 0x%0x, error!\n", path_index);
		return;
	}

	/* wait for AHB path busy cleared */
	while (path->valid && time_out) {
		ret = DCAM_REG_RD(idx, DCAM_AHBM_STS) & mask;
		if (!ret)
			break;

		time_out--;
	}

	DCAM_TRACE("DCAM%d:wait channel 0x%0x stop %d %d\n", idx, path_index,
		   ret, time_out);
}

static void dcam_quickstop_set(enum dcam_id idx,
			       enum dcam_path_index path_index,
			       unsigned int cfg_bit, unsigned int ahbm_bit)
{
	sprd_dcam_glb_reg_owr(idx, DCAM_AHBM_STS, ahbm_bit, DCAM_AHBM_STS_REG);
	udelay(1000);
	sprd_dcam_glb_reg_mwr(idx, DCAM_CFG, cfg_bit, ~cfg_bit, DCAM_CFG_REG);
	dcam_wait_for_channel_stop(idx, path_index);
	dcam_force_copy(idx, path_index);
	sprd_dcam_reset(idx, path_index, 0);
	sprd_dcam_glb_reg_awr(idx, DCAM_AHBM_STS, ~ahbm_bit, DCAM_AHBM_STS_REG);
}

static void dcam_quickstop_set_all(enum dcam_id idx)
{
	sprd_dcam_glb_reg_awr(idx, DCAM_CONTROL, ~(BIT_2), DCAM_CONTROL_REG);
	sprd_dcam_glb_reg_owr(idx, DCAM_AHBM_STS,
			      BIT_3 | BIT_4 | BIT_5 | BIT_20,
			      DCAM_AHBM_STS_REG);
	udelay(10);
	sprd_dcam_glb_reg_awr(idx, DCAM_CFG,
			      ~(BIT_0 | BIT_1 | BIT_2 | BIT_3),
			      DCAM_CFG_REG);
	sprd_dcam_glb_reg_owr(idx, DCAM_CONTROL,
			      (BIT_8 | BIT_10 | BIT_12 | BIT_18),
			      DCAM_CONTROL_REG);
	sprd_dcam_glb_reg_awr(idx, DCAM_CONTROL,
			      ~(BIT_8 | BIT_10 | BIT_12 | BIT_18),
			      DCAM_CONTROL_REG);
	dcam_wait_for_channel_stop(idx, DCAM_PATH_IDX_0);
	dcam_wait_for_channel_stop(idx, DCAM_PATH_IDX_1);
	dcam_wait_for_channel_stop(idx, DCAM_PATH_IDX_2);
	dcam_wait_for_channel_stop(idx, DCAM_PATH_IDX_3);
	sprd_dcam_glb_reg_awr(idx, DCAM_AHBM_STS,
			      ~(BIT_3 | BIT_4 | BIT_5 | BIT_20),
			      DCAM_AHBM_STS_REG);
}

static void dcam_wait_for_quickstop(enum dcam_id idx,
				    enum dcam_path_index path_index)
{
	if (DCAM_ADDR_INVALID(s_p_dcam_mod[idx])) {
		pr_err("zero pointer\n");
		return;
	}

	DCAM_TRACE("DCAM%d: state before stop 0x%x\n", idx,
		   s_p_dcam_mod[idx]->state);

	if (path_index == DCAM_PATH_IDX_ALL) {
		dcam_quickstop_set_all(idx);
	} else {
		if (s_p_dcam_mod[idx]->dcam_path[DCAM_PATH0].valid &&
		    (DCAM_PATH_IDX_0 & path_index)) {
			dcam_quickstop_set(idx, DCAM_PATH_IDX_0, BIT_0, BIT_3);
		}
		if (s_p_dcam_mod[idx]->dcam_path[DCAM_PATH1].valid &&
		    (DCAM_PATH_IDX_1 & path_index)) {
			dcam_quickstop_set(idx, DCAM_PATH_IDX_1, BIT_1, BIT_4);
		}
		if (s_p_dcam_mod[idx]->dcam_path[DCAM_PATH2].valid &&
		    (DCAM_PATH_IDX_2 & path_index)) {
			dcam_quickstop_set(idx, DCAM_PATH_IDX_2, BIT_2, BIT_5);
		}
		if (s_p_dcam_mod[idx]->dcam_path[DCAM_PATH3].valid &&
		    (DCAM_PATH_IDX_3 & path_index)) {
			dcam_quickstop_set(idx, DCAM_PATH_IDX_3, BIT_3,
					   BIT_20);
		}
	}

	DCAM_TRACE("DCAM%d: exit, state: 0x%x\n", idx,
		   s_p_dcam_mod[idx]->state);
}

static int dcam_err_pre_proc(enum dcam_id idx, unsigned int irq_status)
{
	if (s_p_dcam_mod[idx]) {
		DCAM_TRACE("DCAM%d: state in err_pre_proc 0x%x\n", idx,
			   s_p_dcam_mod[idx]->state);
		if (s_p_dcam_mod[idx]->state & DCAM_STATE_QUICKQUIT)
			return -1;

		s_p_dcam_mod[idx]->err_happened = 1;
	}
	pr_info("DCAM%d: err, 0x%x\n", idx, irq_status);

	csi_dump_reg();
	isp_print_reg();
	sprd_dcam2isp_dump_reg();
	sprd_isp2dcam_dump_reg();
	dcam_reg_trace(idx);
	sprd_dcam_glb_reg_mwr(idx, DCAM_CONTROL, BIT_2, 0, DCAM_CONTROL_REG);
	dcam_stopped(idx);

	sprd_dcam_reset(idx, DCAM_PATH_IDX_ALL, 1);

	return 0;
}

static dcam_isr isr_list[DCAM_MAX_COUNT][DCAM_IRQ_NUMBER] = {
	{
		NULL,
		dcam_sensor_eof,
		dcam_cap_sof,
		dcam_cap_eof,
		dcam_path0_done,
		dcam_path0_overflow,
		dcam_path1_done,
		dcam_path1_overflow,
		dcam_path2_done,
		dcam_path2_ov,
		dcam_sensor_line_err,
		dcam_sensor_frame_err,
		dcam_jpeg_buf_ov,
		dcam_isp_ov,
		dcam_mipi_ov,
		NULL,
		NULL,
		NULL,
		dcam_raw_slice_done,
		dcam_path1_sof,
		dcam_path2_sof,
		NULL,
		NULL,
		NULL,
		dcam_path0_sof,
		dcam_path3_sof,
		NULL,
		dcam_path3_done,
		dcam_path3_ov,
		NULL,
		dcam_jpegls_done,
		dcam_jpegls_ov,
	},
};

static irqreturn_t dcam_isr_root(int irq, void *priv)
{
	unsigned int irq_line, status;
	unsigned long flag;
	int i;
	enum dcam_id idx = DCAM_ID_0;
	int irq_numbers = ARRAY_SIZE(s_irq_vect);
	unsigned int vect = 0;

	if (s_dcam_irq[DCAM_ID_0] == irq)
		idx = DCAM_ID_0;
	else if (s_dcam_irq[DCAM_ID_1] == irq)
		idx = DCAM_ID_1;
	else
		return IRQ_NONE;

	status = DCAM_REG_RD(idx, DCAM_INT_STS) & DCAM_IRQ_LINE_MASK;
	if (unlikely(status == 0))
		return IRQ_NONE;
	DCAM_REG_WR(idx, DCAM_INT_CLR, status);

	irq_line = status;
	if (unlikely(DCAM_IRQ_ERR_MASK & status)) {
		if (dcam_err_pre_proc(idx, status))
			return IRQ_HANDLED;
	}

	spin_lock_irqsave(&dcam_lock, flag);

	for (i = 0; i < irq_numbers; i++) {
		vect = s_irq_vect[i];
		if (irq_line & (1 << (unsigned int)vect)) {
			if (isr_list[idx][vect])
				isr_list[idx][vect](idx);
		}
		irq_line &= ~(unsigned int)(1 << (unsigned int)vect);
		if (!irq_line)
			break;
	}

	spin_unlock_irqrestore(&dcam_lock, flag);

	return IRQ_HANDLED;
}

static int dcam_internal_init(enum dcam_id idx)
{
	int ret = 0;
	int i = 0;

	s_p_dcam_mod[idx] = vzalloc(sizeof(struct dcam_module));
	if (DCAM_ADDR_INVALID(s_p_dcam_mod[idx])) {
		pr_err("zero pointer\n");
		return -EFAULT;
	}
	pr_info("alloc s_p_dcam_mod:%d, 0x%p\n", idx, s_p_dcam_mod[idx]);

	for (i = 0; i < DCAM_PATH_MAX; i++) {
		s_p_dcam_mod[idx]->dcam_path[i].id = i;
		init_completion(&s_p_dcam_mod[idx]->dcam_path[i].tx_done_com);
		init_completion(&s_p_dcam_mod[idx]->dcam_path[i].sof_com);
		dcam_buf_queue_init(&s_p_dcam_mod[idx]->dcam_path[i].buf_queue);
	}

	sema_init(&s_p_dcam_mod[idx]->scale_coeff_mem_sema, 1);

	return ret;
}

static void dcam_internal_deinit(enum dcam_id idx)
{
	unsigned long flag;

	spin_lock_irqsave(&dcam_lock, flag);
	if (DCAM_ADDR_INVALID(s_p_dcam_mod[idx])) {
		pr_info("DCAM%d: invalid addr, %p", idx, s_p_dcam_mod[idx]);
	} else {
		pr_info("free s_p_dcam_mod:%d, 0x%p\n", idx, s_p_dcam_mod[idx]);
		vfree(s_p_dcam_mod[idx]);
		s_p_dcam_mod[idx] = NULL;
	}
	spin_unlock_irqrestore(&dcam_lock, flag);
}

static void dcam_wait_path_done(enum dcam_id idx,
				enum dcam_path_index path_index,
				unsigned int *p_flag)
{
	int ret = 0;
	struct dcam_path_desc *p_path = NULL;
	unsigned long flag;

	if (DCAM_ADDR_INVALID(s_p_dcam_mod[idx])) {
		pr_err("zero pointer\n");
		return;
	}

	if (s_p_dcam_mod[idx]->err_happened)
		return;

	if (s_p_dcam_mod[idx]->dcam_cap.cap_mode == DCAM_CAPTURE_MODE_SINGLE)
		return;

	if (path_index >= DCAM_PATH_IDX_0 && path_index <= DCAM_PATH_IDX_3) {
		if (path_index == DCAM_PATH_IDX_0)
			p_path = &s_p_dcam_mod[idx]->dcam_path[DCAM_PATH0];
		else if (path_index == DCAM_PATH_IDX_1)
			p_path = &s_p_dcam_mod[idx]->dcam_path[DCAM_PATH1];
		else if (path_index == DCAM_PATH_IDX_2)
			p_path = &s_p_dcam_mod[idx]->dcam_path[DCAM_PATH2];
		else if (path_index == DCAM_PATH_IDX_3)
			p_path = &s_p_dcam_mod[idx]->dcam_path[DCAM_PATH3];

		DCAM_TRACE("path done wait %d, %d\n",
			   p_path->wait_for_done, p_path->tx_done_com.done);

		spin_lock_irqsave(&dcam_lock, flag);
		if (p_flag)
			*p_flag = 1;

		p_path->wait_for_done = 1;
		spin_unlock_irqrestore(&dcam_lock, flag);
		pr_info("begin to wait tx_done_com: %d\n",
			p_path->tx_done_com.done);
		ret = wait_for_completion_timeout(&p_path->tx_done_com,
						  DCAM_PATH_TIMEOUT);

		if (ret == 0) {
			dcam_reg_trace(idx);
			pr_err("DCAM%d: failed to wait path 0x%x done\n", idx,
			       path_index);
		}
	} else {
		pr_info("DCAM%d: wrong index 0x%x\n", idx, path_index);
		return;
	}
}

static void dcam_wait_update_done(enum dcam_id idx,
				  enum dcam_path_index path_index,
				  unsigned int *p_flag)
{
	int ret = 0;
	struct dcam_path_desc *p_path = NULL;
	unsigned long flag;

	if (DCAM_ADDR_INVALID(s_p_dcam_mod[idx])) {
		pr_err("zero pointer\n");
		return;
	}

	if (s_p_dcam_mod[idx]->err_happened)
		return;

	if (s_p_dcam_mod[idx]->dcam_cap.cap_mode == DCAM_CAPTURE_MODE_SINGLE)
		return;

	if (path_index >= DCAM_PATH_IDX_0 && path_index <= DCAM_PATH_IDX_3) {
		if (path_index == DCAM_PATH_IDX_0)
			p_path = &s_p_dcam_mod[idx]->dcam_path[DCAM_PATH0];
		else if (path_index == DCAM_PATH_IDX_1)
			p_path = &s_p_dcam_mod[idx]->dcam_path[DCAM_PATH1];
		else if (path_index == DCAM_PATH_IDX_2)
			p_path = &s_p_dcam_mod[idx]->dcam_path[DCAM_PATH2];
		else if (path_index == DCAM_PATH_IDX_3)
			p_path = &s_p_dcam_mod[idx]->dcam_path[DCAM_PATH3];

		DCAM_TRACE("DCAM%d: update done wait %d, %d\n", idx,
			   p_path->wait_for_sof, p_path->sof_com.done);

		spin_lock_irqsave(&dcam_lock, flag);
		if (p_flag)
			*p_flag = 1;

		p_path->wait_for_sof = 1;
		spin_unlock_irqrestore(&dcam_lock, flag);
		ret = wait_for_completion_timeout(&p_path->sof_com,
						  DCAM_PATH_TIMEOUT);
		if (ret == 0) {
			dcam_reg_trace(idx);
			pr_err("DCAM%d: failed to wait update path 0x%x done\n",
			       idx, path_index);
		}
	} else {
		pr_info("DCAM%d: wrong index 0x%x\n", idx, path_index);
	}
}

static void dcam_frm_clear(enum dcam_id idx, enum dcam_path_index path_index)
{
	struct dcam_frame frame, *res_frame;
	struct dcam_path_desc *path;

	if (DCAM_ADDR_INVALID(s_p_dcam_mod[idx])) {
		pr_err("zero pointer\n");
		return;
	}

	if (DCAM_PATH_IDX_0 & path_index) {
		path = &s_p_dcam_mod[idx]->dcam_path[DCAM_PATH0];
		while (!dcam_frame_dequeue(&path->frame_queue, &frame))
			pfiommu_free_addr(&frame.pfinfo);

		dcam_frm_queue_clear(&path->frame_queue);
		dcam_buf_queue_init(&path->buf_queue);
		res_frame = &s_p_dcam_mod[idx]->path_reserved_frame[DCAM_PATH0];
		pfiommu_free_addr(&res_frame->pfinfo);
		memset((void *)res_frame, 0, sizeof(struct dcam_frame));
	}

	if (DCAM_PATH_IDX_1 & path_index) {
		path = &s_p_dcam_mod[idx]->dcam_path[DCAM_PATH1];
		while (!dcam_frame_dequeue(&path->frame_queue, &frame))
			pfiommu_free_addr(&frame.pfinfo);

		dcam_frm_queue_clear(&path->frame_queue);
		dcam_buf_queue_init(&path->buf_queue);
		res_frame = &s_p_dcam_mod[idx]->path_reserved_frame[DCAM_PATH1];
		if (frame.pfinfo.mfd[0] != res_frame->pfinfo.mfd[0])
			pfiommu_free_addr(&res_frame->pfinfo);
		memset((void *)res_frame, 0, sizeof(struct dcam_frame));

	}

	if (DCAM_PATH_IDX_2 & path_index) {
		path = &s_p_dcam_mod[idx]->dcam_path[DCAM_PATH2];
		while (!dcam_frame_dequeue(&path->frame_queue, &frame))
			pfiommu_free_addr(&frame.pfinfo);

		dcam_frm_queue_clear(&path->frame_queue);
		dcam_buf_queue_init(&path->buf_queue);
		res_frame = &s_p_dcam_mod[idx]->path_reserved_frame[DCAM_PATH2];
		pfiommu_free_addr(&res_frame->pfinfo);
		memset((void *)res_frame, 0, sizeof(struct dcam_frame));
	}

	if (DCAM_PATH_IDX_3 & path_index) {
		path = &s_p_dcam_mod[idx]->dcam_path[DCAM_PATH3];
		while (!dcam_frame_dequeue(&path->frame_queue, &frame))
			pfiommu_free_addr(&frame.pfinfo);

		dcam_frm_queue_clear(&path->frame_queue);
		dcam_buf_queue_init(&path->buf_queue);
		res_frame = &s_p_dcam_mod[idx]->path_reserved_frame[DCAM_PATH3];
		pfiommu_free_addr(&res_frame->pfinfo);
		memset((void *)res_frame, 0, sizeof(struct dcam_frame));
	}
}

static int dcam_stop_path(enum dcam_id idx, enum dcam_path_index path_index)
{
	enum dcam_drv_rtn rtn = DCAM_RTN_SUCCESS;
	struct dcam_path_desc *p_path = NULL;
	unsigned long flag;

	spin_lock_irqsave(&dcam_lock, flag);

	if (path_index >= DCAM_PATH_IDX_0 && path_index <= DCAM_PATH_IDX_3) {
		if (path_index == DCAM_PATH_IDX_0)
			p_path = &s_p_dcam_mod[idx]->dcam_path[DCAM_PATH0];
		else if (path_index == DCAM_PATH_IDX_1)
			p_path = &s_p_dcam_mod[idx]->dcam_path[DCAM_PATH1];
		else if (path_index == DCAM_PATH_IDX_2)
			p_path = &s_p_dcam_mod[idx]->dcam_path[DCAM_PATH2];
		else if (path_index == DCAM_PATH_IDX_3)
			p_path = &s_p_dcam_mod[idx]->dcam_path[DCAM_PATH3];

		dcam_wait_for_quickstop(idx, path_index);
		p_path->status = DCAM_ST_STOP;
		p_path->valid = 0;
	} else {
		pr_info("DCAM%d: stop path, wrong index 0x%x\n",
			idx, path_index);
		spin_unlock_irqrestore(&dcam_lock, flag);
		return -rtn;
	}

	if (is_dcam_cowork() == 0)
		dcam_frm_clear(idx, path_index);
	else {
		if (idx == DCAM_ID_0)
			dcam_frm_clear(DCAM_ID_0, path_index);
	}
	spin_unlock_irqrestore(&dcam_lock, flag);

	return rtn;
}

void sprd_dcam_glb_reg_awr(enum dcam_id idx, unsigned long addr,
			   unsigned int val, unsigned int reg_id)
{
	unsigned long flag;

	switch (reg_id) {
	case DCAM_CFG_REG:
		spin_lock_irqsave(&dcam_glb_reg_cfg_lock[idx], flag);
		DCAM_REG_WR(idx, addr, DCAM_REG_RD(idx, addr) & (val));
		spin_unlock_irqrestore(&dcam_glb_reg_cfg_lock[idx], flag);
		break;
	case DCAM_CONTROL_REG:
		spin_lock_irqsave(&dcam_glb_reg_control_lock[idx], flag);
		DCAM_REG_WR(idx, addr, DCAM_REG_RD(idx, addr) & (val));
		spin_unlock_irqrestore(&dcam_glb_reg_control_lock[idx], flag);
		break;
	case DCAM_INIT_MASK_REG:
		spin_lock_irqsave(&dcam_glb_reg_mask_lock[idx], flag);
		DCAM_REG_WR(idx, addr, DCAM_REG_RD(idx, addr) & (val));
		spin_unlock_irqrestore(&dcam_glb_reg_mask_lock[idx], flag);
		break;
	case DCAM_INIT_CLR_REG:
		spin_lock_irqsave(&dcam_glb_reg_clr_lock[idx], flag);
		DCAM_REG_WR(idx, addr, DCAM_REG_RD(idx, addr) & (val));
		spin_unlock_irqrestore(&dcam_glb_reg_clr_lock[idx], flag);
		break;
	case DCAM_AHBM_STS_REG:
		spin_lock_irqsave(&dcam_glb_reg_ahbm_sts_lock[idx], flag);
		DCAM_REG_WR(idx, addr, DCAM_REG_RD(idx, addr) & (val));
		spin_unlock_irqrestore(&dcam_glb_reg_ahbm_sts_lock[idx], flag);
		break;
	case DCAM_ENDIAN_REG:
		spin_lock_irqsave(&dcam_glb_reg_endian_lock[idx], flag);
		DCAM_REG_WR(idx, addr, DCAM_REG_RD(idx, addr) & (val));
		spin_unlock_irqrestore(&dcam_glb_reg_endian_lock[idx], flag);
		break;
	default:
		DCAM_REG_WR(idx, addr, DCAM_REG_RD(idx, addr) & (val));
		break;
	}
}

void sprd_dcam_glb_reg_owr(enum dcam_id idx, unsigned long addr,
			   unsigned int val, unsigned int reg_id)
{
	unsigned long flag;

	switch (reg_id) {
	case DCAM_CFG_REG:
		spin_lock_irqsave(&dcam_glb_reg_cfg_lock[idx], flag);
		DCAM_REG_WR(idx, addr, DCAM_REG_RD(idx, addr) | (val));
		spin_unlock_irqrestore(&dcam_glb_reg_cfg_lock[idx], flag);
		break;
	case DCAM_CONTROL_REG:
		spin_lock_irqsave(&dcam_glb_reg_control_lock[idx], flag);
		DCAM_REG_WR(idx, addr, DCAM_REG_RD(idx, addr) | (val));
		spin_unlock_irqrestore(&dcam_glb_reg_control_lock[idx], flag);
		break;
	case DCAM_INIT_MASK_REG:
		spin_lock_irqsave(&dcam_glb_reg_mask_lock[idx], flag);
		DCAM_REG_WR(idx, addr, DCAM_REG_RD(idx, addr) | (val));
		spin_unlock_irqrestore(&dcam_glb_reg_mask_lock[idx], flag);
		break;
	case DCAM_INIT_CLR_REG:
		spin_lock_irqsave(&dcam_glb_reg_clr_lock[idx], flag);
		DCAM_REG_WR(idx, addr, DCAM_REG_RD(idx, addr) | (val));
		spin_unlock_irqrestore(&dcam_glb_reg_clr_lock[idx], flag);
		break;
	case DCAM_AHBM_STS_REG:
		spin_lock_irqsave(&dcam_glb_reg_ahbm_sts_lock[idx], flag);
		DCAM_REG_WR(idx, addr, DCAM_REG_RD(idx, addr) | (val));
		spin_unlock_irqrestore(&dcam_glb_reg_ahbm_sts_lock[idx], flag);
		break;
	case DCAM_ENDIAN_REG:
		spin_lock_irqsave(&dcam_glb_reg_endian_lock[idx], flag);
		DCAM_REG_WR(idx, addr, DCAM_REG_RD(idx, addr) | (val));
		spin_unlock_irqrestore(&dcam_glb_reg_endian_lock[idx], flag);
		break;
	default:
		DCAM_REG_WR(idx, addr, DCAM_REG_RD(idx, addr) | (val));
		break;
	}
}

void sprd_dcam_glb_reg_mwr(enum dcam_id idx, unsigned long addr,
			   unsigned int mask, unsigned int val,
			   unsigned int reg_id)
{
	unsigned long flag;
	unsigned int tmp = 0;

	switch (reg_id) {
	case DCAM_CFG_REG:
		spin_lock_irqsave(&dcam_glb_reg_cfg_lock[idx], flag);
		{
			tmp = DCAM_REG_RD(idx, addr);
			tmp &= ~(mask);
			DCAM_REG_WR(idx, addr, tmp | ((mask) & (val)));
		}
		spin_unlock_irqrestore(&dcam_glb_reg_cfg_lock[idx], flag);
		break;
	case DCAM_CONTROL_REG:
		spin_lock_irqsave(&dcam_glb_reg_control_lock[idx], flag);
		{
			tmp = DCAM_REG_RD(idx, addr);
			tmp &= ~(mask);
			DCAM_REG_WR(idx, addr, tmp | ((mask) & (val)));
		}
		spin_unlock_irqrestore(&dcam_glb_reg_control_lock[idx], flag);
		break;
	case DCAM_INIT_MASK_REG:
		spin_lock_irqsave(&dcam_glb_reg_mask_lock[idx], flag);
		{
			tmp = DCAM_REG_RD(idx, addr);
			tmp &= ~(mask);
			DCAM_REG_WR(idx, addr, tmp | ((mask) & (val)));
		}
		spin_unlock_irqrestore(&dcam_glb_reg_mask_lock[idx], flag);
		break;
	case DCAM_INIT_CLR_REG:
		spin_lock_irqsave(&dcam_glb_reg_clr_lock[idx], flag);
		{
			tmp = DCAM_REG_RD(idx, addr);
			tmp &= ~(mask);
			DCAM_REG_WR(idx, addr, tmp | ((mask) & (val)));
		}
		spin_unlock_irqrestore(&dcam_glb_reg_clr_lock[idx], flag);
		break;
	case DCAM_AHBM_STS_REG:
		spin_lock_irqsave(&dcam_glb_reg_ahbm_sts_lock[idx], flag);
		{
			tmp = DCAM_REG_RD(idx, addr);
			tmp &= ~(mask);
			DCAM_REG_WR(idx, addr, tmp | ((mask) & (val)));
		}
		spin_unlock_irqrestore(&dcam_glb_reg_ahbm_sts_lock[idx], flag);
		break;
	case DCAM_ENDIAN_REG:
		spin_lock_irqsave(&dcam_glb_reg_endian_lock[idx], flag);
		{
			tmp = DCAM_REG_RD(idx, addr);
			tmp &= ~(mask);
			DCAM_REG_WR(idx, addr, tmp | ((mask) & (val)));
		}
		spin_unlock_irqrestore(&dcam_glb_reg_endian_lock[idx], flag);
		break;
	default:
		{
			tmp = DCAM_REG_RD(idx, addr);
			tmp &= ~(mask);
			DCAM_REG_WR(idx, addr, tmp | ((mask) & (val)));
		}
		break;
	}
}

int sprd_dcam_module_init(enum dcam_id idx)
{
	int ret = 0;

	if (atomic_read(&s_dcam_users[idx]) < 1) {
		pr_err("s_dcam_users[%d] equal to %d", idx,
		       atomic_read(&s_dcam_users[idx]));
		return -EIO;
	}
	ret = dcam_internal_init(idx);

	return ret;
}

int sprd_dcam_module_deinit(enum dcam_id idx)
{
	if (atomic_read(&s_dcam_users[idx]) < 1) {
		pr_err("s_dcam_users[%d] equal to %d", idx,
		       atomic_read(&s_dcam_users[idx]));
		return -EIO;
	}

	dcam_internal_deinit(idx);
	if (s_p_dcam_mod[0] == NULL && s_p_dcam_mod[1] == NULL)
		dma_buffer_list_clear();

	return -DCAM_RTN_SUCCESS;
}

int sprd_dcam_drv_init(struct platform_device *pdev)
{
	int ret = 0;
	int i, j;

	s_pdev = pdev;
	for (i = 0; i < DCAM_ID_MAX; i++) {
		atomic_set(&s_dcam_users[i], 0);
		s_dcam_clk[i] = NULL;
		s_p_dcam_mod[i] = NULL;

		if (s_dcam_sc_array[i] == NULL) {
			s_dcam_sc_array[i] =
				vzalloc(sizeof(struct dcam_sc_array));
			if (s_dcam_sc_array[i] == NULL) {
				pr_info("failed to alloc sc array: %d\n", i);
				for (j = 0; j < i; j++) {
					if (s_dcam_sc_array[j])
						vfree(s_dcam_sc_array[j]);
				}
				ret = -1;
				break;
			}
		}

		mutex_init(&dcam_sem[i]);
		mutex_init(&dcam_module_sema[i]);

		dcam_glb_reg_cfg_lock[i] =
			__SPIN_LOCK_UNLOCKED(&dcam_glb_reg_cfg_lock[i]);
		dcam_glb_reg_control_lock[i] =
			__SPIN_LOCK_UNLOCKED(&dcam_glb_reg_control_lock[i]);
		dcam_glb_reg_mask_lock[i] =
			__SPIN_LOCK_UNLOCKED(&dcam_glb_reg_mask_lock[i]);
		dcam_glb_reg_clr_lock[i] =
			__SPIN_LOCK_UNLOCKED(&dcam_glb_reg_clr_lock[i]);
		dcam_glb_reg_ahbm_sts_lock[i] =
			__SPIN_LOCK_UNLOCKED(&dcam_glb_reg_ahbm_sts_lock[i]);
		dcam_glb_reg_endian_lock[i] =
			__SPIN_LOCK_UNLOCKED(&dcam_glb_reg_endian_lock[i]);
		if (i > 0) {
			for (j = 0; j < DCAM_IRQ_NUMBER; j++)
				isr_list[i][j] = isr_list[0][j];
		}
	}
	dcam_lock = __SPIN_LOCK_UNLOCKED(&dcam_lock);

	return ret;
}

void sprd_dcam_drv_deinit(void)
{
	int i;

	for (i = 0; i < DCAM_MAX_COUNT; i++) {
		atomic_set(&s_dcam_users[i], 0);
		s_dcam_clk[i] = NULL;
		s_p_dcam_mod[i] = NULL;
		s_dcam_irq[i] = 0;

		if (s_dcam_sc_array[i]) {
			vfree(s_dcam_sc_array[i]);
			s_dcam_sc_array[i] = NULL;
		}

		mutex_init(&dcam_sem[i]);
		mutex_init(&dcam_module_sema[i]);

		dcam_glb_reg_cfg_lock[i] =
			__SPIN_LOCK_UNLOCKED(&dcam_glb_reg_cfg_lock[i]);
		dcam_glb_reg_control_lock[i] =
			__SPIN_LOCK_UNLOCKED(&dcam_glb_reg_control_lock[i]);
		dcam_glb_reg_mask_lock[i] =
			__SPIN_LOCK_UNLOCKED(&dcam_glb_reg_mask_lock[i]);
		dcam_glb_reg_clr_lock[i] =
			__SPIN_LOCK_UNLOCKED(&dcam_glb_reg_clr_lock[i]);
		dcam_glb_reg_ahbm_sts_lock[i] =
			__SPIN_LOCK_UNLOCKED(&dcam_glb_reg_ahbm_sts_lock[i]);
		dcam_glb_reg_endian_lock[i] =
			__SPIN_LOCK_UNLOCKED(&dcam_glb_reg_endian_lock[i]);
	}
	dcam_lock = __SPIN_LOCK_UNLOCKED(&dcam_lock);
}

int sprd_dcam_module_en(enum dcam_id idx)
{
	pr_info("DCAM%d: enable dcam module, in %d\n", idx,
		   atomic_read(&s_dcam_users[idx]));

	mutex_lock(&dcam_module_sema[idx]);
	if (atomic_inc_return(&s_dcam_users[idx]) == 1) {

		sprd_cam_pw_on();
		dcam_enable_clk(idx);

		sprd_dcam_reset(idx, DCAM_PATH_IDX_ALL, 0);
		memset((void *)&s_user_func[idx][0], 0,
		       sizeof(void *) * DCAM_IRQ_NUMBER);
		memset((void *)&s_user_data[idx][0], 0,
		       sizeof(void *) * DCAM_IRQ_NUMBER);
		pr_info("DCAM%d: register isr, 0x%x\n", idx,
			DCAM_REG_RD(idx, DCAM_INT_MASK));
	}
	mutex_unlock(&dcam_module_sema[idx]);

	return 0;
}

int sprd_dcam_module_dis(enum dcam_id idx)
{
	enum dcam_drv_rtn rtn = DCAM_RTN_SUCCESS;

	pr_info("DCAM%d: disable dcam moduel, in %d cb: %pS\n", idx,
		   atomic_read(&s_dcam_users[idx]),
		   __builtin_return_address(0));

	if (atomic_read(&s_dcam_users[idx]) == 0)
		return rtn;

	mutex_lock(&dcam_module_sema[idx]);
	if (atomic_dec_return(&s_dcam_users[idx]) == 0) {
		dcam_disable_clk(idx);
		sprd_cam_pw_off();
	}

	mutex_unlock(&dcam_module_sema[idx]);

	return rtn;
}

int sprd_dcam_reset(enum dcam_id idx, enum dcam_path_index path_index,
		    unsigned int is_isr)
{
	enum dcam_drv_rtn rtn = DCAM_RTN_SUCCESS;
	unsigned int time_out = 0;
	unsigned int flag = 0;
	unsigned int chip_flag = 0;

	pr_info("DCAM%d: reset: %d\n", idx, path_index);

	/*
	 * 0x402e3120 = 0 AA
	 * 0x402e3120 = 1 AB
	 */

	if (path_index == DCAM_PATH_IDX_ALL) {
		if (atomic_read(&s_dcam_users[idx])) {
			/* firstly, stop AXI writing */
			sprd_dcam_glb_reg_owr(idx, DCAM_AHBM_STS, BIT_6,
					      DCAM_AHBM_STS_REG);
		}

		/* then wait for AHB busy cleared */
		while (++time_out < DCAM_AXI_STOP_TIMEOUT) {
			if (0 == (DCAM_REG_RD(idx, DCAM_AHBM_STS) & BIT_0))
				break;
		}

		if (time_out >= DCAM_AXI_STOP_TIMEOUT) {
			pr_info("DCAM%d: reset timeout %d\n", idx, time_out);
			return DCAM_RTN_TIMEOUT;
		}
	}

	if (idx == DCAM_ID_0) {
		/* do reset action */
		switch (path_index) {
		case DCAM_PATH_IDX_0:
			regmap_update_bits(cam_ahb_gpr,
					   REG_CAM_AHB_AHB_RST,
					   BIT_CAM_AHB_DCAM0_CAM0_SOFT_RST,
					   BIT_CAM_AHB_DCAM0_CAM0_SOFT_RST);
			regmap_update_bits(cam_ahb_gpr,
					   REG_CAM_AHB_AHB_RST,
					   BIT_CAM_AHB_DCAM0_CAM0_SOFT_RST,
					   ~(unsigned int)
					   BIT_CAM_AHB_DCAM0_CAM0_SOFT_RST);
			DCAM_TRACE("DCAM0: reset path0\n");
			break;

		case DCAM_PATH_IDX_1:
			regmap_update_bits(cam_ahb_gpr,
					   REG_CAM_AHB_AHB_RST,
					   BIT_CAM_AHB_DCAM0_CAM1_SOFT_RST,
					   BIT_CAM_AHB_DCAM0_CAM1_SOFT_RST);
			regmap_update_bits(cam_ahb_gpr,
					   REG_CAM_AHB_AHB_RST,
					   BIT_CAM_AHB_DCAM0_CAM1_SOFT_RST,
					   ~(unsigned int)
					   BIT_CAM_AHB_DCAM0_CAM1_SOFT_RST);
			DCAM_TRACE("DCAM0: reset path1\n");
			break;

		case DCAM_PATH_IDX_2:
			regmap_update_bits(cam_ahb_gpr,
					   REG_CAM_AHB_AHB_RST,
					   BIT_CAM_AHB_DCAM0_CAM2_SOFT_RST,
					   BIT_CAM_AHB_DCAM0_CAM2_SOFT_RST);
			regmap_update_bits(cam_ahb_gpr,
					   REG_CAM_AHB_AHB_RST,
					   BIT_CAM_AHB_DCAM0_CAM2_SOFT_RST,
					   ~(unsigned int)
					   BIT_CAM_AHB_DCAM0_CAM2_SOFT_RST);
			DCAM_TRACE("DCAM0: reset path2\n");
			break;

		case DCAM_PATH_IDX_3:
			regmap_update_bits(cam_ahb_gpr,
					   REG_CAM_AHB_AHB_RST,
					   BIT_CAM_AHB_DCAM0_CAM3_SOFT_RST,
					   BIT_CAM_AHB_DCAM0_CAM3_SOFT_RST);
			regmap_update_bits(cam_ahb_gpr,
					   REG_CAM_AHB_AHB_RST,
					   BIT_CAM_AHB_DCAM0_CAM3_SOFT_RST,
					   ~(unsigned int)
					   BIT_CAM_AHB_DCAM0_CAM3_SOFT_RST);
			DCAM_TRACE("DCAM0: reset path3\n");
			break;

		case DCAM_PATH_IDX_ALL:
			flag = BIT_CAM_AHB_SM_DCAM0_IF_IN_DCAM_SOFT_RST;
			regmap_update_bits(cam_ahb_gpr,
					   REG_CAM_AHB_MODULE_SOFT_RST,
					   flag, flag);
			regmap_update_bits(cam_ahb_gpr,
					   REG_CAM_AHB_MODULE_SOFT_RST,
					   flag, ~flag);
			udelay(1);
			if (!chip_flag || atomic_read(&s_dcam_users[1]) == 0) {
				flag = BIT_CAM_AHB_CCIR_SOFT_RST;
				regmap_update_bits(cam_ahb_gpr,
					   REG_CAM_AHB_AHB_RST, flag, flag);
				regmap_update_bits(cam_ahb_gpr,
					   REG_CAM_AHB_AHB_RST, flag, ~flag);
				pr_info("DCAM0: CCIR_SOFT_RST\n");
			}
			flag = BIT_CAM_AHB_DCAM0_IF_SOFT_RST |
				BIT_CAM_AHB_DCAM0_SOFT_RST;
			regmap_update_bits(cam_ahb_gpr,
					   REG_CAM_AHB_AHB_RST, flag, flag);
			regmap_update_bits(cam_ahb_gpr,
					   REG_CAM_AHB_AHB_RST, flag, ~flag);

			sprd_dcam_glb_reg_owr(idx, DCAM_INT_CLR,
					      DCAM_IRQ_LINE_MASK,
					      DCAM_INIT_CLR_REG);
			sprd_dcam_glb_reg_owr(idx, DCAM_INT_MASK,
					      DCAM_IRQ_LINE_MASK,
					      DCAM_INIT_MASK_REG);
			DCAM_TRACE("DCAM0: reset all\n");
			break;

		default:
			rtn = DCAM_RTN_PARA_ERR;
			break;
		}

		if (path_index == DCAM_PATH_IDX_ALL) {
			if (atomic_read(&s_dcam_users[idx])) {
				/* the end, enable AXI writing */
				sprd_dcam_glb_reg_awr(idx, DCAM_AHBM_STS,
						      ~BIT_6,
						      DCAM_AHBM_STS_REG);
			}
		}
	} else {
		/* do reset action */
		switch (path_index) {
		case DCAM_PATH_IDX_0:
			regmap_update_bits(cam_ahb_gpr,
					   REG_CAM_AHB_AHB_RST,
					   BIT_CAM_AHB_DCAM1_CAM0_SOFT_RST,
					   BIT_CAM_AHB_DCAM1_CAM0_SOFT_RST);
			regmap_update_bits(cam_ahb_gpr,
					   REG_CAM_AHB_AHB_RST,
					   BIT_CAM_AHB_DCAM1_CAM0_SOFT_RST,
					   ~(unsigned int)
					   BIT_CAM_AHB_DCAM1_CAM0_SOFT_RST);
			DCAM_TRACE("DCAM1: reset path0\n");
			break;

		case DCAM_PATH_IDX_1:
			regmap_update_bits(cam_ahb_gpr,
					   REG_CAM_AHB_AHB_RST,
					   BIT_CAM_AHB_DCAM1_CAM1_SOFT_RST,
					   BIT_CAM_AHB_DCAM1_CAM1_SOFT_RST);
			regmap_update_bits(cam_ahb_gpr,
					   REG_CAM_AHB_AHB_RST,
					   BIT_CAM_AHB_DCAM1_CAM1_SOFT_RST,
					   ~(unsigned int)
					   BIT_CAM_AHB_DCAM1_CAM1_SOFT_RST);
			DCAM_TRACE("DCAM1: reset path1\n");
			break;

		case DCAM_PATH_IDX_2:
			regmap_update_bits(cam_ahb_gpr,
					   REG_CAM_AHB_AHB_RST,
					   BIT_CAM_AHB_DCAM1_CAM2_SOFT_RST,
					   BIT_CAM_AHB_DCAM1_CAM2_SOFT_RST);
			regmap_update_bits(cam_ahb_gpr,
					   REG_CAM_AHB_AHB_RST,
					   BIT_CAM_AHB_DCAM1_CAM2_SOFT_RST,
					   ~(unsigned int)
					   BIT_CAM_AHB_DCAM1_CAM2_SOFT_RST);
			DCAM_TRACE("DCAM1: reset path2\n");
			break;

		case DCAM_PATH_IDX_3:
			regmap_update_bits(cam_ahb_gpr,
					   REG_CAM_AHB_AHB_RST,
					   BIT_CAM_AHB_DCAM1_CAM3_SOFT_RST,
					   BIT_CAM_AHB_DCAM1_CAM3_SOFT_RST);
			regmap_update_bits(cam_ahb_gpr,
					   REG_CAM_AHB_AHB_RST,
					   BIT_CAM_AHB_DCAM1_CAM3_SOFT_RST,
					   ~(unsigned int)
					   BIT_CAM_AHB_DCAM1_CAM3_SOFT_RST);
			DCAM_TRACE("DCAM1: reset path3\n");
			break;

		case DCAM_PATH_IDX_ALL:
			flag = BIT_CAM_AHB_SM_DCAM1_IF_IN_DCAM_SOFT_RST;
			regmap_update_bits(cam_ahb_gpr,
					   REG_CAM_AHB_MODULE_SOFT_RST,
					   flag, flag);
			regmap_update_bits(cam_ahb_gpr,
					   REG_CAM_AHB_MODULE_SOFT_RST,
					   flag, ~flag);
			udelay(1);
			if (chip_flag && atomic_read(&s_dcam_users[0]) == 0) {
				flag = BIT_CAM_AHB_CCIR_SOFT_RST;
				regmap_update_bits(cam_ahb_gpr,
					   REG_CAM_AHB_AHB_RST, flag, flag);
				regmap_update_bits(cam_ahb_gpr,
					   REG_CAM_AHB_AHB_RST, flag, ~flag);
				pr_info("DCAM1: CCIR_SOFT_RST\n");
			}
			flag = BIT_CAM_AHB_DCAM1_IF_SOFT_RST |
				BIT_CAM_AHB_DCAM1_SOFT_RST;
			regmap_update_bits(cam_ahb_gpr,
					   REG_CAM_AHB_AHB_RST, flag, flag);
			regmap_update_bits(cam_ahb_gpr,
					   REG_CAM_AHB_AHB_RST, flag, ~flag);

			sprd_dcam_glb_reg_owr(idx, DCAM_INT_CLR,
					      DCAM_IRQ_LINE_MASK,
					      DCAM_INIT_CLR_REG);
			sprd_dcam_glb_reg_owr(idx, DCAM_INT_MASK,
					      DCAM_IRQ_LINE_MASK,
					      DCAM_INIT_MASK_REG);
			DCAM_TRACE("DCAM1: reset all\n");
			break;
		default:
			rtn = DCAM_RTN_PARA_ERR;
			break;
		}

		if (path_index == DCAM_PATH_IDX_ALL) {
			if (atomic_read(&s_dcam_users[idx])) {
				/* the end, enable AXI writing */
				sprd_dcam_glb_reg_awr(idx, DCAM_AHBM_STS,
						      ~BIT_6,
						      DCAM_AHBM_STS_REG);
			}
		}
	}

	DCAM_TRACE("DCAM%d: reset_mode=%x  end\n", idx, path_index);

	return -rtn;
}

int sprd_dcam_update_path(enum dcam_id idx, enum dcam_path_index path_index,
			  struct dcam_size *in_size, struct dcam_rect *in_rect,
			  struct dcam_size *out_size)
{
	enum dcam_drv_rtn rtn = DCAM_RTN_SUCCESS;
	unsigned long flags;

	if (DCAM_ADDR_INVALID(s_p_dcam_mod[idx])) {
		pr_err("zero pointer\n");
		return -EFAULT;
	}
	if (DCAM_ADDR_INVALID(s_dcam_sc_array[idx])) {
		pr_err("zero pointer\n");
		return -EFAULT;
	}

	DCAM_TRACE("DCAM%d: update path\n", idx);
	if ((DCAM_PATH_IDX_0 & path_index)
	    && s_p_dcam_mod[idx]->dcam_path[DCAM_PATH0].valid) {
		local_irq_save(flags);
		if (s_p_dcam_mod[idx]->dcam_path[DCAM_PATH0].is_update) {
			local_irq_restore(flags);
			DCAM_TRACE("updating return\n");
			return rtn;
		}
		local_irq_restore(flags);

		rtn = dcam_path0_cfg(idx, DCAM_PATH_INPUT_SIZE, in_size);
		if (rtn) {
			pr_err("%s err, code %d", __func__, rtn);
			return -(rtn);
		}

		rtn = dcam_path0_cfg(idx, DCAM_PATH_INPUT_RECT, in_rect);
		if (rtn) {
			pr_err("%s err, code %d", __func__, rtn);
			return -(rtn);
		}

		rtn = dcam_path0_cfg(idx, DCAM_PATH_OUTPUT_SIZE, out_size);
		if (rtn) {
			pr_err("%s err, code %d", __func__, rtn);
			return -(rtn);
		}

		DCAM_TRACE("to update path0\n");

		local_irq_save(flags);
		s_p_dcam_mod[idx]->dcam_path[DCAM_PATH0].is_update = 1;
		local_irq_restore(flags);
	}

	if ((DCAM_PATH_IDX_1 & path_index)
	    && s_p_dcam_mod[idx]->dcam_path[DCAM_PATH1].valid) {
		if (is_dcam_cowork()) {
			rtn = dcam_path1_cfg(DCAM_ID_0,
					DCAM_PATH_INPUT_SIZE, in_size);
			if (rtn) {
				pr_err("%s err, code %d", __func__, rtn);
				return -(rtn);
			}

			rtn = dcam_path1_cfg(DCAM_ID_0,
					DCAM_PATH_INPUT_RECT, in_rect);
			if (rtn) {
				pr_err("%s err, code %d", __func__, rtn);
				return -(rtn);
			}

			rtn = dcam_path1_cfg(DCAM_ID_0,
					DCAM_PATH_OUTPUT_SIZE, out_size);
			if (rtn) {
				pr_err("%s err, code %d", __func__, rtn);
				return -(rtn);
			}

			rtn = dcam_path1_cfg(DCAM_ID_1,
					DCAM_PATH_INPUT_SIZE, in_size);
			if (rtn) {
				pr_err("%s err, code %d", __func__, rtn);
				return -(rtn);
			}

			rtn = dcam_path1_cfg(DCAM_ID_1,
					DCAM_PATH_INPUT_RECT, in_rect);
			if (rtn) {
				pr_err("%s err, code %d", __func__, rtn);
				return -(rtn);
			}

			rtn = dcam_path1_cfg(DCAM_ID_1,
					DCAM_PATH_OUTPUT_SIZE, out_size);
			if (rtn) {
				pr_err("%s err, code %d", __func__, rtn);
				return -(rtn);
			}
			DCAM_TRACE("DCAM%d: to update path1\n", idx);
			rtn = dcam_path_scaler(DCAM_ID_0, DCAM_PATH_IDX_1);
			if (rtn) {
				pr_err("%s err, code %d", __func__, rtn);
				return -(rtn);
			}
			rtn = dcam_path_scaler(DCAM_ID_1, DCAM_PATH_IDX_1);
			if (rtn) {
				pr_err("%s err, code %d", __func__, rtn);
				return -(rtn);
			}
		} else {
			rtn = dcam_path1_cfg(idx,
					DCAM_PATH_INPUT_SIZE, in_size);
			if (rtn) {
				pr_err("%s err, code %d", __func__, rtn);
				return -(rtn);
			}
			rtn = dcam_path1_cfg(idx,
					DCAM_PATH_INPUT_RECT, in_rect);
			if (rtn) {
				pr_err("%s err, code %d", __func__, rtn);
				return -(rtn);
			}
			rtn = dcam_path1_cfg(idx,
					DCAM_PATH_OUTPUT_SIZE, out_size);
			if (rtn) {
				pr_err("%s err, code %d", __func__, rtn);
				return -(rtn);
			}
			DCAM_TRACE("DCAM%d: to update path1\n", idx);
			rtn = dcam_path_scaler(idx, DCAM_PATH_IDX_1);
			if (rtn) {
				pr_err("%s err, code %d", __func__, rtn);
				return -(rtn);
			}
		}
		if (s_dcam_sc_array[idx]->is_smooth_zoom) {
			spin_lock_irqsave(&dcam_lock, flags);
			s_p_dcam_mod[idx]->dcam_path[DCAM_PATH1].is_update = 1;
			spin_unlock_irqrestore(&dcam_lock, flags);
		} else {
			dcam_wait_update_done(idx, DCAM_PATH_IDX_1,
					      &s_p_dcam_mod[idx]->
					      dcam_path[DCAM_PATH1].is_update);
		}
		if (!s_dcam_sc_array[idx]->is_smooth_zoom)
			dcam_wait_update_done(idx, DCAM_PATH_IDX_1, NULL);
	}

	if ((DCAM_PATH_IDX_2 & path_index)
	    && s_p_dcam_mod[idx]->dcam_path[DCAM_PATH2].valid) {
		local_irq_save(flags);
		if (s_p_dcam_mod[idx]->dcam_path[DCAM_PATH2].is_update) {
			local_irq_restore(flags);
			DCAM_TRACE("updating return\n");
			return rtn;
		}
		local_irq_restore(flags);

		if (is_dcam_cowork()) {
			rtn = dcam_path2_cfg(DCAM_ID_0,
					DCAM_PATH_INPUT_SIZE, in_size);
			if (rtn) {
				pr_err("%s err, code %d", __func__, rtn);
				return -(rtn);
			}
			rtn = dcam_path2_cfg(DCAM_ID_0,
					DCAM_PATH_INPUT_RECT, in_rect);
			if (rtn) {
				pr_err("%s err, code %d", __func__, rtn);
				return -(rtn);
			}
			rtn = dcam_path2_cfg(DCAM_ID_0,
					DCAM_PATH_OUTPUT_SIZE, out_size);
			if (rtn) {
				pr_err("%s err, code %d", __func__, rtn);
				return -(rtn);
			}

			rtn = dcam_path2_cfg(DCAM_ID_1,
					DCAM_PATH_INPUT_SIZE, in_size);
			if (rtn) {
				pr_err("%s err, code %d", __func__, rtn);
				return -(rtn);
			}
			rtn = dcam_path2_cfg(DCAM_ID_1,
					DCAM_PATH_INPUT_RECT, in_rect);
			if (rtn) {
				pr_err("%s err, code %d", __func__, rtn);
				return -(rtn);
			}
			rtn = dcam_path2_cfg(DCAM_ID_1,
					DCAM_PATH_OUTPUT_SIZE, out_size);
			if (rtn) {
				pr_err("%s err, code %d", __func__, rtn);
				return -(rtn);
			}

			rtn = dcam_path_scaler(DCAM_ID_0, DCAM_PATH_IDX_2);
			if (rtn) {
				pr_err("%s err, code %d", __func__, rtn);
				return -(rtn);
			}
			rtn = dcam_path_scaler(DCAM_ID_1, DCAM_PATH_IDX_2);
			if (rtn) {
				pr_err("%s err, code %d", __func__, rtn);
				return -(rtn);
			}
		} else {
			rtn = dcam_path2_cfg(idx,
					DCAM_PATH_INPUT_SIZE, in_size);
			if (rtn) {
				pr_err("%s err, code %d", __func__, rtn);
				return -(rtn);
			}
			rtn = dcam_path2_cfg(idx,
					DCAM_PATH_INPUT_RECT, in_rect);
			if (rtn) {
				pr_err("%s err, code %d", __func__, rtn);
				return -(rtn);
			}
			rtn = dcam_path2_cfg(idx,
					DCAM_PATH_OUTPUT_SIZE, out_size);
			if (rtn) {
				pr_err("%s err, code %d", __func__, rtn);
				return -(rtn);
			}

			rtn = dcam_path_scaler(idx, DCAM_PATH_IDX_2);
			if (rtn) {
				pr_err("%s err, code %d", __func__, rtn);
				return -(rtn);
			}
		}
		local_irq_save(flags);
		s_p_dcam_mod[idx]->dcam_path[DCAM_PATH2].is_update = 1;
		local_irq_restore(flags);
	}

	if ((DCAM_PATH_IDX_3 & path_index)
	    && s_p_dcam_mod[idx]->dcam_path[DCAM_PATH3].valid) {
		local_irq_save(flags);
		if (s_p_dcam_mod[idx]->dcam_path[DCAM_PATH3].is_update) {
			local_irq_restore(flags);
			DCAM_TRACE("updating return\n");
			return rtn;
		}
		local_irq_restore(flags);

		if (is_dcam_cowork()) {
			rtn = dcam_path3_cfg(DCAM_ID_0,
						 DCAM_PATH_INPUT_SIZE,
						 in_size);
			if (rtn) {
				pr_err("%s err, code %d", __func__, rtn);
				return -(rtn);
			}
			rtn = dcam_path3_cfg(DCAM_ID_0,
						 DCAM_PATH_INPUT_RECT,
						 in_rect);
			if (rtn) {
				pr_err("%s err, code %d", __func__, rtn);
				return -(rtn);
			}
			rtn = dcam_path3_cfg(DCAM_ID_0,
						 DCAM_PATH_OUTPUT_SIZE,
						 out_size);
			if (rtn) {
				pr_err("%s err, code %d", __func__, rtn);
				return -(rtn);
			}

			rtn = dcam_path3_cfg(DCAM_ID_1,
						 DCAM_PATH_INPUT_SIZE,
						 in_size);
			if (rtn) {
				pr_err("%s err, code %d", __func__, rtn);
				return -(rtn);
			}
			rtn = dcam_path3_cfg(DCAM_ID_1,
						 DCAM_PATH_INPUT_RECT,
						 in_rect);
			if (rtn) {
				pr_err("%s err, code %d", __func__, rtn);
				return -(rtn);
			}
			rtn = dcam_path3_cfg(DCAM_ID_1,
						 DCAM_PATH_OUTPUT_SIZE,
						 out_size);
			if (rtn) {
				pr_err("%s err, code %d", __func__, rtn);
				return -(rtn);
			}
			rtn = dcam_path_scaler(DCAM_ID_0, DCAM_PATH_IDX_3);
			if (rtn) {
				pr_err("%s err, code %d", __func__, rtn);
				return -(rtn);
			}

			rtn = dcam_path_scaler(DCAM_ID_1, DCAM_PATH_IDX_3);
			if (rtn) {
				pr_err("%s err, code %d", __func__, rtn);
				return -(rtn);
			}
		} else {
			rtn = dcam_path3_cfg(idx,
						 DCAM_PATH_INPUT_SIZE,
						 in_size);
			if (rtn) {
				pr_err("%s err, code %d", __func__, rtn);
				return -(rtn);
			}
			rtn = dcam_path3_cfg(idx,
						 DCAM_PATH_INPUT_RECT,
						 in_rect);
			if (rtn) {
				pr_err("%s err, code %d", __func__, rtn);
				return -(rtn);
			}
			rtn = dcam_path3_cfg(idx,
						 DCAM_PATH_OUTPUT_SIZE,
						 out_size);
			if (rtn) {
				pr_err("%s err, code %d", __func__, rtn);
				return -(rtn);
			}

			rtn = dcam_path_scaler(idx, DCAM_PATH_IDX_3);
			if (rtn) {
				pr_err("%s err, code %d", __func__, rtn);
				return -(rtn);
			}
		}
		dcam_wait_update_done(idx, DCAM_PATH_IDX_3,
					      &s_p_dcam_mod[idx]->
					      dcam_path[DCAM_PATH3].is_update);
		dcam_wait_update_done(idx, DCAM_PATH_IDX_3, NULL);
	}

	DCAM_TRACE("update path done\n");

	return -rtn;
}

int sprd_dcam_start_path(enum dcam_id idx, enum dcam_path_index path_index)
{
	enum dcam_drv_rtn rtn = DCAM_RTN_SUCCESS;
	unsigned int cap_en = 0;

	if (DCAM_ADDR_INVALID(s_p_dcam_mod[idx])) {
		pr_err("zero pointer\n");
		return -EFAULT;
	}

	DCAM_TRACE("DCAM%d: start path 0x%x, mode %x path {%d %d %d %d}\n",
		   idx, path_index, s_p_dcam_mod[idx]->dcam_cap.cap_mode,
		   s_p_dcam_mod[idx]->dcam_path[DCAM_PATH0].valid,
		   s_p_dcam_mod[idx]->dcam_path[DCAM_PATH1].valid,
		   s_p_dcam_mod[idx]->dcam_path[DCAM_PATH2].valid,
		   s_p_dcam_mod[idx]->dcam_path[DCAM_PATH3].valid);

	if (!(s_p_dcam_mod[idx]->dcam_path[DCAM_PATH0].valid) &&
	    is_dcam_cowork()) {
		sprd_dcam_glb_reg_owr(DCAM_ID_0, DCAM_CFG, BIT_19,
			DCAM_CFG_REG);
		sprd_dcam_glb_reg_owr(DCAM_ID_0, DCAM_AHBM_STS, BIT_8,
			DCAM_AHBM_STS_REG);
		sprd_dcam_glb_reg_owr(DCAM_ID_1, DCAM_CFG, BIT_19,
			DCAM_CFG_REG);
		sprd_dcam_glb_reg_owr(DCAM_ID_1, DCAM_AHBM_STS, BIT_8,
			DCAM_AHBM_STS_REG);
	} else {
		sprd_dcam_glb_reg_owr(idx, DCAM_AHBM_STS, BIT_8,
			DCAM_AHBM_STS_REG);
	}

	if ((DCAM_PATH_IDX_0 & path_index) &&
	    s_p_dcam_mod[idx]->dcam_path[DCAM_PATH0].valid) {
		dcam_path_set(idx, DCAM_PATH_IDX_0);
		rtn = dcam_path_set_next_frm(idx, DCAM_PATH_IDX_0);
		if (rtn) {
			pr_err("%s err, code %d", __func__, rtn);
			return -(rtn);
		}
		dcam_auto_copy(idx, DCAM_PATH_IDX_0);
	}

	if ((DCAM_PATH_IDX_1 & path_index) &&
	    s_p_dcam_mod[idx]->dcam_path[DCAM_PATH1].valid) {

		if (is_dcam_cowork()) {
			glb_dcam_init_flag.dcam0_sof &= ~(DCAM_PATH_IDX_1);
			glb_dcam_init_flag.dcam0_eof &= ~(DCAM_PATH_IDX_1);
			glb_dcam_init_flag.dcam1_sof &= ~(DCAM_PATH_IDX_1);
			glb_dcam_init_flag.dcam1_eof &= ~(DCAM_PATH_IDX_1);
			rtn = dcam_path_scaler(DCAM_ID_0, DCAM_PATH_IDX_1);
			if (rtn) {
				pr_err("%s err, code %d", __func__, rtn);
				return -(rtn);
			}

			rtn = dcam_path_scaler(DCAM_ID_1, DCAM_PATH_IDX_1);
			if (rtn) {
				pr_err("%s err, code %d", __func__, rtn);
				return -(rtn);
			}

			rtn = dcam_cowork_para_cfg(DCAM_PATH_IDX_1);
			if (rtn) {
				pr_err("%s err, code %d", __func__, rtn);
				return -(rtn);
			}

			dcam_path_set(DCAM_ID_0, DCAM_PATH_IDX_1);
			dcam_path_set(DCAM_ID_1, DCAM_PATH_IDX_1);

			rtn = dcam_path_set_next_frm(DCAM_ID_0,
				DCAM_PATH_IDX_1);
			if (rtn) {
				pr_err("%s err, code %d", __func__, rtn);
				return -(rtn);
			}

			dcam_force_copy_ext(DCAM_ID_0,
				DCAM_PATH_IDX_1,
				true,
				true);
			sprd_dcam_glb_reg_owr(DCAM_ID_0,
				DCAM_CFG,
				BIT_1,
				DCAM_CFG_REG);

			dcam_force_copy_ext(DCAM_ID_1,
				DCAM_PATH_IDX_1,
				true,
				true);
			sprd_dcam_glb_reg_owr(DCAM_ID_1,
				DCAM_CFG,
				BIT_1,
				DCAM_CFG_REG);
		} else {
			rtn = dcam_path_scaler(idx, DCAM_PATH_IDX_1);
			if (rtn) {
				pr_err("%s err, code %d", __func__, rtn);
				return -(rtn);
			}

			dcam_path_set(idx, DCAM_PATH_IDX_1);
			DCAM_TRACE("DCAM%d: path control 0x%x\n", idx,
				   DCAM_REG_RD(idx, DCAM_CONTROL));

			rtn = dcam_path_set_next_frm(idx, DCAM_PATH_IDX_1);
			if (rtn) {
				pr_err("%s err, code %d", __func__, rtn);
				return -(rtn);
			}

			dcam_force_copy_ext(idx, DCAM_PATH_IDX_1, true, true);
			DCAM_TRACE("int status 0x%x\n",
				DCAM_REG_RD(idx, DCAM_INT_STS));
			sprd_dcam_glb_reg_owr(idx,
				DCAM_CFG,
				BIT_1,
				DCAM_CFG_REG);
		}
	}

	if ((DCAM_PATH_IDX_2 & path_index) &&
	    s_p_dcam_mod[idx]->dcam_path[DCAM_PATH2].valid) {
		if (is_dcam_cowork()) {
			glb_dcam_init_flag.dcam0_sof &= ~(DCAM_PATH_IDX_2);
			glb_dcam_init_flag.dcam0_eof &= ~(DCAM_PATH_IDX_2);
			glb_dcam_init_flag.dcam1_sof &= ~(DCAM_PATH_IDX_2);
			glb_dcam_init_flag.dcam1_eof &= ~(DCAM_PATH_IDX_2);
			rtn = dcam_path_scaler(DCAM_ID_0, DCAM_PATH_IDX_2);
			if (rtn) {
				pr_err("%s err, code %d", __func__, rtn);
				return -(rtn);
			}
			rtn = dcam_path_scaler(DCAM_ID_1, DCAM_PATH_IDX_2);
			if (rtn) {
				pr_err("%s err, code %d", __func__, rtn);
				return -(rtn);
			}
			rtn = dcam_cowork_para_cfg(DCAM_PATH_IDX_2);
			if (rtn) {
				pr_err("%s err, code %d", __func__, rtn);
				return -(rtn);
			}

			dcam_path_set(DCAM_ID_0, DCAM_PATH_IDX_2);
			dcam_path_set(DCAM_ID_1, DCAM_PATH_IDX_2);

			rtn = dcam_path_set_next_frm(DCAM_ID_0,
				DCAM_PATH_IDX_2);
			if (rtn) {
				pr_err("%s err, code %d", __func__, rtn);
				return -(rtn);
			}

			dcam_force_copy_ext(DCAM_ID_0,
				DCAM_PATH_IDX_2,
				true,
				true);
			sprd_dcam_glb_reg_owr(DCAM_ID_0,
				DCAM_CFG,
				BIT_2,
				DCAM_CFG_REG);

			dcam_force_copy_ext(DCAM_ID_1,
				DCAM_PATH_IDX_2,
				true,
				true);
			sprd_dcam_glb_reg_owr(DCAM_ID_1,
				DCAM_CFG,
				BIT_2,
				DCAM_CFG_REG);
		} else {
			rtn = dcam_path_scaler(idx, DCAM_PATH_IDX_2);
			if (rtn) {
				pr_err("%s err, code %d", __func__, rtn);
				return -(rtn);
			}

			dcam_path_set(idx, DCAM_PATH_IDX_2);

			rtn = dcam_path_set_next_frm(idx, DCAM_PATH_IDX_2);
			if (rtn) {
				pr_err("%s err, code %d", __func__, rtn);
				return -(rtn);
			}
			dcam_force_copy_ext(idx, DCAM_PATH_IDX_2, true, true);
			sprd_dcam_glb_reg_owr(idx,
				DCAM_CFG,
				BIT_2,
				DCAM_CFG_REG);
		}
	}

	if ((DCAM_PATH_IDX_3 & path_index) &&
	    s_p_dcam_mod[idx]->dcam_path[DCAM_PATH3].valid) {
		if (is_dcam_cowork()) {
			glb_dcam_init_flag.dcam0_sof &= ~(DCAM_PATH_IDX_3);
			glb_dcam_init_flag.dcam0_eof &= ~(DCAM_PATH_IDX_3);
			glb_dcam_init_flag.dcam1_sof &= ~(DCAM_PATH_IDX_3);
			glb_dcam_init_flag.dcam1_eof &= ~(DCAM_PATH_IDX_3);
			rtn = dcam_path_scaler(DCAM_ID_0, DCAM_PATH_IDX_3);
			if (rtn) {
				pr_err("%s err, code %d", __func__, rtn);
				return -(rtn);
			}
			rtn = dcam_path_scaler(DCAM_ID_1, DCAM_PATH_IDX_3);
			if (rtn) {
				pr_err("%s err, code %d", __func__, rtn);
				return -(rtn);
			}

			rtn = dcam_cowork_para_cfg(DCAM_PATH_IDX_3);
			if (rtn) {
				pr_err("%s err, code %d", __func__, rtn);
				return -(rtn);
			}

			dcam_path_set(DCAM_ID_0, DCAM_PATH_IDX_3);
			dcam_path_set(DCAM_ID_1, DCAM_PATH_IDX_3);

			rtn = dcam_path_set_next_frm(DCAM_ID_0,
				DCAM_PATH_IDX_3);
			if (rtn) {
				pr_err("%s err, code %d", __func__, rtn);
				return -(rtn);
			}

			dcam_force_copy_ext(DCAM_ID_0,
				DCAM_PATH_IDX_3,
				true,
				true);
			sprd_dcam_glb_reg_owr(DCAM_ID_0,
				DCAM_CFG,
				BIT_3,
				DCAM_CFG_REG);

			dcam_force_copy_ext(DCAM_ID_1,
				DCAM_PATH_IDX_3,
				true,
				true);
			sprd_dcam_glb_reg_owr(DCAM_ID_1,
				DCAM_CFG,
				BIT_3,
				DCAM_CFG_REG);
		} else {
		rtn = dcam_path_scaler(idx, DCAM_PATH_IDX_3);
		if (rtn) {
			pr_err("%s err, code %d", __func__, rtn);
			return -(rtn);
		}
		dcam_path_set(idx, DCAM_PATH_IDX_3);
		rtn = dcam_path_set_next_frm(idx, DCAM_PATH_IDX_3);
		if (rtn) {
			pr_err("%s err, code %d", __func__, rtn);
			return -(rtn);
		}
		dcam_force_copy_ext(idx,
			DCAM_PATH_IDX_3,
			true,
			true);
		sprd_dcam_glb_reg_owr(idx,
			DCAM_CFG,
			BIT_3,
			DCAM_CFG_REG);
		}
	}

	if ((DCAM_PATH_IDX_0 & path_index) &&
	    s_p_dcam_mod[idx]->dcam_path[DCAM_PATH0].valid) {
		s_p_dcam_mod[idx]->dcam_path[DCAM_PATH0].need_wait = 0;
		s_p_dcam_mod[idx]->dcam_path[DCAM_PATH0].status = DCAM_ST_START;
		s_p_dcam_mod[idx]->dcam_path[DCAM_PATH0].sof_cnt = 0;
		s_p_dcam_mod[idx]->dcam_path[DCAM_PATH0].done_cnt = 0;
		sprd_dcam_glb_reg_owr(idx, DCAM_CFG, BIT_0, DCAM_CFG_REG);
	}

	if ((DCAM_PATH_IDX_1 & path_index) &&
	    s_p_dcam_mod[idx]->dcam_path[DCAM_PATH1].valid) {
		s_p_dcam_mod[idx]->dcam_path[DCAM_PATH1].need_wait = 0;
		s_p_dcam_mod[idx]->dcam_path[DCAM_PATH1].status = DCAM_ST_START;
		s_p_dcam_mod[idx]->dcam_path[DCAM_PATH1].sof_cnt = 0;
		s_p_dcam_mod[idx]->dcam_path[DCAM_PATH1].done_cnt = 0;
		sprd_dcam_glb_reg_owr(idx, DCAM_CFG, BIT_1, DCAM_CFG_REG);
	}

	if ((DCAM_PATH_IDX_2 & path_index)
	    && s_p_dcam_mod[idx]->dcam_path[DCAM_PATH2].valid) {
		s_p_dcam_mod[idx]->dcam_path[DCAM_PATH2].need_wait = 0;
		s_p_dcam_mod[idx]->dcam_path[DCAM_PATH2].status = DCAM_ST_START;
		s_p_dcam_mod[idx]->dcam_path[DCAM_PATH2].sof_cnt = 0;
		s_p_dcam_mod[idx]->dcam_path[DCAM_PATH2].done_cnt = 0;
		sprd_dcam_glb_reg_owr(idx, DCAM_CFG, BIT_2, DCAM_CFG_REG);
	}

	if ((DCAM_PATH_IDX_3 & path_index) &&
	    s_p_dcam_mod[idx]->dcam_path[DCAM_PATH3].valid) {
		s_p_dcam_mod[idx]->dcam_path[DCAM_PATH3].need_wait = 0;
		s_p_dcam_mod[idx]->dcam_path[DCAM_PATH3].status = DCAM_ST_START;
		s_p_dcam_mod[idx]->dcam_path[DCAM_PATH3].sof_cnt = 0;
		s_p_dcam_mod[idx]->dcam_path[DCAM_PATH3].done_cnt = 0;
		sprd_dcam_glb_reg_owr(idx, DCAM_CFG, BIT_3, DCAM_CFG_REG);
	}

	cap_en = DCAM_REG_RD(idx, DCAM_CONTROL) & BIT_2;
	DCAM_TRACE("DCAM%d: cap_eb %d\n", idx, cap_en);

	if (cap_en == 0) {
#if 0
		sprd_dcam_glb_reg_owr(idx, DCAM_INT_CLR, DCAM_IRQ_LINE_MASK,
				      DCAM_INIT_CLR_REG);
#endif
		/* Cap force copy */
		sprd_dcam_glb_reg_mwr(idx, DCAM_CONTROL, BIT_0, 1 << 0,
				      DCAM_CONTROL_REG);
#if 0
		/* Cap auto  copy */
		DCAM_REG_MWR(idx, DCAM_CONTROL, BIT_1, 1 << 1);
#endif
		/* Cap Enable */
		sprd_dcam_glb_reg_mwr(idx, DCAM_CONTROL, BIT_2, 1 << 2,
				      DCAM_CONTROL_REG);
	}

	if (path_index != DCAM_PATH_IDX_ALL) {
		if ((DCAM_PATH_IDX_0 & path_index))
			dcam_wait_path_done(idx, DCAM_PATH_IDX_0, NULL);
		else if ((DCAM_PATH_IDX_1 & path_index))
			dcam_wait_path_done(idx, DCAM_PATH_IDX_1, NULL);
		else if ((DCAM_PATH_IDX_2 & path_index))
			dcam_wait_path_done(idx, DCAM_PATH_IDX_2, NULL);
		else if ((DCAM_PATH_IDX_3 & path_index))
			dcam_wait_path_done(idx, DCAM_PATH_IDX_3, NULL);
	}

	pr_info("DCAM%d: start path %d, %d\n", idx, path_index, rtn);

#if 0
	csi_dump_reg();
	isp_print_reg();
	sprd_dcam2isp_dump_reg();
	sprd_isp2dcam_dump_reg();
#endif
	if (is_dcam_cowork()) {
		dcam_reg_trace(DCAM_ID_0);
		dcam_reg_trace(DCAM_ID_1);
	} else
		dcam_reg_trace(idx);

	return -rtn;
}

int sprd_dcam_start(enum dcam_id idx)
{
	enum dcam_drv_rtn rtn = DCAM_RTN_SUCCESS;
	int ret = 0;

	if (DCAM_ADDR_INVALID(s_p_dcam_mod[idx])) {
		pr_err("zero pointer\n");
		return -EFAULT;
	}

	DCAM_TRACE("DCAM%d: dcam start %x\n", idx,
		   s_p_dcam_mod[idx]->dcam_cap.cap_mode);

	ret = sprd_dcam_start_path(idx, DCAM_PATH_IDX_ALL);

	return -rtn;
}

int sprd_dcam_stop_path(enum dcam_id idx, enum dcam_path_index path_index)
{
	enum dcam_drv_rtn rtn = DCAM_RTN_SUCCESS;

	if (DCAM_ADDR_INVALID(s_p_dcam_mod[idx])) {
		pr_err("zero pointer\n");
		return -EFAULT;
	}

	pr_info("DCAM%d: stop path 0x%x\n", idx, path_index);

	if (path_index < DCAM_PATH_IDX_0 || path_index > DCAM_PATH_IDX_ALL) {
		pr_err("DCAM%d: error path index %d\n", idx, path_index);
		return -rtn;
	}

	if ((DCAM_PATH_IDX_0 & path_index) &&
	    s_p_dcam_mod[idx]->dcam_path[DCAM_PATH0].valid) {
		DCAM_TRACE("DCAM%d: stop path0 In\n", idx);
		dcam_wait_path_done(idx, DCAM_PATH_IDX_0,
				    &s_p_dcam_mod[idx]->dcam_path[DCAM_PATH0].
				    need_stop);
		dcam_stop_path(idx, DCAM_PATH_IDX_0);
	}

	if ((DCAM_PATH_IDX_1 & path_index) &&
	    s_p_dcam_mod[idx]->dcam_path[DCAM_PATH1].valid) {
		DCAM_TRACE("DCAM%d: stop path1 In\n", idx);
		dcam_wait_path_done(idx, DCAM_PATH_IDX_1,
				    &s_p_dcam_mod[idx]->dcam_path[DCAM_PATH1].
				    need_stop);
		if (is_dcam_cowork()) {
			dcam_stop_path(DCAM_ID_0, DCAM_PATH_IDX_1);
			dcam_stop_path(DCAM_ID_1, DCAM_PATH_IDX_1);
			glb_dcam_init_flag.dcam0_sof &= ~(DCAM_PATH_IDX_1);
			glb_dcam_init_flag.dcam0_eof &= ~(DCAM_PATH_IDX_1);
			glb_dcam_init_flag.dcam1_sof &= ~(DCAM_PATH_IDX_1);
			glb_dcam_init_flag.dcam1_eof &= ~(DCAM_PATH_IDX_1);
		} else
			dcam_stop_path(idx, DCAM_PATH_IDX_1);
	}

	if ((DCAM_PATH_IDX_2 & path_index) &&
	    s_p_dcam_mod[idx]->dcam_path[DCAM_PATH2].valid) {
		DCAM_TRACE("DCAM%d: stop path2 In\n", idx);
		dcam_wait_path_done(idx, DCAM_PATH_IDX_2,
				    &s_p_dcam_mod[idx]->dcam_path[DCAM_PATH2].
				    need_stop);
		if (is_dcam_cowork()) {
			dcam_stop_path(DCAM_ID_0, DCAM_PATH_IDX_2);
			dcam_stop_path(DCAM_ID_1, DCAM_PATH_IDX_2);
			glb_dcam_init_flag.dcam0_sof &= ~(DCAM_PATH_IDX_2);
			glb_dcam_init_flag.dcam0_eof &= ~(DCAM_PATH_IDX_2);
			glb_dcam_init_flag.dcam1_sof &= ~(DCAM_PATH_IDX_2);
			glb_dcam_init_flag.dcam1_eof &= ~(DCAM_PATH_IDX_2);
		} else
			dcam_stop_path(idx, DCAM_PATH_IDX_2);
	}

	if ((DCAM_PATH_IDX_3 & path_index) &&
	    s_p_dcam_mod[idx]->dcam_path[DCAM_PATH3].valid) {
		DCAM_TRACE("DCAM%d: stop path3 In\n", idx);
		dcam_wait_path_done(idx, DCAM_PATH_IDX_3,
				    &s_p_dcam_mod[idx]->dcam_path[DCAM_PATH3].
				    need_stop);
		if (is_dcam_cowork()) {
			dcam_stop_path(DCAM_ID_0, DCAM_PATH_IDX_3);
			dcam_stop_path(DCAM_ID_1, DCAM_PATH_IDX_3);
			glb_dcam_init_flag.dcam0_sof &= ~(DCAM_PATH_IDX_3);
			glb_dcam_init_flag.dcam0_eof &= ~(DCAM_PATH_IDX_3);
			glb_dcam_init_flag.dcam1_sof &= ~(DCAM_PATH_IDX_3);
			glb_dcam_init_flag.dcam1_eof &= ~(DCAM_PATH_IDX_3);
		} else
			dcam_stop_path(idx, DCAM_PATH_IDX_3);
	}

	return -rtn;
}

int sprd_dcam_stop(enum dcam_id idx)
{
	enum dcam_drv_rtn rtn = DCAM_RTN_SUCCESS;
	unsigned long flag;
	unsigned int i = 0;

	if (DCAM_ADDR_INVALID(s_p_dcam_mod[idx])) {
		pr_err("zero pointer\n");
		return -EFAULT;
	}

	s_p_dcam_mod[idx]->state |= DCAM_STATE_QUICKQUIT;

	spin_lock_irqsave(&dcam_lock, flag);

#if 1
	if (is_dcam_cowork()) {
		dcam_wait_for_quickstop(DCAM_ID_0, DCAM_PATH_IDX_ALL);
		dcam_wait_for_quickstop(DCAM_ID_1, DCAM_PATH_IDX_ALL);
		memset(&glb_dcam_init_flag, 0, sizeof(struct dcam_init_flag));
	} else
		dcam_wait_for_quickstop(idx, DCAM_PATH_IDX_ALL);
#else
	sprd_dcam_stop_path(idx, DCAM_PATH_IDX_ALL);
#endif
	for (i = DCAM_PATH0; i < DCAM_PATH_MAX; i++) {
		s_p_dcam_mod[idx]->dcam_path[i].status = DCAM_ST_STOP;
		s_p_dcam_mod[idx]->dcam_path[i].valid = 0;
		s_p_dcam_mod[idx]->dcam_path[i].sof_cnt = 0;
		s_p_dcam_mod[idx]->dcam_path[i].done_cnt = 0;
		dcam_frm_clear(idx, 1 << i);
	}

	spin_unlock_irqrestore(&dcam_lock, flag);

	if (is_dcam_cowork()) {
		sprd_dcam_reset(DCAM_ID_0, DCAM_PATH_IDX_ALL, 0);
		s_p_dcam_mod[DCAM_ID_0]->state &= ~DCAM_STATE_QUICKQUIT;
		sprd_dcam_reset(DCAM_ID_1, DCAM_PATH_IDX_ALL, 0);
		s_p_dcam_mod[DCAM_ID_1]->state &= ~DCAM_STATE_QUICKQUIT;
	} else {
		sprd_dcam_reset(idx, DCAM_PATH_IDX_ALL, 0);
		s_p_dcam_mod[idx]->state &= ~DCAM_STATE_QUICKQUIT;
	}

#if 0
	/* Cap Enable */
	sprd_dcam_glb_reg_mwr(idx, DCAM_CONTROL, BIT_2, 0, DCAM_CONTROL_REG);
#endif

	return -rtn;
}

int sprd_dcam_reg_isr(enum dcam_id idx, enum dcam_irq_id id,
		      dcam_isr_func user_func, void *user_data)
{
	enum dcam_drv_rtn rtn = DCAM_RTN_SUCCESS;
	unsigned long flag;

	if (id >= DCAM_IRQ_NUMBER) {
		rtn = DCAM_RTN_ISR_ID_ERR;
	} else {
		spin_lock_irqsave(&dcam_lock, flag);
		s_user_func[idx][id] = user_func;
		s_user_data[idx][id] = user_data;
		spin_unlock_irqrestore(&dcam_lock, flag);
	}

	return -rtn;
}

int sprd_dcam_cap_cfg(enum dcam_id idx, enum dcam_cfg_id id, void *param)
{
	enum dcam_drv_rtn rtn = DCAM_RTN_SUCCESS;
	struct dcam_cap_desc *cap_desc = NULL;

	if (DCAM_ADDR_INVALID(s_p_dcam_mod[idx])) {
		pr_err("zero pointer\n");
		return -EFAULT;
	}
	if (DCAM_ADDR_INVALID(s_dcam_sc_array[idx])) {
		pr_err("zero pointer\n");
		return -EFAULT;
	}
	cap_desc = &s_p_dcam_mod[idx]->dcam_cap;

	switch (id) {
	case DCAM_CAP_INTERFACE:
		{
			enum dcam_cap_if_mode cap_if_mode =
				*(unsigned int *)param;

			if (cap_if_mode == DCAM_CAP_IF_CSI2)
				cap_desc->cam_if = cap_if_mode;
			else
				rtn = DCAM_RTN_CAP_IF_MODE_ERR;
			break;
		}

	case DCAM_CAP_SENSOR_MODE:
		{
			enum dcam_cap_sensor_mode cap_sensor_mode =
				*(unsigned int *)param;

			if (cap_sensor_mode >= DCAM_CAP_MODE_MAX) {
				rtn = DCAM_RTN_CAP_SENSOR_MODE_ERR;
			} else {
				DCAM_REG_MWR(idx, CAP_MIPI_CTRL, BIT_2 | BIT_1,
					cap_sensor_mode << 1);
				cap_desc->input_format = cap_sensor_mode;
			}
			break;
		}

	case DCAM_CAP_SYNC_POL:
		{
			struct dcam_cap_sync_pol *sync_pol =
				(struct dcam_cap_sync_pol *)param;

			if (sync_pol->need_href)
				DCAM_REG_MWR(idx, CAP_MIPI_CTRL, BIT_5, 1 << 5);
			else
				DCAM_REG_MWR(idx, CAP_MIPI_CTRL, BIT_5, 0 << 5);
			break;
		}

	case DCAM_CAP_DATA_BITS:
		{
			enum dcam_cap_data_bits bits = *(unsigned int *)param;

			if (bits == DCAM_CAP_12_BITS)
				DCAM_REG_MWR(idx, CAP_MIPI_CTRL,
					BIT_4 | BIT_3, 2 << 3);
			else if (bits == DCAM_CAP_10_BITS)
				DCAM_REG_MWR(idx, CAP_MIPI_CTRL,
					BIT_4 | BIT_3, 1 << 3);
			else if (bits == DCAM_CAP_8_BITS)
				DCAM_REG_MWR(idx, CAP_MIPI_CTRL,
					BIT_4 | BIT_3, 0 << 3);
			else
				rtn = DCAM_RTN_CAP_IN_BITS_ERR;
			break;
		}

	case DCAM_CAP_YUV_TYPE:
		{
			enum dcam_cap_pattern pat =
				*(enum dcam_cap_pattern *)param;

			if (pat < DCAM_PATTERN_MAX)
				DCAM_REG_MWR(idx, CAP_MIPI_CTRL,
					BIT_8 | BIT_7, pat << 7);
			else
				rtn = DCAM_RTN_CAP_IN_YUV_ERR;
			break;
		}

	case DCAM_CAP_PRE_SKIP_CNT:
		{
			unsigned int skip_num = *(unsigned int *)param;

			if (skip_num > DCAM_CAP_SKIP_FRM_MAX)
				rtn = DCAM_RTN_CAP_SKIP_FRAME_ERR;
			else
				DCAM_REG_MWR(idx, CAP_MIPI_FRM_CTRL,
					BIT_3 | BIT_2 | BIT_1 | BIT_0,
					skip_num);
			break;
		}

	case DCAM_CAP_FRM_DECI:
		{
			unsigned int deci_factor = *(unsigned int *)param;

			if (deci_factor < DCAM_FRM_DECI_FAC_MAX)
				DCAM_REG_MWR(idx, CAP_MIPI_FRM_CTRL,
					BIT_5 | BIT_4,
					deci_factor << 4);
			else
				rtn = DCAM_RTN_CAP_FRAME_DECI_ERR;
			break;
		}

	case DCAM_CAP_FRM_COUNT_CLR:
		DCAM_REG_MWR(idx, CAP_MIPI_FRM_CTRL, BIT_22, 1 << 22);
		break;

	case DCAM_CAP_INPUT_RECT:
		{
			struct dcam_rect *rect = (struct dcam_rect *)param;
			unsigned int tmp = 0;

			if (rect->x > DCAM_CAP_FRAME_WIDTH_MAX ||
			    rect->y > DCAM_CAP_FRAME_HEIGHT_MAX ||
			    rect->w > DCAM_CAP_FRAME_WIDTH_MAX ||
			    rect->h > DCAM_CAP_FRAME_HEIGHT_MAX) {
				rtn = DCAM_RTN_CAP_FRAME_SIZE_ERR;
				return -rtn;
			}
			cap_desc->cap_rect = *rect;
			tmp = rect->x | (rect->y << 16);
			DCAM_REG_WR(idx, CAP_MIPI_START, tmp);
			tmp = (rect->x + rect->w - 1);
			tmp |= (rect->y + rect->h - 1) << 16;
			DCAM_REG_WR(idx, CAP_MIPI_END, tmp);
			break;
		}

	case DCAM_CAP_IMAGE_XY_DECI:
		{
			struct dcam_cap_dec *cap_dec =
				(struct dcam_cap_dec *)param;

			if (cap_dec->x_factor > DCAM_CAP_X_DECI_FAC_MAX ||
			    cap_dec->y_factor > DCAM_CAP_Y_DECI_FAC_MAX) {
				rtn = DCAM_RTN_CAP_XY_DECI_ERR;
			} else {
				if (DCAM_CAP_MODE_RAWRGB ==
				    cap_desc->input_format) {
					if (cap_dec->x_factor > 1
					    || cap_dec->y_factor > 1)
						rtn = DCAM_RTN_CAP_XY_DECI_ERR;
				}
				if (DCAM_CAP_MODE_RAWRGB ==
				    cap_desc->input_format) {
#if 0
					/* for camera path */
					DCAM_REG_MWR(idx, CAP_MIPI_IMG_DECI,
						BIT_0, cap_dec->x_factor);
#endif
					DCAM_REG_MWR(idx, CAP_MIPI_IMG_DECI,
						BIT_1, cap_dec->x_factor << 1);
				} else {
					DCAM_REG_MWR(idx, CAP_MIPI_IMG_DECI,
						BIT_1 | BIT_0,
						cap_dec->x_factor);
					DCAM_REG_MWR(idx, CAP_MIPI_IMG_DECI,
						BIT_3 | BIT_2,
						cap_dec->y_factor << 2);
				}
			}
			break;
		}

	case DCAM_CAP_JPEG_SET_BUF_LEN:
		{
			unsigned int jpg_buf_size = *(unsigned int *)param;

			jpg_buf_size = jpg_buf_size / DCAM_JPG_BUF_UNIT;
			if (jpg_buf_size >= DCAM_JPG_UNITS)
				rtn = DCAM_RTN_CAP_JPEG_BUF_LEN_ERR;
			else
				DCAM_REG_WR(idx, CAP_MIPI_JPG_CTRL,
					jpg_buf_size);
			break;
		}

	case DCAM_CAP_TO_ISP:
		{
			unsigned int need_isp = *(unsigned int *)param;

			if (need_isp) {
				if (is_dcam_cowork()) {
					sprd_dcam_glb_reg_mwr(DCAM_ID_0,
					DCAM_CFG,
					BIT_7,
					1 << 7,
					DCAM_CFG_REG);
					sprd_dcam_glb_reg_mwr(DCAM_ID_1,
					DCAM_CFG,
					BIT_7,
					1 << 7,
					DCAM_CFG_REG);
				} else
					sprd_dcam_glb_reg_mwr(idx,
					DCAM_CFG,
					BIT_7,
					1 << 7,
					DCAM_CFG_REG);
			} else
				sprd_dcam_glb_reg_mwr(idx, DCAM_CFG, BIT_7,
						      0 << 7,
						      DCAM_CFG_REG);
			break;
		}

	case DCAM_CAP_DATA_PACKET:
		{
			unsigned int is_loose = *(unsigned int *)param;

			if (cap_desc->cam_if == DCAM_CAP_IF_CSI2 &&
			    cap_desc->input_format == DCAM_CAP_MODE_RAWRGB) {
				if (is_loose)
					DCAM_REG_MWR(idx, CAP_MIPI_CTRL,
						BIT_0, 1);
				else
					DCAM_REG_MWR(idx, CAP_MIPI_CTRL,
						BIT_0, 0);
			} else
				rtn = DCAM_RTN_MODE_ERR;
			break;
		}

	case DCAM_CAP_SAMPLE_MODE:
		{
			enum dcam_capture_mode samp_mode =
				*(enum dcam_capture_mode *)param;

			if (samp_mode >= DCAM_CAPTURE_MODE_MAX) {
				rtn = DCAM_RTN_MODE_ERR;
			} else {
				DCAM_REG_MWR(idx, CAP_MIPI_CTRL, BIT_6,
					samp_mode << 6);
				s_p_dcam_mod[idx]->dcam_cap.cap_mode =
					samp_mode;
			}
			break;
		}

	case DCAM_CAP_ZOOM_MODE:
		{
			unsigned int zoom_mode = *(unsigned int *)param;

			s_dcam_sc_array[idx]->is_smooth_zoom = zoom_mode;
			break;
		}
	default:
		rtn = DCAM_RTN_IO_ID_ERR;
		break;

	}

	return -rtn;
}

int sprd_dcam_cap_get_info(enum dcam_id idx, enum dcam_cfg_id id, void *param)
{
	unsigned int *out_value = NULL;
	enum dcam_drv_rtn rtn = DCAM_RTN_SUCCESS;
	struct dcam_cap_desc *cap_desc = NULL;

	if (DCAM_ADDR_INVALID(s_p_dcam_mod[idx])) {
		pr_err("zero pointer\n");
		return -EFAULT;
	}

	cap_desc = &s_p_dcam_mod[idx]->dcam_cap;
	if (0 == (unsigned long)(param)) {
		pr_err("parm is null");
		return -DCAM_RTN_PARA_ERR;
	}

	switch (id) {
	case DCAM_CAP_FRM_COUNT_GET:
		*(unsigned int *) param = DCAM_REG_RD(idx,
			CAP_MIPI_FRM_CTRL) >> 16;
		break;

	case DCAM_CAP_JPEG_GET_LENGTH:
		*(unsigned int *) param = DCAM_REG_RD(idx, CAP_MIPI_FRM_SIZE);
		break;
	case DCAM_CAP_JPEGLS_GET_LENGTH:
		out_value = (unsigned int *) param;
		out_value[0] = DCAM_REG_RD(idx, JPEGLS_BSM_SIZE_Y) << 3;
		out_value[1] = DCAM_REG_RD(idx, JPEGLS_BSM_SIZE_U) << 3;
		out_value[2] = DCAM_REG_RD(idx, JPEGLS_BSM_SIZE_V) << 3;
		break;

	default:
		rtn = DCAM_RTN_IO_ID_ERR;
		break;
	}

	return -rtn;
}

int sprd_dcam_path_cfg(enum dcam_id idx, enum dcam_path_index path_index,
		       enum dcam_cfg_id id, void *param)
{
	enum dcam_drv_rtn rtn = DCAM_RTN_SUCCESS;
	struct dcam_path_desc *path = NULL;
	enum dcam_path_id path_id;
	struct dcam_addr *p_addr;
	struct dcam_module *module;
	unsigned int src_sel;
	unsigned int base_id;
	struct dcam_endian_sel *endian;
	unsigned int rot_mode;
	struct dcam_rect *rect;
	struct dcam_size *size;
	unsigned int format;

	if (DCAM_ADDR_INVALID(s_p_dcam_mod[idx])) {
		pr_err("zero pointer\n");
		return -EFAULT;
	}
	if (0 == (unsigned long)(param)) {
		pr_err("parm is null");
		return -DCAM_RTN_PARA_ERR;
	}

	if (DCAM_PATH_IDX_0 & path_index) {
		path_id = DCAM_PATH0;
	} else if (DCAM_PATH_IDX_1 & path_index) {
		path_id = DCAM_PATH1;
	} else if (DCAM_PATH_IDX_2 & path_index) {
		path_id = DCAM_PATH2;
	} else if (DCAM_PATH_IDX_3 & path_index) {
		path_id = DCAM_PATH3;
	} else {
		pr_err("%s error", __func__);
		return -DCAM_RTN_IO_ID_ERR;
	}

	module = s_p_dcam_mod[idx];
	path = &module->dcam_path[path_id];

	switch (id) {
	case DCAM_PATH_INPUT_SIZE:
		size = (struct dcam_size *)param;

		DCAM_TRACE("DCAM%d: DCAM_PATH%d INPUT_SIZE {%d %d}\n",
			   idx, ffs(path_index) - 1, size->w, size->h);
		if ((path_index & DCAM_PATH_IDX_1) &&
		    (size->w > DCAM_PATH1_FRAME_WIDTH_MAX ||
		     size->h > DCAM_PATH1_FRAME_HEIGHT_MAX)) {
			rtn = DCAM_RTN_PATH_SRC_SIZE_ERR;
		} else if ((path_index & DCAM_PATH_IDX_2) &&
			   (size->w > DCAM_PATH2_FRAME_WIDTH_MAX ||
			    size->h > DCAM_PATH2_FRAME_HEIGHT_MAX)) {
			rtn = DCAM_RTN_PATH_SRC_SIZE_ERR;
		} else if ((path_index & DCAM_PATH_IDX_3) &&
			   (size->w > DCAM_PATH3_FRAME_WIDTH_MAX ||
			    size->h > DCAM_PATH3_FRAME_HEIGHT_MAX)) {
			rtn = DCAM_RTN_PATH_SRC_SIZE_ERR;
		} else {
			path->input_size.w = size->w;
			path->input_size.h = size->h;
			path->valid_param.input_size = 1;
		}
		break;

	case DCAM_PATH_INPUT_RECT:
		rect = (struct dcam_rect *)param;

		pr_info("DCAM%d: path %d rect %d %d %d %d\n",
			   idx, ffs(path_index) - 1, rect->x, rect->y,
			   rect->w, rect->h);

		if ((path_index & DCAM_PATH_IDX_1) &&
		    (rect->x > DCAM_PATH1_FRAME_WIDTH_MAX ||
		     rect->y > DCAM_PATH1_FRAME_HEIGHT_MAX ||
		     rect->w > DCAM_PATH1_FRAME_WIDTH_MAX ||
		     rect->h > DCAM_PATH1_FRAME_HEIGHT_MAX)) {
			rtn = DCAM_RTN_PATH_TRIM_SIZE_ERR;
		} else if ((path_index & DCAM_PATH_IDX_2) &&
			   (rect->x > DCAM_PATH2_FRAME_WIDTH_MAX ||
			    rect->y > DCAM_PATH2_FRAME_HEIGHT_MAX ||
			    rect->w > DCAM_PATH2_FRAME_WIDTH_MAX ||
			    rect->h > DCAM_PATH2_FRAME_HEIGHT_MAX)) {
			rtn = DCAM_RTN_PATH_TRIM_SIZE_ERR;
		} else if ((path_index & DCAM_PATH_IDX_3) &&
			   (rect->x > DCAM_PATH3_FRAME_WIDTH_MAX ||
			    rect->y > DCAM_PATH3_FRAME_HEIGHT_MAX ||
			    rect->w > DCAM_PATH3_FRAME_WIDTH_MAX ||
			    rect->h > DCAM_PATH3_FRAME_HEIGHT_MAX)) {
			rtn = DCAM_RTN_PATH_TRIM_SIZE_ERR;
		} else {
			memcpy((void *)&path->input_rect,
			       (void *)rect,
			       sizeof(struct dcam_rect));
			path->valid_param.input_rect = 1;
		}
		break;

	case DCAM_PATH_OUTPUT_SIZE:
		size = (struct dcam_size *)param;

		if ((DCAM_PATH_IDX_1 | DCAM_PATH_IDX_2 |
		     DCAM_PATH_IDX_3) & path_index) {
			DCAM_TRACE("DCAM%d: path %d out size {%d %d}\n",
				   idx, ffs(path_index) - 1, size->w, size->h);
			if ((path_index & DCAM_PATH_IDX_1) &&
			    (size->w > DCAM_PATH1_FRAME_WIDTH_MAX ||
			     size->h > DCAM_PATH1_FRAME_HEIGHT_MAX)) {
				rtn = DCAM_RTN_PATH_SRC_SIZE_ERR;
			} else if ((path_index & DCAM_PATH_IDX_2) &&
				   (size->w > DCAM_PATH2_FRAME_WIDTH_MAX ||
				    size->h > DCAM_PATH2_FRAME_HEIGHT_MAX)) {
				rtn = DCAM_RTN_PATH_SRC_SIZE_ERR;
			} else if ((path_index & DCAM_PATH_IDX_3) &&
				   (size->w > DCAM_PATH3_FRAME_WIDTH_MAX ||
				    size->h > DCAM_PATH3_FRAME_HEIGHT_MAX)) {
				rtn = DCAM_RTN_PATH_SRC_SIZE_ERR;
			} else {
				path->output_size.w = size->w;
				path->output_size.h = size->h;
				path->valid_param.output_size = 1;
			}
		}
		break;

	case DCAM_PATH_OUTPUT_FORMAT:
		format = *(unsigned int *)param;

		DCAM_TRACE("DCAM%d: path %d output format %d\n",
			   idx, ffs(path_index) - 1, format);
		if (((DCAM_PATH_IDX_0 & path_index) &&
		     (DCAM_RAWRGB == format || DCAM_JPEG == format)) ||
		    (((DCAM_PATH_IDX_1 | DCAM_PATH_IDX_2 |
		       DCAM_PATH_IDX_3) & path_index) &&
		     (DCAM_YUV422 == format || DCAM_YUV420 == format ||
		      format == DCAM_YUV420_3FRAME))) {
			path->output_format = format;
			path->valid_param.output_format = 1;
		} else {
			rtn = DCAM_RTN_PATH_OUT_FMT_ERR;
			path->output_format = DCAM_FTM_MAX;
		}
		break;

	case DCAM_PATH_OUTPUT_ADDR:
		p_addr = (struct dcam_addr *)param;

		if (DCAM_YUV_ADDR_INVALID(p_addr->yaddr, p_addr->uaddr,
					  p_addr->vaddr) &&
		    p_addr->mfd_y == 0) {
			rtn = DCAM_RTN_PATH_ADDR_ERR;
		} else {
			struct dcam_frame frame;

			memset((void *)&frame, 0,
			       sizeof(struct dcam_frame));
			frame.yaddr = p_addr->yaddr;
			frame.uaddr = p_addr->uaddr;
			frame.vaddr = p_addr->vaddr;
			frame.yaddr_vir = p_addr->yaddr_vir;
			frame.uaddr_vir = p_addr->uaddr_vir;
			frame.vaddr_vir = p_addr->vaddr_vir;

			frame.type = path_id;
			frame.fid = path->frame_base_id;

			frame.pfinfo.dev = &s_pdev->dev;
			frame.pfinfo.mfd[0] = p_addr->mfd_y;
			frame.pfinfo.mfd[1] = p_addr->mfd_u;
			frame.pfinfo.mfd[2] = p_addr->mfd_u;
			rtn = pfiommu_get_sg_table(&frame.pfinfo);
			if (rtn) {
				pr_err("cfg output addr failed!\n");
				rtn = DCAM_RTN_PATH_ADDR_ERR;
				break;
			}
			if (path->output_format == DCAM_RAWRGB) {
				size = &path->input_size;
				frame.pfinfo.size[0] =
					(size->w * size->h * 5) / 4;
				frame.pfinfo.size[1] = 0;
				frame.pfinfo.size[2] = 0;
			} else if (path->output_format == DCAM_YUV420) {
				size = &path->output_size;
				frame.pfinfo.size[0] = size->w * size->h;
				frame.pfinfo.size[1] = size->w * size->h / 2;
				frame.pfinfo.size[2] = 0;
			} else {
				size = &path->output_size;
				frame.pfinfo.size[0] = size->w * size->h;
				frame.pfinfo.size[1] = size->w * size->h / 2;
				frame.pfinfo.size[2] = 0;
			}
			frame.pfinfo.offset[0] = frame.yaddr;
			frame.pfinfo.offset[1] = frame.uaddr;
			frame.pfinfo.offset[2] = frame.vaddr;

			if (!dcam_buf_queue_write(&path->buf_queue,
						  &frame))
				path->output_frame_count++;

			DCAM_TRACE("DCAM%d: Path %d set output addr, i %d\n",
				   idx, ffs(path_index) - 1,
				   path->output_frame_count);
			DCAM_TRACE("y=0x%x u=0x%x v=0x%x mfd=0x%x 0x%x\n",
				   p_addr->yaddr, p_addr->uaddr,
				   p_addr->vaddr, frame.pfinfo.mfd[0],
				   frame.pfinfo.mfd[1]);
		}
		break;

	case DCAM_PATH_OUTPUT_RESERVED_ADDR:
		p_addr = (struct dcam_addr *)param;

		if (DCAM_YUV_ADDR_INVALID(p_addr->yaddr,
					  p_addr->uaddr,
					  p_addr->vaddr) &&
		    p_addr->mfd_y == 0) {
			rtn = DCAM_RTN_PATH_ADDR_ERR;
		} else {
			unsigned int output_frame_count = 0;
			struct dcam_frame *frame = NULL;
			struct dcam_size *size;

			frame = &module->path_reserved_frame[path_id];
			output_frame_count = path->output_frame_count;
			frame->yaddr = p_addr->yaddr;
			frame->uaddr = p_addr->uaddr;
			frame->vaddr = p_addr->vaddr;
			frame->yaddr_vir = p_addr->yaddr_vir;
			frame->uaddr_vir = p_addr->uaddr_vir;
			frame->vaddr_vir = p_addr->vaddr_vir;

			frame->pfinfo.dev = &s_pdev->dev;
			frame->pfinfo.mfd[0] = p_addr->mfd_y;
			frame->pfinfo.mfd[1] = p_addr->mfd_u;
			frame->pfinfo.mfd[2] = p_addr->mfd_u;
			rtn = pfiommu_get_sg_table(&frame->pfinfo);
			if (rtn) {
				pr_err("cfg reserved output addr failed!\n");
				rtn = DCAM_RTN_PATH_ADDR_ERR;
				break;
			}

			size = &path->output_size;
			if (path->output_format == DCAM_RAWRGB) {
				frame->pfinfo.size[0] =
					(size->w * size->h * 5) / 4;
				frame->pfinfo.size[1] = 0;
				frame->pfinfo.size[2] = 0;
			} else if (path->output_format == DCAM_YUV420) {
				frame->pfinfo.size[0] = size->w * size->h;
				frame->pfinfo.size[1] = size->w * size->h / 2;
				frame->pfinfo.size[2] = 0;
			} else {
				frame->pfinfo.size[0] = size->w * size->h;
				frame->pfinfo.size[1] = size->w * size->h / 2;
				frame->pfinfo.size[2] = 0;
			}
			frame->pfinfo.offset[0] = frame->yaddr;
			frame->pfinfo.offset[1] = frame->uaddr;
			frame->pfinfo.offset[2] = frame->vaddr;
		}
		break;

	case DCAM_PATH_SRC_SEL:
		src_sel = *(unsigned int *)param;

		if (src_sel >= DCAM_PATH_FROM_NONE) {
			pr_err("DCAM%d, path%d sel src mismatch %d!\n",
			       idx, ffs(path_index) - 1, src_sel);
			rtn = DCAM_RTN_PATH_SRC_ERR;
		} else {
			if ((DCAM_PATH_IDX_1 | DCAM_PATH_IDX_2 |
			     DCAM_PATH_IDX_3) & path_index) {
				path->src_sel = src_sel;
				path->valid_param.src_sel = 1;
			} else {
				pr_info("DCAM%d, path%d no need to sel src!\n",
					idx, ffs(path_index) - 1);
			}
		}
		break;

	case DCAM_PATH_FRAME_BASE_ID:
		base_id = *(unsigned int *)param;

		DCAM_TRACE("DCAM%d: set frame base id 0x%x\n",
			   idx, base_id);
		path->frame_base_id = base_id;
		break;

	case DCAM_PATH_DATA_ENDIAN:
		endian = (struct dcam_endian_sel *)param;

		if (endian->y_endian >= DCAM_ENDIAN_MAX ||
		    endian->uv_endian >= DCAM_ENDIAN_MAX) {
			rtn = DCAM_RTN_PATH_ENDIAN_ERR;
		} else {
			path->data_endian.y_endian = endian->y_endian;
			if ((DCAM_PATH_IDX_1 | DCAM_PATH_IDX_2 |
			     DCAM_PATH_IDX_3) & path_index)
				path->data_endian.uv_endian = endian->uv_endian;
			path->valid_param.data_endian = 1;
		}
		break;

	case DCAM_PATH_ENABLE:
		path->valid = *(unsigned int *)param;
		break;

	case DCAM_PATH_FRAME_TYPE:
		{
			struct dcam_frame *frame = &path->buf_queue.frame[0];
			unsigned int frm_type = *(unsigned int *) param, i;
			int cnt = 0;

			if (DCAM_PATH_IDX_0 & path_index)
				cnt = DCAM_PATH_0_FRM_CNT_MAX;
			else if (DCAM_PATH_IDX_1 & path_index)
				cnt = DCAM_PATH_1_FRM_CNT_MAX;
			else if (DCAM_PATH_IDX_2 & path_index)
				cnt = DCAM_PATH_2_FRM_CNT_MAX;
			else if (DCAM_PATH_IDX_3 & path_index)
				cnt = DCAM_PATH_3_FRM_CNT_MAX;

			DCAM_TRACE("DCAM%d: set frame type 0x%x\n", idx,
				   frm_type);
			for (i = 0; i < cnt; i++)
				(frame + i)->type = frm_type;
			break;
		}

	case DCAM_PATH_FRM_DECI:
		{
			unsigned int deci_factor = *(unsigned int *) param;

			if (deci_factor >= DCAM_FRM_DECI_FAC_MAX) {
				rtn = DCAM_RTN_PATH_FRM_DECI_ERR;
			} else {
				path->frame_deci = deci_factor;
				path->valid_param.frame_deci = 1;
			}
			break;
		}

	case DCAM_PATH_ROT_MODE:
		rot_mode = *(unsigned int *)param;

		if (rot_mode >= DCAM_PATH_FRAME_ROT_MAX) {
			rtn = DCAM_RTN_PATH_FRM_DECI_ERR;
		} else {
			if ((DCAM_PATH_IDX_1 | DCAM_PATH_IDX_2 |
			     DCAM_PATH_IDX_3) & path_index) {
				path->rot_mode = rot_mode;
				path->valid_param.rot_mode = 1;
				DCAM_TRACE("DCAM%d path%d rot = %d\n",
					   idx, ffs(path_index) - 1,
					   path->rot_mode);
			}
		}
		break;

	case DCAM_PATH_SHRINK:
		{
			struct dcam_regular_desc *regular_desc =
				(struct dcam_regular_desc *)param;

			memcpy(&path->regular_desc, param,
			       sizeof(struct dcam_regular_desc));

			if (regular_desc->regular_mode == DCAM_REGULAR_BYPASS)
				path->valid_param.shrink = 0;
			else
				path->valid_param.shrink = 1;
			break;
		}

	case DCAM_PATH_JPEGLS:
		{
			struct dcam_jpegls_desc *jpegls_desc =
				(struct dcam_jpegls_desc *)param;

			memcpy(&path->jpegls_desc, param,
			       sizeof(struct dcam_jpegls_desc));

			if (jpegls_desc->is_jpegls == 1)
				path->valid_param.jpegls = 1;
			break;
		}

	case DCAM_PDAF_CONTROL:
		memcpy(&path->pdaf_ctrl, param,
		       sizeof(struct sprd_pdaf_control));
		path->valid_param.pdaf_ctrl = 1;
		break;

	default:
		pr_err("%s error", __func__);
		break;
	}

	return -rtn;
}

int sprd_dcam_read_registers(enum dcam_id idx, unsigned int *reg_buf,
			     unsigned int *buf_len)
{
	unsigned long reg_addr = DCAM_CFG;

	if (NULL == reg_buf || NULL == buf_len || 0 != (*buf_len % 4))
		return -1;

	while (buf_len != 0 &&
	       reg_addr < JPEGLS_BSM_SIZE_THD_V) {
		*reg_buf++ = DCAM_REG_RD(idx, reg_addr);
		reg_addr += 4;
		*buf_len -= 4;
	}

	*buf_len = reg_addr - DCAM_CFG;

	pr_info("dcam%d registers 0x%lx\n", idx, DCAM_BASE(idx));

	return 0;
}

int sprd_dcam_get_path_id(struct dcam_get_path_id *path_id,
			  unsigned int *channel_id)
{
	int ret = DCAM_RTN_SUCCESS;

	if (NULL == path_id || NULL == channel_id)
		return -1;

	pr_info("DCAM: fourcc 0x%x input %d %d output %d %d\n",
		path_id->fourcc,
		path_id->input_size.w, path_id->input_size.h,
		path_id->output_size.w, path_id->output_size.h);
	pr_info("DCAM: input_trim %d %d %d %d need_isp %d rt_refocus %d\n",
		path_id->input_trim.x, path_id->input_trim.y,
		path_id->input_trim.w, path_id->input_trim.h,
		path_id->need_isp, path_id->rt_refocus);
	pr_info("DCAM: is_path_work %d %d %d %d\n",
		path_id->is_path_work[DCAM_PATH0],
		path_id->is_path_work[DCAM_PATH1],
		path_id->is_path_work[DCAM_PATH2],
		path_id->is_path_work[DCAM_PATH3]);

	if (path_id->need_isp_tool)
		*channel_id = DCAM_PATH0;
	else if (path_id->fourcc == V4L2_PIX_FMT_GREY &&
		 !path_id->is_path_work[DCAM_PATH0])
		*channel_id = DCAM_PATH0;
	else if (path_id->fourcc == V4L2_PIX_FMT_JPEG &&
		 !path_id->is_path_work[DCAM_PATH0])
		*channel_id = DCAM_PATH0;
	else if (path_id->rt_refocus &&
		 !path_id->is_path_work[DCAM_PATH0])
		*channel_id = DCAM_PATH0;
	else if (path_id->output_size.w <= DCAM_PATH1_LINE_BUF_LENGTH &&
		 !path_id->is_path_work[DCAM_PATH1])
		*channel_id = DCAM_PATH1;
	else if (path_id->output_size.w <= DCAM_PATH3_LINE_BUF_LENGTH &&
		 !path_id->is_path_work[DCAM_PATH3])
		*channel_id = DCAM_PATH3;
	else if (path_id->output_size.w <= DCAM_PATH2_LINE_BUF_LENGTH &&
		 !path_id->is_path_work[DCAM_PATH2])
		*channel_id = DCAM_PATH2;
	else
		*channel_id = DCAM_PATH0;

	pr_info("path id %d\n", *channel_id);

	return ret;
}

int sprd_dcam_get_path_capability(struct dcam_path_capability *capacity)
{
	if (capacity == NULL)
		return -1;

	capacity->count = 3;
	capacity->path_info[DCAM_PATH0].line_buf = 0;
	capacity->path_info[DCAM_PATH0].support_yuv = 0;
	capacity->path_info[DCAM_PATH0].support_raw = 1;
	capacity->path_info[DCAM_PATH0].support_jpeg = 1;
	capacity->path_info[DCAM_PATH0].support_scaling = 0;
	capacity->path_info[DCAM_PATH0].support_trim = 0;
	capacity->path_info[DCAM_PATH0].is_scaleing_path = 0;

	capacity->path_info[DCAM_PATH1].line_buf = DCAM_PATH1_LINE_BUF_LENGTH;
	capacity->path_info[DCAM_PATH1].support_yuv = 1;
	capacity->path_info[DCAM_PATH1].support_raw = 0;
	capacity->path_info[DCAM_PATH1].support_jpeg = 0;
	capacity->path_info[DCAM_PATH1].support_scaling = 1;
	capacity->path_info[DCAM_PATH1].support_trim = 1;
	capacity->path_info[DCAM_PATH1].is_scaleing_path = 0;

	capacity->path_info[DCAM_PATH2].line_buf = DCAM_PATH2_LINE_BUF_LENGTH;
	capacity->path_info[DCAM_PATH2].support_yuv = 1;
	capacity->path_info[DCAM_PATH2].support_raw = 0;
	capacity->path_info[DCAM_PATH2].support_jpeg = 0;
	capacity->path_info[DCAM_PATH2].support_scaling = 1;
	capacity->path_info[DCAM_PATH2].support_trim = 1;
	capacity->path_info[DCAM_PATH2].is_scaleing_path = 0;

	capacity->path_info[DCAM_PATH3].line_buf = DCAM_PATH3_LINE_BUF_LENGTH;
	capacity->path_info[DCAM_PATH3].support_yuv = 1;
	capacity->path_info[DCAM_PATH3].support_raw = 0;
	capacity->path_info[DCAM_PATH3].support_jpeg = 0;
	capacity->path_info[DCAM_PATH3].support_scaling = 1;
	capacity->path_info[DCAM_PATH2].support_trim = 1;
	capacity->path_info[DCAM_PATH3].is_scaleing_path = 0;

	return 0;
}

int sprd_dcam_stop_sc_coeff(enum dcam_id idx)
{
	unsigned int zoom_mode;

	if (DCAM_ADDR_INVALID(s_dcam_sc_array[idx])) {
		pr_err("zero pointer\n");
		return -EFAULT;
	}

	zoom_mode = s_dcam_sc_array[idx]->is_smooth_zoom;
	s_dcam_sc_array[idx]->is_smooth_zoom = zoom_mode;
	s_dcam_sc_array[idx]->valid_cnt = 0;
	memset(&s_dcam_sc_array[idx]->scaling_coeff_queue, 0,
	       DCAM_SC_COEFF_BUF_COUNT * sizeof(struct dcam_sc_coeff *));

	return 0;
}

int sprd_dcam_parse_clk(struct platform_device *pdev)
{
	dcam0_clk = devm_clk_get(&pdev->dev, "clk_dcam0");
	if (IS_ERR(dcam0_clk))
		return PTR_ERR(dcam0_clk);

	dcam0_clk_parent = devm_clk_get(&pdev->dev, "clk_dcam0_parent");
	if (IS_ERR(dcam0_clk_parent))
		return PTR_ERR(dcam0_clk_parent);

	dcam1_clk = devm_clk_get(&pdev->dev, "clk_dcam1");
	if (IS_ERR(dcam1_clk))
		return PTR_ERR(dcam1_clk);

	dcam1_clk_parent = devm_clk_get(&pdev->dev, "clk_dcam1_parent");
	if (IS_ERR(dcam1_clk_parent))
		return PTR_ERR(dcam1_clk_parent);

	dcam0_if_clk = devm_clk_get(&pdev->dev, "clk_dcam0_if");
	if (IS_ERR(dcam0_if_clk))
		return PTR_ERR(dcam0_if_clk);

	dcam0_if_clk_parent = devm_clk_get(&pdev->dev, "clk_dcam0_if_parent");
	if (IS_ERR(dcam0_if_clk_parent))
		return PTR_ERR(dcam0_if_clk_parent);

	dcam0_eb = devm_clk_get(&pdev->dev, "dcam0_eb");
	if (IS_ERR(dcam0_eb))
		return PTR_ERR(dcam0_eb);

	dcam0_axi_eb = devm_clk_get(&pdev->dev, "dcam0_axi_eb");
	if (IS_ERR(dcam0_axi_eb))
		return PTR_ERR(dcam0_axi_eb);

	dcam1_eb = devm_clk_get(&pdev->dev, "dcam1_eb");
	if (IS_ERR(dcam1_eb))
		return PTR_ERR(dcam1_eb);

	dcam1_axi_eb = devm_clk_get(&pdev->dev, "dcam1_axi_eb");
	if (IS_ERR(dcam1_axi_eb))
		return PTR_ERR(dcam1_axi_eb);

	d0if_in_d_eb = devm_clk_get(&pdev->dev, "d0if_in_d_eb");
	if (IS_ERR(d0if_in_d_eb))
		return PTR_ERR(d0if_in_d_eb);

	d1if_in_d_eb = devm_clk_get(&pdev->dev, "d1if_in_d_eb");
	if (IS_ERR(d1if_in_d_eb))
		return PTR_ERR(d1if_in_d_eb);

	cam_ahb_gpr = syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
						      "sprd,syscon-cam-ahb");
	if (IS_ERR(cam_ahb_gpr))
		return PTR_ERR(cam_ahb_gpr);

	dcam0_clk_default = clk_get_parent(dcam0_clk);
	if (IS_ERR(dcam0_clk_default))
		return PTR_ERR(dcam0_clk_default);

	dcam1_clk_default = clk_get_parent(dcam1_clk);
	if (IS_ERR(dcam1_clk_default))
		return PTR_ERR(dcam1_clk_default);

	dcam0_if_clk_default = clk_get_parent(dcam0_if_clk);
	if (IS_ERR(dcam0_if_clk_default))
		return PTR_ERR(dcam0_if_clk_default);

	return 0;
}

int sprd_dcam_parse_irq(struct platform_device *pdev)
{
	int i, ret = 0, irq;

	for (i = 0; i < DCAM_MAX_COUNT; i++) {
		irq = platform_get_irq(pdev, i);
		pr_info("irq %d %d\n", i, irq);
		if (irq <= 0) {
			pr_err("failed to get IRQ\n");
			ret = -ENXIO;
			goto exit;
		}
		s_dcam_irq[i] = irq;

		ret = devm_request_irq(&pdev->dev, irq, dcam_isr_root,
				       IRQF_SHARED, "DCAM",
				       (void *)&s_dcam_irq[i]);
		if (ret < 0) {
			pr_err("failed to install IRQ %d\n", ret);
			goto exit;
		}
	}

exit:
	return ret;
}

int sprd_dcam_parse_regbase(struct platform_device *pdev)
{
	int i;
	void __iomem *reg_base;
	struct resource *res = NULL;

	for (i = 0; i < DCAM_MAX_COUNT; i++) {
		res = platform_get_resource(pdev, IORESOURCE_MEM, i);
		if (!res)
			return -ENODEV;

		reg_base = devm_ioremap_nocache(&pdev->dev, res->start,
						resource_size(res));
		if (IS_ERR(reg_base))
			return PTR_ERR(reg_base);
		s_dcam_regbase[i] = (unsigned long)reg_base;
	}

	return 0;
}
