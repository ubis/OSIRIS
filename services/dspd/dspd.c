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
#include <signal.h>
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
#define SAMPLE_HZ	48000.0		/* until told otherwise */
#define NOISE_RMS	0.58194		/* the pink filter's output for an RMS of 1 */
#define METER_MS	100		/* dsp.meter events, 10 a second */
#define BITSTREAM_MS	250		/* how often dsp.bitstream looks for a change */
#define NOISE_MAX	(-15)		/* dBFS RMS; peaks are 12.8 dB higher */
#define NOISE_MIN	(-80)
#define SOUND_CM_S	34300.0
#define LFE_GAIN	3.16227766	/* +10 dB */

enum outputs { OUTPUTS_FRONT, OUTPUTS_ALL, OUTPUTS_MONO, OUTPUTS_MULTI, __OUTPUTS_MAX };

static const char *const outputs_name[__OUTPUTS_MAX] = {
	[OUTPUTS_FRONT] = "front",
	[OUTPUTS_ALL] = "all",
	[OUTPUTS_MONO] = "mono",
	[OUTPUTS_MULTI] = "multichannel",
};

/*
 * What the source is: the analog inputs, a digital one, multichannel PCM
 * from HDMI on four lines, or audio from Linux through the ring (the NET
 * input).
 */
enum input { INPUT_ANALOG, INPUT_DIGITAL, INPUT_MULTI, INPUT_NETWORK, __INPUT_MAX };

static const char *const input_name[__INPUT_MAX] = {
	[INPUT_ANALOG] = "analog",
	[INPUT_DIGITAL] = "digital",
	[INPUT_MULTI] = "multichannel",
	[INPUT_NETWORK] = "network",
};

static struct ubus_context *ctx;
static struct blob_buf b;
static volatile uint32_t *psc, *syscfg, *psc1, *mcasp1, *edma;
static volatile uint8_t *l2;
static volatile uint8_t *ddr;		/* the decoder's SDRAM, DSPAUDIO_DDR */
static volatile struct dspaudio_shared *sh;
static int gpio_fd = -1;

static int volume;			/* 0.01 dB */
static int muted;
static enum outputs outputs = OUTPUTS_FRONT;
static enum input input = INPUT_ANALOG;
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
	double steer;		/* how much the correlation holds the surrounds back */
	double center, surround, wrap, back, sub;
	double delay_ms, back_delay_ms;
	double high_pass_hz, low_pass_hz;	/* band of the surrounds */
};

static const struct surround_preset surround_presets[] = {
	{ "movie", 10, 1.0, 1.0, 1.0, 0.0, 0.7, 0.5, 10, 5, 100, 7000 },
	/* steady surrounds: steering them makes music pulse */
	{ "music", 300, 0.25, 0.5, 0.8, 0.25, 0.5, 0.5, 2, 5, 100, 16000 },
	{ "game", 10, 1.0, 0.8, 1.0, 0.15, 0.8, 0.5, 0, 3, 80, 18000 },
};

static const struct surround_preset *surround;	/* NULL: off */

/*
 * Reverb for the sound field modes (struct dspaudio_reverb): the room's
 * size scales the feedback lines, the reverb time sets their decay.
 */
struct reverb_preset {
	const char *name;
	double size;		/* line length factor */
	double time_s;		/* reverb time, -60 dB */
	double damping;		/* high frequencies die sooner, 0..1 */
	double predelay_ms;
	double front, surround, back;
};

static const struct reverb_preset reverb_presets[] = {
	{ "hall", 2.0, 2.0, 0.35, 30, 0.12, 0.35, 0.30 },
	{ "theater", 1.5, 1.3, 0.40, 20, 0.08, 0.30, 0.30 },
	{ "club", 1.0, 0.9, 0.30, 12, 0.10, 0.30, 0.25 },
	{ "studio", 0.8, 0.6, 0.25, 8, 0.08, 0.25, 0.20 },
	{ "tv", 0.6, 0.4, 0.50, 5, 0.05, 0.15, 0.10 },
};

/* mutually prime, 25 to 41 ms */
static const int reverb_lines[DSPAUDIO_REVERB_LINES] = { 1201, 1423, 1693, 1951 };

static const struct reverb_preset *reverb;	/* NULL: off */

#define CROSSOVER_MIN	20		/* Hz */
#define CROSSOVER_MAX	250
#define DOUBLE_BASS_HZ	80
static int crossover[DSPAUDIO_OUT];	/* Hz, 0 = large: full range */
static int subwoofer, double_bass;
static int lfe_level;			/* dB, the receiver's LFE level; -100 off */
static int av_sync;			/* ms */
static double sample_hz = SAMPLE_HZ;	/* the source's */
static double proc_hz = SAMPLE_HZ;	/* the DSP's: above 48 kHz it processes every other frame */
static int loudness = -1;		/* dB of bass boost, -1 off */
static int dynamic_volume;		/* 0 off, 1 light, 2 medium, 3 heavy */
static int back_fill;			/* multichannel without surround back: fill it */
static int meter;			/* send dsp.meter events */
static int late_night;			/* 0 off, 1 low, 2 high */
static void meter_tick(struct uloop_timeout *t);
static struct uloop_timeout meter_timer = { .cb = meter_tick };
static void bitstream_tick(struct uloop_timeout *t);
static struct uloop_timeout bitstream_timer = { .cb = bitstream_tick };

