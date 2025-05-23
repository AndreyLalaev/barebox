// SPDX-License-Identifier: GPL-2.0-only
/*
 * Texas Instruments DSPS platforms "glue layer"
 *
 * Copyright (C) 2012, by Texas Instruments
 *
 * Based on the am35x "glue layer" code.
 *
 * This file is part of the Inventra Controller Driver for Linux.
 *
 * musb_dsps.c will be a common file for all the TI DSPS platforms
 * such as dm64x, dm36x, dm35x, da8x, am35x and ti81x.
 * For now only ti81x is using this and in future davinci.c, am35x.c
 * da8xx.c would be merged to this file after testing.
 */

#include <common.h>
#include <init.h>
#include <clock.h>
#include <linux/usb/usb.h>
#include <linux/usb/musb.h>
#include <malloc.h>
#include <linux/err.h>
#include <linux/barebox-wrapper.h>

#include "musb_core.h"

static __maybe_unused struct of_device_id musb_dsps_dt_ids[];

/**
 * avoid using musb_readx()/musb_writex() as glue layer should not be
 * dependent on musb core layer symbols.
 */
static inline u8 dsps_readb(const void __iomem *addr, unsigned offset)
{
	return __raw_readb(addr + offset);
}

static inline u32 dsps_readl(const void __iomem *addr, unsigned offset)
{
	return __raw_readl(addr + offset);
}

static inline void dsps_writeb(void __iomem *addr, unsigned offset, u8 data)
{
	__raw_writeb(data, addr + offset);
}

static inline void dsps_writel(void __iomem *addr, unsigned offset, u32 data)
{
	__raw_writel(data, addr + offset);
}

/**
 * DSPS musb wrapper register offset.
 * FIXME: This should be expanded to have all the wrapper registers from TI DSPS
 * musb ips.
 */
struct dsps_musb_wrapper {
	u16	revision;
	u16	control;
	u16	status;
	u16	epintr_set;
	u16	epintr_clear;
	u16	epintr_status;
	u16	coreintr_set;
	u16	coreintr_clear;
	u16	coreintr_status;
	u16	phy_utmi;
	u16	mode;

	/* bit positions for control */
	unsigned	reset:5;

	/* bit positions for interrupt */
	unsigned	usb_shift:5;
	u32		usb_mask;
	u32		usb_bitmap;
	unsigned	drvvbus:5;

	unsigned	txep_shift:5;
	u32		txep_mask;
	u32		txep_bitmap;

	unsigned	rxep_shift:5;
	u32		rxep_mask;
	u32		rxep_bitmap;

	/* bit positions for phy_utmi */
	unsigned	otg_disable:5;

	/* bit positions for mode */
	unsigned	iddig:5;
	/* miscellaneous stuff */
	u8		poll_seconds;
};

/**
 * DSPS glue structure.
 */
struct dsps_glue {
	struct device *dev;
	void __iomem *base;
	unsigned long flags;
	enum musb_mode mode;
	struct musb musb;
	struct musb_hdrc_config	config;
	const struct dsps_musb_wrapper *wrp; /* wrapper register offsets */
	struct poller_async timer;	/* otg_workaround timer */
	uint64_t last_timer;    /* last timer data for each instance */
	struct device otg_dev;
	uint32_t otgmode;
	struct musb_hdrc_platform_data pdata;
};

static struct dsps_glue *to_dsps_glue(struct musb *musb)
{
	return container_of(musb, struct dsps_glue, musb);
}

/**
 * dsps_musb_enable - enable interrupts
 */
static void dsps_musb_enable(struct musb *musb)
{
	struct dsps_glue *glue = to_dsps_glue(musb);
	const struct dsps_musb_wrapper *wrp = glue->wrp;
	void __iomem *reg_base = musb->ctrl_base;
	u32 epmask, coremask;

	/* Workaround: setup IRQs through both register sets. */
	epmask = ((musb->epmask & wrp->txep_mask) << wrp->txep_shift) |
	       ((musb->epmask & wrp->rxep_mask) << wrp->rxep_shift);
	coremask = (wrp->usb_bitmap & ~MUSB_INTR_SOF);

	dsps_writel(reg_base, wrp->epintr_set, epmask);
	dsps_writel(reg_base, wrp->coreintr_set, coremask);
	/* Force the DRVVBUS IRQ so we can start polling for ID change. */
	dsps_writel(reg_base, wrp->coreintr_set,
		    (1 << wrp->drvvbus) << wrp->usb_shift);
}

/**
 * dsps_musb_disable - disable HDRC and flush interrupts
 */
static void dsps_musb_disable(struct musb *musb)
{
	struct dsps_glue *glue = to_dsps_glue(musb);
	const struct dsps_musb_wrapper *wrp = glue->wrp;
	void __iomem *reg_base = musb->ctrl_base;

	dsps_writel(reg_base, wrp->coreintr_clear, wrp->usb_bitmap);
	dsps_writel(reg_base, wrp->epintr_clear,
			 wrp->txep_bitmap | wrp->rxep_bitmap);
	dsps_writeb(musb->mregs, MUSB_DEVCTL, 0);
}

