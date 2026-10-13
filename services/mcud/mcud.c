// SPDX-License-Identifier: BSD-2-Clause
/*
 * mcud: the link to the receiver's main MCU on the ubus. It answers what the
 * MCU expects at once and turns everything else into events; deciding what
 * to do about them is up to the services listening (ubus listen mcu.*).
 *
 *   mcud [-s spi_hz] [-v]
 *     -v  log every frame with a timestamp on stderr
 */
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
#include "link.h"
#include "proto.h"

static struct ubus_context *ctx;
static struct blob_buf b;
static int listening_mode = -1;		/* the last one the MCU sent */

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

static void on_frame(const struct link_frame *f, const char *name)
{
	const uint8_t *d = f->data;

	blob_buf_init(&b, 0);
	switch (f->cmd) {
	case MCU_INPUT:
		if (f->len >= 6) {
			blobmsg_add_u32(&b, "decode", d[1]);
			blobmsg_add_u32(&b, "interface", d[2]);
			blobmsg_add_u32(&b, "fs", d[3]);
			blobmsg_add_u32(&b, "mclk", d[4]);
			blobmsg_add_u32(&b, "status", d[5]);
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
			blobmsg_add_u32(&b, "volume",
					(uint32_t)(int16_t)(d[1] | (d[2] << 8)));
		}

		event("mcu.volume");
		return;
	case MCU_SPEAKERS:
	case MCU_DISTANCE:
	case MCU_LFE_LEVEL:
		blobmsg_add_string(&b, "name", name);
		add_hex("data", d, f->len);
		event("mcu.speakers");
		return;
	case MCU_LISTENING_MODE:
		if (f->len >= 2) {
			listening_mode = d[1];
			blobmsg_add_u32(&b, "mode", d[1]);
		}

		add_hex("data", d, f->len);
		event("mcu.listening_mode");
		return;
	case MCU_TESTTONE_CH:
	case MCU_TESTMODE:
		blobmsg_add_string(&b, "name", name);
		if (f->len >= 2) {
			blobmsg_add_u32(&b, "channel", d[1]);
		}

		add_hex("data", d, f->len);
		event("mcu.testtone");
		return;
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
	case MCU_ISCP:
		if (f->len > 1) {
			char *s = blobmsg_alloc_string_buffer(&b, "command", f->len);

			memcpy(s, d + 1, f->len - 1);
			s[f->len - 1] = 0;
			blobmsg_add_string_buffer(&b);
		}

		event("mcu.iscp");
		return;
	default:
		blobmsg_add_u32(&b, "cmd", f->cmd);
		blobmsg_add_string(&b, "name", name);
		add_hex("data", d, f->len);
		event("mcu.frame");
		return;
	}
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
	if (listening_mode >= 0) {
		blobmsg_add_u32(&b, "listening_mode", listening_mode);
	}

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
	UBUS_METHOD("network", m_network, network_policy),
	UBUS_METHOD("power", m_power, power_policy),
	UBUS_METHOD("send", m_send, send_policy),
};

static struct ubus_object_type mcu_type = UBUS_OBJECT_TYPE("mcu", mcu_methods);

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

	rc = link_open(spi_hz, proto_rx);
	if (rc) {
		log_msg(LOG_ERR, "cannot open the MCU link: %s", strerror(-rc));
		return 1;
	}

	default_version(version, sizeof(version));
	proto_init(&ops, version);
	uloop_run();
	link_close();
	ubus_free(ctx);
	uloop_done();
	return 0;
}
