// SPDX-License-Identifier: BSD-2-Clause
/*
 * The MCU protocol on top of the link: the start-up handshake, the answers
 * the MCU expects at once, and the state it reads (version, id, stream
 * status, levels, active outputs). See docs/mcu-protocol.md.
 */
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <libubox/uloop.h>
#include <libubox/utils.h>

#include "log.h"
#include "cmd.h"
#include "display.h"
#include "proto.h"

#define RETRY_MS	1000
#define RETRIES		3
#define NO_ARG		(-1)
#define POWER_WAIT_MS	3000	/* for the policy before power off / reset */
#define LMD_ASK_AGAIN_MS	5000

static const struct proto_ops *ops;
static enum stage stage;
static struct mcu_model model;
static struct uloop_timeout retry_timer;
static uint8_t pend_cmd, pend_arg;
static struct mcu_network network;
static uint32_t network_asked;	/* MCU_NETWORK items, one bit each */
static struct uloop_timeout power_timer;
static struct uloop_timeout lmd_timer;
static int power_pending = -1;	/* MCU_POWER state not answered yet */
static int pend, retries;
static uint8_t update_stage;	/* the last MCU_UPDATE [0] */
static int update_waiting;	/* asked for an update, not answered yet */

/* PCM, 48 kHz, " 2.0", DSP detected: a capture from the stock firmware */
static uint8_t stream[STREAM_LEN] = {
	[STREAM_TYPE] = STREAM_PCM,
	[STREAM_FS] = FS_48K, FS_48K, FS_48K,
	[STREAM_IN_MASK] = 0x05, 0x00,
	[STREAM_CHANNELS] = ' ', '2', '.', '0',
	[STREAM_MASK] = 0xff, 0x07,
	[STREAM_DETECT] = 0x01,
	[STREAM_FORMAT] = 0x20, 0x20,
	[STREAM_LEN - 1] = 0x01,
};

static uint8_t stream_sent[STREAM_LEN];
static struct uloop_timeout select_timer;	/* pending: E5 seen, no C1 since */
#define SELECT_MS	1000
static struct uloop_timeout relock_timer;	/* pending: a 0x42 for it went out */
#define RELOCK_MS	3000
static uint8_t input_status = 0xff;		/* MCU_INPUT [5], 0 while not locked */
static uint8_t select_was;			/* the last MCU_INPUT_SELECT [0] */

/* what the stock firmware reports; the encoding is not known */
static uint8_t levels[LEVELS_LEN] = {
	0xf0, 0x0a, 0xf0, 0x0a, 0xf0, 0x0a, 0x74, 0x0e, 0x74, 0x0e, 0x74, 0x0e, 0x74,
	0x0e, 0x74, 0x0e, 0x74, 0x0e, 0x74, 0x0e, 0x74, 0x0e, 0x74, 0x0e, 0x74, 0x0e,
};

static uint8_t version[VERSION_LEN];
static uint8_t dsp_id[ID_LEN] = "DA83x 1.02  0000";
static uint8_t outputs = NET_OUTPUTS_FRONT;

static const struct {
	uint8_t cmd;
	const char *name;
} names[] = {
	{ MCU_INITIAL, "initial_settings" },
	{ MCU_INPUT, "input_settings" },
	{ MCU_SPEAKERS, "speaker_config" },
	{ MCU_DISTANCE, "distance" },
	{ MCU_EQUALIZER, "equalizer" },
	{ MCU_THX, "thx" },
	{ MCU_MULTIPLEX_MONO, "multiplex_mono" },
	{ MCU_DD_DTS, "dd_dts_adsy_td" },
	{ MCU_DSD_DIRECT, "dsd_dac_direct" },
	{ MCU_LFE_LEVEL, "lfe_level" },
	{ MCU_AV_SYNC, "av_sync" },
	{ MCU_HDMI_AUDIO, "hdmi_audio_tv_out" },
	{ MCU_BASS_TREBLE, "bass_treble" },
	{ MCU_RE_EQ, "re_eq" },
	{ MCU_MUSIC_OPTIMIZER, "music_optimizer" },
	{ MCU_LATE_NIGHT, "late_night" },
	{ MCU_TESTTONE_CH, "testtone_ch" },
	{ MCU_TESTMODE, "testmode" },
	{ MCU_LISTENING_MODE, "listening_mode" },
	{ MCU_VOLUME, "volume" },
	{ MCU_HEADPHONE, "headphone" },
	{ MCU_CH_LAYOUT, "ch_layout" },
	{ MCU_SPECIAL_MODE, "special_mode" },
	{ MCU_NET_MUTE, "net_mute" },
	{ MCU_NETAUDIO_INFO, "netaudio_info" },
	{ MCU_SPDIF_UPDATE, "spdifupdate_next" },
	{ MCU_AUDYSSEY, "audysseyautosp" },
	{ MCU_NET_VERSION, "net_version" },
	{ MCU_SYSTEM, "system" },
	{ MCU_ITEM, "system_item" },
	{ MCU_DISPLAY, "display" },
	{ MCU_INPUT_SELECT, "inputsel" },
	{ MCU_NETWORK, "network" },
	{ MCU_UPDATE, "update" },
	{ MCU_POWER, "power" },
	{ MCU_FL_REQUEST, "fl_request" },
	{ MCU_UPDATE_MODE, "update_mode" },
	{ MCU_ISCP, "iscp" },
};

