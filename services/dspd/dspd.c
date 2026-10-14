// SPDX-License-Identifier: BSD-2-Clause
/*
 * dspd: runs the DSP firmware (dspaudio) and sets its gain and routing over
 * the ubus. Volume comes in the unit the MCU reports it in (0.01 dB); dspd
 * turns it into the 8.24 gains the DSP multiplies with.
 *
 *   dspd [-f image]    default /usr/lib/osiris/dspaudio.bin
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <libubox/blobmsg.h>
#include <libubox/uloop.h>
#include <libubus.h>

#include "log.h"
#include "dspaudio.h"

#define DEFAULT_IMAGE	"/usr/lib/osiris/dspaudio.bin"

#define PSC0_BASE	0x01C10000u
#define PSC_MDSTAT_DSP	(0x800 + 4 * 15)
#define PSC_MDCTL_DSP	(0xA00 + 4 * 15)
#define PSC_ENABLE	3
#define MDCTL_LRST	(1u << 8)	/* 0 = DSP held in local reset */
#define SYSCFG_BASE	0x01C14000u
#define HOST1CFG	0x44		/* DSP boot address */
#define L2_BASE		0x11800000u
#define L2_SIZE		0x40000u

/* /dev/gpiodev, the kernel's drivers/char/gpiodev */
struct gpio_cmd {
	unsigned int pin;
	unsigned int value;
};

#define GPIO_IOC_MAGIC	'G'
#define GPIO_SET_CMD	_IOW(GPIO_IOC_MAGIC, 1, int)
#define GPIO_DIR_CMD	_IOR(GPIO_IOC_MAGIC, 3, int)
#define DIGMUT_PIN	16	/* GPIO1_0, high = outputs muted */
#define ARMZ2MUT_PIN	24	/* GPIO1_8, zone 2 */

#define START_WAIT_MS	1000
#define VOLUME_MUTE	(-10000)	/* -100 dB, what the MCU sends at the bottom */
#define TONE_MAX	12		/* dB either way; the MCU sends -10..+10 */
#define BASS_HZ		100.0
#define TREBLE_HZ	10000.0
#define SAMPLE_HZ	48000.0
#define NOISE_RMS	0.58194		/* the pink filter's output for an RMS of 1 */
#define NOISE_MAX	(-15)		/* dBFS RMS; peaks are 12.8 dB higher */
#define NOISE_MIN	(-80)
#define SOUND_CM_S	34300.0

enum outputs { OUTPUTS_FRONT, OUTPUTS_ALL, OUTPUTS_MONO, __OUTPUTS_MAX };

static const char *const outputs_name[__OUTPUTS_MAX] = {
	[OUTPUTS_FRONT] = "front",
	[OUTPUTS_ALL] = "all",
	[OUTPUTS_MONO] = "mono",
};

static struct ubus_context *ctx;
static struct blob_buf b;
static volatile uint32_t *psc, *syscfg;
static volatile uint8_t *l2;
static volatile struct dspaudio_shared *sh;
static int gpio_fd = -1;

static int volume;			/* 0.01 dB */
static int muted;
static enum outputs outputs = OUTPUTS_FRONT;
static int bass, treble;		/* dB */
static int noise_out = -1;		/* enum dspaudio_out, -1 off */
static int noise_level = -30;		/* dBFS RMS */
static int distance[DSPAUDIO_OUT];	/* cm, 0 = not known */

/*
 * Surround from stereo (struct dspaudio_surround). Movie puts dialogue in
 * the centre and holds the surrounds back so that the front wins; music
 * keeps a wider front and wraps some of it around; game reacts fastest and
 * keeps the surrounds bright.
 */
struct surround_preset {
	const char *name;
	double tau_ms;		/* correlation estimate */
	double center, surround, wrap, back, sub;
	double delay_ms, back_delay_ms;
	double high_pass_hz, low_pass_hz;	/* band of the surrounds */
};

static const struct surround_preset surround_presets[] = {
	{ "movie", 10, 1.0, 1.0, 0.0, 0.7, 0.5, 10, 5, 100, 7000 },
	{ "music", 40, 0.5, 0.8, 0.25, 0.5, 0.5, 2, 5, 100, 16000 },
	{ "game", 10, 0.8, 1.0, 0.15, 0.8, 0.5, 0, 3, 80, 18000 },
};

static const struct surround_preset *surround;	/* NULL: off */

