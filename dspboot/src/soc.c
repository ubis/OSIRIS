/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * dspboot - SoC bring-up for the OMAP-L137 / DA830 from the C674x DSP:
 * SYSCFG unlock, PSC, PLL0, pinmux, boot mode, ARM release, reset.
 *
 * The PLL0 programming order, the PSC transition sequence and the ARM release
 * (PRU copies a reset vector to ARM local RAM, then HOST0CFG + PSC) are derived
 * from TI's OMAP-L137 FlashAndBootUtils 2.40 (device.c / sft.c, BSD-3-Clause,
 * Copyright (C) 2012 Texas Instruments Incorporated). This is a rewrite, not
 * a copy.
 */
#include "board.h"
#include "errcodes.h"
#include "hw.h"
#include "lib.h"
#include "soc.h"
#include "timer.h"

void soc_unlock(void)
{
	SYSCFG_KICK0R = KICK0_UNLOCK;
	SYSCFG_KICK1R = KICK1_UNLOCK;
}

/* PSC */

static int wait_ptstat(u32 base)
{
	u32 n = 1000000;

	while (PSC_PTSTAT(base) & 1) {
		if (!--n) {
			return -ETIMEDOUT;
		}
	}

	return 0;
}

int psc_set(int psc, int module, int state)
{
	u32 base = psc ? PSC1_BASE : PSC0_BASE;
	u32 n = 1000000;

	if (wait_ptstat(base)) {
		return -ETIMEDOUT;
	}

	if ((PSC_MDSTAT(base, module) & 0x1F) == (u32)state) {
		return 0;
	}

	PSC_MDCTL(base, module) = (PSC_MDCTL(base, module) & ~0x1Fu) | state;
	/* power domain 0 (always on) */
	PSC_PTCMD(base) = 1;
	if (wait_ptstat(base)) {
		return -ETIMEDOUT;
	}
	while ((PSC_MDSTAT(base, module) & 0x1F) != (u32)state) {
		if (!--n) {
			return -ETIMEDOUT;
		}
	}

	return 0;
}

/* PSC modules enabled at start-up, the same set as TI's DEVICE_PSCInit() */
int psc_enable_all(void)
{
	static const u8 psc0_mods[] = { 0, 1, 2,  3,  4,  5, 6,
					8, 9, 10, 11, 12, 13 };
	static const u8 psc1_mods[] = { 1,  2,	3,  4,	5,  6,	7,
					8,  9,	10, 11, 12, 13, 16,
					17, 20, 21, 24, 25, 26, 31 };
	u32 i, n;
	int rc = 0;

	for (n = 1000000; (PSC_PTSTAT(PSC0_BASE) & 1) && n; n--)
		;
	for (i = 0; i < sizeof(psc0_mods); i++) {
		PSC_MDCTL(PSC0_BASE, psc0_mods[i]) =
			(PSC_MDCTL(PSC0_BASE, psc0_mods[i]) & ~0x1Fu) |
			PSC_STATE_ENABLE;
	}

	PSC_PTCMD(PSC0_BASE) = 1;
	for (n = 1000000; (PSC_PTSTAT(PSC0_BASE) & 1) && n; n--)
		;
	if (!n) {
		rc = -ETIMEDOUT;
	}

	for (n = 1000000; (PSC_PTSTAT(PSC1_BASE) & 1) && n; n--)
		;
	for (i = 0; i < sizeof(psc1_mods); i++) {
		PSC_MDCTL(PSC1_BASE, psc1_mods[i]) =
			(PSC_MDCTL(PSC1_BASE, psc1_mods[i]) & ~0x1Fu) |
			PSC_STATE_ENABLE;
	}

	PSC_PTCMD(PSC1_BASE) = 1;
	for (n = 1000000; (PSC_PTSTAT(PSC1_BASE) & 1) && n; n--)
		;
	if (!n) {
		rc = -ETIMEDOUT;
	}

	return rc;
}

