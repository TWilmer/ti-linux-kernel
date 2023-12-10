/*
 *   This file is provided under a dual BSD/GPLv2 license.  When using or
 *   redistributing this file, you may do so under either license.
 *
 *   GPL LICENSE SUMMARY
 * 
 *   Copyright (C) 2023 Mercedes-Benz AG All Rights Reserved.
 *   Thosten Wilmer <thorsten.wilmer@mercedes-benz.com> 
 *  
 *   Based on code from Allen Hubbe <Allen.Hubbe@emc.com>
 * 	Johannes 4 GNU/Linux
 *	https://gist.github.com/laoar/4a7110dcd65dbf2aefb3231146458b39
 * 
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of version 2 of the GNU General Public License as
 *   published by the Free Software Foundation.
 *
 *   This program is distributed in the hope that it will be useful, but
 *   WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *   General Public License for more details.
*/

#include <linux/module.h>
#include <linux/init.h>
#include <linux/pci.h>

#define DRIVER_VERSION		"1.0"
MODULE_LICENSE("GPL");
MODULE_VERSION(DRIVER_VERSION);
MODULE_AUTHOR("Thosten Wilmer <thorsten.wilmer@mercedes-benz.com> ");


#define PCIXMT_VENDOR_ID 0x1E2E
#define PCIXMT_DEVICE_ID 0xFF01


static struct pci_device_id pcixmt_ids[] = {
	{ PCI_DEVICE(PCIXMT_VENDOR_ID, PCIXMT_DEVICE_ID) },
	{ }
};

const u64 MAX_SIZE=1024*1024; 

static struct device*  device;
static struct class*  class;
static int major;
static char *sh_mem = NULL;


struct xmt_dev {
	dma_addr_t dma_handle;
	void *     cpu_addr;
}; 

#define DEVICE_NAME "xmt"
#define CLASS_NAME "xmt"

// @TODO check if this can be attached to the device?
static char *xmt_mem = NULL; 


// could be used to give exclusive access
static int mchar_release(struct inode *inodep, struct file *filep)
{    
   

    return 0;
}


// could be used to give exclusive access
static int pcixmt_open(struct inode *inodep, struct file *filep)
{
	return 0;
}

static int pcixmt_mmap(struct file *filp, struct vm_area_struct *vma)
{
    int ret = 0;
    struct page *page = NULL;
    unsigned long size = (unsigned long)(vma->vm_end - vma->vm_start);

    if (size > MAX_SIZE) {
        ret = -EINVAL;
        goto out;  
    } 
   
    page = virt_to_page((unsigned long)sh_mem + (vma->vm_pgoff << PAGE_SHIFT)); 
    ret = remap_pfn_range(vma, vma->vm_start, page_to_pfn(page), size, vma->vm_page_prot);
    if (ret != 0) {
        goto out;
    }   

out:
    return ret;
}

static ssize_t pcixmt_read(struct file *filep, char *buffer, size_t len, loff_t *offset)
{
    int ret;
    
    if (len > MAX_SIZE) {
        pr_info("read overflow!\n");
        ret = -EFAULT;
        goto out;
    }

    if (copy_to_user(buffer, sh_mem, len) == 0) {
        pr_info("mchar: copy %lu char to the user\n", len);
        ret = len;
    } else {
        ret =  -EFAULT;   
    } 

out:
    return ret;
}

static ssize_t pcixmt_write(struct file *filep, const char *buffer, size_t len, loff_t *offset)
{
    int ret;
 
    if (copy_from_user(sh_mem, buffer, len)) {
        pr_err("mchar: write fault!\n");
        ret = -EFAULT;
        goto out;
    }
    pr_info("mchar: copy %ld char from the user\n", len);
    ret = len;

out:
    return ret;
}

static const struct file_operations pcixmt_fops = {
    .open = pcixmt_open,
    .read = pcixmt_read,
    .write = pcixmt_write,
    .release = mchar_release,
    .mmap = pcixmt_mmap,
    /*.unlocked_ioctl = mchar_ioctl,*/
    .owner = THIS_MODULE,
};


static int pcixmt_probe(struct pci_dev *dev, const struct pci_device_id *id) {
	struct xmt_dev *my_data;

	//@TODO error handling junk - in case of an error taken resources have to be freed
    int status;

    int ret = 0;    
    major = register_chrdev(0, DEVICE_NAME, &pcixmt_fops);

    if (major < 0) {
        pr_info("mchar: fail to register major number!");
        ret = major;
       return -ENOMEM;
    }

    class = class_create(THIS_MODULE, CLASS_NAME);
    if (IS_ERR(class)){ 
        unregister_chrdev(major, DEVICE_NAME);
        pr_info("mchar: failed to register device class");
        ret = PTR_ERR(class);
        return -ENOMEM;
    }

    device = device_create(class, NULL, MKDEV(major, 0), NULL, DEVICE_NAME);
    if (IS_ERR(device)) {
        class_destroy(class);
        unregister_chrdev(major, DEVICE_NAME);
        ret = PTR_ERR(device);
        return -ENOMEM;
    }

	
	my_data = devm_kzalloc(&dev->dev, sizeof(struct xmt_dev), GFP_KERNEL);
	if(my_data == NULL) {
		printk("pcixmt - Error! Out of memory\n");
		return -ENOMEM;
	}
	pci_set_drvdata(dev, my_data);

	// enable the Device to be BusMaster
	pci_set_master(dev);

	status = pci_resource_len(dev, 0);
	printk("pcixmt - BAR0 is %d bytes in size\n", status);
	if(status != 256) {
		printk("pcixmt - Wrong size of BAR0!\n");
		return -1;
	}

	printk("pcixmt - BAR0 is mapped to 0x%llx\n", pci_resource_start(dev, 0));
	status = pcim_enable_device(dev);
	if(status < 0) {
		printk("pcixmt - Could not enable device\n");
		return status;
	}



	my_data->cpu_addr =dma_alloc_coherent(&dev->dev, MAX_SIZE, &my_data->dma_handle, GFP_KERNEL);

	// @TODO check if this can be attached to the device?
	xmt_mem=	my_data->cpu_addr;

	if (!my_data->cpu_addr) {
		printk("pcixmt - Could no allocate memory\n");
		return -ENOMEM;
	}
	printk("pcixmt - Got local memory at 0x%llx\n",(u64) my_data->cpu_addr);

	return 0;
}

static void pcixmt_remove(struct pci_dev *dev) {
	struct xmt_dev * my_data = pci_get_drvdata(dev);
	dma_free_coherent(&dev->dev, 1024*1024*1024, my_data->cpu_addr,  my_data->dma_handle);
}


/* PCI driver struct */
static struct pci_driver pcixmt_driver = {
	.name = "pcixmt",
	.id_table = pcixmt_ids,
	.probe = pcixmt_probe,
	.remove = pcixmt_remove,
};



static int __init xmt_init(void)
{
	printk("pcixmt - Registering\n");
	return pci_register_driver(&pcixmt_driver);
	

}
module_init(xmt_init);

static void __exit xmt_exit(void)
{
	device_destroy(class, MKDEV(major, 0));  
	printk("pcixmt - Removed\n");
	pci_unregister_driver(&pcixmt_driver);

 class_unregister(class);
    class_destroy(class); 
    unregister_chrdev(major, DEVICE_NAME);
	
}
module_exit(xmt_exit);
