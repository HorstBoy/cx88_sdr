// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * cx88_sdr_v4l2.c - CX2388x SDR V4L2 Driver
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

#include <linux/pci.h>
#include <linux/videodev2.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>

#include "cx88_sdr.h"

#define	CX88SDR_V4L2_NAME	"CX2388x SDR V4L2"

/* The base for the cx88_sdr driver controls. Total of 16 controls are reserved
 * for this driver */
#define V4L2_CID_USER_CX88SDR_BASE	(V4L2_CID_USER_BASE + 0x1f10)

enum {
	V4L2_CID_CX88SDR_INPUT = (V4L2_CID_USER_CX88SDR_BASE + 0),
	V4L2_CID_CX88SDR_RATE,
};

struct cx88sdr_fh {
	struct v4l2_fh fh;
	struct cx88sdr_dev *dev;
};

static int cx88sdr_open(struct file *file)
{
	struct video_device *vdev = video_devdata(file);
	struct cx88sdr_dev *dev = container_of(vdev, struct cx88sdr_dev, vdev);
	struct cx88sdr_fh *fh;

	fh = kzalloc(sizeof(*fh), GFP_KERNEL);
	v4l2_fh_init(&fh->fh, vdev);

	fh->dev = dev;
	file->private_data = &fh->fh;
	v4l2_fh_add(&fh->fh);

	dev->initial_page = mmio_ioread32(dev, MO_VBI_GPCNT) - 1;
	mmio_iowrite32(dev, MO_PCI_INTMSK, 1);
	return 0;
}

static int cx88sdr_release(struct file *file)
{
	struct v4l2_fh *vfh = file->private_data;
	struct cx88sdr_fh *fh = container_of(vfh, struct cx88sdr_fh, fh);
	struct cx88sdr_dev *dev = fh->dev;

	mmio_iowrite32(dev, MO_PCI_INTMSK, 0);

	v4l2_fh_del(&fh->fh);
	v4l2_fh_exit(&fh->fh);
	kfree(fh);
	return 0;
}

static ssize_t cx88sdr_read(struct file *file, char __user *buf, size_t size,
			    loff_t *pos)
{
	struct v4l2_fh *vfh = file->private_data;
	struct cx88sdr_fh *fh = container_of(vfh, struct cx88sdr_fh, fh);
	struct cx88sdr_dev *dev = fh->dev;
	ssize_t result = 0;
	uint32_t gp_cnt, pnum;

	pnum = (dev->initial_page +
	       ((*pos % VBI_DMA_SIZE) >> PAGE_SHIFT)) % VBI_DMA_PAGES;

	gp_cnt = mmio_ioread32(dev, MO_VBI_GPCNT);
	gp_cnt = (!gp_cnt) ? (VBI_DMA_PAGES - 1) : (gp_cnt - 1);

	if ((pnum == gp_cnt) && (file->f_flags & O_NONBLOCK))
		return result;

	while (size) {
		while ((size > 0) && (pnum != gp_cnt)) {
			uint32_t len;

			/* Handle partial pages */
			len = (*pos % PAGE_SIZE) ?
			      (PAGE_SIZE - (*pos % PAGE_SIZE)) : PAGE_SIZE;
			if (len > size)
				len = size;
			if (copy_to_user(buf, dev->pgvec_virt[pnum] +
					(*pos % PAGE_SIZE), len))
				return -EFAULT;

			memset(dev->pgvec_virt[pnum] + (*pos % PAGE_SIZE), 0, len);

			result += len;
			buf += len;
			*pos += len;
			size -= len;
			pnum = (dev->initial_page +
			       ((*pos % VBI_DMA_SIZE) >> PAGE_SHIFT)) % VBI_DMA_PAGES;
		}
		if (size) {
			if (file->f_flags & O_NONBLOCK)
				return result;

			gp_cnt = mmio_ioread32(dev, MO_VBI_GPCNT);
			gp_cnt = (!gp_cnt) ? (VBI_DMA_PAGES - 1) : (gp_cnt - 1);
		}
	}
	return result;
}

static const struct v4l2_file_operations cx88sdr_fops = {
	.owner		= THIS_MODULE,
	.open		= cx88sdr_open,
	.release	= cx88sdr_release,
	.read		= cx88sdr_read,
	.poll		= v4l2_ctrl_poll,
	.unlocked_ioctl	= video_ioctl2,
};

static int cx88sdr_querycap(struct file *file, void *priv, struct v4l2_capability *cap)
{
	struct cx88sdr_dev *dev = video_drvdata(file);

	snprintf(cap->bus_info, sizeof(cap->bus_info), "PCI:%s", pci_name(dev->pdev));
	strscpy(cap->card, CX88SDR_DRV_NAME, sizeof(cap->card));
	strscpy(cap->driver, KBUILD_MODNAME, sizeof(cap->driver));
	return 0;
}

