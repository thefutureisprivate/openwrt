// SPDX-License-Identifier: GPL-2.0-only

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/log2.h>
#include <linux/pci.h>
#include <linux/platform_data/vrx619.h>
#include <linux/sizes.h>
#include <linux/unaligned.h>

#include "vrx619_aca.h"

#define ACA_HIF			0x50000
#define ACA_TASK_MASK		0x5c034
#define ACA_START		0x5d090
#define ACA_DMA_ID		0x60008
#define ACA_DMA_CTRL		0x60010
#define ACA_DMA_POLL		0x60014
#define ACA_DMA_CS		0x60018
#define ACA_DMA_CCTRL		0x6001c
#define ACA_DMA_CDBA		0x60020
#define ACA_DMA_CDLEN		0x60024
#define ACA_DMA_CIS		0x60028
#define ACA_DMA_CIE		0x6002c
#define ACA_DMA_PS		0x60040
#define ACA_DMA_PCTRL		0x60044
#define ACA_XBAR_CTRL(n)		(0x17fc20 + (n) * 0x400)
#define ACA_XBAR_STATUS(n)	(0x17fc28 + (n) * 0x400)
#define ACA_READY		0x25062016
#define ACA_TIMEOUT_US		100000

static void vrx619_aca_write(void __iomem *base, u32 reg, u32 value)
{
	writel(value, base + reg);
	readl(base + reg);
}

static void vrx619_aca_update(void __iomem *base, u32 reg, u32 clear, u32 set)
{
	vrx619_aca_write(base, reg, (readl(base + reg) & ~clear) | set);
}

static int vrx619_aca_dma_init(const struct vrx619_platform_data *pdata,
			       const struct vrx619_aca_fw *fw, u32 bus_addr)
{
	void __iomem *base = pdata->membase;
	u32 value;
	unsigned int i;
	int ret;

	vrx619_aca_update(base, ACA_DMA_CTRL, 0, BIT(0));
	ret = readl_poll_timeout(base + ACA_DMA_CTRL, value, !(value & BIT(0)),
				 10, ACA_TIMEOUT_US);
	if (ret)
		return ret;
	value = (readl(base + ACA_DMA_ID) >> 20) & 0x7f;
	if (value < VRX619_ACA_CHANNELS)
		return -ENODEV;
	vrx619_aca_update(base, ACA_DMA_CTRL, 0, 0x8000e302);
	vrx619_aca_write(base, ACA_DMA_POLL, 0x80000080);
	vrx619_aca_write(base, ACA_DMA_PS, 0);
	vrx619_aca_update(base, ACA_DMA_PCTRL, 0xf3c, 0x103);
	for (i = 0; i < VRX619_ACA_CHANNELS; i++) {
		writel(0, base + fw->dma[i]);
		writel(0, base + fw->dma[i] + 4);
		vrx619_aca_write(base, ACA_DMA_CS, i);
		vrx619_aca_update(base, ACA_DMA_CCTRL, 0, BIT(1));
		ret = readl_poll_timeout(base + ACA_DMA_CCTRL, value,
					 !(value & BIT(1)), 10, ACA_TIMEOUT_US);
		if (ret)
			return ret;
		vrx619_aca_write(base, ACA_DMA_CDBA, bus_addr + fw->dma[i]);
		vrx619_aca_write(base, ACA_DMA_CDLEN, 1);
		vrx619_aca_write(base, ACA_DMA_CIS, 0x3e);
		vrx619_aca_write(base, ACA_DMA_CIE, 0x3e);
		vrx619_aca_update(base, ACA_DMA_CCTRL, 0, BIT(0));
	}
	return 0;
}

