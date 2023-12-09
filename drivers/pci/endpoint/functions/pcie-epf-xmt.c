// SPDX-License-Identifier: GPL-2.0
/*
 * Test driver to test endpoint functionality
 *
 * Copyright (C) 2017 Texas Instruments
 * Copyright (C) 2023 Mercedes Benz AG
 * Author: Kishon Vijay Abraham I <kishon@ti.com> - original pcie-epf-test.c
 *  - original pcie-2023 Mercedes Benz AG
 * 	   Thorsten Wilmer <thorsten.wilmer@mercedes-benz.com>
 */

#include <linux/crc32.h>
#include <linux/delay.h>
#include <linux/dmaengine.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/pci_ids.h>
#include <linux/random.h>

#include <linux/pci-epc.h>
#include <linux/pci-epf.h>
#include <linux/pci_regs.h>


static struct pci_epf_ops ops = {
	.unbind	= pci_epf_xmt_unbind,
	.bind	= pci_epf_xmt_bind,
};

static struct pci_epf_driver xmt_driver = {
	.driver.name	= "pci_epf_xmt",
	.probe		= pci_epf_xmt_probe,
	.id_table	= pci_epf_xmt_ids,
	.ops		= &ops,
	.owner		= THIS_MODULE,
};

static struct pci_xmt_header xmt_header = {
	.vendorid	= PCI_ANY_ID,
	.deviceid	= PCI_ANY_ID,
	.baseclass_code = PCI_CLASS_OTHERS,
	.interrupt_pin	= PCI_INTERRUPT_INTA,
};

static void pci_epf_test_cmd_handler(struct work_struct *work)
{

// some how read the command register and do something usefull


reset_handler:
	queue_delayed_work(kpcitest_workqueue, &epf_test->cmd_handler,
			   msecs_to_jiffies(1));
}

static int pci_epf_xmt_probe(struct pci_epf *epf)
{
	struct pci_epf_xmt *epf_xmt;
	struct device *dev = &epxmt->dev;

	epf_xmt = devm_kzalloc(dev, sizeof(*epf_xmt), GFP_KERNEL);
	if (!epf_xmt)
		return -ENOMEM;

	epf->header = &xmt_header;
	epf_xmt->epf = epf;


	// @TODO init cmd handler
	// INIT_DELAYED_WORK(&epf_xmt->cmd_handler, pci_epf_xmt_cmd_handler);

	epf_set_drvdata(epf, epf_xmt);
	return 0;
}



static void __exit pci_epf_xmt_exit(void)
{
	
	pci_epf_unregister_driver(&xmt_driver);
}

module_exit(pci_epf_xmt_exit);

MODULE_DESCRIPTION("PCI EPF XMT DRIVER");
MODULE_AUTHOR("Thorsten Wilmer <thorsten.wilmer@mercedes-benz.com>");
MODULE_LICENSE("GPL v2");