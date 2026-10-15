// SPDX-License-Identifier: BSD-2-Clause
/*
 * mcud: the link to the receiver's main MCU on the ubus. It answers what the
 * MCU expects at once and turns everything else into events; deciding what
 * to do about them is up to the services listening (ubus listen mcu.*).
 *
 *   mcud [-s spi_hz] [-v]
 *     -v  log every frame with a timestamp on stderr
 */
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>

#include <libubox/blobmsg.h>
#include <libubox/uloop.h>
#include <libubus.h>

#include "log.h"
#include "cmd.h"
#include "display.h"
#include "link.h"
#include "proto.h"

static struct ubus_context *ctx;
static struct blob_buf b;
static int listening_mode = -1;		/* the last one the MCU sent */
static int lmd = -1;			/* its ISCP code (LMDxx) */

/* ISCP listening mode codes, as the MCU reports them (!1LMDxx) */
static const struct {
	uint8_t code;
	const char *name;
} lmd_names[] = {
	{ 0x00, "stereo" }, { 0x01, "direct" }, { 0x02, "surround" },
	{ 0x03, "film" }, { 0x04, "thx" }, { 0x05, "action" }, { 0x06, "musical" },
	{ 0x07, "mono_movie" }, { 0x08, "orchestra" }, { 0x09, "unplugged" },
	{ 0x0a, "studio_mix" }, { 0x0b, "tv_logic" }, { 0x0c, "all_ch_stereo" },
	{ 0x0d, "theater_dimensional" }, { 0x0e, "enhanced" }, { 0x0f, "mono" },
	{ 0x11, "pure_audio" }, { 0x12, "multiplex" }, { 0x13, "full_mono" },
	{ 0x14, "dolby_virtual" }, { 0x15, "dts_surround_sensation" },
	{ 0x16, "audyssey_dsx" }, { 0x40, "straight_decode" }, { 0x41, "dolby_ex" },
	{ 0x42, "thx_cinema" }, { 0x43, "thx_surround_ex" }, { 0x44, "thx_music" },
	{ 0x45, "thx_games" }, { 0x50, "thx_u2_cinema" }, { 0x51, "thx_u2_music" },
	{ 0x52, "thx_u2_games" }, { 0x80, "plii_movie" }, { 0x81, "plii_music" },
	{ 0x82, "neo6_cinema" }, { 0x83, "neo6_music" }, { 0x84, "plii_thx_cinema" },
	{ 0x85, "neo6_thx_cinema" }, { 0x86, "plii_game" }, { 0x87, "neural_surround" },
	{ 0x88, "neural_thx" }, { 0x89, "plii_thx_games" }, { 0x8a, "neo6_thx_games" },
	{ 0x8b, "plii_thx_music" }, { 0x8c, "neo6_thx_music" },
	{ 0x90, "pliiz_height" }, { 0x94, "pliiz_height_thx_cinema" },
	{ 0x95, "pliiz_height_thx_music" }, { 0x96, "pliiz_height_thx_games" },
	{ 0xa0, "plii_movie_dsx" }, { 0xa1, "plii_music_dsx" }, { 0xa2, "plii_game_dsx" },
	{ 0xa3, "neo6_cinema_dsx" }, { 0xa4, "neo6_music_dsx" },
	{ 0xa5, "neural_surround_dsx" }, { 0xa7, "dolby_ex_dsx" },
};

static const char *lmd_name(int code)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(lmd_names); i++) {
		if (lmd_names[i].code == code) {
			return lmd_names[i].name;
		}
	}

	return NULL;
}

/* the mode as the MCU's own byte and, once it reported it, by name */
static void add_listening_mode(void)
{
	const char *n = lmd_name(lmd);

	if (listening_mode >= 0) {
		blobmsg_add_u32(&b, "mode", listening_mode);
	}

	if (n) {
		blobmsg_add_string(&b, "name", n);
	}
}
static int tone_known, bass, treble;	/* front tone, dB */
static int input_interface = -1;	/* MCU_INPUT [2], -1 not known */
static int input_fs;			/* MCU_INPUT [3], sample rate code */
static int input_channels;		/* MCU_INPUT [6], channel allocation */
static int av_sync = -1;		/* MCU_AV_SYNC, ms, -1 not known */
static int volume_known, volume;	/* MCU_VOLUME, 0.01 dB */

/* MCU_DD_DTS: one setting per subcommand, each -1 until the MCU sends it */
enum { ADSY_DEQ = 0x07, ADSY_DVOL = 0x08, ADSY_DEQ_OFFSET = 0x0a };
static struct {
	int deq, deq_offset, dvol;
} adsy = { -1, -1, -1 };

static void add_audyssey(void)
{
	if (adsy.deq >= 0) {
		blobmsg_add_u8(&b, "dynamic_eq", adsy.deq);
	}

	if (adsy.deq_offset >= 0) {
		blobmsg_add_u32(&b, "dynamic_eq_offset", (uint32_t)adsy.deq_offset);
	}

	if (adsy.dvol >= 0) {
		blobmsg_add_u32(&b, "dynamic_volume", (uint32_t)adsy.dvol);
	}
}

/* MCU_DISTANCE, in the receiver's order; the 13th value is not a speaker */
static const char *const distance_ch[] = {
	"fl", "c", "fr", "sl", "sr", "sbl", "sbr", "fhl", "fhr", "fwl", "fwr", "sw",
};

static int distance_known, distance_unit = -1, distance[ARRAY_SIZE(distance_ch)];

/* MCU_SPEAKERS [2..7]: 0 no speaker, 1 large, else small with that crossover */
static const char *const speaker_name[] = {
	"front", "center", "surround", "surround_back", "front_high", "front_wide",
};

/*
 * Their DSP_STREAM mask bits, which light the indicators and close the relays.
 * No front wide bit closes the surround back relay; 0x0020 does, and lights
 * the single surround back indicator too.
 */
