// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * cx88_sdr_core.c - CX2388x SDR V4L2 Driver
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

#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/videodev2.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>

#include "cx88_sdr.h"

MODULE_DESCRIPTION("CX2388x SDR V4L2 Driver");
MODULE_AUTHOR("Jorge Maidana <jorgem.seq@gmail.com>");
MODULE_LICENSE("GPL v2");

static int latency = 248;
module_param(latency, int, 0);
MODULE_PARM_DESC(latency, "Set PCI latency timer");

static LIST_HEAD(cx88sdr_devlist);
static DEFINE_MUTEX(cx88sdr_devlist_lock);

static int cx88sdr_devcount;

static void cx88sdr_pci_lat_set(struct cx88sdr_dev *dev)
{
	u8 lat;

	latency = clamp(latency, 32, 248);
	pci_write_config_byte(dev->pdev, PCI_LATENCY_TIMER, latency);
	pci_read_config_byte(dev->pdev, PCI_LATENCY_TIMER, &lat);
	dev->pci_lat = lat;
}

static void cx88sdr_shutdown(struct cx88sdr_dev *dev)
{
	/* Disable RISC Controller and IRQs */
	mmio_iowrite32(dev, MO_DEV_CNTRL2, 0);

	/* Stop DMA transfers */
	mmio_iowrite32(dev, MO_VID_DMACNTRL, 0);

	/* Stop interrupts */
	mmio_iowrite32(dev, MO_PCI_INTMSK, 0);
	mmio_iowrite32(dev, MO_VID_INTMSK, 0);

	/* Stop capturing */
	mmio_iowrite32(dev, MO_CAPTURE_CTRL, 0);

	mmio_iowrite32(dev, MO_VID_INTSTAT, ~0u);
}

static void cx88sdr_sram_setup(struct cx88sdr_dev *dev, uint32_t numbuf,
			       uint32_t buffsize, uint32_t buffptr,
			       uint32_t cdtptr)
{
	int i;
	uint32_t buff, cdt;

	buff = buffptr;
	cdt = cdtptr;

	/* Write CDT */
	for (i = 0; i < numbuf; i++, buff += buffsize)
		mmio_iowrite32(dev, cdt + 16 * i, buff);

	/* Write CMDS */
	mmio_iowrite32(dev, CHN24_CMDS_BASE + 0, dev->risc_inst_phy);
	mmio_iowrite32(dev, CHN24_CMDS_BASE + 4, cdt);
	mmio_iowrite32(dev, CHN24_CMDS_BASE + 8, numbuf * 2);
	mmio_iowrite32(dev, CHN24_CMDS_BASE + 12, RISC_INST_QUEUE);
	mmio_iowrite32(dev, CHN24_CMDS_BASE + 16, 0x40);

	/* Fill registers */
	mmio_iowrite32(dev, MO_DMA24_PTR2, cdt);
	mmio_iowrite32(dev, MO_DMA24_CNT1, (buffsize >> 3) - 1);
	mmio_iowrite32(dev, MO_DMA24_CNT2, numbuf * 2);
}

static void cx88sdr_adc_setup(struct cx88sdr_dev *dev)
{
	mmio_iowrite32(dev, MO_VID_INTSTAT, mmio_ioread32(dev, MO_VID_INTSTAT));

	mmio_iowrite32(dev, MO_OUTPUT_FORMAT, 0xf);
	mmio_iowrite32(dev, MO_CONTR_BRIGHT, 0xff00);
	mmio_iowrite32(dev, MO_COLOR_CTRL, (0xe << 4) | 0xe);
	mmio_iowrite32(dev, MO_VBI_PACKET, (CLUSTER_BUF_SIZE << 17) | (2 << 11));

	/* Power down audio and chroma DAC+ADC */
	mmio_iowrite32(dev, MO_AFECFG_IO, 0x12);

	/* Start DMA */
	mmio_iowrite32(dev, MO_DEV_CNTRL2, (1 << 5));
	mmio_iowrite32(dev, MO_VID_DMACNTRL, (1 << 7) | (1 << 3));
}