static const char *const out_name[DSPAUDIO_OUT] = {
	[DSPAUDIO_OUT_FL] = "fl", [DSPAUDIO_OUT_FR] = "fr",
	[DSPAUDIO_OUT_C] = "c", [DSPAUDIO_OUT_SW] = "sw",
	[DSPAUDIO_OUT_SL] = "sl", [DSPAUDIO_OUT_SR] = "sr",
	[DSPAUDIO_OUT_SBL] = "sbl", [DSPAUDIO_OUT_SBR] = "sbr",
};

static volatile uint32_t *map(int fd, uint32_t phys, uint32_t len)
{
	void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, phys);

	return p == MAP_FAILED ? NULL : p;
}

static void mute_lines(int on)
{
	struct gpio_cmd c;
	const unsigned int pins[] = { DIGMUT_PIN, ARMZ2MUT_PIN };
	size_t i;

	if (gpio_fd < 0) {
		return;
	}

	for (i = 0; i < ARRAY_SIZE(pins); i++) {
		c.pin = pins[i];
		c.value = 0;			/* output */
		ioctl(gpio_fd, GPIO_DIR_CMD, &c);
		c.value = on;
		ioctl(gpio_fd, GPIO_SET_CMD, &c);
	}
}

/* 0.01 dB -> 8.24 gain */
static int32_t gain_of(int vol)
{
	double g;

	if (muted || vol <= VOLUME_MUTE) {
		return 0;
	}

	g = pow(10.0, vol / 2000.0) * DSPAUDIO_GAIN_ONE;
	return g >= INT32_MAX ? INT32_MAX : (int32_t)g;
}

/*
 * How much of the left and right input each output gets, in halves: the
 * front pair alone, stereo on every speaker with left + right at -6 dB
 * each on the centre and the subwoofer, or that mono mix everywhere.
 */
static const uint8_t mix[__OUTPUTS_MAX][DSPAUDIO_OUT][DSPAUDIO_IN] = {
	[OUTPUTS_FRONT] = {
		[DSPAUDIO_OUT_FL] = { 2, 0 },
		[DSPAUDIO_OUT_FR] = { 0, 2 },
	},
	[OUTPUTS_ALL] = {
		[DSPAUDIO_OUT_FL] = { 2, 0 },
		[DSPAUDIO_OUT_FR] = { 0, 2 },
		[DSPAUDIO_OUT_C] = { 1, 1 },
		[DSPAUDIO_OUT_SW] = { 1, 1 },
		[DSPAUDIO_OUT_SL] = { 2, 0 },
		[DSPAUDIO_OUT_SR] = { 0, 2 },
		[DSPAUDIO_OUT_SBL] = { 2, 0 },
		[DSPAUDIO_OUT_SBR] = { 0, 2 },
	},
	[OUTPUTS_MONO] = {
		[DSPAUDIO_OUT_FL] = { 1, 1 },
		[DSPAUDIO_OUT_FR] = { 1, 1 },
		[DSPAUDIO_OUT_C] = { 1, 1 },
		[DSPAUDIO_OUT_SW] = { 1, 1 },
		[DSPAUDIO_OUT_SL] = { 1, 1 },
		[DSPAUDIO_OUT_SR] = { 1, 1 },
		[DSPAUDIO_OUT_SBL] = { 1, 1 },
		[DSPAUDIO_OUT_SBR] = { 1, 1 },
	},
};

/* RBJ cookbook shelving filter, slope 1: low shelf for bass, high for treble */
static void shelf(volatile struct dspaudio_biquad *q, double hz, int db, int high)
{
	double a = pow(10.0, db / 40.0), w = 2.0 * M_PI * hz / SAMPLE_HZ;
	double c = cos(w), alpha = sin(w) / 2.0 * sqrt(2.0), r = 2.0 * sqrt(a) * alpha;
	double s = high ? -1.0 : 1.0, a0;

	a0 = (a + 1) + s * (a - 1) * c + r;
	q->b0 = a * ((a + 1) - s * (a - 1) * c + r) / a0;
	q->b1 = s * 2 * a * ((a - 1) - s * (a + 1) * c) / a0;
	q->b2 = a * ((a + 1) - s * (a - 1) * c - r) / a0;
	q->a1 = -s * 2 * ((a - 1) + s * (a + 1) * c) / a0;
	q->a2 = ((a + 1) + s * (a - 1) * c - r) / a0;
}

/* RBJ cookbook high or low pass, Q 0.707 */
static void pass(volatile struct dspaudio_biquad *q, double hz, int high)
{
	double w = 2.0 * M_PI * hz / SAMPLE_HZ, c = cos(w), alpha = sin(w) / (2.0 * M_SQRT1_2);
	double a0 = 1.0 + alpha, k = high ? (1.0 + c) / 2.0 : (1.0 - c) / 2.0;

	q->b0 = k / a0;
	q->b1 = (high ? -2.0 * k : 2.0 * k) / a0;
	q->b2 = k / a0;
	q->a1 = -2.0 * c / a0;
	q->a2 = (1.0 - alpha) / a0;
}

