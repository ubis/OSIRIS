/* SPDX-License-Identifier: BSD-2-Clause */
/* What the parts of mcud share */
#ifndef MCUD_H
#define MCUD_H

#include <stddef.h>
#include <stdint.h>

#include <libubox/blobmsg.h>
#include <libubus.h>

#include "link.h"
#include "proto.h"

extern struct ubus_context *ctx;
extern struct blob_buf b;

/* frames.c */
void on_frame(const struct link_frame *f, const char *name);
void on_ready(const struct mcu_model *m);
void on_display_mode(const char *mode);
void add_hex(const char *name, const uint8_t *d, size_t len);
void add_model(const struct mcu_model *m);
void add_status(void);
int power_state(const char *name);

/* methods.c */
extern struct ubus_object mcu_object;
extern struct ubus_event_handler dsp_meter_handler;

#endif