/*
 * Caches: make SDRAM non-cacheable for the DSP so that everything we write
 * there is immediately visible to the ARM.
 */

static void cache_op(volatile u32 *reg)
{
	u32 n = 1000000;

	*reg = 1;
	while (*reg && --n)
		;
}

void cache_wb_all(void)
{
	cache_op(&CACHE_L1DWB);
	cache_op(&CACHE_L2WB);
}

void soc_init_cache(void)
{
	int i;

	cache_op(&CACHE_L1DWBINV);
	cache_op(&CACHE_L2WBINV);
	for (i = SDRAM_BASE >> 24; i < (int)((SDRAM_BASE + SDRAM_MAX) >> 24);
	     i++) {
		CACHE_MAR(i) = 0;
	}
}

/* PLL0 */

int pll0_init(void)
{
	u32 n;

	/*
	 * Bypass: PLLENSRC = 0 so PLLEN is register controlled, then PLLEN = 0
	 */
	PLL0_PLLCTL &= ~PLLCTL_PLLENSRC;
	PLL0_PLLCTL &= ~PLLCTL_PLLEN;
	/* >= 4 OSCIN cycles for the mux */
	udelay(1);
	cpu_hz = BOARD_OSC_HZ;

	/* internal oscillator, crystal */
	PLL0_PLLCTL &= ~(PLLCTL_CLKMODE | PLLCTL_EXTCLKSRC);
	PLL0_PLLCTL &= ~PLLCTL_PLLRST; /* assert PLL reset */
	PLL0_PLLCTL |= PLLCTL_PLLDIS;
	PLL0_PLLCTL &= ~PLLCTL_PLLPWRDN; /* power up */
	PLL0_PLLCTL &= ~PLLCTL_PLLDIS;

	PLL0_PLLM = BOARD_PLLM;
	PLL0_POSTDIV = PLLDIV_EN | BOARD_POSTDIV;

	for (n = 1000000; (PLL0_PLLSTAT & PLLSTAT_GOSTAT) && n; n--)
		;
	PLL0_PLLDIV3 = PLLDIV_EN | BOARD_PLLDIV3;
	PLL0_PLLDIV5 = PLLDIV_EN | BOARD_PLLDIV5;
	PLL0_PLLDIV7 = PLLDIV_EN | BOARD_PLLDIV7;
	PLL0_PLLCMD |= PLLCMD_GOSET;
	for (n = 1000000; (PLL0_PLLSTAT & PLLSTAT_GOSTAT) && n; n--)
		;
	if (!n) {
		return -ETIMEDOUT;
	}

	udelay(1); /* PLL reset >= 125 ns */
	PLL0_PLLCTL |= PLLCTL_PLLRST; /* release reset */
	udelay(200); /* lock time */
	PLL0_PLLCTL |= PLLCTL_PLLEN; /* leave bypass */
	cpu_hz = BOARD_CPU_HZ;

	/*
	 * EMIFA from SYSCLK3 or from the PLL /4.5 divider; EMIFB is always
	 * SYSCLK5
	 */
	if (BOARD_EMIFA_DIV4P5) {
		SYSCFG_CFGCHIP(3) |= CFGCHIP3_DIV4P5ENA;
		SYSCFG_CFGCHIP(3) |= CFGCHIP3_EMA_CLKSRC;
	} else {
		SYSCFG_CFGCHIP(3) &= ~CFGCHIP3_EMA_CLKSRC;
	}

	return 0;
}

static u32 divratio(u32 reg)
{
	return (reg & PLLDIV_EN) ? (reg & 0x1F) + 1 : 0;
}