static irqreturn_t dsps_interrupt(struct musb *musb)
{
	void __iomem *reg_base = musb->ctrl_base;
	struct dsps_glue *glue = to_dsps_glue(musb);
	const struct dsps_musb_wrapper *wrp = glue->wrp;
	unsigned long flags;
	irqreturn_t ret = IRQ_NONE;
	u32 epintr, usbintr;

	spin_lock_irqsave(&musb->lock, flags);

	/* Get endpoint interrupts */
	epintr = dsps_readl(reg_base, wrp->epintr_status);
	musb->int_rx = (epintr & wrp->rxep_bitmap) >> wrp->rxep_shift;
	musb->int_tx = (epintr & wrp->txep_bitmap) >> wrp->txep_shift;

	if (epintr)
		dsps_writel(reg_base, wrp->epintr_status, epintr);

	/* Get usb core interrupts */
	usbintr = dsps_readl(reg_base, wrp->coreintr_status);
	if (!usbintr && !epintr)
		goto out;

	musb->int_usb =	(usbintr & wrp->usb_bitmap) >> wrp->usb_shift;
	if (usbintr)
		dsps_writel(reg_base, wrp->coreintr_status, usbintr);

	dev_dbg(musb->controller, "usbintr (%x) epintr(%x)\n",
			usbintr, epintr);

	if (musb->int_tx || musb->int_rx || musb->int_usb)
		ret |= musb_interrupt(musb);

out:
	spin_unlock_irqrestore(&musb->lock, flags);

	return ret;
}

static int dsps_musb_init(struct musb *musb)
{
	struct dsps_glue *glue = to_dsps_glue(musb);
	const struct dsps_musb_wrapper *wrp = glue->wrp;
	u32 rev, val, mode;

	/* Returns zero if e.g. not clocked */
	rev = dsps_readl(musb->ctrl_base, wrp->revision);
	if (!rev)
		return -ENODEV;

	usb_phy_init(musb->xceiv);

	/* Reset the musb */
	dsps_writel(musb->ctrl_base, wrp->control, (1 << wrp->reset));

	musb->isr = dsps_interrupt;

	/* reset the otgdisable bit, needed for host mode to work */
	val = dsps_readl(musb->ctrl_base, wrp->phy_utmi);
	val &= ~(1 << wrp->otg_disable);
	dsps_writel(musb->ctrl_base, wrp->phy_utmi, val);

	mode = dsps_readl(musb->ctrl_base, wrp->mode);

	switch (musb->port_mode) {
	case MUSB_PORT_MODE_HOST:
		mode &= ~0x100;
		break;
	case MUSB_PORT_MODE_GADGET:
		mode |= 0x100;
		break;
	}

	mode |= 0x80;
	dsps_writel(musb->ctrl_base, wrp->mode, mode); /* IDDIG=0, IDDIG_MUX=1 */

	return 0;
}

static int dsps_musb_exit(struct musb *musb)
{
	usb_phy_shutdown(musb->xceiv);
	return 0;
}

static struct musb_platform_ops dsps_ops = {
	.init		= dsps_musb_init,
	.exit		= dsps_musb_exit,

	.enable		= dsps_musb_enable,
	.disable	= dsps_musb_disable,
};

static int get_int_prop(struct device_node *dn, const char *s)
{
	int ret;
	u32 val;

	ret = of_property_read_u32(dn, s, &val);
	if (ret)
		return 0;
	return val;
}

static int get_musb_port_mode(struct device *dev)
{
	enum usb_dr_mode mode;

	mode = of_usb_get_dr_mode(dev->of_node, NULL);
	switch (mode) {
	case USB_DR_MODE_HOST:
		return MUSB_PORT_MODE_HOST;

	case USB_DR_MODE_PERIPHERAL:
		return MUSB_PORT_MODE_GADGET;

	case USB_DR_MODE_UNKNOWN:
	case USB_DR_MODE_OTG:
	default:
		if (!IS_ENABLED(CONFIG_USB_MUSB_HOST))
			return MUSB_PORT_MODE_GADGET;
		if (!IS_ENABLED(CONFIG_USB_MUSB_GADGET))
			return MUSB_PORT_MODE_HOST;
		return MUSB_PORT_MODE_DUAL_ROLE;
	}
}

static int dsps_set_mode(void *ctx, enum usb_dr_mode mode)
{
	struct dsps_glue *glue = ctx;

	if (mode == USB_DR_MODE_HOST)
		glue->pdata.mode = MUSB_PORT_MODE_HOST;
	else if (mode == USB_DR_MODE_PERIPHERAL)
		glue->pdata.mode = MUSB_PORT_MODE_GADGET;
	else
		return -EINVAL;

	return musb_init_controller(&glue->musb, &glue->pdata);
}