static int cx88sdr_alloc_risc_inst_buffer(struct cx88sdr_dev *dev)
{
	/* Add 1 page for sync instructions and jump */
	dev->risc_inst_buff_size = VBI_DMA_BUF_NUM * CLUSTER_BUF_NUM + PAGE_SIZE;
	dev->risc_inst_virt = dma_alloc_coherent(&dev->pdev->dev,
						 dev->risc_inst_buff_size,
						 &dev->risc_inst_phy, GFP_KERNEL);
	if (!dev->risc_inst_virt)
		return -ENOMEM;

	memset(dev->risc_inst_virt, 0, dev->risc_inst_buff_size);

	cx88sdr_pr_info("RISC Buffer size %uKiB\n",
			dev->risc_inst_buff_size / 1024);
	return 0;
}

static void cx88sdr_free_risc_inst_buffer(struct cx88sdr_dev *dev)
{
	if (dev->risc_inst_virt)
		dma_free_coherent(&dev->pdev->dev, dev->risc_inst_buff_size,
				  dev->risc_inst_virt, dev->risc_inst_phy);
}

static int cx88sdr_alloc_dma_buffer(struct cx88sdr_dev *dev)
{
	int i;
	u32 dma_size = 0;

	for (i = 0; i < (VBI_DMA_PAGES + 1); i++) {
		dev->pgvec_virt[i] = NULL;
		dev->pgvec_phy[i] = 0;
	}

	for (i = 0; i < VBI_DMA_PAGES; i++) {
		dma_addr_t dma_handle;

		dev->pgvec_virt[i] = dma_alloc_coherent(&dev->pdev->dev,
							PAGE_SIZE, &dma_handle,
							GFP_KERNEL);
		if (!dev->pgvec_virt[i])
			return -ENOMEM;
		dev->pgvec_phy[i] = dma_handle;
		dma_size += PAGE_SIZE;
	}

	cx88sdr_pr_info("DMA size %uMiB\n", dma_size / 1024 / 1024);
	return 0;
}

static void cx88sdr_free_dma_buffer(struct cx88sdr_dev *dev)
{
	int i;

	for (i = 0; i < VBI_DMA_PAGES; i++) {
		if (dev->pgvec_virt[i])
			dma_free_coherent(&dev->pdev->dev, PAGE_SIZE,
					  dev->pgvec_virt[i], dev->pgvec_phy[i]);
	}
}

static void cx88sdr_make_risc_instructions(struct cx88sdr_dev *dev)
{
	int i, irqt = 0;
	uint32_t dma_addr, loop_addr;
	uint32_t *pp = dev->risc_inst_virt;

	loop_addr = dev->risc_inst_phy + 4;
	*pp++ = RISC_SYNC | (3 << 16);

	for (i = 0; i < VBI_DMA_PAGES; i++) {
		irqt++;
		irqt &= 0x1ff;
		*pp++ = RISC_WRITE | CLUSTER_BUF_SIZE | (3 << 26);
		dma_addr = dev->pgvec_phy[i];
		*pp++ = dma_addr;
		*pp++ = RISC_WRITE | CLUSTER_BUF_SIZE | (3 << 26) |
		       (((irqt == 0) ? 1 : 0) << 24) |
		       (((i < VBI_DMA_PAGES - 1) ? 1 : 3) << 16);
		*pp++ = dma_addr + CLUSTER_BUF_SIZE;
	}
	*pp++ = RISC_JUMP;
	*pp++ = loop_addr;

	cx88sdr_pr_info("RISC Instructions using %uKiB of Buffer\n",
		       (uint32_t)(((void *)pp - (void *)dev->risc_inst_virt) / 1024));
}

