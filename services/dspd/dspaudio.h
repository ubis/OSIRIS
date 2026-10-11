/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * The block the DSP firmware (dspaudio) and dspd share in DSP L2 RAM: the
 * DSP reports its state there, dspd sets gain and routing per output.
 * Both sides are 32-bit little endian; the ARM reaches it through /dev/mem.
 */
#ifndef DSPAUDIO_H
#define DSPAUDIO_H

#include <stdint.h>

#define DSPAUDIO_LOAD		0x11800000u	/* image start, DSP boot address */
#define DSPAUDIO_SHARED		0x1181F000u	/* this block; the image stays below */
#define DSPAUDIO_MAGIC		0x41505344u	/* "DSPA" once running */
#define DSPAUDIO_ABI		1

#define DSPAUDIO_IN		2	/* PCM9211 I2S: left, right */
#define DSPAUDIO_OUT		8	/* PCM1690: 4 serializers x 2 slots */
#define DSPAUDIO_ROUTE_OFF	0xff
#define DSPAUDIO_GAIN_ONE	(1 << 24)	/* gains are 8.24 fixed point */

/*
 * Outputs: serializer AXR1_n slot s is output 2n + s. AXR1_0 drives the
 * front pair; which speakers AXR1_1..3 feed is not known yet.
 */
#define DSPAUDIO_OUT_FL		0
#define DSPAUDIO_OUT_FR		1

enum dspaudio_state {
	DSPAUDIO_STOPPED	= 0,
	DSPAUDIO_POWER		= 1,	/* enabling McASP1 */
	DSPAUDIO_RUNNING	= 3,
	DSPAUDIO_INIT		= 10,	/* 10..15: McASP init steps */
	DSPAUDIO_STUCK		= 100,	/* + the init step that timed out */
};

struct dspaudio_shared {
	/* written by the DSP */
	uint32_t magic;
	uint32_t abi;
	char version[16];
	uint32_t state;
	uint32_t samples;		/* input samples moved */
	uint32_t underruns;		/* transmit restarts */
	uint32_t overruns;		/* receive overruns */
	uint32_t peak[DSPAUDIO_IN];	/* largest |input| since dspd cleared it */
	/* written by dspd */
	int32_t gain[DSPAUDIO_OUT];
	uint8_t route[DSPAUDIO_OUT];	/* input per output, or DSPAUDIO_ROUTE_OFF */
};

#endif
