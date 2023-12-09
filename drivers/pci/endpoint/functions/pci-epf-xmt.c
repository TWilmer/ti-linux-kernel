// SPDX-License-Identifier: GPL-2.0
/*
 * XMT  driver endpoint functionality
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

static struct workqueue_struct *kpcixmt_workqueue;

struct pci_epf_xmt_reg {
	u32	magic;
	u32	command;
	u32	status;
	u64	src_addr;
	u64	dst_addr;
	u32	size;
	u32	checksum;
	u32	irq_type;
	u32	irq_number;
	u32	flags;
} __packed;


static size_t bar_size[] = { 512, 512, 1024, 16384, 131072, 1048576 };

static struct pci_epf_header xmt_header = {
	.vendorid	= 0x1E2E,  // Mercedes-Benz R&D North America, Inc.
	.deviceid	= 0xFF01,  // @TODO allocate the "right" number
	.baseclass_code = PCI_CLASS_OTHERS,
	.interrupt_pin	= PCI_INTERRUPT_INTA,
};

struct pci_epf_xmt {
	void			*reg[PCI_STD_NUM_BARS];
	struct pci_epf		*epf;
	enum pci_barno		xmt_reg_bar;
	size_t			msix_table_offset;
	struct delayed_work	cmd_handler;
	
	struct completion	transfer_complete;
	
	const struct pci_epc_features *epc_features;
};


static int pci_epf_xmt_set_bar(struct pci_epf *epf)
{
	int bar, add;
	int ret;
	struct pci_epf_bar *epf_bar;
	struct pci_epc *epc = epf->epc;
	struct device *dev = &epf->dev;
	struct pci_epf_xmt *epf_xmt = epf_get_drvdata(epf);
	enum pci_barno xmt_reg_bar = epf_xmt->xmt_reg_bar;
	const struct pci_epc_features *epc_features;

	epc_features = epf_xmt->epc_features;

	for (bar = 0; bar < PCI_STD_NUM_BARS; bar += add) {
		epf_bar = &epf->bar[bar];
		/*
		 * pci_epc_set_bar() sets PCI_BASE_ADDRESS_MEM_TYPE_64
		 * if the specific implementation required a 64-bit BAR,
		 * even if we only requested a 32-bit BAR.
		 */
		add = (epf_bar->flags & PCI_BASE_ADDRESS_MEM_TYPE_64) ? 2 : 1;

		if (!!(epc_features->reserved_bar & (1 << bar)))
			continue;

		ret = pci_epc_set_bar(epc, epf->func_no, epf->vfunc_no,
				      epf_bar);
		if (ret) {
			pci_epf_free_space(epf, epf_xmt->reg[bar], bar,
					   PRIMARY_INTERFACE);
			dev_err(dev, "Failed to set BAR%d\n", bar);
			if (bar == xmt_reg_bar)
				return ret;
		}
	}

	return 0;
}


static int pci_epf_xmt_core_init(struct pci_epf *epf)
{
	
	struct pci_epf_header *header = epf->header;
	
	struct pci_epc *epc = epf->epc;
	struct device *dev = &epf->dev;
	
	int ret;

	
	if (epf->vfunc_no <= 1) {
		ret = pci_epc_write_header(epc, epf->func_no, epf->vfunc_no, header);
		if (ret) {
			dev_err(dev, "Configuration header write failed\n");
			return ret;
		}
	}

	ret = pci_epf_xmt_set_bar(epf);
	if (ret)
		return ret;

	

	return 0;
}

static void pci_epf_xmt_unbind(struct pci_epf *epf)
{
	struct pci_epf_xmt *epf_xmt  = epf_get_drvdata(epf);
	struct pci_epc *epc = epf->epc;
	struct pci_epf_bar *epf_bar;
	int bar;

	cancel_delayed_work(&epf_xmt->cmd_handler);
	
	for (bar = 0; bar < PCI_STD_NUM_BARS; bar++) {
		epf_bar = &epf->bar[bar];

		if (epf_xmt->reg[bar]) {
			pci_epc_clear_bar(epc, epf->func_no, epf->vfunc_no,
					  epf_bar);
			pci_epf_free_space(epf, epf_xmt->reg[bar], bar,
					   PRIMARY_INTERFACE);
		}
	}
}