static void start(void);

const char *proto_cmd_name(uint8_t cmd)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(names); i++) {
		if (names[i].cmd == cmd) {
			return names[i].name;
		}
	}

	return "unknown";
}

static void queue(uint8_t cmd, const void *data, uint16_t len)
{
	if (link_send(cmd, data, len)) {
		log_msg(LOG_WARNING, "queue full, dropped 0x%02x", cmd);
	}
}

/* only when a field changed: resending it restarts the MCU's input handling */
static void stream_update(void)
{
	if (!memcmp(stream, stream_sent, STREAM_LEN)) {
		return;
	}

	memcpy(stream_sent, stream, STREAM_LEN);
	queue(DSP_STREAM, stream, STREAM_LEN);
}

static void request(uint8_t cmd, int arg)
{
	pend_cmd = cmd;
	pend_arg = arg;
	pend = 1;
	if (arg == NO_ARG) {
		queue(cmd, NULL, 0);
	} else {
		queue(cmd, &pend_arg, 1);
	}

	uloop_timeout_set(&retry_timer, RETRY_MS);
}

/* first two printable strings of MCU_SYSTEM: model code and model name */
static void model_from_system(const uint8_t *d, uint16_t len)
{
	char run[32];
	int n = 0, runs = 0;
	uint16_t i;

	for (i = 0; i <= len; i++) {
		if (i < len && isprint(d[i])) {
			if (n < (int)sizeof(run) - 1) {
				run[n++] = d[i];
			}

			continue;
		}

		if (n >= 3) {
			run[n] = 0;
			if (!runs) {
				snprintf(model.code, sizeof(model.code), "%s", run);
			} else if (runs == 1) {
				snprintf(model.name, sizeof(model.name), "%s", run);
			}

			runs++;
		}

		n = 0;
	}
}

static void beacon_field(char *dst, size_t size, const char *s, const char *key)
{
	const char *p = strstr(s, key), *e;

	if (!p) {
		return;
	}

	p += strlen(key);
	e = strchr(p, '>');
	if (e) {
		snprintf(dst, size, "%.*s", (int)(e - p), p);
	}
}

/* "AMXB<-SDKClass=Receiver><-Make=ONKYO><-Model=TX-NR717>" */
static void model_from_beacon(const uint8_t *d, uint16_t len)
{
	char s[300];

	if (len > sizeof(s) - 1) {
		len = sizeof(s) - 1;
	}

	memcpy(s, d, len);
	s[len] = 0;
	beacon_field(model.make, sizeof(model.make), s, "<-Make=");
	if (!model.name[0]) {
		beacon_field(model.name, sizeof(model.name), s, "<-Model=");
	}
}

/* NET_ISCP as stock sends it: 00, the command, CR */
static const char iscp_lmd_query[] = "\0!1LMDQSTN\r";

void proto_ask_listening_mode(void)
{
	queue(NET_ISCP, iscp_lmd_query, sizeof(iscp_lmd_query) - 1);
}

int proto_iscp(const char *command)
{
	char f[64];
	int n = snprintf(f + 1, sizeof(f) - 1, "!1%s\r", command);

	/* the handshake drops anything else */
	if (stage != STAGE_READY || n < 0 || (size_t)n >= sizeof(f) - 1) {
		return -1;
	}

	f[0] = 0;
	queue(NET_ISCP, f, (uint16_t)(n + 1));
	return 0;
}

