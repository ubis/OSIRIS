// SPDX-License-Identifier: BSD-2-Clause
/*
 * The front panel on the NET input: the MCU shows what the network side
 * sends in NET_FL frames (two lines of 13 characters, Latin-1, a play state
 * icon) and asks for the whole screen again with MCU_FL_REQUEST, once for
 * every press of DISPLAY. Each press after the first redraw steps through
 * the modes: the service and the album, title and artist, a level meter.
 */
#include <string.h>
#include <time.h>

#include <libubox/uloop.h>
#include <libubox/utils.h>

#include "cmd.h"
#include "display.h"
#include "link.h"
#include "proto.h"

#define COLS		13
#define TEXT_MAX	128
#define NET_INTERFACE	5	/* MCU_INPUT [2] */
#define SETTLE_MS	3000	/* requests this soon after NET comes up only redraw */
#define SCROLL_MS	400
#define SCROLL_HOLD	5	/* steps at either end of a long text */
#define METER_MS	80	/* at most this often (dspd sends every 100 ms): each frame is a full SPI transfer */
#define METER_FALL	4	/* half cells per update */
#define METER_FLOOR	-42	/* dBFS at the left end ... */
#define METER_TOP	-6	/* ... and the right: AirPlay's volume cuts before us */
#define PEAK_LIFT	10	/* dB under the peak the bar reaches at least */
#define PEAK_HOLD_MS	1200
#define PEAK_FALL_MS	80	/* per half cell */

/* NET_FL header, then text blocks [n + 6] [attr] [type] [col] [width] [n] text */
enum {
	FL_SCREEN, FL_KIND, FL_MASK, FL_STATUS = 4, FL_ELAPSED = 5, FL_TOTAL = 7,
	FL_TRACK = 9, FL_BLOCKS = 19, FL_HEADER,
};

#define FL_SCREEN_PLAY	5
#define FL_KIND_MAIN	0x11	/* kind 1, main zone */
#define FL_MASK_STATUS	(1u << 0)
#define FL_MASK_BLOCK(n)	(0x80u << (n))
#define FL_MASK_ALL	(1u << 15)
#define FL_PLAIN	0	/* text block type without an icon in front */

/* play state codes, as stock netapp maps its states */
#define FL_STOPPED	0x02
#define FL_PLAYING	0x04
#define FL_PAUSED	0x05

#define CH_BLOCK	0x7f	/* all dots */
#define CH_HALF		0x7c	/* '|' */

enum mode { MODE_NAME, MODE_TEXT, MODE_METER, __MODE_MAX };

static const char *const mode_name[__MODE_MAX] = {
	[MODE_NAME] = "name",
	[MODE_TEXT] = "text",
	[MODE_METER] = "meter",
};

static enum mode mode;
static void (*mode_changed)(const char *mode);
static enum display_state state;
static char title[TEXT_MAX], artist[TEXT_MAX], album[TEXT_MAX];	/* Latin-1 */
static int on;					/* the input is NET */
static long on_since;				/* ms */
static unsigned int scroll;

static void scroll_tick(struct uloop_timeout *t);
static void draw_later(struct uloop_timeout *t);
static struct uloop_timeout scroll_timer = { .cb = scroll_tick };
static struct uloop_timeout draw_timer = { .cb = draw_later };

static int level[2], peak[2];			/* half cells, 0..2 * COLS */
static long peak_at[2], peak_fell[2], meter_at;

static long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000;
}

/* UTF-8 to what the display shows: Latin-1 letters, '?' for the rest */
static void to_latin1(char *dst, const char *src)
{
	const unsigned char *s = (const unsigned char *)src;
	size_t n = 0;

	while (*s && n < TEXT_MAX - 1) {
		unsigned int c = *s++;

		if (c >= 0xc0 && c < 0xe0 && (*s & 0xc0) == 0x80) {
			c = (c & 0x1f) << 6 | (*s++ & 0x3f);
		} else if (c >= 0x80) {
			while ((*s & 0xc0) == 0x80) {
				s++;
			}
			c = '?';
		}

		dst[n++] = (c >= 0x20 && c < 0x7f) || (c >= 0xa1 && c <= 0xff) ? (char)c : '?';
	}

	dst[n] = 0;
}

