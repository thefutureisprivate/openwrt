/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_PLATFORM_DATA_VRX619_H
#define _LINUX_PLATFORM_DATA_VRX619_H

#include <linux/io.h>
#include <linux/types.h>

struct pci_dev;
struct device;

/*
 * Shared by the native PMI and datapath children. This is not the vendor
 * platform-data ABI. All DMA mappings must use &pdev->dev, not a child's
 * platform device. Children must stop DMA and interrupts before removal.
 */
struct vrx619_platform_data {
	struct pci_dev *pdev;
	void __iomem *membase;
	resource_size_t memsize;
};

/* Sleepable; callers must be children of the PMI device. Payloads are LE. */
int vrx619_pmi_xfer(struct device *dev, u16 command, u16 field,
		    const void *tx, size_t tx_size, void *rx, size_t rx_size);
unsigned int vrx619_pmi_transport(struct device *dev);

#endif
