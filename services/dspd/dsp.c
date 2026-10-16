// SPDX-License-Identifier: BSD-2-Clause
/* The DSP itself: its power and reset, the image and its SDRAM part, the mute lines */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/gpio.h>

#include "dspd.h"
#include "log.h"

#define PSC0_BASE	0x01C10000u
#define PSC_MDSTAT_DSP	(0x800 + 4 * 15)
#define PSC_MDCTL_DSP	(0xA00 + 4 * 15)
#define PSC_ENABLE	3
#define MDCTL_LRST	(1u << 8)	/* 0 = DSP held in local reset */
#define SYSCFG_BASE	0x01C14000u
#define PSC1_BASE	0x01E27000u
#define PSC1_MDSTAT_MCASP1 (0x800 + 4 * 8)
#define MCASP1_BASE	0x01D04000u
#define MCASP_GBLCTL	0x44
#define EDMA_BASE	0x01C00000u
#define EDMA_EECR	0x1028
#define EDMA_MCASP1	(1u << 2 | 1u << 3)	/* its receive and transmit events */
#define HOST1CFG	0x44		/* DSP boot address */
#define L2_BASE		0x11800000u
#define L2_SIZE		0x40000u

#define GPIO_CHIP	"/dev/gpiochip0"
#define DIGMUT_PIN	16	/* GPIO1_0, high = outputs muted */
#define ARMZ2MUT_PIN	24	/* GPIO1_8, zone 2 */

#define START_WAIT_MS	1000

static volatile uint32_t *psc, *syscfg, *psc1, *mcasp1, *edma;
static volatile uint8_t *l2;
static volatile uint8_t *ddr;		/* the decoder's SDRAM, DSPAUDIO_DDR */
volatile struct dspaudio_shared *sh;
static int gpio_fd = -1;

static volatile uint32_t *map(int fd, uint32_t phys, uint32_t len)
{
	void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, phys);

	return p == MAP_FAILED ? NULL : p;
}

/* both mute lines as outputs of the GPIO character device, muted */
static int mute_request(void)
{
	struct gpio_v2_line_request req;
	int fd, ret;

	fd = open(GPIO_CHIP, O_RDWR);
	if (fd < 0) {
		return -1;
	}

	memset(&req, 0, sizeof(req));
	req.offsets[0] = DIGMUT_PIN;
	req.offsets[1] = ARMZ2MUT_PIN;
	req.num_lines = 2;
	strcpy(req.consumer, "dspd");
	req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
	req.config.num_attrs = 1;
	req.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
	req.config.attrs[0].attr.values = 3;
	req.config.attrs[0].mask = 3;

	ret = ioctl(fd, GPIO_V2_GET_LINE_IOCTL, &req);
	close(fd);
	return ret < 0 ? -1 : req.fd;
}

void mute_lines(int on)
{
	struct gpio_v2_line_values v = { .bits = on ? 3 : 0, .mask = 3 };

	if (gpio_fd < 0) {
		return;
	}

	ioctl(gpio_fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &v);
}

/*
 * A DMA image's McASP and EDMA run on without the DSP: stop them, or they
 * write into L2 while the next image is copied there.
 */
void dsp_stop(void)
{
	/* the DSP first, so that it cannot start them again */
	psc[PSC_MDCTL_DSP / 4] &= ~MDCTL_LRST;
	usleep(1000);

	if (edma) {
		edma[EDMA_EECR / 4] = EDMA_MCASP1;
	}
	if (psc1 && mcasp1 && (psc1[PSC1_MDSTAT_MCASP1 / 4] & 0x1F) == PSC_ENABLE) {
		mcasp1[MCASP_GBLCTL / 4] = 0;
	}
}

/*
 * An image built with the AC-3 decoder has its SDRAM part in a file beside
 * it, .ddr for .bin; none there is no error. SDRAM keeps what the last image
 * left there: its magic word is cleared first, so that an image never runs a
 * decoder it was not loaded with.
 */