static size_t block(uint8_t *p, uint8_t type, const uint8_t *text)
{
	p[0] = COLS + 6;
	p[1] = 0;
	p[2] = type;
	p[3] = 0;
	p[4] = 0;
	p[5] = COLS;
	memcpy(p + 6, text, COLS);
	return COLS + 6;
}

/* COLS characters of s from 'from', space padded */
static void window(uint8_t *out, const char *s, size_t from)
{
	size_t len = strlen(s), i;

	for (i = 0; i < COLS; i++) {
		out[i] = from + i < len ? (uint8_t)s[from + i] : ' ';
	}
}

/* a long text moves one step per tick, holding at either end */
static size_t scroll_from(const char *s)
{
	size_t len = strlen(s), span;
	unsigned int pos;

	if (len <= COLS) {
		return 0;
	}

	span = len - COLS;
	pos = scroll % (unsigned int)(span + 2 * SCROLL_HOLD);
	return pos < SCROLL_HOLD ? 0 : pos - SCROLL_HOLD > span ? span : pos - SCROLL_HOLD;
}

static int has_text(void)
{
	return title[0] || artist[0];
}

/* what the mode shows now: the meter only while playing, text only with some */
static enum mode shown(void)
{
	enum mode m = mode;

	if (state == DISPLAY_STOPPED) {
		return MODE_NAME;
	}

	if (m == MODE_METER && state != DISPLAY_PLAYING) {
		m = MODE_TEXT;
	}

	return m == MODE_TEXT && !has_text() ? MODE_NAME : m;
}

/* under "AirPlay": the album, else the play state */
static const char *second_line(void)
{
	if (album[0] || state == DISPLAY_STOPPED) {
		return album;
	}

	return state == DISPLAY_PAUSED ? "Paused" : "Playing";
}

static void bar(uint8_t *out, int lvl, int pk)
{
	int i;

	for (i = 0; i < COLS; i++) {
		out[i] = lvl >= 2 * (i + 1) ? CH_BLOCK : lvl == 2 * i + 1 ? CH_HALF : ' ';
	}

	if (pk > lvl + 1) {
		out[pk > 1 ? (pk - 1) / 2 : 0] = CH_HALF;
	}
}

static void send_screen(uint16_t mask)
{
	uint8_t f[FL_HEADER + 2 * (COLS + 6)], top[COLS], bottom[COLS];
	size_t len = FL_HEADER;

	if (!on || proto_stage() != STAGE_READY) {
		return;
	}

	if (shown() == MODE_NAME) {
		window(top, "AirPlay", 0);
		window(bottom, second_line(), scroll_from(second_line()));
	} else if (shown() == MODE_METER) {
		bar(top, level[0], peak[0]);
		bar(bottom, level[1], peak[1]);
	} else {
		window(top, title, scroll_from(title));
		window(bottom, artist, scroll_from(artist));
	}

	memset(f, 0, FL_HEADER);
	f[FL_SCREEN] = FL_SCREEN_PLAY;
	f[FL_KIND] = FL_KIND_MAIN;
	mask |= FL_MASK_STATUS | FL_MASK_BLOCK(0) | FL_MASK_BLOCK(1);
	f[FL_MASK] = mask & 0xff;
	f[FL_MASK + 1] = mask >> 8;
	f[FL_STATUS] = state == DISPLAY_PLAYING ? FL_PLAYING :
		       state == DISPLAY_PAUSED ? FL_PAUSED : FL_STOPPED;
	f[FL_BLOCKS] = 2;
	len += block(f + len, FL_PLAIN, top);
	len += block(f + len, FL_PLAIN, bottom);
	link_send(NET_FL, f, len);
}

static int scrolling(void)
{
	if (!on) {
		return 0;
	}

	if (shown() == MODE_NAME) {
		return strlen(second_line()) > COLS;
	}

	return shown() == MODE_TEXT && (strlen(title) > COLS || strlen(artist) > COLS);
}

static void scroll_tick(struct uloop_timeout *t)
{
	scroll++;
	send_screen(0);
	if (scrolling()) {
		uloop_timeout_set(t, SCROLL_MS);
	}
}

static void redraw(void)
{
	scroll = 0;
	send_screen(FL_MASK_ALL);
	uloop_timeout_cancel(&scroll_timer);
	if (scrolling()) {
		uloop_timeout_set(&scroll_timer, SCROLL_MS);
	}
}