static const uint16_t speaker_bit[] = { 0x0001, 0x0002, 0x0008, 0x0010, 0x0200, 0x1020 };
#define SB_SINGLE_BIT	0x0020
#define SUBWOOFER_BIT	0x4000

static const uint8_t crossover_hz[16] = {
	0, 0, 40, 45, 50, 55, 60, 70, 80, 90, 100, 110, 120, 130, 150, 200,
};

static const uint8_t lfe_lowpass_hz[] = { 80, 90, 100, 110, 120 };	/* 5: bypass */

/* MCU_CH_LAYOUT: front high, front wide or surround back, then pre-outs */
static const char *const layout_name[] = {
	"none", "sb", "fh", "fw", "sb_fh", "sb_fw", "fh_fw", "fh_sb_pre", "fw_sb_pre",
	"fw_fh_pre", "sb_pre", "fh_pre", "fw_pre", "sb_fh_fw_pre",
};

static const char *const lfe_format[] = {
	"dd", "dts", "aac", "multichannel_pcm", "truehd", "dts_hd_ma", "dsd",
};

static const int lfe_level_db[] = { -100, -20, -10, 0 };	/* -100: off */

/* MCU_EQUALIZER: 15 bands for each speaker, 25 Hz to 16 kHz, 5 for the subwoofer */
#define EQ_BANDS	15
#define EQ_SUB_BANDS	5

static struct {
	int on, on_known, known;
	int8_t db[6 * EQ_BANDS + EQ_SUB_BANDS];
} eq;

static struct {
	int known;
	uint8_t setup[10];		/* MCU_SPEAKERS [1..10] */
	int layout;			/* -1 not known */
} speakers = { .layout = -1 };

/* MCU_TESTTONE_CH channel codes, in the receiver's own order */
static const char *const testtone_ch[] = {
	"fl", "c", "fr", "sr", "sbr", "sbl", "sl", "sw", "fhl", "fhr", "fwl", "fwr",
};

static void add_hex(const char *name, const uint8_t *d, size_t len)
{
	char *s;
	size_t i;

	s = blobmsg_alloc_string_buffer(&b, name, 2 * len + 1);
	for (i = 0; i < len; i++) {
		sprintf(s + 2 * i, "%02x", d[i]);
	}

	s[2 * len] = 0;
	blobmsg_add_string_buffer(&b);
}

static int parse_hex(const char *s, uint8_t *out, size_t len)
{
	unsigned int v;
	size_t i;

	if (strlen(s) != 2 * len) {
		return -EINVAL;
	}

	for (i = 0; i < len; i++) {
		if (sscanf(s + 2 * i, "%2x", &v) != 1) {
			return -EINVAL;
		}

		out[i] = v;
	}

	return 0;
}

static void add_distances(void)
{
	void *t = blobmsg_open_table(&b, "distances");
	size_t i;

	for (i = 0; i < ARRAY_SIZE(distance_ch); i++) {
		blobmsg_add_u32(&b, distance_ch[i], distance[i]);
	}

	blobmsg_close_table(&b, t);
}

/* unit: the byte before them in MCU_DISTANCE; d: the 13 values */
static void set_distances(uint8_t unit, const uint8_t *d)
{
	size_t i;

	distance_known = 1;
	/* not known yet, perhaps the unit the menu shows: logged, not reported */
	if (unit != distance_unit) {
		log_msg(LOG_INFO, "distance byte 0: %u", unit);
		distance_unit = unit;
	}

	for (i = 0; i < ARRAY_SIZE(distance_ch); i++) {
		distance[i] = d[2 * i] | (d[1 + 2 * i] << 8);
	}
}

/*
 * The speakers there are, each with its crossover in Hz, 0 for a large one;
 * the subwoofers, the LFE low pass (0 bypass), double bass and what the
 * surround back amplifiers drive.
 */
static void add_speakers(void)
{
	const uint8_t *s = speakers.setup;
	void *t = blobmsg_open_table(&b, "speakers");
	size_t i;

	for (i = 0; i < ARRAY_SIZE(speaker_name); i++) {
		if (s[1 + i]) {
			blobmsg_add_u32(&b, speaker_name[i], crossover_hz[s[1 + i] & 15]);
		}
	}

	blobmsg_close_table(&b, t);
	blobmsg_add_u32(&b, "subwoofers", s[0]);
	blobmsg_add_u32(&b, "surround_back_channels", s[7] ? 2 : 1);
	blobmsg_add_u32(&b, "lfe_lowpass", s[8] < ARRAY_SIZE(lfe_lowpass_hz) ? lfe_lowpass_hz[s[8]] : 0);
	blobmsg_add_u8(&b, "double_bass", s[9] == 1);
	if (speakers.layout >= 0 && speakers.layout < (int)ARRAY_SIZE(layout_name)) {
		blobmsg_add_string(&b, "layout", layout_name[speakers.layout]);
	}
}

/* the speakers the DSP plays on; the surround back amplifiers drive the first in the layout */
static void set_stream_speakers(void)
{
	const uint8_t *s = speakers.setup;
	const char *l = speakers.layout >= 0 && speakers.layout < (int)ARRAY_SIZE(layout_name) ?
			layout_name[speakers.layout] : "sb";
	uint8_t st[STREAM_LEN];
	uint16_t mask = s[0] ? SUBWOOFER_BIT : 0;
	size_t i;

	for (i = 0; i < 3; i++) {
		if (s[1 + i]) {
			mask |= speaker_bit[i];
		}
	}

	i = !strncmp(l, "sb", 2) ? 3 : !strncmp(l, "fh", 2) ? 4 : !strncmp(l, "fw", 2) ? 5 : 0;
	if (i && s[1 + i]) {
		mask |= i == 3 && !s[7] ? SB_SINGLE_BIT : speaker_bit[i];
	}

	proto_get_stream(st);
	st[STREAM_MASK] = mask & 0xff;
	st[STREAM_MASK + 1] = mask >> 8;
	proto_set_stream(st);
}