static int cx88sdr_enum_fmt_sdr(struct file *file, void *priv, struct v4l2_fmtdesc *f)
{
	if (f->index > 1)
		return -EINVAL;

	if (f->index == 0)
		f->pixelformat = V4L2_SDR_FMT_CU8;
	else if (f->index == 1)
		f->pixelformat = V4L2_SDR_FMT_CU16LE;
	return 0;
}

static int cx88sdr_try_fmt_sdr(struct file *file, void *priv, struct v4l2_format *f)
{
	memset(f->fmt.sdr.reserved, 0, sizeof(f->fmt.sdr.reserved));
	switch (f->fmt.sdr.pixelformat) {
	case V4L2_SDR_FMT_CU8:
		f->fmt.sdr.buffersize = 1;
		break;
	case V4L2_SDR_FMT_CU16LE:
		f->fmt.sdr.buffersize = 2;
		break;
	default:
		f->fmt.sdr.pixelformat = V4L2_SDR_FMT_CU8;
		f->fmt.sdr.buffersize = 1;
		break;
	}
	return 0;
}

static int cx88sdr_g_fmt_sdr(struct file *file, void *priv, struct v4l2_format *f)
{
	struct cx88sdr_dev *dev = video_drvdata(file);

	memset(f->fmt.sdr.reserved, 0, sizeof(f->fmt.sdr.reserved));
	f->fmt.sdr.pixelformat = dev->pixelformat;
	f->fmt.sdr.buffersize = dev->buffersize;
	return 0;
}

static int cx88sdr_s_fmt_sdr(struct file *file, void *priv, struct v4l2_format *f)
{
	struct cx88sdr_dev *dev = video_drvdata(file);

	memset(f->fmt.sdr.reserved, 0, sizeof(f->fmt.sdr.reserved));

	switch (f->fmt.sdr.pixelformat) {
	case V4L2_SDR_FMT_CU8:
		dev->pixelformat = V4L2_SDR_FMT_CU8;
		dev->buffersize = 1;
		f->fmt.sdr.buffersize = 1;
		break;
	case V4L2_SDR_FMT_CU16LE:
		dev->pixelformat = V4L2_SDR_FMT_CU16LE;
		dev->buffersize = 2;
		f->fmt.sdr.buffersize = 2;
		break;
	default:
		dev->pixelformat = V4L2_SDR_FMT_CU8;
		dev->buffersize = 1;
		f->fmt.sdr.pixelformat = V4L2_SDR_FMT_CU8;
		f->fmt.sdr.buffersize = 1;
		break;
	}
	return 0;
}

static const struct v4l2_ioctl_ops cx88sdr_ioctl_ops = {
	.vidioc_querycap		= cx88sdr_querycap,
	.vidioc_enum_fmt_sdr_cap	= cx88sdr_enum_fmt_sdr, /* Fictitious */
	.vidioc_try_fmt_sdr_cap		= cx88sdr_try_fmt_sdr, /* Fictitious */
	.vidioc_g_fmt_sdr_cap		= cx88sdr_g_fmt_sdr, /* Fictitious */
	.vidioc_s_fmt_sdr_cap		= cx88sdr_s_fmt_sdr, /* Fictitious */
	.vidioc_log_status		= v4l2_ctrl_log_status,
	.vidioc_subscribe_event		= v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event	= v4l2_event_unsubscribe,
};

const struct video_device cx88sdr_template = {
	.device_caps	= (V4L2_CAP_SDR_CAPTURE | V4L2_CAP_READWRITE),
	.fops		= &cx88sdr_fops,
	.ioctl_ops	= &cx88sdr_ioctl_ops,
	.name		= CX88SDR_V4L2_NAME,
	.release	= video_device_release_empty,
};

static void cx88sdr_gain_set(struct cx88sdr_dev *dev)
{
	mmio_iowrite32(dev, MO_AGC_GAIN_ADJ4, (1 << 23) | (dev->gain << 16) |
					      (0xff << 8));
}

void cx88sdr_agc_setup(struct cx88sdr_dev *dev)
{
	mmio_iowrite32(dev, MO_AGC_BACK_VBI, (1 << 25) | (0x100 << 16) | 0xfff);
	mmio_iowrite32(dev, MO_AGC_SYNC_SLICER, 0x0);
	mmio_iowrite32(dev, MO_AGC_SYNC_TIP2, (0x20 << 17) | 0xf);
	mmio_iowrite32(dev, MO_AGC_SYNC_TIP3, (0x1e48 << 16) | (0xff << 8) | 0x8);
	mmio_iowrite32(dev, MO_AGC_GAIN_ADJ2, (0x20 << 17) | 0xf);
	mmio_iowrite32(dev, MO_AGC_GAIN_ADJ3, (0x28 << 16) | (0x28 << 8) | 0x50);
	cx88sdr_gain_set(dev);
}

void cx88sdr_input_set(struct cx88sdr_dev *dev)
{
	mmio_iowrite32(dev, MO_INPUT_FORMAT, (1 << 16) | (dev->input << 14) |
					     (1 << 13) | (1 << 4) | 0x1);
}