/* Dynamic Volume: how hard each level pulls towards the target */
static const struct {
	double strength, boost_max, cut_max;
} dvol_presets[] = {
	{ 0.4, 6, 10 },
	{ 0.6, 10, 15 },
	{ 0.8, 15, 20 },
};

#define DVOL_GATE	-60.0		/* dB RMS of the front pair and centre */
#define DVOL_AVERAGE_MS	10000.0
#define DVOL_RMS_MS	400.0
#define DVOL_ATTACK_MS	20.0
#define DVOL_RELEASE_MS	2000.0
#define LOUDNESS_MAX	12

/* the receiver's equalizer bands; the subwoofer has the first five */
#define EQ_BANDS	15
#define EQ_DB_MAX	12
#define EQ_Q		2.0		/* about the 2/3 octave between bands */
static const double eq_hz[EQ_BANDS] = {
	25, 40, 63, 100, 160, 250, 400, 630, 1000, 1600, 2500, 4000, 6300, 10000, 16000,
};
static int eq[DSPAUDIO_OUT][EQ_BANDS];	/* dB */
static int sub_lowpass = 120;		/* Hz, 0 = none */
static uint32_t absent;			/* outputs without a speaker */

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
 * Multichannel from a stereo source is the front pair.
 */
static const uint8_t mix[__OUTPUTS_MAX][DSPAUDIO_OUT][2] = {
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
	[OUTPUTS_MULTI] = {
		[DSPAUDIO_OUT_FL] = { 2, 0 },
		[DSPAUDIO_OUT_FR] = { 0, 2 },
	},
};

/* a multichannel source as a stereo pair, without the LFE */
static const double downmix[2][DSPAUDIO_IN] = {
	{ 1, 0, M_SQRT1_2, 0, M_SQRT1_2, 0, M_SQRT1_2, 0 },
	{ 0, 1, M_SQRT1_2, 0, 0, M_SQRT1_2, 0, M_SQRT1_2 },
};

/* RBJ cookbook shelving filter, slope 1: low shelf for bass, high for treble */
static void shelf(volatile struct dspaudio_biquad *q, double hz, int db, int high)
{
	double a = pow(10.0, db / 40.0), w = 2.0 * M_PI * hz / proc_hz;
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
	double w = 2.0 * M_PI * hz / proc_hz, c = cos(w), alpha = sin(w) / (2.0 * M_SQRT1_2);
	double a0 = 1.0 + alpha, k = high ? (1.0 + c) / 2.0 : (1.0 - c) / 2.0;

	q->b0 = k / a0;
	q->b1 = (high ? -2.0 * k : 2.0 * k) / a0;
	q->b2 = k / a0;
	q->a1 = -2.0 * c / a0;
	q->a2 = (1.0 - alpha) / a0;
}

/* RBJ cookbook peaking filter */
static void peak(volatile struct dspaudio_biquad *q, double hz, int db)
{
	double a = pow(10.0, db / 40.0), w = 2.0 * M_PI * hz / proc_hz;
	double alpha = sin(w) / (2.0 * EQ_Q), a0 = 1.0 + alpha / a;

	q->b0 = (1.0 + alpha * a) / a0;
	q->b1 = -2.0 * cos(w) / a0;
	q->b2 = (1.0 - alpha * a) / a0;
	q->a1 = q->b1;
	q->a2 = (1.0 - alpha / a) / a0;
}

/* the bands that are not flat, the largest first when there are too many */
static unsigned int apply_eq(volatile struct dspaudio_bass *bm, unsigned int out)
{
	int used[EQ_BANDS] = { 0 };
	unsigned int n, k, best;

	for (n = 0; n < DSPAUDIO_EQ; n++) {
		best = EQ_BANDS;
		for (k = 0; k < EQ_BANDS; k++) {
			if (eq[out][k] && !used[k] &&
			    (best == EQ_BANDS || abs(eq[out][k]) > abs(eq[out][best]))) {
				best = k;
			}
		}

		if (best == EQ_BANDS) {
			break;
		}

		used[best] = 1;
		peak(&bm->eq[out][n], eq_hz[best], eq[out][best]);
	}

	return n;
}

/* Linkwitz-Riley: two Butterworth sections each side, the sides add up flat */
static void apply_bass(void)
{
	uint32_t gen = sh->bass_gen + 1, next = gen & 1;
	volatile struct dspaudio_bass *bm = &sh->bass[next];
	uint32_t small = 0, copy = 0;
	unsigned int out, k;
	int hz;

	for (out = 0; out < DSPAUDIO_OUT; out++) {
		bm->eq_n[out] = absent & (1u << out) ? 0 : apply_eq(bm, out);
		if (out == DSPAUDIO_OUT_SW || absent & (1u << out)) {
			continue;
		}

		hz = crossover[out];
		if (hz) {
			small |= 1u << out;
		} else if (double_bass && subwoofer &&
			   (out == DSPAUDIO_OUT_FL || out == DSPAUDIO_OUT_FR)) {
			copy |= 1u << out;
			hz = DOUBLE_BASS_HZ;
		} else {
			continue;
		}

		for (k = 0; k < DSPAUDIO_XO; k++) {
			pass(&bm->high[out][k], hz, 1);
			pass(&bm->low[out][k], hz, 0);
		}
	}

	for (k = 0; k < DSPAUDIO_XO; k++) {
		if (sub_lowpass) {
			pass(&bm->sub[k], sub_lowpass, 0);
		} else {
			bm->sub[k].b0 = 1;
			bm->sub[k].b1 = bm->sub[k].b2 = bm->sub[k].a1 = bm->sub[k].a2 = 0;
		}
	}

	/* on at 0 dB too: switching it on and off would clear the filters */
	bm->loud_on = loudness >= 0;
	if (loudness >= 0) {
		shelf(&bm->loud, BASS_HZ, loudness, 0);
	}

	bm->small = small;
	bm->copy = copy;
	bm->subwoofer = subwoofer;
	__sync_synchronize();
	sh->bass_gen = gen;
}