/* Clocks as the hardware runs them now (read back, not the board's wishes) */
void soc_clocks(struct soc_clocks *c)
{
	u32 ctl = PLL0_PLLCTL;
	u32 post = divratio(PLL0_POSTDIV);
	u32 d3 = divratio(PLL0_PLLDIV3), d5 = divratio(PLL0_PLLDIV5),
	    d7 = divratio(PLL0_PLLDIV7);

	c->osc = BOARD_OSC_HZ;
	c->bypass = !(ctl & PLLCTL_PLLEN);
	c->pll = c->bypass ? c->osc
			   : c->osc / (post ? post : 1) *
				     ((PLL0_PLLM & 0x1F) + 1);
	c->cpu = c->pll / (divratio(PLL0_PLLDIV1) ? divratio(PLL0_PLLDIV1) : 1);
	c->emifb = d5 ? c->pll / d5 : 0;
	if (SYSCFG_CFGCHIP(3) & CFGCHIP3_EMA_CLKSRC) {
		c->emifa = c->pll / 9 * 2;
	} else {
		c->emifa = d3 ? c->pll / d3 : 0;
	}

	c->rmii = d7 ? c->pll / d7 : 0;
}

void pll0_report(void)
{
	struct soc_clocks c;

	soc_clocks(&c);
	printf("PLL0:   PLLCTL %08x, PLLM x%u, POSTDIV /%u -> %u kHz%s\n",
	       PLL0_PLLCTL, (PLL0_PLLM & 0x1F) + 1, divratio(PLL0_POSTDIV),
	       c.pll / 1000, c.bypass ? " (bypassed)" : "");
	printf("        PLLDIV1 /%u PLLDIV2 /%u PLLDIV3 /%u PLLDIV4 /%u PLLDIV5 /%u PLLDIV6 /%u PLLDIV7 /%u\n",
	       divratio(PLL0_PLLDIV1), divratio(PLL0_PLLDIV2),
	       divratio(PLL0_PLLDIV3), divratio(PLL0_PLLDIV4),
	       divratio(PLL0_PLLDIV5), divratio(PLL0_PLLDIV6),
	       divratio(PLL0_PLLDIV7));
	printf("        DSP/ARM %u kHz, EMIFA %u kHz%s, EMIFB %u kHz, RMII %u kHz, CFGCHIP3 %08x\n",
	       c.cpu / 1000, c.emifa / 1000,
	       (SYSCFG_CFGCHIP(3) & CFGCHIP3_EMA_CLKSRC) ? " (PLL/4.5)" : "",
	       c.emifb / 1000, c.rmii / 1000, SYSCFG_CFGCHIP(3));
}

/* Pinmux */

void pinmux_set(int reg, u32 mask, u32 val)
{
	SYSCFG_PINMUX(reg) = (SYSCFG_PINMUX(reg) & ~mask) | (val & mask);
}

void pinmux_uart(int n)
{
	switch (n) {
	case 0:
		/* UART0_RXD, UART0_TXD */
		pinmux_set(8, 0x000FF000, 0x00011000);
		break;
	case 1:
		/* UART1_RXD, UART1_TXD */
		pinmux_set(11, 0x0000FF00, 0x00001100);
		break;
	case 2:
		pinmux_set(8, 0xF0000000, 0x20000000); /* UART2_RXD */
		pinmux_set(9, 0x0000000F, 0x00000002); /* UART2_TXD */
		break;
	}
}

/*
 * Only the pins an 8-bit NAND on CS3 needs (same set the ROM NAND boot uses).
 */
void pinmux_nand(void)
{
	pinmux_set(13, 0xFF000000, 0x11000000); /* EMA_D[0..1] */
	pinmux_set(14, 0x00FFFFFF, 0x00111111); /* EMA_D[2..7] */
	pinmux_set(15, 0xF0000000, 0x10000000); /* EMA_A[1]  = ALE */
	pinmux_set(16, 0x0000000F, 0x00000001); /* EMA_A[2]  = CLE */
	/* nEMA_WE, nEMA_CS[3], nEMA_OE */
	pinmux_set(18, 0x00FF00F0, 0x00110010);
	pinmux_set(19, 0x0000000F, 0x00000001); /* EMA_WAIT[0] (R/B) */
}

/* Identification */