static uint32_t ms_samples(double ms)
{
	uint32_t n = lround(ms * SAMPLE_HZ / 1000.0);

	return n > DSPAUDIO_SURROUND_DELAY_MAX ? DSPAUDIO_SURROUND_DELAY_MAX : n;
}

/* the DSP leaves the decoder alone while it is off: write it, then turn it on */
static void apply_surround(void)
{
	volatile struct dspaudio_surround *s = &sh->surround;
	const struct surround_preset *p = surround;

	s->on = 0;
	__sync_synchronize();
	if (!p) {
		return;
	}

	s->smooth = 1.0 - exp(-1000.0 / (p->tau_ms * SAMPLE_HZ));
	s->center = p->center;
	s->surround = p->surround;
	s->wrap = p->wrap;
	s->back = p->back;
	s->sub = p->sub;
	s->delay = ms_samples(p->delay_ms);
	s->back_delay = ms_samples(p->back_delay_ms);
	pass(&s->band[0], p->high_pass_hz, 1);
	pass(&s->band[1], p->low_pass_hz, 0);
	__sync_synchronize();
	s->on = 1;
}

static void apply_noise(void)
{
	sh->noise_gain = pow(10.0, noise_level / 20.0) * NOISE_RMS * 2147483647.0;
	__sync_synchronize();
	sh->noise_out = noise_out < 0 ? DSPAUDIO_NOISE_OFF : (uint32_t)noise_out;
}

/* fill the set the DSP is not using, then switch it over */
static void apply_tone(void)
{
	uint32_t next = (sh->tone_sel + 1) & 1;

	shelf(&sh->tone[next][0], BASS_HZ, bass, 0);
	shelf(&sh->tone[next][1], TREBLE_HZ, treble, 1);
	__sync_synchronize();
	sh->tone_sel = next;
}

/* hold the nearer speakers back until the farthest one's sound arrives */
static void apply_delays(void)
{
	int far = 0, d;
	unsigned int out;

	for (out = 0; out < DSPAUDIO_OUT; out++) {
		if (distance[out] > far) {
			far = distance[out];
		}
	}

	for (out = 0; out < DSPAUDIO_OUT; out++) {
		d = distance[out] ? (int)lround((far - distance[out]) * SAMPLE_HZ / SOUND_CM_S) : 0;
		sh->delay[out] = d > DSPAUDIO_DELAY_MAX ? DSPAUDIO_DELAY_MAX : d;
	}
}

static void apply(void)
{
	/* the test noise plays alone */
	int32_t g = noise_out < 0 ? gain_of(volume) : 0;
	unsigned int out, in;

	for (out = 0; out < DSPAUDIO_OUT; out++) {
		for (in = 0; in < DSPAUDIO_IN; in++) {
			/* the decoder makes the outputs; [0] only scales them */
			if (surround) {
				sh->gain[out][in] = in == 0 ? g : 0;
			} else {
				sh->gain[out][in] = g / 2 * mix[outputs][out][in];
			}
		}
	}
}

static void dsp_stop(void)
{
	psc[PSC_MDCTL_DSP / 4] &= ~MDCTL_LRST;
	usleep(1000);
}