/* the words after the on flag, which the DSP does not read while it is off */
static void put_words(volatile void *dst, const void *src, size_t len)
{
	volatile uint32_t *d = (volatile uint32_t *)dst + 1;
	const uint32_t *w = (const uint32_t *)src + 1;
	size_t i;

	for (i = 1; i < len / 4; i++) {
		*d++ = *w++;
	}
}

static void apply_reverb(void)
{
	volatile struct dspaudio_reverb *s = &sh->reverb;
	const struct reverb_preset *p = reverb;
	struct dspaudio_reverb n = { 0 };
	unsigned int i;
	long k;

	if (p) {
		for (i = 0; i < DSPAUDIO_REVERB_LINES; i++) {
			k = lround(reverb_lines[i] * p->size * proc_hz / SAMPLE_HZ);
			k = k > DSPAUDIO_REVERB_LINE_MAX ? DSPAUDIO_REVERB_LINE_MAX : k;
			n.length[i] = k;
			n.decay[i] = pow(10.0, -3.0 * k / (p->time_s * proc_hz));
		}

		n.damping = p->damping;
		k = lround(p->predelay_ms * proc_hz / 1000.0);
		n.predelay = k > DSPAUDIO_PREDELAY_MAX ? DSPAUDIO_PREDELAY_MAX : k;
		n.front = p->front;
		n.surround = p->surround;
		n.back = p->back;
	}

	s->on = 0;
	__sync_synchronize();
	if (!p) {
		return;
	}

	put_words(s, &n, sizeof(n));
	__sync_synchronize();
	s->on = 1;
}

static float per_frame(double ms)
{
	return 1.0 - exp(-1000.0 / (ms * proc_hz));
}

static void apply_dvol(void)
{
	volatile struct dspaudio_dvol *s = &sh->dvol;
	struct dspaudio_dvol n = { 0 };

	s->on = 0;
	__sync_synchronize();
	if (dynamic_volume < 1 || dynamic_volume > (int)ARRAY_SIZE(dvol_presets)) {
		return;
	}

	n.strength = dvol_presets[dynamic_volume - 1].strength;
	n.boost_max = dvol_presets[dynamic_volume - 1].boost_max;
	n.cut_max = dvol_presets[dynamic_volume - 1].cut_max;
	n.gate = DVOL_GATE;
	n.rms = per_frame(DVOL_RMS_MS);
	n.average = per_frame(DVOL_AVERAGE_MS);
	n.attack = per_frame(DVOL_ATTACK_MS);
	n.release = per_frame(DVOL_RELEASE_MS);
	put_words(s, &n, sizeof(n));
	__sync_synchronize();
	s->on = 1;
}

static uint32_t ms_samples(double ms)
{
	uint32_t n = lround(ms * proc_hz / 1000.0);

	return n > DSPAUDIO_SURROUND_DELAY_MAX ? DSPAUDIO_SURROUND_DELAY_MAX : n;
}

/*
 * The DSP leaves the decoder alone while it is off, so it is worked out
 * first and only copied in that window. During it the DSP plays the gain
 * matrix, which for a sample or two is the left input on every output.
 */
static void apply_surround(void)
{
	volatile struct dspaudio_surround *s = &sh->surround;
	const struct surround_preset *p = surround;
	struct dspaudio_surround n = { 0 };

	if (p) {
		n.smooth = 1.0 - exp(-1000.0 / (p->tau_ms * proc_hz));
		/* without a centre speaker the voice stays in the front pair */
		n.center = absent & 1u << DSPAUDIO_OUT_C ? 0 : p->center;
		n.surround = p->surround;
		n.steer = p->steer;
		n.wrap = p->wrap;
		n.back = p->back;
		n.sub = p->sub;
		n.delay = ms_samples(p->delay_ms);
		n.back_delay = ms_samples(p->back_delay_ms);
		pass(&n.band[0], p->high_pass_hz, 1);
		pass(&n.band[1], p->low_pass_hz, 0);
	}

	s->on = 0;
	__sync_synchronize();
	if (!p) {
		return;
	}

	put_words(s, &n, sizeof(n));
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

/* every output held back by as much, the delay line takes 2.7 s at 48 kHz */
static void apply_av_sync(void)
{
	long n = av_sync > 0 ? lround(av_sync * proc_hz / 1000.0) : 0;

	sh->av_delay = n < DSPAUDIO_AV_LEN ? n : DSPAUDIO_AV_LEN - 1;
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
		d = distance[out] ? (int)lround((far - distance[out]) * proc_hz / SOUND_CM_S) : 0;
		sh->delay[out] = d > DSPAUDIO_DELAY_MAX ? DSPAUDIO_DELAY_MAX : d;
	}
}