static void handshake(uint8_t cmd, const uint8_t *d, uint16_t len)
{
	static const uint8_t done[][2] = {
		{ NET_STATE_UP_0 }, { NET_STATE_UP_1 }, { NET_STATE_READY },
	};
	size_t i;

	if (cmd == MCU_SYSTEM && stage == STAGE_SYSTEM) {
		retries = 0;
		request(NET_ITEM, 0);
		stage = STAGE_ITEMS;
	} else if (cmd == MCU_ITEM && stage == STAGE_ITEMS && len) {
		retries = 0;
		if (d[0] < ITEM_LAST) {
			request(NET_ITEM, d[0] + 1);
		} else {
			queue(DSP_OUTPUTS, &outputs, 1);
			request(NET_DISPLAY, 0);
			stage = STAGE_DISPLAY;
		}
	} else if (cmd == MCU_DISPLAY && stage == STAGE_DISPLAY && len) {
		retries = 0;
		if (!d[0]) {
			request(NET_DISPLAY, 1);
		} else {
			request(NET_SYSTEM, NO_ARG);
			stage = STAGE_FINAL;
		}
	} else if (cmd == MCU_SYSTEM && stage == STAGE_FINAL) {
		for (i = 0; i < ARRAY_SIZE(done); i++) {
			queue(NET_STATE, done[i], 2);
		}

		pend = 0;
		uloop_timeout_cancel(&retry_timer);
		stage = STAGE_READY;
		/*
		 * The MCU reports the listening mode as ISCP only when it changes;
		 * right after a cold start it drops the first question.
		 */
		proto_ask_listening_mode();
		uloop_timeout_set(&lmd_timer, LMD_ASK_AGAIN_MS);
		ops->ready(&model);
	}
}

/* the MCU sometimes drops a request: resend, then fake the answer and go on */
static void retry_cb(struct uloop_timeout *t)
{
	uint8_t fake;

	if (!pend || stage == STAGE_READY) {
		return;
	}

	if (++retries <= RETRIES) {
		request(pend_cmd, pend_cmd == NET_SYSTEM ? NO_ARG : pend_arg);
		return;
	}

	log_msg(LOG_WARNING, "no answer to %02x %02x, going on", pend_cmd,
		pend_arg);
	pend = 0;
	retries = 0;
	fake = pend_arg;
	handshake(CMD_REQUEST(pend_cmd), &fake, 1);
}

void proto_rx(const struct link_frame *f)
{
	const uint8_t *d = f->data;
	int relock;

	if (!f->csum_ok) {
		log_msg(LOG_WARNING, "checksum error in 0x%02x", f->cmd);
		return;
	}

	switch (f->cmd) {
	case MCU_INPUT:
		if (f->len >= 3) {
			display_input(d[2]);
		}

		/* follow the input's sample rate in the stream status */
		if (f->len >= 4 && d[3]) {
			memset(stream + STREAM_FS, d[3], 3);
		}

		/*
		 * After a switch, or when the source comes back on the same
		 * input, the MCU unmutes a digital input only on a 0x42. It
		 * restarts its input handling on one: not again for a while.
		 */
		relock = f->len >= 6 && d[5] && !input_status;
		if (f->len >= 6) {
			input_status = d[5];
		}

		if ((select_timer.pending || (relock && !relock_timer.pending)) &&
		    stage == STAGE_READY) {
			uloop_timeout_cancel(&select_timer);
			uloop_timeout_set(&relock_timer, RELOCK_MS);
			memset(stream_sent, 0, STREAM_LEN);
		}

		stream_update();
		break;
	case MCU_INPUT_SELECT:
		uloop_timeout_set(&select_timer, SELECT_MS);
		/*
		 * A switch to a network source (NET, USB, ...): AudioApp sends a
		 * bare 0x40, without it the MCU keeps restarting its input
		 * sequence and the outputs stay muted.
		 */
		if (f->len >= 1 && d[0] != select_was && stage == STAGE_READY &&
		    (d[0] == 1 || d[0] == 2 || d[0] == 3 || d[0] == 5 || d[0] == 6)) {
			queue(DSP_READY, NULL, 0);
		}
		if (f->len >= 1) {
			select_was = d[0];
		}
		break;
	case MCU_SPEAKERS:
		queue(DSP_LEVELS, levels, LEVELS_LEN);
		break;
	case MCU_FL_REQUEST:
		if (f->len >= 1 && d[0] == 0) {
			display_request();
		}
		break;
	case MCU_NET_VERSION:
		queue(NET_VERSION, version, VERSION_LEN - 1);
		/* asked when the MCU (re)starts, e.g. out of standby */
		if (stage == STAGE_READY) {
			log_msg(LOG_INFO, "MCU started again, starting the link over");
			start();
		}

		break;
	case MCU_POWER:
		if (!f->len) {
			break;
		}

		if (d[0] == POWER_ON) {
			queue(NET_POWER, d, 1);
			break;
		}

		/* the MCU cuts the power after the answer: the policy goes first */
		log_msg(LOG_INFO, "MCU powers the board off (%u)", d[0]);
		power_pending = d[0];
		uloop_timeout_set(&power_timer, POWER_WAIT_MS);
		break;
	case MCU_NETWORK:
		if (f->len && d[0] < 32) {
			network_asked |= 1u << d[0];
		}

		break;
	case MCU_UPDATE:
		if (!f->len) {
			break;
		}

		update_stage = d[0];
		if (d[0] == UPDATE_VERSIONS) {
			update_waiting = 1;
		}

		if (d[0] == UPDATE_UPLOAD) {
			log_msg(LOG_WARNING, "MCU asks to upload its own firmware, not answered");
		}

		break;
	case MCU_SYSTEM:
		model_from_system(d, f->len);
		break;
	case MCU_ITEM:
		if (f->len > 1 && d[0] == ITEM_BEACON) {
			model_from_beacon(d + 1, f->len - 1);
		}

		break;
	default:
		break;
	}

	if (f->cmd == MCU_SYSTEM || f->cmd == MCU_ITEM || f->cmd == MCU_DISPLAY) {
		handshake(f->cmd, d, f->len);
	}

	ops->frame(f, proto_cmd_name(f->cmd));
}

