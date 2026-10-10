/*
 * OMAP-L137/DA830 GPIO Character Device Driver
 * Compatible with 2.6.33-rc kernel
 *
 * Provides /dev/gpiodev interface for GPIO control via ioctl
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/gpio.h>
#include <linux/uaccess.h>
#include <linux/slab.h>

#include "gpiodev.h"

#define DEVICE_NAME "gpiodev"
#define CLASS_NAME "gpiodev"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("OMAP GPIO Driver");
MODULE_DESCRIPTION("GPIO character device for OMAP-L137/DA830");
MODULE_VERSION("1.0");

static int major_number;
static struct class *gpiodev_class = NULL;
static struct device *gpiodev_device = NULL;

/* Track which GPIOs have been requested */
#define MAX_GPIOS 144  /* OMAP-L137 has up to 144 GPIOs */
static unsigned long gpio_requested[MAX_GPIOS / (sizeof(unsigned long) * 8) + 1];

static int gpiodev_open(struct inode *inodep, struct file *filep)
{
	// printk(KERN_INFO "gpiodev: Device opened\n");
	return 0;
}

static int gpiodev_release(struct inode *inodep, struct file *filep)
{
	// printk(KERN_INFO "gpiodev: Device closed\n");
	return 0;
}

static int gpio_setup_pin(unsigned int gpio_pin, int value)
{
	int ret;

	if (gpio_pin >= MAX_GPIOS) {
		// printk(KERN_ERR "gpiodev: Invalid GPIO pin %u\n", gpio_pin);
		return -EINVAL;
	}

	/* Request GPIO if not already requested */
	if (!test_bit(gpio_pin, gpio_requested)) {
		ret = gpio_request(gpio_pin, DEVICE_NAME);
		if (ret < 0) {
			// printk(KERN_ERR "gpiodev: Failed to request GPIO %u: %d\n",
			//        gpio_pin, ret);
			return ret;
		}
		set_bit(gpio_pin, gpio_requested);
	}

	/* Set direction to output */
	ret = gpio_direction_output(gpio_pin, value);
	if (ret < 0) {
		// printk(KERN_ERR "gpiodev: Failed to set GPIO %u direction: %d\n",
		//        gpio_pin, ret);
		return ret;
	}

	return 0;
}

