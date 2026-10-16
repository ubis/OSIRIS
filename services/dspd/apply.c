// SPDX-License-Identifier: BSD-2-Clause
/* The current settings, turned into gains, filters and delays in the shared block */
#include <math.h>
#include <stdlib.h>

#include "dspd.h"

#define BASS_HZ		100.0
#define TREBLE_HZ	10000.0
#define NOISE_RMS	0.58194		/* the pink filter's output for an RMS of 1 */
#define SOUND_CM_S	34300.0
#define LFE_GAIN	3.16227766	/* +10 dB */
#define DOUBLE_BASS_HZ	80

int volume;			/* 0.01 dB */
int muted;
enum outputs outputs = OUTPUTS_FRONT;
enum input input = INPUT_ANALOG;
int bass, treble;		/* dB */
int noise_out = -1;		/* enum dspaudio_out, -1 off */
int noise_level = -30;		/* dBFS RMS */
int distance[DSPAUDIO_OUT];	/* cm, 0 = not known */
const struct surround_preset *surround;	/* NULL: off */
const struct reverb_preset *reverb;	/* NULL: off */
int crossover[DSPAUDIO_OUT];	/* Hz, 0 = large: full range */
int subwoofer, double_bass;
int lfe_level;			/* dB, the receiver's LFE level; -100 off */
int av_sync;			/* ms */
double sample_hz = SAMPLE_HZ;	/* the source's */
double proc_hz = SAMPLE_HZ;	/* the DSP's: above 48 kHz it processes every other frame, at most */
int loudness = -1;		/* dB of bass boost, -1 off */
int dynamic_volume;		/* 0 off, 1 light, 2 medium, 3 heavy */
int back_fill;			/* multichannel without surround back: fill it */
int meter;			/* send dsp.meter events */
int late_night;			/* 0 off, 1 low, 2 high */
int eq[DSPAUDIO_OUT][EQ_BANDS];	/* dB */
int sub_lowpass = 120;		/* Hz, 0 = none */
uint32_t absent;			/* outputs without a speaker */

const struct surround_preset surround_presets[] = {
	{ "movie", 10, 1.0, 1.0, 1.0, 0.0, 0.7, 0.5, 10, 5, 100, 7000 },
	/* steady surrounds: steering them makes music pulse */
	{ "music", 300, 0.25, 0.5, 0.8, 0.25, 0.5, 0.5, 2, 5, 100, 16000 },
	{ "game", 10, 1.0, 0.8, 1.0, 0.15, 0.8, 0.5, 0, 3, 80, 18000 },
};

const unsigned int n_surround_presets = ARRAY_SIZE(surround_presets);

const struct reverb_preset reverb_presets[] = {
	{ "hall", 2.0, 2.0, 0.35, 30, 0.12, 0.35, 0.30 },
	{ "theater", 1.5, 1.3, 0.40, 20, 0.08, 0.30, 0.30 },
	{ "club", 1.0, 0.9, 0.30, 12, 0.10, 0.30, 0.25 },
	{ "studio", 0.8, 0.6, 0.25, 8, 0.08, 0.25, 0.20 },
	{ "tv", 0.6, 0.4, 0.50, 5, 0.05, 0.15, 0.10 },
};

const unsigned int n_reverb_presets = ARRAY_SIZE(reverb_presets);

/* mutually prime, 25 to 41 ms */
static const int reverb_lines[DSPAUDIO_REVERB_LINES] = { 1201, 1423, 1693, 1951 };

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

/* the receiver's equalizer bands; the subwoofer has the first five */
static const double eq_hz[EQ_BANDS] = {
	25, 40, 63, 100, 160, 250, 400, 630, 1000, 1600, 2500, 4000, 6300, 10000, 16000,
};

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

/*
 * The bands that are not flat, the largest when there are too many, in
 * the order of their frequencies: a band changing its level keeps the
 * others in their filters.
 */
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
	}

	for (n = 0, k = 0; k < EQ_BANDS; k++) {
		if (used[k]) {
			peak(&bm->eq[out][n++], eq_hz[k], eq[out][k]);
		}
	}

	return n;
}

/* Linkwitz-Riley: two Butterworth sections each side, the sides add up flat */
void apply_bass(void)
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

void apply_reverb(void)
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

void apply_dvol(void)
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
void apply_surround(void)
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

void apply_noise(void)
{
	sh->noise_gain = pow(10.0, noise_level / 20.0) * NOISE_RMS * 2147483647.0;
	__sync_synchronize();
	sh->noise_out = noise_out < 0 ? DSPAUDIO_NOISE_OFF : (uint32_t)noise_out;
}

/* fill the set the DSP is not using, then switch it over */
void apply_tone(void)
{
	uint32_t next = (sh->tone_sel + 1) & 1;

	shelf(&sh->tone[next][0], BASS_HZ, bass, 0);
	shelf(&sh->tone[next][1], TREBLE_HZ, treble, 1);
	__sync_synchronize();
	sh->tone_sel = next;
}

/* every output held back by as much, the delay line takes 2.7 s at 48 kHz */
void apply_av_sync(void)
{
	long n = av_sync > 0 ? lround(av_sync * proc_hz / 1000.0) : 0;

	sh->av_delay = n < DSPAUDIO_AV_LEN ? n : DSPAUDIO_AV_LEN - 1;
}

/* hold the nearer speakers back until the farthest one's sound arrives */
void apply_delays(void)
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

void apply(void)
{
	/*
	 * The test noise plays alone. The largest boost, of the bass with
	 * Dynamic EQ's or of the treble, comes off every channel, so that it
	 * has room below full scale and the limiter stays out of it; the analog
	 * volume after the DSP makes up the level.
	 */
	int low = bass + (loudness > 0 ? loudness : 0);
	int boost = low > treble ? low : treble;
	int32_t g = noise_out < 0 ? gain_of(volume - 100 * (boost > 0 ? boost : 0)) : 0;
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
	/* above 96 kHz even every other frame is too much for the DSP */
	sh->decim = sample_hz > 2 * SAMPLE_HZ ? 0 : sample_hz > SAMPLE_HZ ? 2 : 1;
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