/* move what an output without a speaker would play onto others */
static void fold(double (*m)[DSPAUDIO_IN], unsigned int from, unsigned int to,
		 unsigned int to2, double k)
{
	unsigned int in;

	if (!(absent & 1u << from)) {
		return;
	}

	for (in = 0; in < DSPAUDIO_IN; in++) {
		m[to][in] += k * m[from][in];
		m[to2][in] += to2 == to ? 0 : k * m[from][in];
		m[from][in] = 0;
	}
}

/* each input on its own output, the inputs are in the outputs' order */
static void discrete(double (*m)[DSPAUDIO_IN])
{
	unsigned int out;

	for (out = 0; out < DSPAUDIO_OUT; out++) {
		m[out][out] = 1;
	}

	/* a 5.1 source on surround back speakers: they repeat the surrounds */
	if (back_fill && !(absent & 1u << DSPAUDIO_OUT_SBL) && !(absent & 1u << DSPAUDIO_OUT_SBR)) {
		m[DSPAUDIO_OUT_SBL][DSPAUDIO_IN_SL] = M_SQRT1_2;
		m[DSPAUDIO_OUT_SBR][DSPAUDIO_IN_SR] = M_SQRT1_2;
	}

	/* the LFE channel is recorded 10 dB down */
	m[DSPAUDIO_OUT_SW][DSPAUDIO_IN_LFE] = lfe_level <= -100 ? 0 : LFE_GAIN * pow(10.0, lfe_level / 20.0);

	fold(m, DSPAUDIO_OUT_SBL, DSPAUDIO_OUT_SL, DSPAUDIO_OUT_SL, 1);
	fold(m, DSPAUDIO_OUT_SBR, DSPAUDIO_OUT_SR, DSPAUDIO_OUT_SR, 1);
	fold(m, DSPAUDIO_OUT_SL, DSPAUDIO_OUT_FL, DSPAUDIO_OUT_FL, 1);
	fold(m, DSPAUDIO_OUT_SR, DSPAUDIO_OUT_FR, DSPAUDIO_OUT_FR, 1);
	fold(m, DSPAUDIO_OUT_C, DSPAUDIO_OUT_FL, DSPAUDIO_OUT_FR, M_SQRT1_2);
	fold(m, DSPAUDIO_OUT_SW, DSPAUDIO_OUT_FL, DSPAUDIO_OUT_FR, M_SQRT1_2);
}

static void apply(void)
{
	/* the test noise plays alone */
	int32_t g = noise_out < 0 ? gain_of(volume) : 0;
	double m[DSPAUDIO_OUT][DSPAUDIO_IN] = { { 0 } }, v;
	unsigned int out, in, s;

	int multi = input == INPUT_MULTI;

	if (outputs == OUTPUTS_MULTI && multi) {
		discrete(m);
	} else {
		for (out = 0; out < DSPAUDIO_OUT; out++) {
			for (s = 0; s < 2; s++) {
				for (in = 0; in < DSPAUDIO_IN; in++) {
					v = multi ? downmix[s][in] : in == s;
					m[out][in] += mix[outputs][out][s] / 2.0 * v;
				}
			}
		}
	}

	sh->inputs = multi ? 0xff : 0x03;
	sh->decim = sample_hz > SAMPLE_HZ ? 2 : 1;
	sh->ring_on = input == INPUT_NETWORK;
	sh->late_night = late_night;
	for (out = 0; out < DSPAUDIO_OUT; out++) {
		for (in = 0; in < DSPAUDIO_IN; in++) {
			/* the decoder makes the outputs; [0] only scales them */
			if (absent & 1u << out) {
				v = 0;
			} else if (surround) {
				v = in == 0 ? g : 0;
			} else {
				v = g * m[out][in];
			}

			sh->gain[out][in] = v >= INT32_MAX ? INT32_MAX : v <= -INT32_MAX ? -INT32_MAX : (int32_t)v;
		}
	}
}

/*
 * A DMA image's McASP and EDMA run on without the DSP: stop them, or they
 * write into L2 while the next image is copied there.
 */
static void dsp_stop(void)
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

/* liba52's layout as front/rear.lfe: 3/2.1 */
static const char *ac3_layout(uint32_t mode)
{
	static const char *const name[8] = {
		"1+1", "1/0", "2/0", "3/0", "2/1", "3/1", "2/2", "3/2",
	};
	static char buf[8];

	snprintf(buf, sizeof(buf), "%s.%u", name[mode & 7], (mode >> 4) & 1);
	return buf;
}

/* IEC 61937 data types, as Pc names them */
static const char *bitstream_name(uint32_t type)
{
	switch (type) {
	case 0:
		return "none";
	case 1:
		return "ac3";
	case 11: case 12: case 13: case 17:
		return "dts";
	case 21:
		return "eac3";
	default:
		return "other";
	}
}

/* dBFS of a peak, -999 for silence */
static int peak_db(uint32_t peak)
{
	return peak ? (int)lround(20.0 * log10(peak / 2147483648.0)) : -999;
}