static int dsp_start(const char *image)
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
	if (size <= 0 || DSPAUDIO_LOAD + size > DSPAUDIO_SHARED) {
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
	memset((void *)sh, 0, sizeof(*sh));
	apply();
	apply_tone();
	apply_noise();
	apply_delays();
	apply_surround();
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

/* dBFS of a peak, -999 for silence */
static int peak_db(uint32_t peak)
{
	return peak ? (int)lround(20.0 * log10(peak / 2147483648.0)) : -999;
}

static int m_status(struct ubus_context *c, struct ubus_object *obj,
		    struct ubus_request_data *req, const char *method,
		    struct blob_attr *msg)
{
	char version[sizeof(sh->version) + 1];
	void *a;
	int i;

	memcpy(version, (const void *)sh->version, sizeof(sh->version));
	version[sizeof(sh->version)] = 0;
	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "version", version);
	blobmsg_add_u8(&b, "running", sh->magic == DSPAUDIO_MAGIC);
	blobmsg_add_u32(&b, "state", sh->state);
	blobmsg_add_u32(&b, "samples", sh->samples);
	blobmsg_add_u32(&b, "underruns", sh->underruns);
	blobmsg_add_u32(&b, "overruns", sh->overruns);
	a = blobmsg_open_array(&b, "peak_db");
	for (i = 0; i < DSPAUDIO_IN; i++) {
		blobmsg_add_u32(&b, NULL, (uint32_t)peak_db(sh->peak[i]));
		sh->peak[i] = 0;
	}

	blobmsg_close_array(&b, a);
	blobmsg_add_u32(&b, "volume", (uint32_t)volume);
	blobmsg_add_u8(&b, "mute", muted);
	blobmsg_add_string(&b, "outputs", outputs_name[outputs]);
	blobmsg_add_string(&b, "surround", surround ? surround->name : "off");
	blobmsg_add_u32(&b, "bass", (uint32_t)bass);
	blobmsg_add_u32(&b, "treble", (uint32_t)treble);
	blobmsg_add_string(&b, "testtone", noise_out < 0 ? "off" : out_name[noise_out]);
	blobmsg_add_u32(&b, "testtone_level", (uint32_t)noise_level);
	a = blobmsg_open_table(&b, "distances");
	for (i = 0; i < DSPAUDIO_OUT; i++) {
		if (distance[i]) {
			blobmsg_add_u32(&b, out_name[i], (uint32_t)distance[i]);
		}
	}

	blobmsg_close_table(&b, a);
	ubus_send_reply(c, req, b.head);
	return 0;
}

enum { SET_VOLUME, SET_MUTE, SET_OUTPUTS, SET_BASS, SET_TREBLE, SET_TESTTONE,
	SET_TESTTONE_LEVEL, SET_DISTANCES, SET_SURROUND, __SET_MAX };

static const struct blobmsg_policy set_policy[__SET_MAX] = {
	[SET_VOLUME] = { .name = "volume", .type = BLOBMSG_TYPE_INT32 },
	[SET_MUTE] = { .name = "mute", .type = BLOBMSG_TYPE_BOOL },
	[SET_OUTPUTS] = { .name = "outputs", .type = BLOBMSG_TYPE_STRING },
	[SET_BASS] = { .name = "bass", .type = BLOBMSG_TYPE_INT32 },
	[SET_TREBLE] = { .name = "treble", .type = BLOBMSG_TYPE_INT32 },
	[SET_TESTTONE] = { .name = "testtone", .type = BLOBMSG_TYPE_STRING },
	[SET_TESTTONE_LEVEL] = { .name = "testtone_level", .type = BLOBMSG_TYPE_INT32 },
	[SET_DISTANCES] = { .name = "distances", .type = BLOBMSG_TYPE_TABLE },
	[SET_SURROUND] = { .name = "surround", .type = BLOBMSG_TYPE_STRING },
};

static int out_index(const char *name)
{
	int i;

	for (i = 0; i < DSPAUDIO_OUT; i++) {
		if (!strcmp(name, out_name[i])) {
			return i;
		}
	}

	return -1;
}

/* { "fl": 300, ... } in cm; outputs not named keep their distance */
static int set_distances(struct blob_attr *t)
{
	int d[DSPAUDIO_OUT], i, v;
	struct blob_attr *cur;
	unsigned int rem;

	memcpy(d, distance, sizeof(d));
	blobmsg_for_each_attr(cur, t, rem) {
		i = out_index(blobmsg_name(cur));
		if (i < 0 || blobmsg_type(cur) != BLOBMSG_TYPE_INT32) {
			return -1;
		}

		v = (int32_t)blobmsg_get_u32(cur);
		if (v < 0 || v > 2000) {
			return -1;
		}

		d[i] = v;
	}

	memcpy(distance, d, sizeof(d));
	apply_delays();
	return 0;
}

static int m_set(struct ubus_context *c, struct ubus_object *obj,
		 struct ubus_request_data *req, const char *method,
		 struct blob_attr *msg)
{
	struct blob_attr *tb[__SET_MAX];
	unsigned int i;
	const char *o;

	blobmsg_parse(set_policy, __SET_MAX, tb, blob_data(msg), blob_len(msg));
	if (tb[SET_OUTPUTS]) {
		o = blobmsg_get_string(tb[SET_OUTPUTS]);
		for (i = 0; i < __OUTPUTS_MAX; i++) {
			if (!strcmp(o, outputs_name[i])) {
				break;
			}
		}

		if (i == __OUTPUTS_MAX) {
			return UBUS_STATUS_INVALID_ARGUMENT;
		}

		outputs = i;
	}

	if (tb[SET_VOLUME]) {
		volume = (int32_t)blobmsg_get_u32(tb[SET_VOLUME]);
	}

	if (tb[SET_MUTE]) {
		muted = blobmsg_get_bool(tb[SET_MUTE]);
	}

