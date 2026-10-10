/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef DSPBOOT_SHELL_H
#define DSPBOOT_SHELL_H

/*
 * The "dspboot>" prompt; never returns (commands may start the ARM or reset).
 */
void shell_run(void) __attribute__((noreturn));

#endif