static irqreturn_t cx88sdr_irq(int irq, void *dev_id)
{
	struct cx88sdr_dev *dev = dev_id;
	int i, handled = 0;
	uint32_t mask, status;

	for (i = 0; i < 10; i++) {
		status = mmio_ioread32(dev, MO_VID_INTSTAT);
		mask = mmio_ioread32(dev, MO_VID_INTMSK);
		if ((status & mask) == 0)
			goto out;
		mmio_iowrite32(dev, MO_VID_INTSTAT, status);
		handled = 1;
	}

out:
	return IRQ_RETVAL(handled);
}

static int cx88sdr_probe(struct pci_dev *pdev, const struct pci_device_id *pci_id)
{
	struct cx88sdr_dev *dev;
	struct v4l2_device *v4l2_dev;
	struct v4l2_ctrl_handler *hdl;
	int ret;

	if (cx88sdr_devcount == CX88SDR_MAX_CARDS)
		return -ENODEV;

	ret = pci_enable_device(pdev);
	if (ret)
		return ret;

	pci_set_master(pdev);

	if (pci_set_dma_mask(pdev, DMA_BIT_MASK(32))) {
		dev_err(&pdev->dev, "no suitable DMA support available\n");
		ret = -EFAULT;
		goto disable_device;
	}

	dev = devm_kzalloc(&pdev->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev) {
		ret = -ENOMEM;
		dev_err(&pdev->dev, "can't allocate memory\n");
		goto disable_device;
	}

	dev->nr = cx88sdr_devcount;
	dev->pdev = pdev;

	cx88sdr_pci_lat_set(dev);

	ret = pci_request_regions(pdev, KBUILD_MODNAME);
	if (ret) {
		cx88sdr_pr_err("can't request memory regions\n");
		goto disable_device;
	}

	ret = cx88sdr_alloc_risc_inst_buffer(dev);
	if (ret) {
		cx88sdr_pr_err("can't alloc risc buffers\n");
		goto free_pci_regions;
	}

	ret = cx88sdr_alloc_dma_buffer(dev);
	if (ret) {
		cx88sdr_pr_err("can't alloc DMA buffers\n");
		goto cx88sdr_free_risc_inst_buffer;
	}

	cx88sdr_make_risc_instructions(dev);

	dev->mmio = pci_ioremap_bar(pdev, 0);
	if (dev->mmio == NULL) {
		ret = -ENODEV;
		cx88sdr_pr_err("can't ioremap BAR 0\n");
		goto cx88sdr_free_dma_buffer;
	}

	cx88sdr_shutdown(dev);
	wmb(); /* Ensure card reset */

	cx88sdr_sram_setup(dev, CLUSTER_BUF_NUM, CLUSTER_BUF_SIZE,
			   CLUSTER_BUFFER_BASE, CDT_BASE);

	ret = request_irq(pdev->irq, cx88sdr_irq, IRQF_SHARED, KBUILD_MODNAME, dev);
	if (ret) {
		cx88sdr_pr_err("failed to request IRQ\n");
		goto free_mmio;
	}

	dev->irq = pdev->irq;
	synchronize_irq(dev->irq);

	/* Set initial values */
	dev->gain = 0;
	dev->input = VMUX_01;
	dev->rate = RATE_8FSC_8BIT;
	dev->pixelformat = V4L2_SDR_FMT_CU8; /* Fictitious */
	dev->buffersize = 1; /* Fictitious */
	snprintf(dev->name, sizeof(dev->name), CX88SDR_DRV_NAME " [%d]", dev->nr);

	cx88sdr_adc_setup(dev);
	cx88sdr_rate_set(dev);
	cx88sdr_agc_setup(dev);
	cx88sdr_input_set(dev);

	mutex_lock(&cx88sdr_devlist_lock);
	list_add_tail(&dev->devlist, &cx88sdr_devlist);
	mutex_unlock(&cx88sdr_devlist_lock);

	mutex_init(&dev->vdev_mlock);
	v4l2_dev = &dev->v4l2_dev;
	ret = v4l2_device_register(&pdev->dev, v4l2_dev);
	if (ret) {
		v4l2_err(v4l2_dev, "can't register V4L2 device\n");
		goto free_irq;
	}

	hdl = &dev->ctrl_handler;
	v4l2_ctrl_handler_init(hdl, 3);
	v4l2_ctrl_new_std(hdl, &cx88sdr_ctrl_ops, V4L2_CID_GAIN, 0, 31, 1, dev->gain);
	v4l2_ctrl_new_custom(hdl, &cx88sdr_ctrl_input, NULL);
	v4l2_ctrl_new_custom(hdl, &cx88sdr_ctrl_rate, NULL);
	v4l2_dev->ctrl_handler = hdl;
	if (hdl->error) {
		ret = hdl->error;
		v4l2_err(v4l2_dev, "can't register V4L2 controls\n");
		goto free_v4l2;
	}

	/* Initialize the video_device structure */
	strscpy(v4l2_dev->name, dev->name, sizeof(v4l2_dev->name));
	dev->vdev = cx88sdr_template;
	dev->vdev.ctrl_handler = &dev->ctrl_handler;
	dev->vdev.lock = &dev->vdev_mlock;
	dev->vdev.v4l2_dev = v4l2_dev;
	video_set_drvdata(&dev->vdev, dev);

	ret = video_register_device(&dev->vdev, VFL_TYPE_SDR, -1);
	if (ret)
		goto free_v4l2;

	cx88sdr_pr_info("irq: %d, MMIO: 0x%p, PCI latency: %d\n",
			dev->pdev->irq, dev->mmio, dev->pci_lat);
	cx88sdr_pr_info("registered as %s\n",
			video_device_node_name(&dev->vdev));

	mmio_iowrite32(dev, MO_VID_INTMSK, INTERRUPT_MASK);
	cx88sdr_devcount++;
	return 0;

free_v4l2:
	v4l2_ctrl_handler_free(hdl);
	v4l2_device_unregister(v4l2_dev);
free_irq:
	free_irq(dev->irq, dev);
free_mmio:
	iounmap(dev->mmio);
cx88sdr_free_dma_buffer:
	cx88sdr_free_dma_buffer(dev);
cx88sdr_free_risc_inst_buffer:
	cx88sdr_free_risc_inst_buffer(dev);
free_pci_regions:
	pci_release_regions(pdev);
disable_device:
	pci_disable_device(pdev);
	return ret;
}