/*
 * dsp.bitstream when what the DSP detects changes: the type ("none", "ac3",
 * ...) and, while it decodes AC-3, the layout ("3/2.1", acmod and lfe).
 */
static void bitstream_tick(struct uloop_timeout *t)
{
	static uint32_t last;
	uint32_t now = sh->bitstream;
	struct blob_buf eb = { 0 };

	if (now != last) {
		last = now;
		blob_buf_init(&eb, 0);
		blobmsg_add_string(&eb, "bitstream", bitstream_name(now & 0x1f));
		if (now >> 8) {
			blobmsg_add_string(&eb, "decoded", ac3_layout(now >> 8));
			blobmsg_add_u32(&eb, "acmod", (now >> 8) & 7);
			blobmsg_add_u8(&eb, "lfe", (now >> 12) & 1);
		}
		ubus_send_event(ctx, "dsp.bitstream", eb.head);
		blob_buf_free(&eb);
	}
	uloop_timeout_set(t, BITSTREAM_MS);
}

/* dBFS of a mean square, -999 for silence */
static int rms_db(float sq, uint32_t n)
{
	return n && sq > 0 ? (int)lround(10.0 * log10(sq / n)) : -999;
}

/* the front pair as played, as dsp.meter events: RMS and peak, dBFS */
static void meter_tick(struct uloop_timeout *t)
{
	struct blob_buf mb = { 0 };
	uint32_t n = sh->meter_n;

	blob_buf_init(&mb, 0);
	blobmsg_add_u32(&mb, "left", (uint32_t)rms_db(sh->meter_sq[0], n));
	blobmsg_add_u32(&mb, "right", (uint32_t)rms_db(sh->meter_sq[1], n));
	blobmsg_add_u32(&mb, "left_peak", (uint32_t)peak_db(sh->meter[0]));
	blobmsg_add_u32(&mb, "right_peak", (uint32_t)peak_db(sh->meter[1]));
	sh->meter[0] = sh->meter[1] = 0;
	sh->meter_sq[0] = sh->meter_sq[1] = 0;
	sh->meter_n = 0;
	ubus_send_event(ctx, "dsp.meter", mb.head);
	blob_buf_free(&mb);
	uloop_timeout_set(t, METER_MS);
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
	blobmsg_add_u64(&b, "samples", sh->samples);	/* a u32 would print as signed */
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
	blobmsg_add_string(&b, "reverb", reverb ? reverb->name : "off");
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
	a = blobmsg_open_table(&b, "eq");
	for (i = 0; i < DSPAUDIO_OUT; i++) {
		void *e;
		int k, any = 0;

		for (k = 0; k < EQ_BANDS; k++) {
			any |= eq[i][k];
		}

		if (!any) {
			continue;
		}

		e = blobmsg_open_array(&b, out_name[i]);
		for (k = 0; k < EQ_BANDS; k++) {
			blobmsg_add_u32(&b, NULL, (uint32_t)eq[i][k]);
		}

		blobmsg_close_array(&b, e);
	}

	blobmsg_close_table(&b, a);
	a = blobmsg_open_table(&b, "crossover");
	for (i = 0; i < DSPAUDIO_OUT; i++) {
		if (crossover[i]) {
			blobmsg_add_u32(&b, out_name[i], (uint32_t)crossover[i]);
		}
	}

	blobmsg_close_table(&b, a);
	blobmsg_add_u8(&b, "subwoofer", subwoofer);
	blobmsg_add_u8(&b, "double_bass", double_bass);
	blobmsg_add_string(&b, "input", input_name[input]);
	blobmsg_add_string(&b, "bitstream", bitstream_name(sh->bitstream & 0x1f));
	if (sh->bitstream >> 8) {
		blobmsg_add_string(&b, "decoded", ac3_layout(sh->bitstream >> 8));
	}
	blobmsg_add_u32(&b, "lfe_level", (uint32_t)lfe_level);
	blobmsg_add_u32(&b, "av_sync", (uint32_t)av_sync);
	blobmsg_add_u32(&b, "late_night", (uint32_t)late_night);
	blobmsg_add_u32(&b, "loudness", (uint32_t)loudness);
	blobmsg_add_u32(&b, "fs", (uint32_t)sample_hz);
	blobmsg_add_u8(&b, "back_fill", back_fill);
	blobmsg_add_u8(&b, "meter", meter);
	blobmsg_add_u32(&b, "dynamic_volume", (uint32_t)dynamic_volume);
	blobmsg_add_u32(&b, "dynamic_volume_db", (uint32_t)lround(sh->dvol_gain));
	a = blobmsg_open_array(&b, "absent");
	for (i = 0; i < DSPAUDIO_OUT; i++) {
		if (absent & (1u << i)) {
			blobmsg_add_string(&b, NULL, out_name[i]);
		}
	}

	blobmsg_close_array(&b, a);
	blobmsg_add_u32(&b, "sub_lowpass", (uint32_t)sub_lowpass);
	ubus_send_reply(c, req, b.head);
	return 0;
}