static int dsps_probe(struct device *dev)
{
	struct resource *iores[2];
	struct musb_hdrc_platform_data *pdata;
	struct musb_hdrc_config	*config;
	struct device_node *dn = dev->of_node;
	const struct dsps_musb_wrapper *wrp;
	struct device_node *phy_node;
	struct device *phy_dev;
	struct dsps_glue *glue;
	int ret;

	wrp = device_get_match_data(dev);
	if (!wrp)
		return -ENODEV;

	if (!IS_ENABLED(CONFIG_USB_MUSB_HOST) &&
			!IS_ENABLED(CONFIG_USB_MUSB_GADGET)) {
		dev_err(dev, "Both host and device driver disabled.\n");
		return -ENODEV;
	}

	phy_node = of_parse_phandle(dn, "phys", 0);
	if (!phy_node)
		return -ENODEV;

	phy_dev = of_find_device_by_node(phy_node);
	if (!phy_dev || !phy_dev->priv)
		return -EPROBE_DEFER;

	/* allocate glue */
	glue = kzalloc(sizeof(*glue), GFP_KERNEL);
	if (!glue) {
		dev_err(dev, "unable to allocate glue memory\n");
		return -ENOMEM;
	}

	glue->dev = dev;
	glue->wrp = wrp;

	pdata = &glue->pdata;

	iores[0] = dev_request_mem_resource(dev, 0);
	if (IS_ERR(iores[0])) {
		ret = PTR_ERR(iores[0]);
		goto free_glue;
	}
	glue->musb.mregs = IOMEM(iores[0]->start);

	iores[1] = dev_request_mem_resource(dev, 1);
	if (IS_ERR(iores[1])) {
		ret = PTR_ERR(iores[1]);
		goto release_iores0;
	}
	glue->musb.ctrl_base = IOMEM(iores[1]->start);

	glue->musb.controller = dev;
	glue->musb.xceiv = phy_dev->priv;

	config = &glue->config;

	pdata->config = config;
	pdata->platform_ops = &dsps_ops;

	config->num_eps = get_int_prop(dn, "mentor,num-eps");
	config->ram_bits = get_int_prop(dn, "mentor,ram-bits");

	pdata->mode = get_musb_port_mode(dev);
	/* DT keeps this entry in mA, musb expects it as per USB spec */
	pdata->power = get_int_prop(dn, "mentor,power") / 2;
	config->multipoint = of_property_read_bool(dn, "mentor,multipoint");

	if (pdata->mode == MUSB_PORT_MODE_DUAL_ROLE) {
		ret = usb_register_otg_device(dev, dsps_set_mode, glue);
		if (ret)
			goto release_iores1;
		return 0;
	}

	ret = musb_init_controller(&glue->musb, pdata);
	if (ret)
		goto release_iores1;

	return 0;

release_iores1:
	release_region(iores[1]);
release_iores0:
	release_region(iores[0]);
free_glue:
	free(glue);

	return ret;
}

static const struct dsps_musb_wrapper am33xx_driver_data = {
	.revision		= 0x00,
	.control		= 0x14,
	.status			= 0x18,
	.epintr_set		= 0x38,
	.epintr_clear		= 0x40,
	.epintr_status		= 0x30,
	.coreintr_set		= 0x3c,
	.coreintr_clear		= 0x44,
	.coreintr_status	= 0x34,
	.phy_utmi		= 0xe0,
	.mode			= 0xe8,
	.reset			= 0,
	.otg_disable		= 21,
	.iddig			= 8,
	.usb_shift		= 0,
	.usb_mask		= 0x1ff,
	.usb_bitmap		= (0x1ff << 0),
	.drvvbus		= 8,
	.txep_shift		= 0,
	.txep_mask		= 0xffff,
	.txep_bitmap		= (0xffff << 0),
	.rxep_shift		= 16,
	.rxep_mask		= 0xfffe,
	.rxep_bitmap		= (0xfffe << 16),
	.poll_seconds		= 2,
};

static __maybe_unused struct of_device_id musb_dsps_dt_ids[] = {
	{
		.compatible = "ti,musb-am33xx",
		.data = &am33xx_driver_data,
	}, {
		/* sentinel */
	},
};
MODULE_DEVICE_TABLE(of, musb_dsps_dt_ids);

static struct driver dsps_usbss_driver = {
	.name   = "musb-dsps",
	.probe  = dsps_probe,
	.of_compatible = DRV_OF_COMPAT(musb_dsps_dt_ids),
};
device_platform_driver(dsps_usbss_driver);

MODULE_DESCRIPTION("TI DSPS MUSB Glue Layer");
MODULE_AUTHOR("Ravi B <ravibabu@ti.com>");
MODULE_AUTHOR("Ajay Kumar Gupta <ajay.gupta@ti.com>");
MODULE_LICENSE("GPL v2");