static long gpiodev_ioctl(struct file *filep, unsigned int cmd, unsigned long arg)
{
	struct gpio_cmd gpio_data;
	int ret;

	switch (cmd) {
	case GPIO_SET_CMD:
		/* Copy data from user space */
		if (copy_from_user(&gpio_data, (struct gpio_cmd __user *)arg,
				   sizeof(struct gpio_cmd))) {
			// printk(KERN_ERR "gpiodev: Failed to copy data from user\n");
			return -EFAULT;
		}

		// printk(KERN_INFO "gpiodev: GPIO_SET_CMD - pin=%u, value=%u\n",
		//        gpio_data.pin, gpio_data.value);

		/* Setup and set the GPIO */
		ret = gpio_setup_pin(gpio_data.pin, gpio_data.value);
		if (ret < 0)
			return ret;

		/* Set the value */
		gpio_set_value(gpio_data.pin, gpio_data.value);
		break;

	case GPIO_GET_CMD:
		/* Copy pin number from user space */
		if (copy_from_user(&gpio_data, (struct gpio_cmd __user *)arg,
				   sizeof(struct gpio_cmd))) {
			return -EFAULT;
		}

		if (gpio_data.pin >= MAX_GPIOS)
			return -EINVAL;

		/* Request GPIO if needed and set as input */
		if (!test_bit(gpio_data.pin, gpio_requested)) {
			ret = gpio_request(gpio_data.pin, DEVICE_NAME);
			if (ret < 0)
				return ret;
			set_bit(gpio_data.pin, gpio_requested);
		}

		ret = gpio_direction_input(gpio_data.pin);
		if (ret < 0)
			return ret;

		/* Read the value and normalize to 0 or 1 */
		gpio_data.value = gpio_get_value(gpio_data.pin) ? 1 : 0;

		/* Copy result back to user space */
		if (copy_to_user((struct gpio_cmd __user *)arg, &gpio_data,
				 sizeof(struct gpio_cmd))) {
			return -EFAULT;
		}

		// printk(KERN_INFO "gpiodev: GPIO_GET_CMD - pin=%u, value=%u\n",
		//        gpio_data.pin, gpio_data.value);
		break;

	case GPIO_DIR_CMD:
		/* Copy data from user space */
		if (copy_from_user(&gpio_data, (struct gpio_cmd __user *)arg,
				   sizeof(struct gpio_cmd))) {
			return -EFAULT;
		}

		if (gpio_data.pin >= MAX_GPIOS)
			return -EINVAL;

		// printk(KERN_INFO "gpiodev: GPIO_DIR_CMD - pin=%u, dir=%u (0=out,1=in)\n",
		//        gpio_data.pin, gpio_data.value);

		/* Request GPIO if not already requested */
		if (!test_bit(gpio_data.pin, gpio_requested)) {
			ret = gpio_request(gpio_data.pin, DEVICE_NAME);
			if (ret < 0) {
				// printk(KERN_ERR "gpiodev: Failed to request GPIO %u: %d\n",
				//        gpio_data.pin, ret);
				return ret;
			}
			set_bit(gpio_data.pin, gpio_requested);
		}

		/* Set direction: 0=output, non-zero=input */
		if (gpio_data.value == 0) {
			ret = gpio_direction_output(gpio_data.pin, 0);
		} else {
			ret = gpio_direction_input(gpio_data.pin);
		}

		if (ret < 0) {
			// printk(KERN_ERR "gpiodev: Failed to set GPIO %u direction: %d\n",
			//        gpio_data.pin, ret);
			return ret;
		}
		break;

	case GPIO_SPI0_MODE_CMD:
		/* Copy data from user space */
		if (copy_from_user(&gpio_data, (struct gpio_cmd __user *)arg,
				   sizeof(struct gpio_cmd))) {
			return -EFAULT;
		}

		printk(KERN_INFO "gpiodev: GPIO_SPI0_MODE_CMD - mode=%u (0=SPI0,1=GPIO)\n",
		       gpio_data.value);

		/*
		 * This command toggles SPI0 pins between SPI0 mode and GPIO mode.
		 * On OMAP-L137/DA830, this typically involves pinmux configuration.
		 *
		 * For now, we just acknowledge the command. To fully implement this,
		 * you would need to use davinci_cfg_reg() or similar pinmux API
		 * to reconfigure the pins between SPI0 and GPIO functions.
		 *
		 * Example pins affected (OMAP-L137):
		 * - SPI0_SCLK, SPI0_SCS0, SPI0_SDI, SPI0_SDO
		 */

		/* TODO: Implement actual pinmux switching if needed */
		printk(KERN_WARNING "gpiodev: SPI0 mode switching not fully implemented\n");
		break;

	default:
		printk(KERN_WARNING "gpiodev: Unknown ioctl command 0x%x\n", cmd);
		return -EINVAL;
	}

	return 0;
}

static struct file_operations fops = {
	.open = gpiodev_open,
	.release = gpiodev_release,
	.unlocked_ioctl = gpiodev_ioctl,
	.owner = THIS_MODULE,
};

static int __init gpiodev_init(void)
{
	printk(KERN_INFO "gpiodev: Initializing GPIO device driver\n");

	/* Register character device */
	major_number = register_chrdev(0, DEVICE_NAME, &fops);
	if (major_number < 0) {
		printk(KERN_ALERT "gpiodev: Failed to register major number\n");
		return major_number;
	}
	printk(KERN_INFO "gpiodev: Registered with major number %d\n", major_number);

	/* Register device class */
	gpiodev_class = class_create(THIS_MODULE, CLASS_NAME);
	if (IS_ERR(gpiodev_class)) {
		unregister_chrdev(major_number, DEVICE_NAME);
		printk(KERN_ALERT "gpiodev: Failed to register device class\n");
		return PTR_ERR(gpiodev_class);
	}
	printk(KERN_INFO "gpiodev: Device class registered\n");

	/* Register device driver */
	gpiodev_device = device_create(gpiodev_class, NULL, MKDEV(major_number, 0),
				       NULL, DEVICE_NAME);
	if (IS_ERR(gpiodev_device)) {
		class_destroy(gpiodev_class);
		unregister_chrdev(major_number, DEVICE_NAME);
		printk(KERN_ALERT "gpiodev: Failed to create device\n");
		return PTR_ERR(gpiodev_device);
	}

	printk(KERN_INFO "gpiodev: Device created successfully\n");
	return 0;
}

static void __exit gpiodev_exit(void)
{
	int i;

	/* Free all requested GPIOs */
	for (i = 0; i < MAX_GPIOS; i++) {
		if (test_bit(i, gpio_requested)) {
			gpio_free(i);
		}
	}

	device_destroy(gpiodev_class, MKDEV(major_number, 0));
	class_unregister(gpiodev_class);
	class_destroy(gpiodev_class);
	unregister_chrdev(major_number, DEVICE_NAME);

	printk(KERN_INFO "gpiodev: Device driver unloaded\n");
}

module_init(gpiodev_init);
module_exit(gpiodev_exit);