enum { SET_VOLUME, SET_MUTE, SET_OUTPUTS, SET_BASS, SET_TREBLE, SET_TESTTONE,
	SET_TESTTONE_LEVEL, SET_DISTANCES, SET_SURROUND, SET_CROSSOVER, SET_SUBWOOFER,
	SET_SUB_LOWPASS, SET_REVERB, SET_DOUBLE_BASS, SET_ABSENT, SET_INPUT,
	SET_LFE_LEVEL, SET_EQ, SET_AV_SYNC, SET_LOUDNESS,
	SET_DYNAMIC_VOLUME, SET_FS, SET_BACK_FILL, SET_METER, SET_LATE_NIGHT, __SET_MAX };

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
	[SET_CROSSOVER] = { .name = "crossover", .type = BLOBMSG_TYPE_TABLE },
	[SET_SUBWOOFER] = { .name = "subwoofer", .type = BLOBMSG_TYPE_BOOL },
	[SET_SUB_LOWPASS] = { .name = "sub_lowpass", .type = BLOBMSG_TYPE_INT32 },
	[SET_REVERB] = { .name = "reverb", .type = BLOBMSG_TYPE_STRING },
	[SET_DOUBLE_BASS] = { .name = "double_bass", .type = BLOBMSG_TYPE_BOOL },
	[SET_ABSENT] = { .name = "absent", .type = BLOBMSG_TYPE_ARRAY },
	[SET_INPUT] = { .name = "input", .type = BLOBMSG_TYPE_STRING },
	[SET_LFE_LEVEL] = { .name = "lfe_level", .type = BLOBMSG_TYPE_INT32 },
	[SET_EQ] = { .name = "eq", .type = BLOBMSG_TYPE_TABLE },
	[SET_AV_SYNC] = { .name = "av_sync", .type = BLOBMSG_TYPE_INT32 },
	[SET_LOUDNESS] = { .name = "loudness", .type = BLOBMSG_TYPE_INT32 },
	[SET_DYNAMIC_VOLUME] = { .name = "dynamic_volume", .type = BLOBMSG_TYPE_INT32 },
	[SET_FS] = { .name = "fs", .type = BLOBMSG_TYPE_INT32 },
	[SET_BACK_FILL] = { .name = "back_fill", .type = BLOBMSG_TYPE_BOOL },
	[SET_METER] = { .name = "meter", .type = BLOBMSG_TYPE_BOOL },
	[SET_LATE_NIGHT] = { .name = "late_night", .type = BLOBMSG_TYPE_INT32 },
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

/* { "fl": 300, ... } in cm over the current ones, checked, not stored yet */
static int parse_distances(struct blob_attr *t, int *d)
{
	struct blob_attr *cur;
	unsigned int rem;
	int i, v;

	memcpy(d, distance, sizeof(distance));
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

	return 0;
}

/* { "fl": 80, ... } in Hz, 0 = large, over the current ones, not stored yet */
static int parse_crossover(struct blob_attr *t, int *x)
{
	struct blob_attr *cur;
	unsigned int rem;
	int i, v;

	memcpy(x, crossover, sizeof(crossover));
	blobmsg_for_each_attr(cur, t, rem) {
		i = out_index(blobmsg_name(cur));
		if (i < 0 || i == DSPAUDIO_OUT_SW || blobmsg_type(cur) != BLOBMSG_TYPE_INT32) {
			return -1;
		}

		v = (int32_t)blobmsg_get_u32(cur);
		if (v && (v < CROSSOVER_MIN || v > CROSSOVER_MAX)) {
			return -1;
		}

		x[i] = v;
	}

	return 0;
}

/* {"fl": [dB, ...], ...}: the bands from 25 Hz up, missing ones flat */
static int parse_eq(struct blob_attr *t, int (*e)[EQ_BANDS])
{
	struct blob_attr *cur, *v;
	unsigned int rem, rem2, k;
	int i, db;

	memcpy(e, eq, sizeof(eq));
	blobmsg_for_each_attr(cur, t, rem) {
		i = out_index(blobmsg_name(cur));
		if (i < 0 || blobmsg_type(cur) != BLOBMSG_TYPE_ARRAY) {
			return -1;
		}

		k = 0;
		memset(e[i], 0, sizeof(e[i]));
		blobmsg_for_each_attr(v, cur, rem2) {
			if (k == EQ_BANDS || blobmsg_type(v) != BLOBMSG_TYPE_INT32) {
				return -1;
			}

			db = (int32_t)blobmsg_get_u32(v);
			if (db < -EQ_DB_MAX || db > EQ_DB_MAX) {
				return -1;
			}

			e[i][k++] = db;
		}
	}

	return 0;
}

static int name_index(const char *name, const char *const *names, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		if (names[i] && !strcmp(name, names[i])) {
			return i;
		}
	}

	return -1;
}

/* everything is checked before anything changes: a bad call changes nothing */
static int m_set(struct ubus_context *c, struct ubus_object *obj,
		 struct ubus_request_data *req, const char *method,
		 struct blob_attr *msg)
{
	struct blob_attr *tb[__SET_MAX], *cur;
	const struct surround_preset *new_surround = surround;
	const struct reverb_preset *new_reverb = reverb;
	int new_outputs = outputs, new_input = input, nb = bass, nt = treble, level = noise_level;
	uint32_t new_hz = 0;
	int nout = noise_out, lowpass = sub_lowpass, i;
	int dist[DSPAUDIO_OUT], xover[DSPAUDIO_OUT], new_eq[DSPAUDIO_OUT][EQ_BANDS];
	uint32_t new_absent = absent;
	unsigned int rem;
	const char *o;

