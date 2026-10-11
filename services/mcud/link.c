// SPDX-License-Identifier: BSD-2-Clause
/*
 * The SPI link to the receiver's main MCU: 1032-byte full duplex transfers
 * on SPI0, ARM_CS to select the MCU and to ask for a slot, ARM_RDY from the
 * MCU when it is ready. One queued frame goes out per transfer and whatever
 * the MCU clocks back is its frame for us. Timing as the stock spid.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <linux/spi/spidev.h>

#include <libubox/uloop.h>

#include "log.h"
#include "link.h"

/* /dev/gpiodev, the kernel's drivers/char/gpiodev */
struct gpio_cmd {
	unsigned int pin;
	unsigned int value;
};

#define GPIO_IOC_MAGIC	'G'
#define GPIO_SET_CMD	_IOW(GPIO_IOC_MAGIC, 1, int)
#define GPIO_GET_CMD	_IOR(GPIO_IOC_MAGIC, 2, int)
#define GPIO_DIR_CMD	_IOR(GPIO_IOC_MAGIC, 3, int)

#define ARM_CS_PIN	30	/* GPIO1_14, out, low = select / want a slot */
#define ARM_RDY_PIN	34	/* GPIO2_2, in, low = MCU ready */

#define QUEUE_LEN	32
#define CNT_SINGLE	0xffff	/* frame count field of a single frame */
#define BUS_IDLE	0x00	/* first byte when the MCU sends nothing */
#define BUS_FLOAT	0xff	/* ... or when nobody drives MISO */
#define IDLE_MS		12	/* poll interval with nothing to send */
#define GAP_MS		2	/* pause after a transfer */
#define ASK_MS		1	/* re-check RDY while asking for a slot */

static int gpio_fd = -1, spi_fd = -1;
static uint32_t speed;
static link_rx_fn rx_cb;
int link_trace;
static uint8_t queue[QUEUE_LEN][LINK_XFER_LEN];
static unsigned int q_head, q_count;
static struct uloop_timeout poll_timer;

static int gpio_dir(int pin, int in)
{
	struct gpio_cmd c = { pin, in };

	return ioctl(gpio_fd, GPIO_DIR_CMD, &c);
}

static void gpio_set(int pin, int value)
{
	struct gpio_cmd c = { pin, value };

	ioctl(gpio_fd, GPIO_SET_CMD, &c);
}

static int gpio_get(int pin)
{
	struct gpio_cmd c = { pin, 0 };

	if (ioctl(gpio_fd, GPIO_GET_CMD, &c) < 0) {
		return -errno;
	}

	return c.value;
}

int link_send(uint8_t cmd, const void *data, uint16_t len)
{
	uint8_t *f, sum = 0;
	int i;

	if (len > LINK_MAX_DATA) {
		return -EINVAL;
	}

	if (q_count == QUEUE_LEN) {
		return -ENOSPC;
	}

	f = queue[(q_head + q_count) % QUEUE_LEN];
	memset(f, 0, LINK_XFER_LEN);
	f[0] = cmd;
	f[1] = CNT_SINGLE & 0xff;
	f[2] = CNT_SINGLE >> 8;
	f[3] = len & 0xff;
	f[4] = len >> 8;
	if (len) {
		memcpy(f + 5, data, len);
	}

	for (i = 0; i < 5 + len; i++) {
		sum += f[i];
	}

	f[5 + len] = ~sum;
	q_count++;
	uloop_timeout_set(&poll_timer, 0);
	return 0;
}

unsigned int link_queued(void)
{
	return q_count;
}

/* "12345.678 tx 42 [20] 02 09 ..." on stderr, for -v */
static void trace(const char *dir, const uint8_t *frame)
{
	struct timespec ts;
	uint16_t len = frame[3] | (frame[4] << 8);
	int i;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	fprintf(stderr, "%5ld.%03ld %s %02x [%u]", (long)(ts.tv_sec % 100000),
		ts.tv_nsec / 1000000, dir, frame[0], len);
	for (i = 0; i < len && i < 32; i++) {
		fprintf(stderr, " %02x", frame[5 + i]);
	}

	fprintf(stderr, "%s\n", len > 32 ? " ..." : "");
}