/* BOOT[7,2,1,0,3] -> mode, from TI device.c / the DA830 data sheet */
static const u8 bootmode_lut[32] = {
	BM_I2C0_MASTER, BM_I2C0_SLAVE,	BM_NOR,		BM_NOR,
	BM_UHPI,	BM_UHPI,	BM_I2C1_MASTER, BM_I2C1_SLAVE,
	BM_SPI0_EEPROM, BM_SPI1_EEPROM, BM_SPI0_FLASH,	BM_SPI0_FLASH,
	BM_SPI1_FLASH,	BM_SPI1_FLASH,	BM_NAND8,	BM_NAND8,
	BM_NAND16,	BM_NONE,	BM_SPI0_SLAVE,	BM_SPI1_SLAVE,
	BM_UART2,	BM_THB,		BM_UART0,	BM_UART1,
	BM_ESF,		BM_ESF,		BM_USB11,	BM_USB20,
	BM_MMC,		BM_RMII,	BM_EMU,		BM_NONE,
};

static const char *const bootmode_names[] = {
	"none",
	"I2C0 master",
	"I2C0 slave",
	"NOR (EMIFA)",
	"UHPI",
	"I2C1 master",
	"I2C1 slave",
	"SPI0 EEPROM",
	"SPI1 EEPROM",
	"SPI0 flash",
	"SPI1 flash",
	"NAND 8-bit",
	"NAND 16-bit",
	"SPI0 slave",
	"SPI1 slave",
	"UART2",
	"THB",
	"UART0",
	"UART1",
	"ESF",
	"USB1.1",
	"USB2.0",
	"MMC/SD",
	"RMII",
	"emulation debug",
};

enum bootmode soc_bootmode(void)
{
	u32 b = SYSCFG_BOOTCFG & 0xFF;
	u32 i = ((b & 0x07) << 1) | ((b & 0x88) >> 3); /* -> 7,2,1,0,3 */

	return (enum bootmode)bootmode_lut[i & 31];
}

const char *soc_bootmode_name(enum bootmode bm)
{
	return (u32)bm < ARRAY_SIZE(bootmode_names) ? bootmode_names[bm] : "?";
}

const char *soc_name(void)
{
	return ((SYSCFG_JTAGID >> 12) & 0xFFFF) == JTAGID_PART_DA830
		       ? "OMAP-L137/DA830"
		       : "unknown SoC";
}

/* "d800k" and a three-digit revision, else "?" */
const char *soc_rom_id(void)
{
	static char id[9];
	const volatile char *rom = (const volatile char *)ROM_ID_ADDR;
	u32 i;

	for (i = 0; i < 8; i++) {
		id[i] = rom[i];
	}
	id[8] = 0;
	if (memcmp(id, "d800k", 5)) {
		return "?";
	}
	for (i = 5; i < 8; i++) {
		if (id[i] < '0' || id[i] > '9') {
			return "?";
		}
	}

	return id;
}

/* JTAGID reads the same on 2.0 and 2.1; the 2.1 boot ROM is d800k005 */
const char *soc_revision(void)
{
	switch (SYSCFG_JTAGID >> 28) {
	case 0x0: return "1.0";
	case 0x8: return "1.1";
	case 0x9: return strcmp(soc_rom_id(), "d800k005") ? "2.0" : "2.1";
	default: return "?";
	}
}

void soc_report(void)
{
	printf("SoC:    %s rev %s, ROM %s, JTAGID %08x, REVID %08x, CHIPREVID %08x\n",
	       soc_name(), soc_revision(), soc_rom_id(), SYSCFG_JTAGID,
	       SYSCFG_REVID, SYSCFG_CHIPREVID);
	printf("        DIEID %08x %08x %08x %08x\n", SYSCFG_DIEIDR(0),
	       SYSCFG_DIEIDR(1), SYSCFG_DIEIDR(2), SYSCFG_DIEIDR(3));
	printf("Boot:   %s (BOOTCFG %08x)\n", soc_bootmode_name(soc_bootmode()),
	       SYSCFG_BOOTCFG);
}

