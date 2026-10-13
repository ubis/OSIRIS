/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef MCUD_PROTO_H
#define MCUD_PROTO_H

#include <stdint.h>

#include "cmd.h"
#include "link.h"

#define LEVELS_LEN	26	/* DSP_LEVELS */
#define VERSION_LEN	14	/* DSP_VERSION */
#define ID_LEN		16	/* DSP_ID */

enum stage {
	STAGE_START,		/* AudioApp part queued */
	STAGE_SYSTEM,		/* NET_SYSTEM sent, waiting for MCU_SYSTEM */
	STAGE_ITEMS,		/* NET_ITEM n, waiting for MCU_ITEM */
	STAGE_DISPLAY,		/* NET_DISPLAY n, waiting for MCU_DISPLAY */
	STAGE_FINAL,		/* second NET_SYSTEM, waiting for MCU_SYSTEM */
	STAGE_READY,		/* handshake done, relays released */
};

struct mcu_model {
	char code[24];		/* "AVR000E" */
	char name[32];		/* "TX-NR717" */
	char make[32];		/* "ONKYO", from the AMX beacon */
};

/* the network menu (MCU_NETWORK); addresses in network byte order */
struct mcu_network {
	int link;		/* cable in */
	int dhcp;
	uint8_t ip[4];
	uint8_t netmask[4];
	uint8_t gateway[4];
	uint8_t dns[2][4];
	uint8_t mac[6];
	char name[64];
};

/* what the bus side gets told */
struct proto_ops {
	void (*ready)(const struct mcu_model *m);
	void (*frame)(const struct link_frame *f, const char *name);
};

/* queue the start-up sequence; dsp_version is the first 0x41 */
void proto_init(const struct proto_ops *ops, const char *dsp_version);
void proto_rx(const struct link_frame *f);
enum stage proto_stage(void);
const char *proto_stage_name(void);
const struct mcu_model *proto_model(void);

/* values the MCU reads; each is sent when it changes */
void proto_set_version(const char *dsp_version, const char *dsp_id);
void proto_set_stream(const uint8_t *stream);
void proto_get_stream(uint8_t *stream);
void proto_set_levels(const uint8_t *levels);
void proto_set_outputs(uint8_t mask);
/* answers the MCU_NETWORK items asked for since the last call */
void proto_set_network(const struct mcu_network *n);

const char *proto_cmd_name(uint8_t cmd);

#endif
