// SPDX-License-Identifier: BSD-2-Clause
/*
 * mcud: the link to the receiver's main MCU on the ubus. It answers what the
 * MCU expects at once and turns everything else into events; deciding what
 * to do about them is up to the services listening (ubus listen mcu.*).
 *
 *   mcud [-s spi_hz] [-v]
 *     -v  log every frame with a timestamp on stderr
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libubox/uloop.h>

#include "display.h"
#include "log.h"
#include "mcud.h"

struct ubus_context *ctx;
struct blob_buf b;

/* "OSIRIS <build>": what the receiver shows as the DSP version */
static void default_version(char *v, size_t size)
{
	char line[64], *p;
	const char *build = "";
	FILE *f = fopen("/etc/os-release", "r");

	while (f && fgets(line, sizeof(line), f)) {
		if (!strncmp(line, "VERSION=", 8)) {
			p = line + 8 + (line[8] == '"');
			p[strcspn(p, "\"\n")] = 0;
			build = p;
			break;
		}
	}

	if (f) {
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