/* manual EQ on or off, and each speaker's bands in dB, as far as known */
static void add_equalizer(void)
{
	void *a;
	size_t i, k;

	if (eq.on_known) {
		blobmsg_add_u8(&b, "eq_on", eq.on);
	}

	for (i = 0; eq.known && i <= ARRAY_SIZE(speaker_name); i++) {
		a = blobmsg_open_array(&b, i < ARRAY_SIZE(speaker_name) ?
				       speaker_name[i] : "subwoofer");
		for (k = 0; k < (i < ARRAY_SIZE(speaker_name) ? EQ_BANDS : EQ_SUB_BANDS); k++) {
			blobmsg_add_u32(&b, NULL, (uint32_t)eq.db[i * EQ_BANDS + k]);
		}

		blobmsg_close_array(&b, a);
	}
}

static void add_model(const struct mcu_model *m)
{
	blobmsg_add_string(&b, "make", m->make);
	blobmsg_add_string(&b, "model", m->name);
	blobmsg_add_string(&b, "code", m->code);
}

/* Events */

static void event(const char *id)
{
	ubus_send_event(ctx, id, b.head);
}

static void on_display_mode(const char *mode)
{
	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "mode", mode);
	event("mcu.display");
}

static void on_ready(const struct mcu_model *m)
{
	blob_buf_init(&b, 0);
	add_model(m);
	event("mcu.ready");
}

static const char *const power_names[] = {
	[POWER_ON] = "on", [POWER_OFF] = "off", [POWER_RESET] = "reset",
};

static const char *power_name(uint8_t state)
{
	return state < ARRAY_SIZE(power_names) ? power_names[state] : "unknown";
}

static const char *const update_stages[] = {
	[UPDATE_VERSIONS] = "check", [UPDATE_MODE] = "download",
	[UPDATE_START] = "install", [UPDATE_UPLOAD] = "upload",
};

static int update_usb;			/* the MCU looks on USB, not the network */

static void on_frame(const struct link_frame *f, const char *name)
{
	const uint8_t *d = f->data;

	blob_buf_init(&b, 0);
	switch (f->cmd) {
	case MCU_INPUT:
		if (f->len >= 6) {
			blobmsg_add_u32(&b, "decode", d[1]);
			blobmsg_add_u32(&b, "interface", d[2]);
			input_interface = d[2];
			input_fs = d[3];
			blobmsg_add_u32(&b, "fs", d[3]);
			blobmsg_add_u32(&b, "mclk", d[4]);
			blobmsg_add_u32(&b, "status", d[5]);
		}

		/* the source's channels, as HDMI codes them (CEA-861 allocation) */
		if (f->len >= 7) {
			input_channels = d[6];
			blobmsg_add_u32(&b, "channels", d[6]);
		}

		add_hex("data", d, f->len);
		event("mcu.input");
		return;
	case MCU_INPUT_SELECT:
		add_hex("data", d, f->len);
		event("mcu.input");
		return;
	case MCU_VOLUME:
		/* s16 LE in 0.01 dB */
		if (f->len >= 3) {
			volume = (int16_t)(d[1] | (d[2] << 8));
			volume_known = 1;
			blobmsg_add_u32(&b, "volume", (uint32_t)volume);
		}

		event("mcu.volume");
		return;
	case MCU_DISTANCE:
		if (f->len >= 1 + 2 * ARRAY_SIZE(distance_ch)) {
			set_distances(d[0], d + 1);
			add_distances();
			event("mcu.distance");
			return;
		}

		break;
	case MCU_INITIAL:
		/* everything at start-up: the speakers, the distances, the layout */
		if (f->len > 0xc6) {
			memcpy(speakers.setup, d + 1, sizeof(speakers.setup));
			speakers.known = 1;
			speakers.layout = d[0xc6];
			set_stream_speakers();
			add_speakers();
			event("mcu.speakers");
			blob_buf_init(&b, 0);
			set_distances(d[11], d + 13);
			add_distances();
			event("mcu.distance");
			blob_buf_init(&b, 0);
		}

		break;
	case MCU_SPEAKERS:
		if (f->len >= 1 + sizeof(speakers.setup)) {
			memcpy(speakers.setup, d + 1, sizeof(speakers.setup));
			speakers.known = 1;
			set_stream_speakers();
			add_speakers();
			event("mcu.speakers");
			return;
		}

		break;
	case MCU_CH_LAYOUT:
		if (f->len >= 2) {
			speakers.layout = d[1];
			if (speakers.known) {
				set_stream_speakers();
				add_speakers();
				event("mcu.speakers");
				return;
			}
		}

		break;
	case MCU_EQUALIZER:
		/* the MCU sends either only when it changes, not at start-up */
		if (f->len == 2 && d[0] == 0) {
			eq.on = d[1] != 0;
			eq.on_known = 1;
		} else if (f->len >= 1 + sizeof(eq.db) && d[0] == 1) {
			memcpy(eq.db, d + 1, sizeof(eq.db));
			eq.known = 1;
		} else {
			break;
		}

		{
			void *t = blobmsg_open_table(&b, "equalizer");

			add_equalizer();
			blobmsg_close_table(&b, t);
			event("mcu.equalizer");
		}

		return;
	case MCU_DD_DTS:
		/* the others are reported as mcu.frame */
		if (f->len < 2 || (d[0] != ADSY_DEQ && d[0] != ADSY_DEQ_OFFSET && d[0] != ADSY_DVOL)) {
			break;
		}

		if (d[0] == ADSY_DEQ) {
			adsy.deq = d[1] != 0;
		} else if (d[0] == ADSY_DEQ_OFFSET) {
			/* reference level offset in 5 dB steps */
			adsy.deq_offset = 5 * d[1];
		} else {
			/* 0 off, then light, medium, heavy */
			adsy.dvol = d[1];
		}

		add_audyssey();
		event("mcu.audyssey");
		return;
	case MCU_AV_SYNC:
		/* the menu's value less 22 ms, what the stock DSP delayed anyway */
		if (f->len < 3) {
			break;
		}

		av_sync = d[1] | (d[2] << 8);
		blobmsg_add_u32(&b, "ms", (uint32_t)av_sync);
		event("mcu.av_sync");
		return;
	case MCU_LFE_LEVEL:
		if (f->len < 2 || d[0] >= ARRAY_SIZE(lfe_format) || d[1] >= ARRAY_SIZE(lfe_level_db)) {
			break;
		}

		blobmsg_add_string(&b, "format", lfe_format[d[0]]);
		blobmsg_add_u32(&b, "level", (uint32_t)lfe_level_db[d[1]]);
		event("mcu.lfe_level");
		return;
	case MCU_LISTENING_MODE:
		if (f->len < 2) {
			break;
		}

		/*
		 * The ISCP code comes only when it changes, and may come before or
		 * after this: ask for it, the name follows if it is a new one.
		 */
		if (d[1] != listening_mode) {
			proto_ask_listening_mode();
		}

		listening_mode = d[1];
		add_listening_mode();
		event("mcu.listening_mode");
		return;
	case MCU_BASS_TREBLE:
		if (f->len >= 9 && d[0] == 0) {
			tone_known = 1;
			bass = (int8_t)d[1];
			treble = (int8_t)d[8];
			blobmsg_add_u32(&b, "bass", (uint32_t)bass);
			blobmsg_add_u32(&b, "treble", (uint32_t)treble);
		}

		event("mcu.tone");
		return;
	case MCU_TESTTONE_CH:
		if (f->len >= 2 && d[1] < ARRAY_SIZE(testtone_ch)) {
			blobmsg_add_string(&b, "channel", testtone_ch[d[1]]);
			event("mcu.testtone");
			return;
		}

		break;
	case MCU_POWER:
		if (f->len) {
			blobmsg_add_string(&b, "state", power_name(d[0]));
		}

		event("mcu.power");
		return;
	case MCU_NETWORK:
		if (f->len) {
			blobmsg_add_u32(&b, "item", d[0]);
		}

		add_hex("data", d, f->len);
		event("mcu.network");
		return;
	case MCU_UPDATE:
		if (!f->len) {
			break;
		}

		if (d[0] == UPDATE_SOURCE) {
			update_usb = f->len > 1 && d[1] == 1;
			return;
		}

		if (d[0] >= ARRAY_SIZE(update_stages) || !update_stages[d[0]]) {
			break;
		}

		blobmsg_add_string(&b, "stage", update_stages[d[0]]);
		blobmsg_add_string(&b, "source", update_usb ? "usb" : "network");
		event("mcu.update");
		return;
	case MCU_ISCP:
		if (f->len >= 8 && !memcmp(d + 1, "!1LMD", 5) && isxdigit(d[6]) && isxdigit(d[7])) {
			char hex[3] = { d[6], d[7], 0 };
			long v = strtol(hex, NULL, 16);

			if (v != lmd) {
				lmd = v;
				add_listening_mode();
				event("mcu.listening_mode");
				blob_buf_init(&b, 0);
			}
		}

		if (f->len > 1) {
			char *s = blobmsg_alloc_string_buffer(&b, "command", f->len);

			memcpy(s, d + 1, f->len - 1);
			s[f->len - 1] = 0;
			blobmsg_add_string_buffer(&b);
		}

		event("mcu.iscp");
		return;
	default:
		break;
	}

	blobmsg_add_u32(&b, "cmd", f->cmd);
	blobmsg_add_string(&b, "name", name);
	add_hex("data", d, f->len);
	event("mcu.frame");
}