static void draw_later(struct uloop_timeout *t)
{
	/* the MCU drops a screen sent during the handshake */
	if (proto_stage() != STAGE_READY) {
		uloop_timeout_set(t, 500);
		return;
	}

	redraw();
}

void display_set(const char *t, const char *a, const char *al, enum display_state s)
{
	char nt[TEXT_MAX], na[TEXT_MAX], ns[TEXT_MAX];

	to_latin1(nt, t ? t : "");
	to_latin1(na, a ? a : "");
	to_latin1(ns, al ? al : "");
	if (s == state && !strcmp(nt, title) && !strcmp(na, artist) && !strcmp(ns, album)) {
		return;
	}

	strcpy(title, nt);
	strcpy(artist, na);
	strcpy(album, ns);
	state = s;
	level[0] = level[1] = peak[0] = peak[1] = 0;
	redraw();
}

static int half_cells(int db)
{
	int h = (db - METER_FLOOR) * 2 * COLS / (METER_TOP - METER_FLOOR);

	return h < 0 ? 0 : h > 2 * COLS ? 2 * COLS : h;
}

void display_meter(int left, int right, int left_peak, int right_peak)
{
	int db[2] = { left, right }, pk[2] = { left_peak, right_peak }, c, changed = 0;
	long now = now_ms();

	for (c = 0; c < 2; c++) {
		/* a transient lifts the bar at once: peaks sit 10-14 dB over the RMS */
		int h = half_cells(db[c] > pk[c] - PEAK_LIFT ? db[c] : pk[c] - PEAK_LIFT);
		int p = half_cells(pk[c]);

		/* up at once, down a cell per update: no flicker */
		h = h >= level[c] - METER_FALL ? h : level[c] - METER_FALL;
		changed |= h != level[c];
		level[c] = h;
		if (p >= peak[c]) {
			changed |= p != peak[c];
			peak[c] = p;
			peak_at[c] = peak_fell[c] = now;
		} else if (now - peak_at[c] > PEAK_HOLD_MS && now - peak_fell[c] >= PEAK_FALL_MS) {
			peak[c]--;
			peak_fell[c] = now;
			changed = 1;
		}
	}

	if (changed && shown() == MODE_METER &&
	    now - meter_at >= METER_MS) {
		meter_at = now;
		send_screen(0);
	}
}

void display_input(int interface)
{
	int was = on;

	on = interface == NET_INTERFACE;
	if (on && !was) {
		on_since = now_ms();
		/* after the MCU's own redraw request, if it sends one */
		uloop_timeout_set(&draw_timer, 300);
	} else if (!on) {
		uloop_timeout_cancel(&draw_timer);
		uloop_timeout_cancel(&scroll_timer);
	}
}

void display_request(void)
{
	/* the MCU asks the network side for its screen on NET only */
	if (!on) {
		on = 1;
		on_since = now_ms();
	}

	/* the handshake drops screens: draw once it is done */
	if (proto_stage() != STAGE_READY) {
		uloop_timeout_set(&draw_timer, 500);
		return;
	}

	/* DISPLAY: the next mode; the first requests after NET comes up only redraw */
	if (now_ms() - on_since > SETTLE_MS) {
		/* without a title (a video, say) there is no text screen */
		do {
			mode = (mode + 1) % __MODE_MAX;
		} while (mode == MODE_TEXT && !has_text());
		if (mode_changed) {
			mode_changed(mode_name[mode]);
		}
	}

	uloop_timeout_cancel(&draw_timer);
	redraw();
}

void display_reset(void)
{
	on = 0;
	uloop_timeout_cancel(&draw_timer);
	uloop_timeout_cancel(&scroll_timer);
}

int display_set_mode(const char *name)
{
	int m;

	for (m = 0; m < __MODE_MAX; m++) {
		if (!strcmp(name, mode_name[m])) {
			if ((enum mode)m != mode) {
				mode = m;
				redraw();
			}
			return 0;
		}
	}

	return -1;
}

const char *display_mode_name(void)
{
	return mode_name[mode];
}

void display_on_mode(void (*fn)(const char *mode))
{
	mode_changed = fn;
}