	blobmsg_parse(set_policy, __SET_MAX, tb, blob_data(msg), blob_len(msg));
	if (tb[SET_OUTPUTS]) {
		new_outputs = name_index(blobmsg_get_string(tb[SET_OUTPUTS]), outputs_name,
					 __OUTPUTS_MAX);
		if (new_outputs < 0) {
			return UBUS_STATUS_INVALID_ARGUMENT;
		}
	}

	if (tb[SET_INPUT]) {
		new_input = name_index(blobmsg_get_string(tb[SET_INPUT]), input_name, __INPUT_MAX);
		if (new_input < 0) {
			return UBUS_STATUS_INVALID_ARGUMENT;
		}
	}

	/* above 96 kHz even every other frame is too much for the DSP */
	if (tb[SET_FS]) {
		new_hz = blobmsg_get_u32(tb[SET_FS]);
		if (new_hz < 32000 || new_hz > 96000) {
			return UBUS_STATUS_INVALID_ARGUMENT;
		}
	}

	if (tb[SET_BASS]) {
		nb = (int32_t)blobmsg_get_u32(tb[SET_BASS]);
	}

	if (tb[SET_TREBLE]) {
		nt = (int32_t)blobmsg_get_u32(tb[SET_TREBLE]);
	}

	if (nb < -TONE_MAX || nb > TONE_MAX || nt < -TONE_MAX || nt > TONE_MAX) {
		return UBUS_STATUS_INVALID_ARGUMENT;
	}

	if (tb[SET_TESTTONE_LEVEL]) {
		level = (int32_t)blobmsg_get_u32(tb[SET_TESTTONE_LEVEL]);
		if (level < NOISE_MIN || level > NOISE_MAX) {
			return UBUS_STATUS_INVALID_ARGUMENT;
		}
	}

	if (tb[SET_TESTTONE]) {
		o = blobmsg_get_string(tb[SET_TESTTONE]);
		nout = name_index(o, out_name, DSPAUDIO_OUT);
		if (nout < 0 && strcmp(o, "off")) {
			return UBUS_STATUS_INVALID_ARGUMENT;
		}
	}

	memcpy(dist, distance, sizeof(dist));
	if (tb[SET_DISTANCES] && parse_distances(tb[SET_DISTANCES], dist)) {
		return UBUS_STATUS_INVALID_ARGUMENT;
	}

	memcpy(new_eq, eq, sizeof(eq));
	if (tb[SET_EQ] && parse_eq(tb[SET_EQ], new_eq)) {
		return UBUS_STATUS_INVALID_ARGUMENT;
	}

	memcpy(xover, crossover, sizeof(xover));
	if (tb[SET_CROSSOVER] && parse_crossover(tb[SET_CROSSOVER], xover)) {
		return UBUS_STATUS_INVALID_ARGUMENT;
	}

	if (tb[SET_ABSENT]) {
		new_absent = 0;
		blobmsg_for_each_attr(cur, tb[SET_ABSENT], rem) {
			i = blobmsg_type(cur) == BLOBMSG_TYPE_STRING ? out_index(blobmsg_get_string(cur)) : -1;
			if (i < 0) {
				return UBUS_STATUS_INVALID_ARGUMENT;
			}

			new_absent |= 1u << i;
		}
	}

	if (tb[SET_SUB_LOWPASS]) {
		lowpass = (int32_t)blobmsg_get_u32(tb[SET_SUB_LOWPASS]);
		if (lowpass && (lowpass < CROSSOVER_MIN || lowpass > CROSSOVER_MAX)) {
			return UBUS_STATUS_INVALID_ARGUMENT;
		}
	}

	if (tb[SET_REVERB]) {
		o = blobmsg_get_string(tb[SET_REVERB]);
		new_reverb = NULL;
		for (i = 0; i < (int)ARRAY_SIZE(reverb_presets); i++) {
			if (!strcmp(o, reverb_presets[i].name)) {
				new_reverb = &reverb_presets[i];
			}
		}

		if (!new_reverb && strcmp(o, "off")) {
			return UBUS_STATUS_INVALID_ARGUMENT;
		}
	}

	if (tb[SET_SURROUND]) {
		o = blobmsg_get_string(tb[SET_SURROUND]);
		new_surround = NULL;
		for (i = 0; i < (int)ARRAY_SIZE(surround_presets); i++) {
			if (!strcmp(o, surround_presets[i].name)) {
				new_surround = &surround_presets[i];
			}
		}

		if (!new_surround && strcmp(o, "off")) {
			return UBUS_STATUS_INVALID_ARGUMENT;
		}
	}

	/* all good: store and apply */
	outputs = new_outputs;
	if (tb[SET_VOLUME]) {
		volume = (int32_t)blobmsg_get_u32(tb[SET_VOLUME]);
	}

	if (tb[SET_MUTE]) {
		muted = blobmsg_get_bool(tb[SET_MUTE]);
	}

	input = new_input;

	/* a new sample rate: every filter, delay and time constant again */
	if (tb[SET_FS] && new_hz != sample_hz) {
		sample_hz = new_hz;
		proc_hz = new_hz > SAMPLE_HZ ? new_hz / 2.0 : new_hz;
		apply_tone();
		apply_delays();
		apply_av_sync();
		apply_dvol();
		apply_surround();
		apply_reverb();
		apply_bass();
	}