/* Methods */

static int m_status(struct ubus_context *c, struct ubus_object *obj,
		    struct ubus_request_data *req, const char *method,
		    struct blob_attr *msg)
{
	uint8_t s[STREAM_LEN];

	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "stage", proto_stage_name());
	add_model(proto_model());
	proto_get_stream(s);
	add_hex("stream", s, STREAM_LEN);
	if (eq.known || eq.on_known) {
		void *t = blobmsg_open_table(&b, "equalizer");

		add_equalizer();
		blobmsg_close_table(&b, t);
	}

	if (av_sync >= 0) {
		blobmsg_add_u32(&b, "av_sync", (uint32_t)av_sync);
	}

	if (volume_known) {
		blobmsg_add_u32(&b, "volume", (uint32_t)volume);
	}

	add_audyssey();

	if (input_interface >= 0) {
		blobmsg_add_u32(&b, "interface", (uint32_t)input_interface);
		blobmsg_add_u32(&b, "fs", (uint32_t)input_fs);
		blobmsg_add_u32(&b, "channels", (uint32_t)input_channels);
	}

	blobmsg_add_string(&b, "display", display_mode_name());

	if (tone_known) {
		blobmsg_add_u32(&b, "bass", (uint32_t)bass);
		blobmsg_add_u32(&b, "treble", (uint32_t)treble);
	}

	if (listening_mode >= 0) {
		blobmsg_add_u32(&b, "listening_mode", listening_mode);
	}

	if (lmd_name(lmd)) {
		blobmsg_add_string(&b, "listening_mode_name", lmd_name(lmd));
	}

	if (distance_known) {
		add_distances();
	}

	if (speakers.known) {
		add_speakers();
	}

	blobmsg_add_u8(&b, "update_check", proto_update_waiting());
	blobmsg_add_u32(&b, "queued", link_queued());
	ubus_send_reply(c, req, b.head);
	return 0;
}

enum { ID_VERSION, ID_ID, __ID_MAX };

static const struct blobmsg_policy identity_policy[__ID_MAX] = {
	[ID_VERSION] = { .name = "dsp_version", .type = BLOBMSG_TYPE_STRING },
	[ID_ID] = { .name = "dsp_id", .type = BLOBMSG_TYPE_STRING },
};

static int m_identity(struct ubus_context *c, struct ubus_object *obj,
		      struct ubus_request_data *req, const char *method,
		      struct blob_attr *msg)
{
	struct blob_attr *tb[__ID_MAX];