static int pci_epf_xmt_notifier(struct notifier_block *nb, unsigned long val,
				 void *data)
{
	struct pci_epf *epf = container_of(nb, struct pci_epf, nb);
	struct pci_epf_xmt *epf_xmt = epf_get_drvdata(epf);
	int ret;

	switch (val) {
	case CORE_INIT:
		ret = pci_epf_xmt_core_init(epf);
		if (ret)
			return NOTIFY_BAD;
		break;

	case LINK_UP:
		queue_delayed_work(kpcixmt_workqueue, &epf_xmt->cmd_handler,
				   msecs_to_jiffies(1));
		break;

	default:
		dev_err(&epf->dev, "Invalid EPF xmt notifier event\n");
		return NOTIFY_BAD;
	}

	return NOTIFY_OK;
}

static int pci_epf_xmt_alloc_space(struct pci_epf *epf)
{
	struct pci_epf_xmt *epf_xmt = epf_get_drvdata(epf);
	struct device *dev = &epf->dev;
	struct pci_epf_bar *epf_bar;

	size_t xmt_reg_bar_size;

	
	void *base;
	int bar, add;
	enum pci_barno xmt_reg_bar = epf_xmt->xmt_reg_bar;
	const struct pci_epc_features *epc_features;
	size_t xmt_reg_size;

	epc_features = epf_xmt->epc_features;

	xmt_reg_bar_size = ALIGN(sizeof(struct pci_epf_xmt_reg), 128);

	

	if (epc_features->bar_fixed_size[xmt_reg_bar]) {
		if (xmt_reg_size > bar_size[xmt_reg_bar])
			return -ENOMEM;
		xmt_reg_size = bar_size[xmt_reg_bar];
	}

	base = pci_epf_alloc_space(epf, xmt_reg_size, xmt_reg_bar,
				   epc_features->align, PRIMARY_INTERFACE);
	if (!base) {
		dev_err(dev, "Failed to allocated register space\n");
		return -ENOMEM;
	}
	epf_xmt->reg[xmt_reg_bar] = base;

	for (bar = 0; bar < PCI_STD_NUM_BARS; bar += add) {
		epf_bar = &epf->bar[bar];
		add = (epf_bar->flags & PCI_BASE_ADDRESS_MEM_TYPE_64) ? 2 : 1;

		if (bar == xmt_reg_bar)
			continue;

		if (!!(epc_features->reserved_bar & (1 << bar)))
			continue;

		base = pci_epf_alloc_space(epf, bar_size[bar], bar,
					   epc_features->align,
					   PRIMARY_INTERFACE);
		if (!base)
			dev_err(dev, "Failed to allocate space for BAR%d\n",
				bar);
		epf_xmt->reg[bar] = base;
	}

	return 0;
}

static void pci_epf_xmt_configure_bar(struct pci_epf *epf,
				  const struct pci_epc_features *epc_features)
{
	struct pci_epf_bar *epf_bar;
	bool bar_fixed_64bit;
	int i;

	for (i = 0; i < PCI_STD_NUM_BARS; i++) {
		epf_bar = &epf->bar[i];
		bar_fixed_64bit = !!(epc_features->bar_fixed_64bit & (1 << i));
		if (bar_fixed_64bit)
			epf_bar->flags |= PCI_BASE_ADDRESS_MEM_TYPE_64;
		if (epc_features->bar_fixed_size[i])
			bar_size[i] = epc_features->bar_fixed_size[i];
	}
}

