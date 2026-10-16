// SPDX-License-Identifier: BSD-2-Clause
/*
 * dspd: runs the DSP firmware (dspaudio) and sets its gain and routing over
 * the ubus. Volume comes in the unit the MCU reports it in (0.01 dB); dspd
 * turns it into the 8.24 gains the DSP multiplies with.
 *
 *   dspd [-f image]    default /lib/firmware/dspaudio.bin
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <libubox/uloop.h>

#include "dspd.h"
#include "log.h"

#define DEFAULT_IMAGE	"/lib/firmware/dspaudio.bin"

struct ubus_context *ctx;
static struct blob_buf b;

/* uloop_run() only ends on SIGINT; stopping must mute the outputs too */
static void on_term(int sig)
{
	uloop_end();
}

int main(int argc, char **argv)
{
	struct sigaction sa = { .sa_handler = on_term };
	const char *image = DEFAULT_IMAGE;
	int opt, rc;

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
	if (dsp_open()) {
		return 1;
	}

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
	bitstream_watch();

	uloop_run();
	mute_lines(1);
	dsp_stop();
	ubus_free(ctx);
	uloop_done();
	return 0;
}