	blobmsg_parse(identity_policy, __ID_MAX, tb, blob_data(msg), blob_len(msg));
	if (!tb[ID_VERSION] && !tb[ID_ID]) {
		return UBUS_STATUS_INVALID_ARGUMENT;
	}

	proto_set_version(tb[ID_VERSION] ? blobmsg_get_string(tb[ID_VERSION]) : NULL,
			  tb[ID_ID] ? blobmsg_get_string(tb[ID_ID]) : NULL);
	return 0;
}

enum { ST_HEX, ST_TYPE, ST_FS, ST_CHANNELS, ST_MASK, __ST_MAX };

static const struct blobmsg_policy stream_policy[__ST_MAX] = {
	[ST_HEX] = { .name = "hex", .type = BLOBMSG_TYPE_STRING },
	[ST_TYPE] = { .name = "type", .type = BLOBMSG_TYPE_INT32 },
	[ST_FS] = { .name = "fs", .type = BLOBMSG_TYPE_INT32 },
	[ST_CHANNELS] = { .name = "channels", .type = BLOBMSG_TYPE_STRING },
	[ST_MASK] = { .name = "mask", .type = BLOBMSG_TYPE_INT32 },
};

static int m_stream(struct ubus_context *c, struct ubus_object *obj,
		    struct ubus_request_data *req, const char *method,
		    struct blob_attr *msg)
{
	struct blob_attr *tb[__ST_MAX];
	uint8_t s[STREAM_LEN];
	uint32_t v;
	const char *ch;
	size_t n;

	blobmsg_parse(stream_policy, __ST_MAX, tb, blob_data(msg), blob_len(msg));
	proto_get_stream(s);
	if (tb[ST_HEX] && parse_hex(blobmsg_get_string(tb[ST_HEX]), s, STREAM_LEN)) {
		return UBUS_STATUS_INVALID_ARGUMENT;
	}

	if (tb[ST_TYPE]) {
		s[STREAM_TYPE] = blobmsg_get_u32(tb[ST_TYPE]);
	}

	if (tb[ST_FS]) {
		memset(s + STREAM_FS, blobmsg_get_u32(tb[ST_FS]), 3);
	}

	if (tb[ST_CHANNELS]) {
		ch = blobmsg_get_string(tb[ST_CHANNELS]);
		n = strlen(ch);
		memset(s + STREAM_CHANNELS, ' ', 4);
		memcpy(s + STREAM_CHANNELS + (n < 4 ? 4 - n : 0), ch, n < 4 ? n : 4);
	}

	if (tb[ST_MASK]) {
		v = blobmsg_get_u32(tb[ST_MASK]);
		s[STREAM_MASK] = v & 0xff;
		s[STREAM_MASK + 1] = v >> 8;
	}

	proto_set_stream(s);
	return 0;
}

enum { OUT_MASK, __OUT_MAX };

static const struct blobmsg_policy outputs_policy[__OUT_MAX] = {
	[OUT_MASK] = { .name = "mask", .type = BLOBMSG_TYPE_INT32 },
};

static int m_outputs(struct ubus_context *c, struct ubus_object *obj,
		     struct ubus_request_data *req, const char *method,
		     struct blob_attr *msg)
{
	struct blob_attr *tb[__OUT_MAX];

	blobmsg_parse(outputs_policy, __OUT_MAX, tb, blob_data(msg), blob_len(msg));
	if (!tb[OUT_MASK]) {
		return UBUS_STATUS_INVALID_ARGUMENT;
	}

	proto_set_outputs(blobmsg_get_u32(tb[OUT_MASK]));
	return 0;
}

enum { DSP_TITLE, DSP_ARTIST, DSP_ALBUM, DSP_STATE, DSP_MODE, __DSP_MAX };

static const struct blobmsg_policy display_policy[__DSP_MAX] = {
	[DSP_TITLE] = { .name = "title", .type = BLOBMSG_TYPE_STRING },
	[DSP_ARTIST] = { .name = "artist", .type = BLOBMSG_TYPE_STRING },
	[DSP_ALBUM] = { .name = "album", .type = BLOBMSG_TYPE_STRING },
	[DSP_STATE] = { .name = "state", .type = BLOBMSG_TYPE_STRING },
	[DSP_MODE] = { .name = "mode", .type = BLOBMSG_TYPE_STRING },
};

/* what plays on NET (state "play", "pause" or "stop"), or the mode alone */
static int m_display(struct ubus_context *c, struct ubus_object *obj,
		     struct ubus_request_data *req, const char *method,
		     struct blob_attr *msg)
{
	struct blob_attr *tb[__DSP_MAX];
	const char *s;
	enum display_state state;

	blobmsg_parse(display_policy, __DSP_MAX, tb, blob_data(msg), blob_len(msg));
	if (tb[DSP_MODE] && display_set_mode(blobmsg_get_string(tb[DSP_MODE]))) {
		return UBUS_STATUS_INVALID_ARGUMENT;
	}

	if (tb[DSP_MODE] && !tb[DSP_TITLE] && !tb[DSP_ARTIST] && !tb[DSP_ALBUM] &&
	    !tb[DSP_STATE]) {
		return 0;
	}

	s = tb[DSP_STATE] ? blobmsg_get_string(tb[DSP_STATE]) : "stop";
	if (!strcmp(s, "play")) {
		state = DISPLAY_PLAYING;
	} else if (!strcmp(s, "pause")) {
		state = DISPLAY_PAUSED;
	} else if (!strcmp(s, "stop")) {
		state = DISPLAY_STOPPED;
	} else {
		return UBUS_STATUS_INVALID_ARGUMENT;
	}

	display_set(tb[DSP_TITLE] ? blobmsg_get_string(tb[DSP_TITLE]) : NULL,
		    tb[DSP_ARTIST] ? blobmsg_get_string(tb[DSP_ARTIST]) : NULL,
		    tb[DSP_ALBUM] ? blobmsg_get_string(tb[DSP_ALBUM]) : NULL, state);
	return 0;
}

