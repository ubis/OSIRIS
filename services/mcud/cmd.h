/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * MCU link commands (docs/mcu-protocol.md). The DSP side sends 0x40-0x7F, the MCU
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
	NET_NETWORK		= 0x66,	/* answer to MCU_NETWORK */
	NET_UPDATE		= 0x68,	/* [0] UPDATE_CHECK..., answers MCU_UPDATE */
	NET_POWER		= 0x6a,	/* answer to MCU_POWER, echoes its byte */
	NET_ISCP		= 0x73,	/* 00 or 01, "!1...", CR */

	/* MCU -> DSP */
	MCU_INITIAL		= 0xc0,	/* [1..10] as MCU_SPEAKERS, [11] and [13..38] as MCU_DISTANCE, [0xc6] as MCU_CH_LAYOUT [1] */
	MCU_INPUT		= 0xc1,	/* [1] decode [2] interface [3] fs [4] MCLK [5] status */
	MCU_SPEAKERS		= 0xc2,	/* [1] subwoofers, [2..7] SPEAKER_* crossover codes, [8] SB channels, [9] LFE low pass, [10] double bass */
	MCU_DISTANCE		= 0xc3,	/* [0] ?, [1..26] 13 x u16 LE cm, DISTANCE_ORDER */
	MCU_EQUALIZER		= 0xc4,	/* [2] 00, then on; [96] 01, then dB per band */
	MCU_THX			= 0xc5,
	MCU_MULTIPLEX_MONO	= 0xc6,
	MCU_DD_DTS		= 0xc7,
	MCU_DSD_DIRECT		= 0xc8,
	MCU_LFE_LEVEL		= 0xc9,	/* [0] format, [1] 0 off, 1 -20 dB, 2 -10 dB, 3 0 dB */
	MCU_AV_SYNC		= 0xca,
	MCU_HDMI_AUDIO		= 0xcb,
	MCU_BASS_TREBLE		= 0xcc,	/* [0] 0 front, [1] bass, [8] treble: s8 dB */
	MCU_RE_EQ		= 0xcd,
	MCU_MUSIC_OPTIMIZER	= 0xce,
	MCU_LATE_NIGHT		= 0xcf,
	MCU_TESTTONE_CH		= 0xd0,	/* [1] channel of the calibration noise */
	MCU_TESTMODE		= 0xd1,
	MCU_LISTENING_MODE	= 0xd2,	/* [1] mode, LM_TESTTONE for calibration */
	MCU_VOLUME		= 0xd3,	/* [1..2] s16 LE, 0.01 dB */
	MCU_HEADPHONE		= 0xd4,
	MCU_CH_LAYOUT		= 0xd5,	/* [1] what the surround back amplifiers drive */
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
	MCU_NETWORK		= 0xe6,	/* [0] item, asked when the network menu opens */
	MCU_UPDATE		= 0xe8,	/* [0] UPDATE_SOURCE... */
	MCU_POWER		= 0xea,	/* POWER_* */
	MCU_UPDATE_SOURCE	= 0xf1,
	MCU_UPDATE_MODE		= 0xf2,
	MCU_ISCP		= 0xf3,	/* 01 "!1..." */
};

/* NET_STATE payloads, in the order the stock network module sends them */
#define NET_STATE_START		0x00, 0x01
#define NET_STATE_UP_0		0x05, 0x00
#define NET_STATE_UP_1		0x05, 0x01
#define NET_STATE_READY		0x00, 0x02	/* MCU closes the speaker relays */
#define NET_STATE_USB		0x03	/* [1] 0 [2] port [3] devices, 1..31 = storage */

/*
 * The firmware update dialogue (docs/mcu-protocol.md). MCU_UPDATE [0]: the
 * MCU asks whether there is an update, then to download and to write it.
 */
#define UPDATE_SOURCE		0x00	/* [1] 0 network, 1 USB */
#define UPDATE_VERSIONS		0x01	/* the MCU's parts' versions: is there one? */
#define UPDATE_MODE		0x03	/* [1] part: download it */
#define UPDATE_START		0x04	/* write it */
#define UPDATE_UPLOAD		0x05	/* send the MCU its own firmware: never */

/* NET_UPDATE [0]; a code of 0 is success */
#define UPDATE_CHECK		0x00	/* [1] code, [2..8] one byte per part: 1 newer */
#define UPDATE_DOWNLOADED	0x01	/* [1] code, [2..5] u32 LE size */
#define UPDATE_PROGRESS		0x03	/* [1] percent; the MCU gives up after ~20 s */
#define UPDATE_RESULT		0x04	/* [1] code */
#define UPDATE_PARTS		7
#define UPDATE_PART_ARM		4	/* "DSP1", shown as ARM */
#define UPDATE_DL_ERROR		0x1a	/* "DL Error" */
#define UPDATE_ERROR		0x28	/* "Update Error" */

/* MCU_NETWORK items we answer; proxy (2) is not answered by stock either */
#define NETWORK_STATUS		0x00
#define NETWORK_IP		0x01
#define NETWORK_NAME		0x05	/* friendly name */

/* MCU_POWER: the MCU cuts the board's power right after the answer to OFF */
#define POWER_ON		0x00
#define POWER_OFF		0x01
#define POWER_RESET		0x02	/* factory reset */

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
	STREAM_MASK		= 12,	/* u16 LE, speaker indicators and relays */
	STREAM_DETECT		= 14,	/* bit 0: DSP detected */
	STREAM_LAYOUT		= 15,	/* 2 bytes */
	STREAM_LEN		= 20,
};

#define FS_48K			0x09
#define STREAM_PCM		0x02

#endif