	if (tb[SET_METER]) {
		meter = blobmsg_get_bool(tb[SET_METER]);
		if (meter) {
			/* not what built up while it was off */
			sh->meter[0] = sh->meter[1] = 0;
			sh->meter_sq[0] = sh->meter_sq[1] = 0;
			sh->meter_n = 0;
			uloop_timeout_set(&meter_timer, METER_MS);
		} else {
			uloop_timeout_cancel(&meter_timer);
		}
	}

	if (tb[SET_BACK_FILL]) {
		back_fill = blobmsg_get_bool(tb[SET_BACK_FILL]);
	}

	/* only on a change: it starts the leveller over */
	if (tb[SET_DYNAMIC_VOLUME] &&
	    (int32_t)blobmsg_get_u32(tb[SET_DYNAMIC_VOLUME]) != dynamic_volume) {
		dynamic_volume = (int32_t)blobmsg_get_u32(tb[SET_DYNAMIC_VOLUME]);
		apply_dvol();
	}

	if (tb[SET_AV_SYNC]) {
		av_sync = (int32_t)blobmsg_get_u32(tb[SET_AV_SYNC]);
		av_sync = av_sync < 0 ? 0 : av_sync;
		apply_av_sync();
	}

	if (tb[SET_LATE_NIGHT]) {
		late_night = (int32_t)blobmsg_get_u32(tb[SET_LATE_NIGHT]);
		late_night = late_night < 0 ? 0 : late_night > 2 ? 2 : late_night;
		sh->late_night = late_night;
	}

	if (tb[SET_LFE_LEVEL]) {
		lfe_level = (int32_t)blobmsg_get_u32(tb[SET_LFE_LEVEL]);
		lfe_level = lfe_level > 0 ? 0 : lfe_level;
	}

	if (tb[SET_BASS] || tb[SET_TREBLE]) {
		bass = nb;
		treble = nt;
		apply_tone();
	}

	if (tb[SET_TESTTONE] || tb[SET_TESTTONE_LEVEL]) {
		noise_level = level;
		noise_out = nout;
		apply_noise();
	}

	if (tb[SET_DISTANCES]) {
		memcpy(distance, dist, sizeof(distance));
		apply_delays();
	}

	if (tb[SET_CROSSOVER] || tb[SET_SUBWOOFER] || tb[SET_SUB_LOWPASS] ||
	    tb[SET_DOUBLE_BASS] || tb[SET_ABSENT] || tb[SET_EQ] || tb[SET_LOUDNESS]) {
		/* the decoder only cares whether there is a centre speaker */
		int centre = (absent ^ new_absent) & 1u << DSPAUDIO_OUT_C;

		memcpy(crossover, xover, sizeof(crossover));
		memcpy(eq, new_eq, sizeof(eq));
		if (tb[SET_LOUDNESS]) {
			loudness = (int32_t)blobmsg_get_u32(tb[SET_LOUDNESS]);
			loudness = loudness < 0 ? -1 : loudness > LOUDNESS_MAX ? LOUDNESS_MAX : loudness;
		}

		absent = new_absent;
		sub_lowpass = lowpass;
		if (tb[SET_SUBWOOFER]) {
			subwoofer = blobmsg_get_bool(tb[SET_SUBWOOFER]);
		}

		if (tb[SET_DOUBLE_BASS]) {
			double_bass = blobmsg_get_bool(tb[SET_DOUBLE_BASS]);
		}

		apply_bass();
		if (centre && !tb[SET_SURROUND]) {
			apply_surround();
		}
	}

	if (tb[SET_REVERB]) {
		reverb = new_reverb;
		apply_reverb();
	}

	if (tb[SET_SURROUND]) {
		surround = new_surround;
		/* turning it off: the matrix gains first, or L plays everywhere */
		if (!surround) {
			apply();
		}

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

/* uloop_run() only ends on SIGINT; stopping must mute the outputs too */
static void on_term(int sig)
{
	uloop_end();
}

int main(int argc, char **argv)
{
	struct sigaction sa = { .sa_handler = on_term };
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
	psc1 = map(fd, PSC1_BASE, 0x1000);
	mcasp1 = map(fd, MCASP1_BASE, 0x1000);
	edma = map(fd, EDMA_BASE, 0x2000);
	l2 = (volatile uint8_t *)map(fd, L2_BASE, L2_SIZE);
	ddr = (volatile uint8_t *)map(fd, DSPAUDIO_DDR, DSPAUDIO_DDR_LEN);
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
	sigaction(SIGTERM, &sa, NULL);
	rc = ubus_add_object(ctx, &dsp_object);
	if (rc) {
		log_msg(LOG_ERR, "ubus: %s", ubus_strerror(rc));
		return 1;
	}

	/* a new DSP knows nothing: the policy sets it up again */
	blob_buf_init(&b, 0);
	ubus_send_event(ctx, "dsp.ready", b.head);
	uloop_timeout_set(&bitstream_timer, BITSTREAM_MS);

	uloop_run();
	mute_lines(1);
	dsp_stop();
	ubus_free(ctx);
	uloop_done();
	return 0;
}
