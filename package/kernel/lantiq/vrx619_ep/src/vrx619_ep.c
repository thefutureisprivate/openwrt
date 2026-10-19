// SPDX-License-Identifier: GPL-2.0-only

#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/platform_data/vrx619.h>

#define VRX619_BAR	0

static void vrx619_clear_master(void *data)
{
	pci_clear_master(data);
}

static void vrx619_unregister_child(void *data)
{
	platform_device_unregister(data);
}

static int vrx619_add_child(struct pci_dev *pdev, const char *name,
			    const struct vrx619_platform_data *pdata)
{
	struct platform_device *child;

	child = platform_device_register_data(&pdev->dev, name,
					      PLATFORM_DEVID_AUTO,
					      pdata, sizeof(*pdata));
	if (IS_ERR(child))
		return dev_err_probe(&pdev->dev, PTR_ERR(child),
				     "failed to register %s\n", name);

	return devm_add_action_or_reset(&pdev->dev, vrx619_unregister_child,
					child);
}

static int vrx619_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct vrx619_platform_data pdata = {
		.pdev = pdev,
	};
	int ret;

	ret = pcim_enable_device(pdev);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "failed to enable device\n");

	pdata.memsize = pci_resource_len(pdev, VRX619_BAR);
	if (!(pci_resource_flags(pdev, VRX619_BAR) & IORESOURCE_MEM) ||
	    !pdata.memsize)
		return dev_err_probe(&pdev->dev, -ENODEV, "invalid BAR0\n");

	/* Match the 32-bit streaming and coherent masks used by the vendor. */
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "32-bit DMA unavailable\n");

	pdata.membase = pcim_iomap_region(pdev, VRX619_BAR, KBUILD_MODNAME);
	if (IS_ERR(pdata.membase))
		return dev_err_probe(&pdev->dev, PTR_ERR(pdata.membase),
				     "failed to map BAR0\n");

	pci_set_master(pdev);
	udelay(1);

	/* Children are removed before bus mastering and BAR0 are released. */
	ret = devm_add_action_or_reset(&pdev->dev, vrx619_clear_master, pdev);
	if (ret)
		return ret;

	return vrx619_add_child(pdev, "vrx619-pmi", &pdata);
}

static void vrx619_shutdown(struct pci_dev *pdev)
{
	pci_clear_master(pdev);
}

static const struct pci_device_id vrx619_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, 0x09aa) },
	{ }
};
MODULE_DEVICE_TABLE(pci, vrx619_ids);

static struct pci_driver vrx619_driver = {
	.name = KBUILD_MODNAME,
	.id_table = vrx619_ids,
	.probe = vrx619_probe,
	.shutdown = vrx619_shutdown,
};
module_pci_driver(vrx619_driver);

MODULE_DESCRIPTION("VRX619 PCIe endpoint resource management");
MODULE_LICENSE("GPL");
