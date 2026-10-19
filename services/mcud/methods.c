// SPDX-License-Identifier: BSD-2-Clause
/* The mcu object on the ubus: what the services ask of the MCU */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <arpa/inet.h>

#include "display.h"
#include "mcud.h"

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
	add_status();
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

enum { ST_HEX, ST_TYPE, ST_FS, ST_CHANNELS, ST_MASK, ST_IN_MASK, ST_LFE, ST_FORMAT, __ST_MAX };

static const struct blobmsg_policy stream_policy[__ST_MAX] = {
	[ST_HEX] = { .name = "hex", .type = BLOBMSG_TYPE_STRING },
	[ST_TYPE] = { .name = "type", .type = BLOBMSG_TYPE_INT32 },
	[ST_FS] = { .name = "fs", .type = BLOBMSG_TYPE_INT32 },
	[ST_CHANNELS] = { .name = "channels", .type = BLOBMSG_TYPE_STRING },
	[ST_MASK] = { .name = "mask", .type = BLOBMSG_TYPE_INT32 },
	[ST_IN_MASK] = { .name = "input_mask", .type = BLOBMSG_TYPE_INT32 },
	[ST_LFE] = { .name = "lfe", .type = BLOBMSG_TYPE_BOOL },
	[ST_FORMAT] = { .name = "format", .type = BLOBMSG_TYPE_INT32 },
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

	if (tb[ST_IN_MASK]) {
		v = blobmsg_get_u32(tb[ST_IN_MASK]);
		s[STREAM_IN_MASK] = v & 0xff;
		s[STREAM_IN_MASK + 1] = v >> 8;
	}

	if (tb[ST_LFE]) {
		memset(s + STREAM_LFE, blobmsg_get_bool(tb[ST_LFE]) ? 1 : 0, 2);
	}

	if (tb[ST_FORMAT]) {
		memset(s + STREAM_FORMAT, blobmsg_get_u32(tb[ST_FORMAT]), 2);
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

/* ISCP MVL: the master volume in 1 dB steps from -82 dB (00) */
#define MVL_DB_MIN	(-82)
#define MVL_MAX		0x64

enum { VOL_DB, __VOL_MAX };

static const struct blobmsg_policy volume_policy[__VOL_MAX] = {
	[VOL_DB] = { .name = "db", .type = BLOBMSG_TYPE_INT32 },
};

/* set the master volume, as the knob would */
static int m_volume(struct ubus_context *c, struct ubus_object *obj,
		    struct ubus_request_data *req, const char *method,
		    struct blob_attr *msg)
{
	struct blob_attr *tb[__VOL_MAX];
	char cmd[8];
	int mvl;

	blobmsg_parse(volume_policy, __VOL_MAX, tb, blob_data(msg), blob_len(msg));
	if (!tb[VOL_DB]) {
		return UBUS_STATUS_INVALID_ARGUMENT;
	}

	mvl = (int32_t)blobmsg_get_u32(tb[VOL_DB]) - MVL_DB_MIN;
	mvl = mvl < 0 ? 0 : mvl > MVL_MAX ? MVL_MAX : mvl;
	snprintf(cmd, sizeof(cmd), "MVL%02X", mvl);
	return proto_iscp(cmd) ? UBUS_STATUS_UNKNOWN_ERROR : 0;
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
	int i;

	blobmsg_parse(power_policy, __PW_MAX, tb, blob_data(msg), blob_len(msg));
	if (!tb[PW_STATE]) {
		return UBUS_STATUS_INVALID_ARGUMENT;
	}

	i = power_state(blobmsg_get_string(tb[PW_STATE]));
	if (i < 0) {
		return UBUS_STATUS_INVALID_ARGUMENT;
	}

	return proto_power_done(i) ? UBUS_STATUS_NOT_FOUND : 0;
}

enum { UP_AVAILABLE, UP_DOWNLOADED, UP_PROGRESS, UP_RESULT, UP_ASK, __UP_MAX };

static const struct blobmsg_policy update_policy[__UP_MAX] = {
	[UP_AVAILABLE] = { .name = "available", .type = BLOBMSG_TYPE_BOOL },
	[UP_DOWNLOADED] = { .name = "downloaded", .type = BLOBMSG_TYPE_INT32 },
	[UP_PROGRESS] = { .name = "progress", .type = BLOBMSG_TYPE_INT32 },
	[UP_RESULT] = { .name = "result", .type = BLOBMSG_TYPE_STRING },
	[UP_ASK] = { .name = "ask", .type = BLOBMSG_TYPE_BOOL },
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
	} else if (tb[UP_ASK]) {
		return proto_update_ask() ? UBUS_STATUS_NOT_FOUND : 0;
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
	UBUS_METHOD("volume", m_volume, volume_policy),
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

struct ubus_event_handler dsp_meter_handler = { .cb = on_dsp_meter };

struct ubus_object mcu_object = {
	.name = "mcu",
	.type = &mcu_type,
	.methods = mcu_methods,
	.n_methods = ARRAY_SIZE(mcu_methods),
};
