/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef MCUD_DISPLAY_H
#define MCUD_DISPLAY_H

enum display_state { DISPLAY_STOPPED, DISPLAY_PLAYING, DISPLAY_PAUSED };

/* what plays on the NET input, in UTF-8 */
void display_set(const char *title, const char *artist, const char *album,
		 enum display_state state);
/* the levels the DSP plays, RMS and peak, dBFS */
void display_meter(int left, int right, int left_peak, int right_peak);

/* from the MCU: the input's C1 interface, and its requests for the screen */
void display_input(int interface);
void display_request(void);
/* the link starts over: NET is known again from the next C1 or F1 */
void display_reset(void);

/* "name", "text" or "meter"; -1 for anything else */
int display_set_mode(const char *name);
const char *display_mode_name(void);
/* called when DISPLAY changes the mode */
void display_on_mode(void (*fn)(const char *mode));

#endif