enum { MTR_LEFT, MTR_RIGHT, MTR_LEFT_PEAK, MTR_RIGHT_PEAK, __MTR_MAX };

static const struct blobmsg_policy meter_policy[__MTR_MAX] = {
	[MTR_LEFT] = { .name = "left", .type = BLOBMSG_TYPE_INT32 },
	[MTR_RIGHT] = { .name = "right", .type = BLOBMSG_TYPE_INT32 },
	[MTR_LEFT_PEAK] = { .name = "left_peak", .type = BLOBMSG_TYPE_INT32 },
	[MTR_RIGHT_PEAK] = { .name = "right_peak", .type = BLOBMSG_TYPE_INT32 },
};

/* the levels for the meter: RMS, and peaks for the markers, dBFS */
static int m_meter(struct ubus_context *c, struct ubus_object *obj,
		   struct ubus_request_data *req, const char *method,
		   struct blob_attr *msg)
{
	struct blob_attr *tb[__MTR_MAX];
	int l, r;

	blobmsg_parse(meter_policy, __MTR_MAX, tb, blob_data(msg), blob_len(msg));
	if (!tb[MTR_LEFT] || !tb[MTR_RIGHT]) {
		return UBUS_STATUS_INVALID_ARGUMENT;
	}

	l = (int32_t)blobmsg_get_u32(tb[MTR_LEFT]);
	r = (int32_t)blobmsg_get_u32(tb[MTR_RIGHT]);
	display_meter(l, r, tb[MTR_LEFT_PEAK] ? (int32_t)blobmsg_get_u32(tb[MTR_LEFT_PEAK]) : l,
		      tb[MTR_RIGHT_PEAK] ? (int32_t)blobmsg_get_u32(tb[MTR_RIGHT_PEAK]) : r);
	return 0;
}

enum { ISCP_COMMAND, __ISCP_MAX };

static const struct blobmsg_policy iscp_policy[__ISCP_MAX] = {
	[ISCP_COMMAND] = { .name = "command", .type = BLOBMSG_TYPE_STRING },
};

static int m_iscp(struct ubus_context *c, struct ubus_object *obj,
		  struct ubus_request_data *req, const char *method,
		  struct blob_attr *msg)
{
	struct blob_attr *tb[__ISCP_MAX];

	blobmsg_parse(iscp_policy, __ISCP_MAX, tb, blob_data(msg), blob_len(msg));
	if (!tb[ISCP_COMMAND] || proto_iscp(blobmsg_get_string(tb[ISCP_COMMAND]))) {
		return UBUS_STATUS_INVALID_ARGUMENT;
	}

	return 0;
}

/* the receiver's inputs by name, as ISCP SLI codes */
static const struct {
	const char *name, *code;
} inputs[] = {
	{ "vcr_dvr", "00" }, { "cbl_sat", "01" }, { "game", "02" }, { "aux", "03" },
	{ "pc", "05" }, { "bd_dvd", "10" }, { "phono", "22" }, { "tv_cd", "23" },
	{ "tuner", "26" }, { "usb", "29" }, { "net", "2B" },
};

enum { IN_INPUT, __IN_MAX };

static const struct blobmsg_policy input_policy[__IN_MAX] = {
	[IN_INPUT] = { .name = "input", .type = BLOBMSG_TYPE_STRING },
};

/* select an input, as the remote would */
static int m_input(struct ubus_context *c, struct ubus_object *obj,
		   struct ubus_request_data *req, const char *method,
		   struct blob_attr *msg)
{
	struct blob_attr *tb[__IN_MAX];
	char cmd[8];
	size_t i;

	blobmsg_parse(input_policy, __IN_MAX, tb, blob_data(msg), blob_len(msg));
	for (i = 0; tb[IN_INPUT] && i < ARRAY_SIZE(inputs); i++) {
		if (!strcmp(blobmsg_get_string(tb[IN_INPUT]), inputs[i].name)) {
			snprintf(cmd, sizeof(cmd), "SLI%s", inputs[i].code);
			return proto_iscp(cmd) ? UBUS_STATUS_UNKNOWN_ERROR : 0;
		}
	}

	return UBUS_STATUS_INVALID_ARGUMENT;
}

enum { LV_HEX, __LV_MAX };

static const struct blobmsg_policy levels_policy[__LV_MAX] = {
	[LV_HEX] = { .name = "hex", .type = BLOBMSG_TYPE_STRING },
};

static int m_levels(struct ubus_context *c, struct ubus_object *obj,
		    struct ubus_request_data *req, const char *method,
		    struct blob_attr *msg)
{
	struct blob_attr *tb[__LV_MAX];
	uint8_t l[LEVELS_LEN];

	blobmsg_parse(levels_policy, __LV_MAX, tb, blob_data(msg), blob_len(msg));
	if (!tb[LV_HEX] || parse_hex(blobmsg_get_string(tb[LV_HEX]), l, LEVELS_LEN)) {
		return UBUS_STATUS_INVALID_ARGUMENT;
	}

	proto_set_levels(l);
	return 0;
}

enum { NW_LINK, NW_DHCP, NW_IP, NW_NETMASK, NW_GATEWAY, NW_DNS, NW_MAC, NW_NAME, __NW_MAX };

static const struct blobmsg_policy network_policy[__NW_MAX] = {
	[NW_LINK] = { .name = "link", .type = BLOBMSG_TYPE_BOOL },
	[NW_DHCP] = { .name = "dhcp", .type = BLOBMSG_TYPE_BOOL },
	[NW_IP] = { .name = "ip", .type = BLOBMSG_TYPE_STRING },
	[NW_NETMASK] = { .name = "netmask", .type = BLOBMSG_TYPE_STRING },
	[NW_GATEWAY] = { .name = "gateway", .type = BLOBMSG_TYPE_STRING },
	[NW_DNS] = { .name = "dns", .type = BLOBMSG_TYPE_ARRAY },
	[NW_MAC] = { .name = "mac", .type = BLOBMSG_TYPE_STRING },
	[NW_NAME] = { .name = "name", .type = BLOBMSG_TYPE_STRING },
};

