/*
 * gpiodev.h - Header file for GPIO device driver
 *
 * ioctl definitions and data structures for /dev/gpiodev
 */

#ifndef GPIODEV_H
#define GPIODEV_H

#include <linux/ioctl.h>

/* GPIO command structure */
struct gpio_cmd {
	unsigned int pin;    /* GPIO pin number */
	unsigned int value;  /* GPIO value (0 or 1) */
};

/* ioctl magic number */
#define GPIO_IOC_MAGIC 'G'

/*
 * ioctl commands discovered from binary analysis:
 *
 * GPIO_SET_CMD       (0x40044701) - Set GPIO pin output value
 * GPIO_GET_CMD       (0x80044702) - Get GPIO pin input value
 * GPIO_DIR_CMD       (0x80044703) - Set GPIO pin direction (0=output, 1=input)
 * GPIO_SPI0_MODE_CMD (0x40044704) - Toggle SPI0/GPIO mode (0=SPI0, 1=GPIO)
 *
 * Note: The binary encodes size=4 in the ioctl commands, even though
 * struct gpio_cmd is 8 bytes. We must match the exact values.
 */
#define GPIO_SET_CMD        _IOW(GPIO_IOC_MAGIC, 1, int)
#define GPIO_GET_CMD        _IOR(GPIO_IOC_MAGIC, 2, int)
#define GPIO_DIR_CMD        _IOR(GPIO_IOC_MAGIC, 3, int)
#define GPIO_SPI0_MODE_CMD  _IOW(GPIO_IOC_MAGIC, 4, int)

#endif /* GPIODEV_H */