static void pad_copy(uint8_t *dst, size_t size, const char *src)
{
	size_t n = strlen(src);

	memset(dst, ' ', size);
	memcpy(dst, src, n < size ? n : size);
}

static void start(void)
{
	static const uint8_t state_start[2] = { NET_STATE_START };

	/* a question in the middle of the handshake would be dropped */
	display_reset();
	uloop_timeout_cancel(&lmd_timer);
	uloop_timeout_cancel(&select_timer);
	uloop_timeout_cancel(&relock_timer);

	/* the AudioApp part, in the stock order */
	queue(DSP_VERSION, version, VERSION_LEN);
	queue(DSP_READY, NULL, 0);
	memcpy(stream_sent, stream, STREAM_LEN);
	queue(DSP_STREAM, stream, STREAM_LEN);
	queue(DSP_ID, dsp_id, ID_LEN);
	queue(DSP_LEVELS, levels, LEVELS_LEN);
	/* the network module part */
	queue(NET_STATE, state_start, 2);
	request(NET_SYSTEM, NO_ARG);
	stage = STAGE_SYSTEM;
}

/* each address as a 32-bit little endian value: 172.16.10.88 -> 58 0a 10 ac */
static void put_addresses(uint8_t *d, const struct mcu_network *n)
{
	const uint8_t *a[] = { n->ip, n->netmask, n->gateway, n->dns[0], n->dns[1] };
	size_t i, j;

	for (i = 0; i < ARRAY_SIZE(a); i++) {
		for (j = 0; j < 4; j++) {
			d[i * 4 + j] = a[i][3 - j];
		}
	}
}

/* the layouts of the stock network module */
static void network_answer(uint8_t item)
{
	const struct mcu_network *n = &network;
	uint8_t d[1 + sizeof(n->name)] = { 0 };
	int has_ip = n->ip[0] || n->ip[1] || n->ip[2] || n->ip[3];
	size_t len;

	switch (item) {
	case NETWORK_STATUS:
		/* the menu shows NETWORK_IP; this layout is not checked on a display */
		d[2] = n->link && has_ip ? 1 : 3;
		/* [3] 0: wired; [4] 0 static, 1 DHCP, 2 no lease (link-local) */
		if (n->dhcp) {
			d[4] = n->ip[0] == 169 && n->ip[1] == 254 ? 2 : 1;
		}

		put_addresses(d + 5, n);
		memcpy(d + 25, n->mac, 6);
		len = 32;
		break;
	case NETWORK_IP:
		d[1] = 1;
		d[3] = n->dhcp;
		put_addresses(d + 4, n);
		memcpy(d + 24, n->mac, 6);
		len = 30;
		break;
	case NETWORK_NAME:
		d[0] = NETWORK_NAME;
		len = strlen(n->name);
		memcpy(d + 1, n->name, len);
		len++;
		break;
	default:
		return;
	}

	log_msg(LOG_INFO, "network item %u answered", item);
	queue(NET_NETWORK, d, len);
}

static void power_answer(void)
{
	uint8_t s = power_pending;

	uloop_timeout_cancel(&power_timer);
	power_pending = -1;
	queue(NET_POWER, &s, 1);
}

