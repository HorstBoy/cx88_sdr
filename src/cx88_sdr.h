/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * cx88_sdr.h - CX2388x SDR V4L2 Driver
 * Copyright (c) 2020 Jorge Maidana <jorgem.seq@gmail.com>
 *
 * This driver is a derivative of:
 *
 * device driver for Conexant 2388x based TV cards
 * Copyright (c) 2003 Gerd Knorr <kraxel@bytesex.org> [SuSE Labs]
 *
 * device driver for Conexant 2388x based TV cards
 * Copyright (c) 2005-2006 Mauro Carvalho Chehab <mchehab@kernel.org>
 *
 * cxadc.c - CX2388x ADC DMA driver for Linux 2.6.18 version 0.3
 * Copyright (c) 2005-2007 Hew How Chee <how_chee@yahoo.com>
 *
 * cxadc.c - CX2388x ADC DMA driver for Linux 3.x version 0.5
 * Copyright (c) 2013-2015 Chad Page <Chad.Page@gmail.com>
 */

#ifndef CX88SDR_H
#define CX88SDR_H

#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>

#define	CX88SDR_DRV_NAME		"CX2388x SDR"
#define	CX88SDR_MAX_CARDS		32

#define INTERRUPT_MASK			0x018888

#define MO_DEV_CNTRL2			0x200034 // Device control
#define MO_PCI_INTMSK			0x200040 // PCI interrupt mask
#define MO_VID_INTMSK			0x200050
#define MO_VID_INTSTAT			0x200054
#define MO_DMA24_PTR2			0x3000cc // {24}RW* DMA Tab Ptr : Ch#24
#define MO_DMA24_CNT1			0x30010c // {11}RW* DMA Buffer Size : Ch#24
#define MO_DMA24_CNT2			0x30014c // {11}RW* DMA Table Size : Ch#24
#define MO_VBI_GPCNT			0x31c02c // {16}RO VBI general purpose counter
#define MO_VID_DMACNTRL			0x31c040 // {8}RW Video DMA control
#define MO_INPUT_FORMAT			0x310104
#define MO_CONTR_BRIGHT			0x310110
#define MO_OUTPUT_FORMAT		0x310164
#define MO_PLL_REG			0x310168 // PLL register
#define MO_SCONV_REG			0x310170 // sample rate conversion register
#define MO_CAPTURE_CTRL			0x310180 // capture control
#define MO_COLOR_CTRL			0x310184
#define MO_VBI_PACKET			0x310188 // vbi packet size / delay
#define MO_AGC_BACK_VBI			0x310200
#define MO_AGC_SYNC_SLICER		0x310204
#define MO_AGC_SYNC_TIP2		0x31020c
#define MO_AGC_SYNC_TIP3		0x310210
#define MO_AGC_GAIN_ADJ2		0x310218
#define MO_AGC_GAIN_ADJ3		0x31021c
#define MO_AGC_GAIN_ADJ4		0x310220
#define MO_AFECFG_IO			0x35c04c

#define CX_SRAM_BASE			0x180000
#define CHN24_CMDS_BASE			0x180100
#define RISC_INST_QUEUE			(CX_SRAM_BASE + 0x0800)
#define CDT_BASE			(CX_SRAM_BASE + 0x1000)
#define RISC_BUFFER_BASE		(CX_SRAM_BASE + 0x2000)
#define CLUSTER_BUFFER_BASE		(CX_SRAM_BASE + 0x4000)

#define RISC_WRITE			0x10000000
#define RISC_JUMP			0x70000000
#define RISC_SYNC			0x80000000

#define CLUSTER_BUF_NUM			8
#define CLUSTER_BUF_SIZE		2048

#define VBI_DMA_SIZE			(64 * 1024 * 1024)
#define VBI_DMA_PAGES			(VBI_DMA_SIZE >> PAGE_SHIFT)
#define VBI_DMA_BUF_NUM			(VBI_DMA_SIZE / CLUSTER_BUF_SIZE)

enum {
	VMUX_00,
	VMUX_01,
	VMUX_02,
	VMUX_03,
};

enum {
	RATE_4FSC_8BIT,
	RATE_8FSC_8BIT,
	RATE_10FSC_8BIT,
	RATE_2FSC_16BIT,
	RATE_4FSC_16BIT,
	RATE_5FSC_16BIT
};

struct cx88sdr_dev {
	struct	list_head		devlist;
	int				irq;
	int				nr;
	char				name[32];

	/* IO */
	struct	pci_dev			*pdev;
	dma_addr_t			risc_inst_phy;
	dma_addr_t			pgvec_phy[VBI_DMA_PAGES + 1];
	uint32_t	__iomem		*mmio;
	uint32_t			risc_inst_buff_size;
	uint32_t			*risc_inst_virt;
	uint32_t			initial_page;
	void				*pgvec_virt[VBI_DMA_PAGES + 1];
	int				pci_lat;

	/* V4L2 */
	struct	v4l2_device		v4l2_dev;
	struct	v4l2_ctrl_handler	ctrl_handler;
	struct	video_device		vdev;
	struct	mutex			vdev_mlock;
	u32				gain;
	u32				input;
	u32				rate;

	/* V4L2 SDR */
	u32				pixelformat;
	u32				buffersize;
};

/* Helpers */
static inline uint32_t mmio_ioread32(struct cx88sdr_dev *dev, uint32_t reg)
{
	return ioread32(dev->mmio + ((reg) >> 2));
}

static inline void mmio_iowrite32(struct cx88sdr_dev *dev, uint32_t reg, uint32_t val)
{
	iowrite32((val), dev->mmio + ((reg) >> 2));
}

#define cx88sdr_pr_info(fmt, ...)	pr_info(KBUILD_MODNAME " %s: " fmt,		\
						pci_name(dev->pdev), ##__VA_ARGS__)
#define cx88sdr_pr_err(fmt, ...)	pr_err(KBUILD_MODNAME " %s: " fmt,		\
						pci_name(dev->pdev), ##__VA_ARGS__)

/* cx88sdr_v4l2.c */
extern const struct v4l2_ctrl_ops cx88sdr_ctrl_ops;
extern const struct v4l2_ctrl_config cx88sdr_ctrl_input;
extern const struct v4l2_ctrl_config cx88sdr_ctrl_rate;
extern const struct video_device cx88sdr_template;

void cx88sdr_rate_set(struct cx88sdr_dev *dev);
void cx88sdr_agc_setup(struct cx88sdr_dev *dev);
void cx88sdr_input_set(struct cx88sdr_dev *dev);

#endif