static int parse_addr(struct blob_attr *a, uint8_t *dst)
{
	if (!a) {
		return 0;
	}

	return inet_pton(AF_INET, blobmsg_get_string(a), dst) == 1 ? 0 : -1;
}

static int m_network(struct ubus_context *c, struct ubus_object *obj,
		     struct ubus_request_data *req, const char *method,
		     struct blob_attr *msg)
{
	struct blob_attr *tb[__NW_MAX], *cur;
	struct mcu_network n;
	unsigned int rem, i = 0;

	memset(&n, 0, sizeof(n));
	blobmsg_parse(network_policy, __NW_MAX, tb, blob_data(msg), blob_len(msg));
	n.link = tb[NW_LINK] && blobmsg_get_bool(tb[NW_LINK]);
	n.dhcp = tb[NW_DHCP] && blobmsg_get_bool(tb[NW_DHCP]);
	if (parse_addr(tb[NW_IP], n.ip) || parse_addr(tb[NW_NETMASK], n.netmask) ||
	    parse_addr(tb[NW_GATEWAY], n.gateway)) {
		return UBUS_STATUS_INVALID_ARGUMENT;
	}

	if (tb[NW_DNS]) {
		blobmsg_for_each_attr(cur, tb[NW_DNS], rem) {
			if (i == 2) {
				break;
			}

			if (blobmsg_type(cur) != BLOBMSG_TYPE_STRING || parse_addr(cur, n.dns[i++])) {
				return UBUS_STATUS_INVALID_ARGUMENT;
			}
		}
	}

	if (tb[NW_MAC] && sscanf(blobmsg_get_string(tb[NW_MAC]),
				 "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &n.mac[0], &n.mac[1],
				 &n.mac[2], &n.mac[3], &n.mac[4], &n.mac[5]) != 6) {
		return UBUS_STATUS_INVALID_ARGUMENT;
	}

	if (tb[NW_NAME]) {
		snprintf(n.name, sizeof(n.name), "%s", blobmsg_get_string(tb[NW_NAME]));
	}

	proto_set_network(&n);
	return 0;
}

enum { PW_STATE, __PW_MAX };

static const struct blobmsg_policy power_policy[__PW_MAX] = {
	[PW_STATE] = { .name = "state", .type = BLOBMSG_TYPE_STRING },
};

/* the policy is done with power off / reset, answer the MCU */
static int m_power(struct ubus_context *c, struct ubus_object *obj,
		   struct ubus_request_data *req, const char *method,
		   struct blob_attr *msg)
{
	struct blob_attr *tb[__PW_MAX];
	uint8_t i;

	blobmsg_parse(power_policy, __PW_MAX, tb, blob_data(msg), blob_len(msg));
	if (!tb[PW_STATE]) {
		return UBUS_STATUS_INVALID_ARGUMENT;
	}

	for (i = 0; i < ARRAY_SIZE(power_names); i++) {
		if (!strcmp(blobmsg_get_string(tb[PW_STATE]), power_names[i])) {
			return proto_power_done(i) ? UBUS_STATUS_NOT_FOUND : 0;
		}
	}

	return UBUS_STATUS_INVALID_ARGUMENT;
}

enum { UP_AVAILABLE, UP_DOWNLOADED, UP_PROGRESS, UP_RESULT, __UP_MAX };

static const struct blobmsg_policy update_policy[__UP_MAX] = {
	[UP_AVAILABLE] = { .name = "available", .type = BLOBMSG_TYPE_BOOL },
	[UP_DOWNLOADED] = { .name = "downloaded", .type = BLOBMSG_TYPE_INT32 },
	[UP_PROGRESS] = { .name = "progress", .type = BLOBMSG_TYPE_INT32 },
	[UP_RESULT] = { .name = "result", .type = BLOBMSG_TYPE_STRING },
};

/* the answers in the update dialogue: one of them per call */
static int m_update(struct ubus_context *c, struct ubus_object *obj,
		    struct ubus_request_data *req, const char *method,
		    struct blob_attr *msg)
{
	struct blob_attr *tb[__UP_MAX];

	blobmsg_parse(update_policy, __UP_MAX, tb, blob_data(msg), blob_len(msg));
	if (tb[UP_AVAILABLE]) {
		proto_update_check(blobmsg_get_bool(tb[UP_AVAILABLE]));
	} else if (tb[UP_PROGRESS]) {
		int32_t p = blobmsg_get_u32(tb[UP_PROGRESS]);

		proto_update_progress(p < 0 ? 0 : p > 100 ? 100 : p);
	} else if (tb[UP_DOWNLOADED]) {
		proto_update_downloaded(blobmsg_get_u32(tb[UP_DOWNLOADED]));
	} else if (tb[UP_RESULT]) {
		proto_update_result(!strcmp(blobmsg_get_string(tb[UP_RESULT]), "ok"));
	} else {
		return UBUS_STATUS_INVALID_ARGUMENT;
	}

	return 0;
}

enum { USB_PRESENT, USB_PORT, __USB_MAX };

static const struct blobmsg_policy usb_policy[__USB_MAX] = {
	[USB_PRESENT] = { .name = "present", .type = BLOBMSG_TYPE_BOOL },
	[USB_PORT] = { .name = "port", .type = BLOBMSG_TYPE_INT32 },
};

static int m_usb(struct ubus_context *c, struct ubus_object *obj,
		 struct ubus_request_data *req, const char *method,
		 struct blob_attr *msg)
{
	struct blob_attr *tb[__USB_MAX];

	blobmsg_parse(usb_policy, __USB_MAX, tb, blob_data(msg), blob_len(msg));
	if (!tb[USB_PRESENT] || (tb[USB_PORT] && blobmsg_get_u32(tb[USB_PORT]) > 1)) {
		return UBUS_STATUS_INVALID_ARGUMENT;
	}