	if (tb[SET_BASS] || tb[SET_TREBLE]) {
		int nb = tb[SET_BASS] ? (int32_t)blobmsg_get_u32(tb[SET_BASS]) : bass;
		int nt = tb[SET_TREBLE] ? (int32_t)blobmsg_get_u32(tb[SET_TREBLE]) : treble;

		if (nb < -TONE_MAX || nb > TONE_MAX || nt < -TONE_MAX || nt > TONE_MAX) {
			return UBUS_STATUS_INVALID_ARGUMENT;
		}

		bass = nb;
		treble = nt;
		apply_tone();
	}

	if (tb[SET_TESTTONE_LEVEL]) {
		int l = (int32_t)blobmsg_get_u32(tb[SET_TESTTONE_LEVEL]);

		if (l < NOISE_MIN || l > NOISE_MAX) {
			return UBUS_STATUS_INVALID_ARGUMENT;
		}

		noise_level = l;
	}

	if (tb[SET_TESTTONE]) {
		o = blobmsg_get_string(tb[SET_TESTTONE]);
		for (i = 0; i < DSPAUDIO_OUT; i++) {
			if (!strcmp(o, out_name[i])) {
				break;
			}
		}

		if (i == DSPAUDIO_OUT && strcmp(o, "off")) {
			return UBUS_STATUS_INVALID_ARGUMENT;
		}

		noise_out = i == DSPAUDIO_OUT ? -1 : (int)i;
	}

	if (tb[SET_TESTTONE] || tb[SET_TESTTONE_LEVEL]) {
		apply_noise();
	}

	if (tb[SET_DISTANCES] && set_distances(tb[SET_DISTANCES])) {
		return UBUS_STATUS_INVALID_ARGUMENT;
	}

	if (tb[SET_SURROUND]) {
		const struct surround_preset *p = NULL;

		o = blobmsg_get_string(tb[SET_SURROUND]);
		for (i = 0; i < ARRAY_SIZE(surround_presets); i++) {
			if (!strcmp(o, surround_presets[i].name)) {
				p = &surround_presets[i];
			}
		}

		if (!p && strcmp(o, "off")) {
			return UBUS_STATUS_INVALID_ARGUMENT;
		}

		surround = p;
		apply_surround();
	}

	apply();
	return 0;
}

static const struct ubus_method dsp_methods[] = {
	UBUS_METHOD_NOARG("status", m_status),
	UBUS_METHOD("set", m_set, set_policy),
};

static struct ubus_object_type dsp_type = UBUS_OBJECT_TYPE("dsp", dsp_methods);

static struct ubus_object dsp_object = {
	.name = "dsp",
	.type = &dsp_type,
	.methods = dsp_methods,
	.n_methods = ARRAY_SIZE(dsp_methods),
};

int main(int argc, char **argv)
{
	const char *image = DEFAULT_IMAGE;
	int opt, fd, rc;

	while ((opt = getopt(argc, argv, "f:")) != -1) {
		switch (opt) {
		case 'f':
			image = optarg;
			break;
		default:
			fprintf(stderr, "usage: %s [-f image]\n", argv[0]);
			return 1;
		}
	}

	openlog("dspd", LOG_PID, LOG_DAEMON);
	fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) {
		log_msg(LOG_ERR, "/dev/mem: %s", strerror(errno));
		return 1;
	}

	psc = map(fd, PSC0_BASE, 0x1000);
	syscfg = map(fd, SYSCFG_BASE, 0x1000);
	l2 = (volatile uint8_t *)map(fd, L2_BASE, L2_SIZE);
	if (!psc || !syscfg || !l2) {
		log_msg(LOG_ERR, "mmap: %s", strerror(errno));
		return 1;
	}

	sh = (volatile struct dspaudio_shared *)(l2 + DSPAUDIO_SHARED - L2_BASE);
	gpio_fd = open("/dev/gpiodev", O_RDWR);
	rc = dsp_start(image);
	if (rc) {
		log_msg(LOG_ERR, "%s: %s", image, strerror(-rc));
		return 1;
	}

	mute_lines(0);
	uloop_init();
	ctx = ubus_connect(NULL);
	if (!ctx) {
		log_msg(LOG_ERR, "cannot connect to ubusd");
		return 1;
	}

	ubus_add_uloop(ctx);
	rc = ubus_add_object(ctx, &dsp_object);
	if (rc) {
		log_msg(LOG_ERR, "ubus: %s", ubus_strerror(rc));
		return 1;
	}

	uloop_run();
	mute_lines(1);
	dsp_stop();
	ubus_free(ctx);
	uloop_done();
	return 0;
}