/* ARM release */
/*
 * After reset the ARM926 fetches from 0xFFFF0000 (its local RAM). The DSP
 * cannot write there directly, so - like TI's DSP-side boot code - we let
 * PRU0 copy an 11-word stub into place:
 *   0x00: b  boot      ; reset vector
 *   0x04..0x1C: b .    ; other vectors park
 *   0x20: .word entry
 *   0x24: boot: ldr r0, [pc, #-12] ; mov pc, r0
 */
static u32 arm_stub[11] __attribute__((aligned(8)));

static const u32 arm_stub_tmpl[11] = {
	0xEA000007, 0xEAFFFFFE, 0xEAFFFFFE, 0xEAFFFFFE, 0xEAFFFFFE, 0xEAFFFFFE,
	0xEAFFFFFE, 0xEAFFFFFE, 0x00000000, 0xE51F000C, 0xE1A0F000,
};

/*
 * PRU program: r0 = 0xFFFF0000, r1 = &arm_stub, LBBO 44 bytes, SBBO 44 bytes,
 * halt-loop
 */
static const u32 pru_prog_tmpl[7] = {
	0x24000080, /* ldi r0.w0, 0x0000 */
	0x24ffffc0, /* ldi r0.w2, 0xffff */
	0x24000081, /* ldi r1.w0, lo16(stub)  (patched) */
	0x240000c1, /* ldi r1.w2, hi16(stub)  (patched) */
	0xf500e182, /* lbbo r2, r1, 0, 44 */
	0xe500e082, /* sbbo r2, r0, 0, 44 */
	0x79000000, /* qba 0 (spin) */
};

int arm_release(u32 entry)
{
	u32 i, stub = (u32)arm_stub, n;
	volatile u32 *iram = (volatile u32 *)PRU0_IRAM_BASE;

	for (i = 0; i < 11; i++) {
		arm_stub[i] = arm_stub_tmpl[i];
	}

	arm_stub[8] = entry;
	/* stub and payload must be in memory, not L1D */
	cache_wb_all();

	if (psc_set(0, LPSC0_ARMRAM, PSC_STATE_ENABLE) ||
	    psc_set(0, LPSC0_PRU, PSC_STATE_ENABLE)) {
		return -ETIMEDOUT;
	}

	PRU0_CONTROL = 0; /* halt + reset PRU0 */
	for (i = 0; i < 7; i++) {
		iram[i] = pru_prog_tmpl[i];
	}

	iram[2] |= (stub & 0xFFFF) << 8;
	iram[3] |= (stub >> 16) << 8;
	PRU0_CONTROL |= PRU_CONTROL_COUNTEN;
	PRU0_CONTROL |= PRU_CONTROL_ENABLE;
	for (n = 1000000; PRU0_STATUS != 6 && n; n--)
		;
	PRU0_CONTROL = 0;
	if (!n) {
		/* PRU did not finish the copy */
		return -ETIMEDOUT;
	}

	SYSCFG_HOST0CFG = 1; /* BOOTRDY */
	if (psc_set(0, LPSC0_ARM, PSC_STATE_ENABLE)) {
		return -ETIMEDOUT;
	}

	/* release ARM local reset */
	PSC_MDCTL(PSC0_BASE, LPSC0_ARM) |= PSC_MDCTL_LRSTZ;
	return 0;
}

/* Chip reset through the Timer64P1 watchdog (same sequence as Linux davinci) */

void soc_reset(void)
{
	TMR_TCR = 0;
	TMR_TGCR = 0;
	/* 64-bit watchdog mode, TIM12/TIM34 out of reset */
	TMR_TGCR = (2u << 2) | 1u | 2u;
	TMR_TIM12 = 0;
	TMR_TIM34 = 0;
	TMR_PRD12 = 0;
	TMR_PRD34 = 0;
	TMR_WDTCR = (0xA5C6u << 16) | (1u << 14);
	TMR_WDTCR = (0xDA7Eu << 16) | (1u << 14);
	TMR_WDTCR = (1u << 14); /* invalid key -> immediate reset */
	for (;;)
		;
}