static int pci_epf_xmt_bind(struct pci_epf *epf)
{
	int ret;
	struct pci_epf_xmt *epf_xmt = epf_get_drvdata(epf);
	const struct pci_epc_features *epc_features;
	enum pci_barno xmt_reg_bar = BAR_0;
	struct pci_epc *epc = epf->epc;
	bool linkup_notifier = false;
	bool core_init_notifier = false;

	if (WARN_ON_ONCE(!epc))
		return -EINVAL;

	epc_features = pci_epc_get_features(epc, epf->func_no, epf->vfunc_no);
	if (!epc_features) {
		dev_err(&epf->dev, "epc_features not implemented\n");
		return -EOPNOTSUPP;
	}

	linkup_notifier = epc_features->linkup_notifier;
	core_init_notifier = epc_features->core_init_notifier;
	xmt_reg_bar = pci_epc_get_first_free_bar(epc_features);
	if (xmt_reg_bar < 0)
		return -EINVAL;
	pci_epf_xmt_configure_bar(epf, epc_features);

	epf_xmt->xmt_reg_bar = xmt_reg_bar;
	epf_xmt->epc_features = epc_features;

	ret = pci_epf_xmt_alloc_space(epf);
	if (ret)
		return ret;

	if (!core_init_notifier) {
		ret = pci_epf_xmt_core_init(epf);
		if (ret)
			return ret;
	}




	if (linkup_notifier || core_init_notifier) {
		epf->nb.notifier_call = pci_epf_xmt_notifier;
		pci_epc_register_notifier(epc, &epf->nb);
	} else {
		queue_work(kpcixmt_workqueue, &epf_xmt->cmd_handler.work);
	}

	return 0;
}





static void pci_epf_xmt_cmd_handler(struct work_struct *work)
{
	struct pci_epf_xmt *epf_xmt = container_of(work, struct pci_epf_xmt,
						     cmd_handler.work);

// some how read the command register and do something usefull


reset_handler:
	queue_delayed_work(kpcixmt_workqueue, &epf_xmt->cmd_handler,
			   msecs_to_jiffies(1));
}

static int pci_epf_xmt_probe(struct pci_epf *epf)
{
	struct pci_epf_xmt *epf_xmt;
	struct device *dev = &epf->dev;

	epf_xmt = devm_kzalloc(dev, sizeof(*epf_xmt), GFP_KERNEL);
	if (!epf_xmt)
		return -ENOMEM;

	epf->header = &xmt_header;
	epf_xmt->epf = epf;


	
	INIT_DELAYED_WORK(&epf_xmt->cmd_handler, pci_epf_xmt_cmd_handler);

	epf_set_drvdata(epf, epf_xmt);
	return 0;
}

static const struct pci_epf_device_id pci_epf_xmt_ids[] = {
	{
		.name = "pci_epf_xmt",
	},
	{},
};


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



static int __init pci_epf_xmt_init(void)
{
	int ret;

	kpcixmt_workqueue = alloc_workqueue("kpcixmt",
					     WQ_MEM_RECLAIM | WQ_HIGHPRI, 0);
	if (!kpcixmt_workqueue) {
		pr_err("Failed to allocate the kpcixmt work queue\n");
		return -ENOMEM;
	}

	ret = pci_epf_register_driver(&xmt_driver);
	if (ret) {
		destroy_workqueue(kpcixmt_workqueue);
		pr_err("Failed to register pci epf xmt driver --> %d\n", ret);
		return ret;
	}

	return 0;
}


module_init(pci_epf_xmt_init);

static void __exit pci_epf_xmt_exit(void)
{
	if (kpcixmt_workqueue)
		destroy_workqueue(kpcixmt_workqueue);
	pci_epf_unregister_driver(&xmt_driver);
}

module_exit(pci_epf_xmt_exit);

MODULE_DESCRIPTION("PCI EPF XMT DRIVER");
MODULE_AUTHOR("Thorsten Wilmer <thorsten.wilmer@mercedes-benz.com>");
MODULE_LICENSE("GPL v2");