static void cx88sdr_remove(struct pci_dev *pdev)
{
	struct v4l2_device *v4l2_dev = pci_get_drvdata(pdev);
	struct cx88sdr_dev *dev = container_of(v4l2_dev, struct cx88sdr_dev, v4l2_dev);

	cx88sdr_shutdown(dev);
	wmb(); /* Ensure card reset */

	cx88sdr_pr_info("removing %s\n", video_device_node_name(&dev->vdev));

	mutex_lock(&cx88sdr_devlist_lock);
	list_del(&dev->devlist);
	mutex_unlock(&cx88sdr_devlist_lock);
	cx88sdr_devcount--;

	video_unregister_device(&dev->vdev);
	v4l2_ctrl_handler_free(&dev->ctrl_handler);
	v4l2_device_unregister(&dev->v4l2_dev);

	/* Release resources */
	free_irq(dev->irq, dev);
	iounmap(dev->mmio);
	cx88sdr_free_dma_buffer(dev);
	cx88sdr_free_risc_inst_buffer(dev);
	pci_release_regions(pdev);
	pci_disable_device(pdev);
}

static struct pci_device_id cx88sdr_pci_tbl[] = {
	{
		.vendor		= 0x14f1,
		.device		= 0x8800,
		.subvendor	= PCI_ANY_ID,
		.subdevice	= PCI_ANY_ID,
	}, {

	}
};
MODULE_DEVICE_TABLE(pci, cx88sdr_pci_tbl);

static struct pci_driver cx88sdr_pci_driver = {
	.name		= KBUILD_MODNAME,
	.id_table	= cx88sdr_pci_tbl,
	.probe		= cx88sdr_probe,
	.remove		= cx88sdr_remove,
};

module_pci_driver(cx88sdr_pci_driver);
