/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * MCU link commands (PROTOCOL.md). The DSP side sends 0x40-0x7F, the MCU
 * 0xC0-0xFF; an answer to a request is the request's number minus 0x80.
 */
#ifndef MCUD_CMD_H
#define MCUD_CMD_H

#define CMD_ANSWER(req)		((req) - 0x80)
#define CMD_REQUEST(answer)	((answer) + 0x80)

enum mcu_cmd {
	/* DSP -> MCU, AudioApp part */
	DSP_READY		= 0x40,
	DSP_VERSION		= 0x41,	/* 14 ASCII, shown as the DSP version */
	DSP_STREAM		= 0x42,	/* 20 bytes, see enum stream_field */
	DSP_LEVELS		= 0x45,	/* 13 x u16 LE */
	DSP_ID			= 0x46,	/* 16 ASCII */
	DSP_OUTPUTS		= 0x4a,	/* active outputs, NET_OUTPUTS_* */

	/* DSP -> MCU, network module part */
	NET_VERSION		= 0x60,	/* answer to MCU_NET_VERSION */
	NET_SYSTEM		= 0x62,	/* ask for MCU_SYSTEM */
	NET_ITEM		= 0x63,	/* ask for MCU_ITEM n */
	NET_DISPLAY		= 0x64,	/* ask for MCU_DISPLAY 0 (text) / 1 (OSD) */
	NET_STATE		= 0x65,	/* two bytes, NET_STATE_* */
	NET_ACK			= 0x6a,	/* answer to MCU_ACK_REQ, echoes its byte */
	NET_ISCP		= 0x73,	/* 01 "!1..." */

	/* MCU -> DSP */
	MCU_INITIAL		= 0xc0,
	MCU_INPUT		= 0xc1,	/* [1] decode [2] interface [3] fs [4] MCLK [5] status */
	MCU_SPEAKERS		= 0xc2,
	MCU_DISTANCE		= 0xc3,
	MCU_EQUALIZER		= 0xc4,
	MCU_THX			= 0xc5,
	MCU_MULTIPLEX_MONO	= 0xc6,
	MCU_DD_DTS		= 0xc7,
	MCU_DSD_DIRECT		= 0xc8,
	MCU_LFE_LEVEL		= 0xc9,
	MCU_AV_SYNC		= 0xca,
	MCU_HDMI_AUDIO		= 0xcb,
	MCU_BASS_TREBLE		= 0xcc,
	MCU_RE_EQ		= 0xcd,
	MCU_MUSIC_OPTIMIZER	= 0xce,
	MCU_LATE_NIGHT		= 0xcf,
	MCU_TESTTONE_CH		= 0xd0,
	MCU_TESTMODE		= 0xd1,
	MCU_LISTENING_MODE	= 0xd2,	/* [1] mode, LM_TESTTONE for calibration */
	MCU_VOLUME		= 0xd3,	/* [1..2] s16 LE, 0.01 dB */
	MCU_HEADPHONE		= 0xd4,
	MCU_CH_LAYOUT		= 0xd5,
	MCU_SPECIAL_MODE	= 0xd6,
	MCU_NET_MUTE		= 0xdb,
	MCU_NETAUDIO_INFO	= 0xdc,
	MCU_SPDIF_UPDATE	= 0xdd,
	MCU_AUDYSSEY		= 0xde,
	MCU_NET_VERSION		= 0xe0,
	MCU_SYSTEM		= 0xe2,	/* model code and name */
	MCU_ITEM		= 0xe3,	/* [0] item; ITEM_BEACON is the AMX beacon */
	MCU_DISPLAY		= 0xe4,
	MCU_INPUT_SELECT	= 0xe5,
	MCU_UPDATE		= 0xe8,
	MCU_ACK_REQ		= 0xea,
	MCU_UPDATE_SOURCE	= 0xf1,
	MCU_UPDATE_MODE		= 0xf2,
	MCU_ISCP		= 0xf3,	/* 01 "!1..." */
};

/* NET_STATE payloads, in the order the stock network module sends them */
#define NET_STATE_START		0x00, 0x01
#define NET_STATE_UP_0		0x05, 0x00
#define NET_STATE_UP_1		0x05, 0x01
#define NET_STATE_READY		0x00, 0x02	/* MCU closes the speaker relays */

#define NET_OUTPUTS_FRONT	0x01

#define ITEM_LAST		7	/* this MCU answers items 0..7 */
#define ITEM_BEACON		4

#define LM_TESTTONE		0xf1

/* DSP_STREAM payload */
enum stream_field {
	STREAM_TYPE		= 0,	/* 0 unlocked, 1 analog, 2 PCM, ... */
	STREAM_FS		= 1,	/* 3 bytes, sample rate codes */
	STREAM_FLAGS		= 6,	/* 2 bytes */
	STREAM_CHANNELS		= 8,	/* 4 ASCII, front panel */
	STREAM_MASK		= 12,	/* u16 LE, outputs with relays closed */
	STREAM_DETECT		= 14,	/* bit 0: DSP detected */
	STREAM_LAYOUT		= 15,	/* 2 bytes */
	STREAM_LEN		= 20,
};

#define FS_48K			0x09
#define STREAM_PCM		0x02

#endif
