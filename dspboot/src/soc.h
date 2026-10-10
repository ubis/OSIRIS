/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef DSPBOOT_SOC_H
#define DSPBOOT_SOC_H

#include "types.h"

enum bootmode {
	BM_NONE,
	BM_I2C0_MASTER,
	BM_I2C0_SLAVE,
	BM_NOR,
	BM_UHPI,
	BM_I2C1_MASTER,
	BM_I2C1_SLAVE,
	BM_SPI0_EEPROM,
	BM_SPI1_EEPROM,
	BM_SPI0_FLASH,
	BM_SPI1_FLASH,
	BM_NAND8,
	BM_NAND16,
	BM_SPI0_SLAVE,
	BM_SPI1_SLAVE,
	BM_UART2,
	BM_THB,
	BM_UART0,
	BM_UART1,
	BM_ESF,
	BM_USB11,
	BM_USB20,
	BM_MMC,
	BM_RMII,
	BM_EMU,
};

struct soc_clocks {
	u32 osc, pll, cpu, emifa, emifb, rmii; /* Hz */
	/* PLL0 in bypass: everything runs from OSCIN */
	int bypass;
};

void soc_unlock(void);
int psc_set(int psc, int module, int state); /* 0 or -ETIMEDOUT */
/* the modules TI's DEVICE_PSCInit() enables */
int psc_enable_all(void);
void soc_init_cache(void);
/* 0 or -ETIMEDOUT (PLL in bypass) */
int pll0_init(void);
void pll0_report(void);
void soc_clocks(struct soc_clocks *c);
void pinmux_set(int reg, u32 mask, u32 val);
void pinmux_uart(int n);
void pinmux_nand(void);

enum bootmode soc_bootmode(void);
const char *soc_bootmode_name(enum bootmode bm);
const char *soc_name(void);
const char *soc_revision(void);
void soc_report(void);

void cache_wb_all(void);
/* 0 or -ETIMEDOUT; does not touch the UART */
int arm_release(u32 entry);
void soc_reset(void) __attribute__((noreturn));

#endif
