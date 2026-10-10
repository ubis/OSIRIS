/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef DSPBOOT_TYPES_H
#define DSPBOOT_TYPES_H

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef signed int s32;

#ifndef NULL
#define NULL	((void *)0)
#endif

#define ARRAY_SIZE(a)	(sizeof(a) / sizeof((a)[0]))

#endif