static void parse_rx(const uint8_t *b)
{
	struct link_frame f;
	uint8_t sum = 0;
	int i;

	if (b[0] == BUS_IDLE || b[0] == BUS_FLOAT) {
		return;
	}

	if (link_trace) {
		trace("rx", b);
	}

	f.cmd = b[0];
	f.cnt = b[1] | (b[2] << 8);
	f.len = b[3] | (b[4] << 8);
	if (f.len > LINK_MAX_DATA) {
		log_msg(LOG_WARNING, "frame 0x%02x with length %u", f.cmd, f.len);
		return;
	}

	for (i = 0; i < 5 + f.len; i++) {
		sum += b[i];
	}

	sum = ~sum;
	f.data = b + 5;
	f.csum_ok = sum == b[5 + f.len];
	rx_cb(&f);
}

/* one transfer: select, 1 ms, 1032 bytes each way, 0.5 ms, deselect */
static void transfer(void)
{
	static const uint8_t idle[LINK_XFER_LEN];
	static uint8_t rx[LINK_XFER_LEN];
	const uint8_t *tx = q_count ? queue[q_head] : idle;
	struct spi_ioc_transfer x;

	gpio_set(ARM_CS_PIN, 0);
	usleep(1000);
	memset(&x, 0, sizeof(x));
	memset(rx, 0, sizeof(rx));
	x.tx_buf = (unsigned long)tx;
	x.rx_buf = (unsigned long)rx;
	x.len = LINK_XFER_LEN;
	x.speed_hz = speed;
	x.bits_per_word = 8;
	if (ioctl(spi_fd, SPI_IOC_MESSAGE(1), &x) < 0) {
		log_msg(LOG_ERR, "SPI transfer: %s", strerror(errno));
	}

	usleep(500);
	gpio_set(ARM_CS_PIN, 1);
	if (q_count) {
		if (link_trace) {
			trace("tx", tx);
		}

		q_head = (q_head + 1) % QUEUE_LEN;
		q_count--;
	}

	parse_rx(rx);
}

static void poll_cb(struct uloop_timeout *t)
{
	if (gpio_get(ARM_RDY_PIN) != 0) {
		/* MCU busy; holding ARM_CS low asks it for a slot */
		if (q_count) {
			gpio_set(ARM_CS_PIN, 0);
			uloop_timeout_set(t, ASK_MS);
		} else {
			uloop_timeout_set(t, IDLE_MS);
		}

		return;
	}

	transfer();
	uloop_timeout_set(t, GAP_MS);
}

int link_open(uint32_t spi_hz, link_rx_fn rx)
{
	uint8_t mode = SPI_MODE_0, bits = 8;

	gpio_fd = open("/dev/gpiodev", O_RDWR);
	if (gpio_fd < 0) {
		return -errno;
	}

	spi_fd = open("/dev/spidev0.0", O_RDWR);
	if (spi_fd < 0) {
		return -errno;
	}

	speed = spi_hz;
	ioctl(spi_fd, SPI_IOC_WR_MODE, &mode);
	ioctl(spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bits);
	ioctl(spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed);
	gpio_dir(ARM_CS_PIN, 0);
	gpio_set(ARM_CS_PIN, 1);
	gpio_dir(ARM_RDY_PIN, 1);
	rx_cb = rx;
	poll_timer.cb = poll_cb;
	uloop_timeout_set(&poll_timer, 0);
	return 0;
}

void link_close(void)
{
	uloop_timeout_cancel(&poll_timer);
	if (gpio_fd >= 0) {
		gpio_set(ARM_CS_PIN, 1);
		close(gpio_fd);
	}

	if (spi_fd >= 0) {
		close(spi_fd);
	}
}