	proto_set_usb(tb[USB_PORT] ? blobmsg_get_u32(tb[USB_PORT]) : 0,
		      blobmsg_get_bool(tb[USB_PRESENT]));
	return 0;
}

enum { SEND_CMD, SEND_DATA, __SEND_MAX };

static const struct blobmsg_policy send_policy[__SEND_MAX] = {
	[SEND_CMD] = { .name = "cmd", .type = BLOBMSG_TYPE_INT32 },
	[SEND_DATA] = { .name = "data", .type = BLOBMSG_TYPE_STRING },
};

static int m_send(struct ubus_context *c, struct ubus_object *obj,
		  struct ubus_request_data *req, const char *method,
		  struct blob_attr *msg)
{
	struct blob_attr *tb[__SEND_MAX];
	uint8_t d[LINK_MAX_DATA];
	size_t len = 0;
	const char *hex;

	blobmsg_parse(send_policy, __SEND_MAX, tb, blob_data(msg), blob_len(msg));
	if (!tb[SEND_CMD]) {
		return UBUS_STATUS_INVALID_ARGUMENT;
	}

	if (tb[SEND_DATA]) {
		hex = blobmsg_get_string(tb[SEND_DATA]);
		len = strlen(hex) / 2;
		if (len > sizeof(d) || parse_hex(hex, d, len)) {
			return UBUS_STATUS_INVALID_ARGUMENT;
		}
	}

	if (link_send(blobmsg_get_u32(tb[SEND_CMD]), d, len)) {
		return UBUS_STATUS_UNKNOWN_ERROR;
	}

	return 0;
}

static const struct ubus_method mcu_methods[] = {
	UBUS_METHOD_NOARG("status", m_status),
	UBUS_METHOD("identity", m_identity, identity_policy),
	UBUS_METHOD("stream", m_stream, stream_policy),
	UBUS_METHOD("outputs", m_outputs, outputs_policy),
	UBUS_METHOD("levels", m_levels, levels_policy),
	UBUS_METHOD("display", m_display, display_policy),
	UBUS_METHOD("meter", m_meter, meter_policy),
	UBUS_METHOD("iscp", m_iscp, iscp_policy),
	UBUS_METHOD("input", m_input, input_policy),
	UBUS_METHOD("network", m_network, network_policy),
	UBUS_METHOD("power", m_power, power_policy),
	UBUS_METHOD("update", m_update, update_policy),
	UBUS_METHOD("usb", m_usb, usb_policy),
	UBUS_METHOD("send", m_send, send_policy),
};

static struct ubus_object_type mcu_type = UBUS_OBJECT_TYPE("mcu", mcu_methods);

/* dspd's levels go straight to the meter, ten a second */
static void on_dsp_meter(struct ubus_context *c, struct ubus_event_handler *ev,
			 const char *type, struct blob_attr *msg)
{
	struct blob_attr *tb[__MTR_MAX];
	int l, r;

	blobmsg_parse(meter_policy, __MTR_MAX, tb, blob_data(msg), blob_len(msg));
	if (!tb[MTR_LEFT] || !tb[MTR_RIGHT]) {
		return;
	}

	l = (int32_t)blobmsg_get_u32(tb[MTR_LEFT]);
	r = (int32_t)blobmsg_get_u32(tb[MTR_RIGHT]);
	display_meter(l, r, tb[MTR_LEFT_PEAK] ? (int32_t)blobmsg_get_u32(tb[MTR_LEFT_PEAK]) : l,
		      tb[MTR_RIGHT_PEAK] ? (int32_t)blobmsg_get_u32(tb[MTR_RIGHT_PEAK]) : r);
}

static struct ubus_event_handler dsp_meter_handler = { .cb = on_dsp_meter };

static struct ubus_object mcu_object = {
	.name = "mcu",
	.type = &mcu_type,
	.methods = mcu_methods,
	.n_methods = ARRAY_SIZE(mcu_methods),
};

/* "OSIRIS <build>": what the receiver shows as the DSP version */
static void default_version(char *v, size_t size)
{
	char build[32] = "";
	FILE *f = fopen("/etc/osiris-version", "r");

	if (f) {
		if (fgets(build, sizeof(build), f)) {
			build[strcspn(build, "\n")] = 0;
		}

		fclose(f);
	}

	snprintf(v, size, "OSIRIS %.7s", build);
}

static const struct proto_ops ops = {
	.ready = on_ready,
	.frame = on_frame,
};

int main(int argc, char **argv)
{
	uint32_t spi_hz = 1000000;
	char version[32];
	int opt, rc;

	while ((opt = getopt(argc, argv, "s:v")) != -1) {
		switch (opt) {
		case 's':
			spi_hz = strtoul(optarg, NULL, 0);
			break;
		case 'v':
			link_trace = 1;
			break;
		default:
			fprintf(stderr, "usage: %s [-s spi_hz] [-v]\n", argv[0]);
			return 1;
		}
	}

	openlog("mcud", LOG_PID, LOG_DAEMON);
	uloop_init();
	ctx = ubus_connect(NULL);
	if (!ctx) {
		log_msg(LOG_ERR, "cannot connect to ubusd");
		return 1;
	}

	ubus_add_uloop(ctx);
	rc = ubus_add_object(ctx, &mcu_object);
	if (rc) {
		log_msg(LOG_ERR, "ubus: %s", ubus_strerror(rc));
		return 1;
	}

	rc = ubus_register_event_handler(ctx, &dsp_meter_handler, "dsp.meter");
	if (rc) {
		log_msg(LOG_ERR, "ubus: %s", ubus_strerror(rc));
		return 1;
	}

	rc = link_open(spi_hz, proto_rx);
	if (rc) {
		log_msg(LOG_ERR, "cannot open the MCU link: %s", strerror(-rc));
		return 1;
	}

	default_version(version, sizeof(version));
	display_on_mode(on_display_mode);
	proto_init(&ops, version);
	uloop_run();
	link_close();
	ubus_free(ctx);
	uloop_done();
	return 0;
}
