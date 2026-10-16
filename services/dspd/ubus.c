// SPDX-License-Identifier: BSD-2-Clause
/* The dsp object on the ubus: status, set, and the meter and bitstream events */
#include <math.h>
#include <string.h>

#include <libubox/uloop.h>

#include "dspd.h"

static const char *const outputs_name[__OUTPUTS_MAX] = {
	[OUTPUTS_FRONT] = "front",
	[OUTPUTS_ALL] = "all",
	[OUTPUTS_MONO] = "mono",
	[OUTPUTS_MULTI] = "multichannel",
};

static const char *const input_name[__INPUT_MAX] = {
	[INPUT_ANALOG] = "analog",
	[INPUT_DIGITAL] = "digital",
	[INPUT_MULTI] = "multichannel",
	[INPUT_NETWORK] = "network",
};

static struct blob_buf b;

static void meter_tick(struct uloop_timeout *t);
static struct uloop_timeout meter_timer = { .cb = meter_tick };
static void bitstream_tick(struct uloop_timeout *t);
static struct uloop_timeout bitstream_timer = { .cb = bitstream_tick };

static const char *const out_name[DSPAUDIO_OUT] = {
	[DSPAUDIO_OUT_FL] = "fl", [DSPAUDIO_OUT_FR] = "fr",
	[DSPAUDIO_OUT_C] = "c", [DSPAUDIO_OUT_SW] = "sw",
	[DSPAUDIO_OUT_SL] = "sl", [DSPAUDIO_OUT_SR] = "sr",
	[DSPAUDIO_OUT_SBL] = "sbl", [DSPAUDIO_OUT_SBR] = "sbr",
};

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
		for (i = 0; i < (int)n_reverb_presets; i++) {
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
		for (i = 0; i < (int)n_surround_presets; i++) {
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

struct ubus_object dsp_object = {
	.name = "dsp",
	.type = &dsp_type,
	.methods = dsp_methods,
	.n_methods = ARRAY_SIZE(dsp_methods),
};

/* dsp.bitstream from now on */
void bitstream_watch(void)
{
	uloop_timeout_set(&bitstream_timer, BITSTREAM_MS);
}

