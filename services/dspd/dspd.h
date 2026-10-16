/* SPDX-License-Identifier: BSD-2-Clause */
/* What the parts of dspd share: the settings, the shared block, the ubus */
#ifndef DSPD_H
#define DSPD_H

#include <stdint.h>

#include <libubox/blobmsg.h>
#include <libubus.h>

#include "dspaudio.h"

#define VOLUME_MUTE	(-10000)	/* -100 dB, what the MCU sends at the bottom */
#define TONE_MAX	12		/* dB either way; the MCU sends -10..+10 */
#define SAMPLE_HZ	48000.0		/* until told otherwise */
#define METER_MS	100		/* dsp.meter events, 10 a second */
#define BITSTREAM_MS	250		/* how often dsp.bitstream looks for a change */
#define NOISE_MAX	(-15)		/* dBFS RMS; peaks are 12.8 dB higher */
#define NOISE_MIN	(-80)
#define CROSSOVER_MIN	20		/* Hz */
#define CROSSOVER_MAX	250
#define LOUDNESS_MAX	12
#define EQ_BANDS	15
#define EQ_DB_MAX	12

enum outputs { OUTPUTS_FRONT, OUTPUTS_ALL, OUTPUTS_MONO, OUTPUTS_MULTI, __OUTPUTS_MAX };

/*
 * What the source is: the analog inputs, a digital one, multichannel PCM
 * from HDMI on four lines, or audio from Linux through the ring (the NET
 * input).
 */
enum input { INPUT_ANALOG, INPUT_DIGITAL, INPUT_MULTI, INPUT_NETWORK, __INPUT_MAX };

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

extern const struct surround_preset surround_presets[];
extern const unsigned int n_surround_presets;
extern const struct reverb_preset reverb_presets[];
extern const unsigned int n_reverb_presets;

extern struct ubus_context *ctx;
extern volatile struct dspaudio_shared *sh;

/* the settings, as the ubus last set them */
extern int volume;			/* 0.01 dB */
extern int muted;
extern enum outputs outputs;
extern enum input input;
extern int bass, treble;		/* dB */
extern int noise_out;			/* enum dspaudio_out, -1 off */
extern int noise_level;			/* dBFS RMS */
extern int distance[DSPAUDIO_OUT];	/* cm, 0 = not known */
extern const struct surround_preset *surround;	/* NULL: off */
extern const struct reverb_preset *reverb;	/* NULL: off */
extern int crossover[DSPAUDIO_OUT];	/* Hz, 0 = large: full range */
extern int subwoofer, double_bass;
extern int lfe_level;			/* dB, the receiver's LFE level; -100 off */
extern int av_sync;			/* ms */
extern double sample_hz;		/* the source's */
extern double proc_hz;			/* the DSP's: above 48 kHz it processes every other frame, at most */
extern int loudness;			/* dB of bass boost, -1 off */
extern int dynamic_volume;		/* 0 off, 1 light, 2 medium, 3 heavy */
extern int back_fill;			/* multichannel without surround back: fill it */
extern int meter;			/* send dsp.meter events */
extern int late_night;			/* 0 off, 1 low, 2 high */
extern int eq[DSPAUDIO_OUT][EQ_BANDS];	/* dB */
extern int sub_lowpass;			/* Hz, 0 = none */
extern uint32_t absent;			/* outputs without a speaker */

/* dsp.c */
int dsp_open(void);
int dsp_start(const char *image);
void dsp_stop(void);
void mute_lines(int on);

/* filters.c */
void shelf(volatile struct dspaudio_biquad *q, double hz, int db, int high);
void pass(volatile struct dspaudio_biquad *q, double hz, int high);
void peak(volatile struct dspaudio_biquad *q, double hz, int db);

/* apply.c */
void apply(void);
void apply_tone(void);
void apply_noise(void);
void apply_delays(void);
void apply_av_sync(void);
void apply_dvol(void);
void apply_surround(void);
void apply_reverb(void);
void apply_bass(void);

/* ubus.c */
extern struct ubus_object dsp_object;
void bitstream_watch(void);

#endif