void cx88sdr_rate_set(struct cx88sdr_dev *dev)
{
	switch (dev->rate) {
	/* 8-bit */
	case RATE_4FSC_8BIT: /* 14.318182 MHz */
		mmio_iowrite32(dev, MO_CAPTURE_CTRL, (1 << 6) | (3 << 1));
		mmio_iowrite32(dev, MO_SCONV_REG, (1 << 17) * 2); // Freq / 2
		mmio_iowrite32(dev, MO_PLL_REG, (1 << 26) | (0x14 << 20)); // Freq / 5 / 8 * 20
		break;
	case RATE_8FSC_8BIT: /* 28.636363 MHz */
		mmio_iowrite32(dev, MO_CAPTURE_CTRL, (1 << 6) | (3 << 1));
		mmio_iowrite32(dev, MO_SCONV_REG, (1 << 17)); // Freq
		mmio_iowrite32(dev, MO_PLL_REG, (0x10 << 20)); // Freq / 2 / 8 * 16
		break;
	case RATE_10FSC_8BIT: /* 35.795454 MHz */
		mmio_iowrite32(dev, MO_CAPTURE_CTRL, (1 << 6) | (3 << 1));
		mmio_iowrite32(dev, MO_SCONV_REG, (1 << 17) * 4 / 5); // Freq * 5 / 4
		mmio_iowrite32(dev, MO_PLL_REG, (0x14 << 20)); // Freq / 2 / 8 * 20
		break;
	/* 16-bit */
	case RATE_2FSC_16BIT: /* 7.159091 MHz */
		mmio_iowrite32(dev, MO_CAPTURE_CTRL, (1 << 6) | (1 << 5) | (3 << 1));
		mmio_iowrite32(dev, MO_SCONV_REG, (1 << 17) * 2); // Freq / 2
		mmio_iowrite32(dev, MO_PLL_REG, (1 << 26) | (0x14 << 20)); // Freq / 5 / 8 * 20
		break;
	case RATE_4FSC_16BIT: /* 14.318182 MHz */
		mmio_iowrite32(dev, MO_CAPTURE_CTRL, (1 << 6) | (1 << 5) | (3 << 1));
		mmio_iowrite32(dev, MO_SCONV_REG, (1 << 17)); // Freq
		mmio_iowrite32(dev, MO_PLL_REG, (0x10 << 20)); // Freq / 2 / 8 * 16
		break;
	case RATE_5FSC_16BIT: /* 17.897727 MHz */
		mmio_iowrite32(dev, MO_CAPTURE_CTRL, (1 << 6) | (1 << 5) | (3 << 1));
		mmio_iowrite32(dev, MO_SCONV_REG, (1 << 17) * 4 / 5); // Freq * 5 / 4
		mmio_iowrite32(dev, MO_PLL_REG, (0x14 << 20)); // Freq / 2 / 8 * 20
		break;
	}
}

static int cx88sdr_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct cx88sdr_dev *dev = container_of(ctrl->handler,
					       struct cx88sdr_dev, ctrl_handler);

	switch (ctrl->id) {
	case V4L2_CID_GAIN:
		dev->gain = ctrl->val;
		cx88sdr_gain_set(dev);
		break;
	case V4L2_CID_CX88SDR_INPUT:
		dev->input = ctrl->val;
		cx88sdr_input_set(dev);
		break;
	case V4L2_CID_CX88SDR_RATE:
		dev->rate = ctrl->val;
		cx88sdr_rate_set(dev);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

const struct v4l2_ctrl_ops cx88sdr_ctrl_ops = {
	.s_ctrl = cx88sdr_s_ctrl,
};

static const char * const cx88sdr_ctrl_input_menu_strings[] = {
	"Input 1",
	"Input 2",
	"Input 3",
	"Input 4",
	NULL,
};

const struct v4l2_ctrl_config cx88sdr_ctrl_input = {
	.ops	= &cx88sdr_ctrl_ops,
	.id	= V4L2_CID_CX88SDR_INPUT,
	.name	= "Input",
	.type	= V4L2_CTRL_TYPE_MENU,
	.min	= 0,
	.max	= 3,
	.def	= 1,
	.qmenu	= cx88sdr_ctrl_input_menu_strings,
};

static const char * const cx88sdr_ctrl_rate_menu_strings[] = {
	"14.318182 MHz, 8-bit",
	"28.636363 MHz, 8-bit",
	"35.795454 MHz, 8-bit",
	" 7.159091 MHz, 16-bit",
	"14.318182 MHz, 16-bit",
	"17.897727 MHz, 16-bit",
	NULL,
};

const struct v4l2_ctrl_config cx88sdr_ctrl_rate = {
	.ops	= &cx88sdr_ctrl_ops,
	.id	= V4L2_CID_CX88SDR_RATE,
	.name	= "Sampling Rate",
	.type	= V4L2_CTRL_TYPE_MENU,
	.min	= 0,
	.max	= 5,
	.def	= 1,
	.qmenu	= cx88sdr_ctrl_rate_menu_strings,
};
