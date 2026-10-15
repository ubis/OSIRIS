// SPDX-License-Identifier: GPL-2.0-or-later
/* liba52's CPU acceleration hooks: nothing to do on the DSP */
#include <stdint.h>

uint32_t a52_detect_accel(uint32_t accel);
void a52_cpu_state_init(uint32_t accel);

uint32_t a52_detect_accel(uint32_t accel)
{
	(void)accel;
	return 0;
}

void a52_cpu_state_init(uint32_t accel)
{
	(void)accel;
}