int vrx619_aca_init(const struct vrx619_platform_data *pdata,
		    const struct vrx619_aca_fw *fw,
		    const __le32 hif[VRX619_ACA_HIF_WORDS])
{
	void __iomem *base = pdata->membase;
	resource_size_t bus = pci_bus_address(pdata->pdev, 0);
	u32 bus_addr, mask, value, reg;
	unsigned int i, j;
	int ret;

	if (pdata->memsize < SZ_8M || pdata->memsize > SZ_2G ||
	    !is_power_of_2(pdata->memsize) ||
	    bus > U32_MAX - (pdata->memsize - 1) ||
	    (bus & (pdata->memsize - 1)))
		return -EOPNOTSUPP;
	bus_addr = bus;
	mask = -(u32)pdata->memsize;
	vrx619_aca_write(base, 0x50234, 1);
	vrx619_aca_write(base, 0x500a0, 0xf);
	vrx619_aca_write(base, 0x5d400, 0x4000);
	value = mask | ((bus_addr & mask) >> 16);
	vrx619_aca_write(base, 0x5d5c8, value);
	vrx619_aca_write(base, 0x5d5c4, value);
	vrx619_aca_write(base, ACA_TASK_MASK, 0);
	vrx619_aca_update(base, ACA_XBAR_CTRL(4), BIT(4), 0);
	vrx619_aca_update(base, ACA_XBAR_CTRL(6), BIT(4), 0);
	/* APR window 7 is private to the packet accelerator. */
	vrx619_aca_write(base, 0x10024, 0xe);
	for (i = 0; i < 0x400; i += 4)
		writel(0, base + 0x50800 + i);
	for (i = 0; i < 0x2000; i += 4)
		writel(0, base + 0x100000 + i);
	ret = vrx619_aca_dma_init(pdata, fw, bus_addr);
	if (ret)
		return ret;

	vrx619_aca_update(base, 0x50098, 0xffff, le32_to_cpu(hif[9]));
	vrx619_aca_write(base, 0x50240, fw->host[VRX619_TXOUT].count);
	vrx619_aca_write(base, 0x500ac, 0xffff0000);
	vrx619_aca_update(base, 0x5009c, 0xffff, le32_to_cpu(hif[23]));
	vrx619_aca_write(base, 0x50244, fw->host[VRX619_RXOUT].count);
	vrx619_aca_write(base, 0x500b4, 0xffff0000);
	for (i = 0; i < fw->reg_count; i++) {
		reg = get_unaligned_be32(fw->regs + i * 8);
		value = get_unaligned_be32(fw->regs + i * 8 + 4);
		vrx619_aca_write(base, ACA_HIF + reg, value);
	}
	for (i = 0; i < VRX619_ACA_CODE_BLOCKS; i++) {
		const struct vrx619_aca_code *code = &fw->code[i];

		for (j = 0; j < code->size; j += 4)
			writel(get_unaligned_be32(code->data + j), base + code->base + j);
		for (j = 0; j < code->size; j += 4)
			if (readl(base + code->base + j) !=
			    get_unaligned_be32(code->data + j))
				return -EIO;
	}
	for (i = 0; i < VRX619_ACA_HIF_WORDS; i++)
		writel(le32_to_cpu(hif[i]), base + fw->init_addr + i * 4);
	readl(base + fw->init_addr);
	for (i = 0; i < 8; i++) {
		reg = get_unaligned_be32(fw->sifs + i * 8);
		value = get_unaligned_be32(fw->sifs + i * 8 + 4);
		vrx619_aca_write(base, 0x740000 + reg, value);
	}
	return 0;
}

int vrx619_aca_start(const struct vrx619_platform_data *pdata,
		     const struct vrx619_aca_fw *fw)
{
	u32 value;

	vrx619_aca_write(pdata->membase, ACA_TASK_MASK, 0xf);
	vrx619_aca_write(pdata->membase, ACA_START, 1);
	return readl_poll_timeout(pdata->membase + fw->init_addr + 0x7c, value,
				  value == ACA_READY, 100, ACA_TIMEOUT_US);
}

int vrx619_aca_stop(const struct vrx619_platform_data *pdata)
{
	void __iomem *base = pdata->membase;
	u32 value;
	unsigned int i;
	int ret, error = 0;

	vrx619_aca_write(base, ACA_TASK_MASK, 0);
	for (i = 0; i < VRX619_ACA_CHANNELS; i++) {
		vrx619_aca_write(base, ACA_DMA_CS, i);
		vrx619_aca_write(base, ACA_DMA_CIE, 0);
		vrx619_aca_update(base, ACA_DMA_CCTRL, BIT(0), 0);
		ret = readl_poll_timeout(base + ACA_DMA_CCTRL, value,
					 !(value & BIT(0)), 10, ACA_TIMEOUT_US);
		if (ret)
			error = ret;
	}
	for (i = 4; i <= 6; i += 2) {
		vrx619_aca_update(base, ACA_XBAR_CTRL(i), 0, BIT(4));
		ret = readl_poll_timeout(base + ACA_XBAR_STATUS(i), value,
					 !(value & 0xf0), 10, ACA_TIMEOUT_US);
		if (ret)
			error = ret;
	}
	vrx619_aca_write(base, 0x5d41c, 0x7c004000);
	udelay(1);
	vrx619_aca_write(base, 0x5d404, 0x7c004000);
	return error;
}