static void power_timeout(struct uloop_timeout *t)
{
	log_msg(LOG_WARNING, "power %d: no answer from the policy", power_pending);
	sync();
	power_answer();
}

int proto_power_done(uint8_t state)
{
	if (power_pending != state) {
		return -1;
	}

	power_answer();
	return 0;
}

void proto_set_network(const struct mcu_network *n)
{
	uint8_t i;

	network = *n;
	network.name[sizeof(network.name) - 1] = 0;
	for (i = 0; i < 32; i++) {
		if (network_asked & (1u << i)) {
			network_answer(i);
		}
	}

	network_asked = 0;
}

/* the ARM part: the NET part ends in "Error 9-1", it checks for a BridgeCo module */
void proto_update_check(int available)
{
	uint8_t d[2 + UPDATE_PARTS] = { UPDATE_CHECK };

	d[2 + UPDATE_PART_ARM] = !!available;
	update_waiting = 0;
	queue(NET_UPDATE, d, sizeof(d));
}

int proto_update_waiting(void)
{
	return update_waiting;
}

void proto_update_downloaded(uint32_t size)
{
	uint8_t d[6] = { UPDATE_DOWNLOADED, 0, size, size >> 8, size >> 16, size >> 24 };

	queue(NET_UPDATE, d, sizeof(d));
}

void proto_update_progress(uint8_t percent)
{
	uint8_t d[2] = { UPDATE_PROGRESS, percent > 100 ? 100 : percent };

	queue(NET_UPDATE, d, sizeof(d));
}

/* a failed download is told in the answer the MCU waits for there */
void proto_update_result(int ok)
{
	uint8_t d[6] = { UPDATE_RESULT, ok ? 0 : UPDATE_ERROR };

	if (update_stage == UPDATE_MODE) {
		d[0] = UPDATE_DOWNLOADED;
		d[1] = ok ? 0 : UPDATE_DL_ERROR;
		queue(NET_UPDATE, d, 6);
		return;
	}

	queue(NET_UPDATE, d, 2);
}

/* network start, then ready: the MCU looks for an update on the network again */
int proto_update_ask(void)
{
	static const uint8_t start[2] = { NET_STATE_START }, ready[2] = { NET_STATE_READY };

	if (stage != STAGE_READY) {
		return -1;
	}

	queue(NET_STATE, start, 2);
	queue(NET_STATE, ready, 2);
	return 0;
}

/* storage on a USB port makes the MCU look for an update there */
void proto_set_usb(uint8_t port, int present)
{
	uint8_t d[4] = { NET_STATE_USB, 0, port, !!present };

	queue(NET_STATE, d, sizeof(d));
}

static void lmd_ask(struct uloop_timeout *t)
{
	proto_ask_listening_mode();
}

void proto_init(const struct proto_ops *o, const char *dsp_version)
{
	ops = o;
	retry_timer.cb = retry_cb;
	power_timer.cb = power_timeout;
	lmd_timer.cb = lmd_ask;
	pad_copy(version, VERSION_LEN, dsp_version);
	start();
}

enum stage proto_stage(void)
{
	return stage;
}

const char *proto_stage_name(void)
{
	static const char *const n[] = {
		[STAGE_START] = "start", [STAGE_SYSTEM] = "system",
		[STAGE_ITEMS] = "items", [STAGE_DISPLAY] = "display",
		[STAGE_FINAL] = "final", [STAGE_READY] = "ready",
	};

	return n[stage];
}

const struct mcu_model *proto_model(void)
{
	return &model;
}

void proto_set_version(const char *v, const char *id)
{
	if (v) {
		pad_copy(version, VERSION_LEN, v);
		queue(DSP_VERSION, version, VERSION_LEN);
	}

	if (id) {
		pad_copy(dsp_id, ID_LEN, id);
		queue(DSP_ID, dsp_id, ID_LEN);
	}
}

void proto_set_stream(const uint8_t *s)
{
	memcpy(stream, s, STREAM_LEN);
	stream_update();
}

void proto_get_stream(uint8_t *s)
{
	memcpy(s, stream, STREAM_LEN);
}

void proto_set_levels(const uint8_t *l)
{
	if (!memcmp(levels, l, LEVELS_LEN)) {
		return;
	}

	memcpy(levels, l, LEVELS_LEN);
	queue(DSP_LEVELS, levels, LEVELS_LEN);
}

void proto_set_outputs(uint8_t mask)
{
	if (mask == outputs) {
		return;
	}

	outputs = mask;
	queue(DSP_OUTPUTS, &outputs, 1);
}
