/**
*  \details    Simple Linux device driver (IOCTL)
*  \author     EmbeTronicX
**/
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kdev_t.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/slab.h>	
#include <linux/uaccess.h>
#include <linux/ioctl.h>

#include <linux/dma-noncoherent.h>
 
#define WR_VALUE _IOW('a','a',int32_t*)
#define RD_VALUE _IOR('a','b',int32_t*)
#define UNIT_NAME	"Siklu DMA cache invalidator: "
 
dev_t dev = 0;
static struct class *dev_class;
static struct cdev etx_cdev;

static int      __init etx_driver_init(void);
static void     __exit etx_driver_exit(void);
static int      etx_open(struct inode *inode, struct file *file);
static int      etx_release(struct inode *inode, struct file *file);
static ssize_t  etx_read(struct file *filp, char __user *buf, size_t len,loff_t * off);
static ssize_t  etx_write(struct file *filp, const char *buf, size_t len, loff_t * off);
static long     etx_ioctl(struct file *file, unsigned int cmd, unsigned long arg);

struct siklu_dma_cache_args {
	phys_addr_t	paddr;
	size_t		len;
	uint32_t	dir;
};

static struct file_operations fops = {
	.owner		= THIS_MODULE,
	.read		= etx_read,
	.write		= etx_write,
	.open		= etx_open,
	.unlocked_ioctl = etx_ioctl,
	.release	= etx_release,
};

static int etx_open(struct inode *inode, struct file *file) {
	pr_info(UNIT_NAME "Device file opened\n");
	return 0;
}

static int etx_release(struct inode *inode, struct file *file) {
	pr_info(UNIT_NAME "Device file closed\n");
	return 0;
}

static ssize_t etx_read(struct file *filp, char __user *buf, size_t len,
		loff_t *off) {
	pr_info(UNIT_NAME "Read function\n");
	return 0;
}

static ssize_t etx_write(struct file *filp, const char __user *buf, size_t len,
		loff_t *off) {
	pr_info(UNIT_NAME "Write function\n");
	return len;
}

static long etx_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
	struct siklu_dma_cache_args args;
	switch(cmd) {
		case WR_VALUE:
			if (copy_from_user(&args ,(int32_t*) arg,
						sizeof(args))) {
				pr_err(UNIT_NAME "Data write error\n");
				return -EFAULT;
			}
			if ((enum dma_data_direction)args.dir == DMA_TO_DEVICE) {
				arch_sync_dma_for_device(NULL, args.paddr,
						args.len, DMA_TO_DEVICE);
				// pr_info(UNIT_NAME "DMA TO DEVICE [%lu]\n", args.len);
			}
			else if ((enum dma_data_direction)args.dir == 
					DMA_FROM_DEVICE) {
				arch_sync_dma_for_cpu(NULL, args.paddr,
					       args.len, DMA_FROM_DEVICE);
				// pr_info(UNIT_NAME "DMA FROM DEVICE [%lu]\n", args.len);
			}
			else {
				pr_err(UNIT_NAME "Unsupported direction\n");
				return -EINVAL;
			}
			break;
		default:
			pr_err(UNIT_NAME "IOCTL unsupported\n");
			return -EINVAL;
	}
	// pr_info(UNIT_NAME "IOCTL succeeded\n");
	return 0;
}
 
static int __init etx_driver_init(void) {
	/*Allocating Major number*/
	if ((alloc_chrdev_region(&dev, 0, 1, "etx_Dev")) < 0) {
		pr_err(UNIT_NAME "Cannot allocate major number\n");
		return -1;
	}
	pr_info(UNIT_NAME "Device numbers: major [%d], minor [%d]\n",
			MAJOR(dev), MINOR(dev));
 
	/*Creating cdev structure*/
	cdev_init(&etx_cdev,&fops);
 
	/*Adding character device to the system*/
	if ((cdev_add(&etx_cdev, dev, 1)) < 0) {
	    pr_err(UNIT_NAME "Cannot add the device\n");
	    goto r_class;
	}
 
	/*Creating struct class*/
	if ((dev_class = class_create(THIS_MODULE, "etx_class")) == NULL) {
	    pr_err(UNIT_NAME "Cannot create the struct class\n");
	    goto r_class;
	}
 
	/*Creating device*/
	if ((device_create(dev_class, NULL, dev, NULL, "etx_device")) == NULL) {
	    pr_err(UNIT_NAME "Cannot create device\n");
	    goto r_device;
	}
	pr_info(UNIT_NAME "Successfully inited\n");
	return 0;
 
r_device:
	class_destroy(dev_class);
r_class:
	unregister_chrdev_region(dev,1);
	return -1;
}

static void __exit etx_driver_exit(void)
{
	device_destroy(dev_class,dev);
	class_destroy(dev_class);
	cdev_del(&etx_cdev);
	unregister_chrdev_region(dev, 1);
}
 
module_init(etx_driver_init);
module_exit(etx_driver_exit);
 
MODULE_LICENSE("GPL");
MODULE_AUTHOR("EmbeTronicX <embetronicx@gmail.com>");
MODULE_DESCRIPTION("Simple Linux device driver (IOCTL)");
MODULE_VERSION("1.5");