static int dsp_load_ddr(const char *image)
{
	char path[256];
	size_t n = strlen(image);
	long size;
	FILE *f;
	int rc = 0;

	if (ddr) {
		*(volatile uint32_t *)ddr = 0;
	}

	if (n > 4 && !strcmp(image + n - 4, ".bin")) {
		n -= 4;
	}
	if (n + 5 > sizeof(path)) {
		return -ENAMETOOLONG;
	}
	memcpy(path, image, n);
	strcpy(path + n, ".ddr");

	f = fopen(path, "rb");
	if (!f) {
		return errno == ENOENT ? 0 : -errno;
	}

	fseek(f, 0, SEEK_END);
	size = ftell(f);
	rewind(f);
	if (!ddr) {
		rc = -ENXIO;
	} else if (size < 4 || size > (long)DSPAUDIO_DDR_LEN) {
		rc = -EFBIG;
	} else if (fread((void *)ddr, 1, size, f) != (size_t)size) {
		*(volatile uint32_t *)ddr = 0;
		rc = -EIO;
	}
	if (ddr) {
		msync((void *)ddr, DSPAUDIO_DDR_LEN, MS_SYNC);
	}

	fclose(f);
	return rc;
}

int dsp_start(const char *image)
{
	uint8_t *img;
	long size;
	FILE *f;
	int t;

	if ((psc[PSC_MDSTAT_DSP / 4] & 0x1F) != PSC_ENABLE) {
		log_msg(LOG_ERR, "DSP module not powered");
		return -ENODEV;
	}

	f = fopen(image, "rb");
	if (!f) {
		return -errno;
	}

	fseek(f, 0, SEEK_END);
	size = ftell(f);
	rewind(f);
	if (size <= 0 || DSPAUDIO_LOAD + size > DSPAUDIO_L2_END) {
		fclose(f);
		return -EFBIG;
	}

	img = malloc(size);
	if (!img || fread(img, 1, size, f) != (size_t)size) {
		free(img);
		fclose(f);
		return -EIO;
	}

	fclose(f);
	dsp_stop();
	memcpy((void *)(l2 + DSPAUDIO_LOAD - L2_BASE), img, size);
	free(img);
	t = dsp_load_ddr(image);
	if (t) {
		log_msg(LOG_ERR, "%s: its SDRAM part: %s", image, strerror(-t));
		return t;
	}
	memset((void *)sh, 0, sizeof(*sh));
	apply();
	apply_tone();
	apply_noise();
	apply_delays();
	apply_av_sync();
	apply_dvol();
	apply_surround();
	apply_reverb();
	apply_bass();
	msync((void *)l2, L2_SIZE, MS_SYNC);
	syscfg[HOST1CFG / 4] = DSPAUDIO_LOAD;
	psc[PSC_MDCTL_DSP / 4] |= MDCTL_LRST;
	for (t = 0; t < START_WAIT_MS; t += 10) {
		if (sh->magic == DSPAUDIO_MAGIC) {
			if (sh->abi != DSPAUDIO_ABI) {
				log_msg(LOG_ERR, "%s has ABI %u, dspd needs %u",
					image, sh->abi, DSPAUDIO_ABI);
				return -EPROTO;
			}

			return 0;
		}

		usleep(10000);
	}

	log_msg(LOG_ERR, "DSP did not start, state %u", sh->state);
	return -ETIMEDOUT;
}

/* the registers and memory dspd works with, through /dev/mem */
int dsp_open(void)
{
	int fd = open("/dev/mem", O_RDWR | O_SYNC);

	if (fd < 0) {
		log_msg(LOG_ERR, "/dev/mem: %s", strerror(errno));
		return -1;
	}

	psc = map(fd, PSC0_BASE, 0x1000);
	syscfg = map(fd, SYSCFG_BASE, 0x1000);
	psc1 = map(fd, PSC1_BASE, 0x1000);
	mcasp1 = map(fd, MCASP1_BASE, 0x1000);
	edma = map(fd, EDMA_BASE, 0x2000);
	l2 = (volatile uint8_t *)map(fd, L2_BASE, L2_SIZE);
	ddr = (volatile uint8_t *)map(fd, DSPAUDIO_DDR, DSPAUDIO_DDR_LEN);
	if (!psc || !syscfg || !l2) {
		log_msg(LOG_ERR, "mmap: %s", strerror(errno));
		return -1;
	}

	sh = (volatile struct dspaudio_shared *)(l2 + DSPAUDIO_SHARED - L2_BASE);
	gpio_fd = mute_request();
	return 0;
}

