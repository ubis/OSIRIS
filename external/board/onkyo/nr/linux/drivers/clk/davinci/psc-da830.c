// SPDX-License-Identifier: GPL-2.0
/*
 * PSC clock descriptions for TI DA830/OMAP-L137/AM17XX
 *
 * Copyright (C) 2018 David Lechner <david@lechnology.com>
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/types.h>

#include "psc.h"

/*
 * The DSP (PSC0 module 15) and the McASPs (PSC1 modules 7-9) belong to
 * the DSP firmware, which the first stage powers up: they are left out
 * so that Linux never gates them as unused.
 */

static const struct davinci_lpsc_clk_info da830_psc0_info[] = {
	LPSC(0,  0, tpcc,     pll0_sysclk2, NULL, LPSC_ALWAYS_ENABLED),
	LPSC(1,  0, tptc0,    pll0_sysclk2, NULL, LPSC_ALWAYS_ENABLED),
	LPSC(2,  0, tptc1,    pll0_sysclk2, NULL, LPSC_ALWAYS_ENABLED),
	LPSC(3,  0, aemif,    pll0_sysclk3, NULL, LPSC_ALWAYS_ENABLED),
	LPSC(4,  0, spi0,     pll0_sysclk2, NULL, 0),
	LPSC(5,  0, mmcsd,    pll0_sysclk2, NULL, 0),
	LPSC(6,  0, aintc,    pll0_sysclk4, NULL, LPSC_ALWAYS_ENABLED),
	LPSC(7,  0, arm_rom,  pll0_sysclk2, NULL, LPSC_ALWAYS_ENABLED),
	LPSC(8,  0, secu_mgr, pll0_sysclk4, NULL, LPSC_ALWAYS_ENABLED),
	LPSC(9,  0, uart0,    pll0_sysclk2, NULL, 0),
	LPSC(10, 0, scr0_ss,  pll0_sysclk2, NULL, LPSC_ALWAYS_ENABLED),
	LPSC(11, 0, scr1_ss,  pll0_sysclk2, NULL, LPSC_ALWAYS_ENABLED),
	LPSC(12, 0, scr2_ss,  pll0_sysclk2, NULL, LPSC_ALWAYS_ENABLED),
	LPSC(13, 0, pruss,    pll0_sysclk2, NULL, LPSC_ALWAYS_ENABLED),
	LPSC(14, 0, arm,      pll0_sysclk6, NULL, LPSC_ALWAYS_ENABLED),
	{ }
};

static int of_da830_psc0_init(struct device *dev, void __iomem *base)
{
	return of_davinci_psc_clk_init(dev, da830_psc0_info, 16, base);
}

static struct clk_bulk_data da830_psc0_parent_clks[] = {
	{ .id = "pll0_sysclk2" },
	{ .id = "pll0_sysclk3" },
	{ .id = "pll0_sysclk4" },
	{ .id = "pll0_sysclk6" },
};

const struct davinci_psc_init_data of_da830_psc0_init_data = {
	.parent_clks		= da830_psc0_parent_clks,
	.num_parent_clks	= ARRAY_SIZE(da830_psc0_parent_clks),
	.psc_init		= &of_da830_psc0_init,
};

static const struct davinci_lpsc_clk_info da830_psc1_info[] = {
	LPSC(1,  0, usb0,   pll0_sysclk2, NULL, 0),
	LPSC(2,  0, usb1,   pll0_sysclk4, NULL, 0),
	LPSC(3,  0, gpio,   pll0_sysclk4, NULL, 0),
	LPSC(5,  0, emac,   pll0_sysclk4, NULL, 0),
	LPSC(6,  0, emif3,  pll0_sysclk5, NULL, LPSC_ALWAYS_ENABLED),
	LPSC(10, 0, spi1,   pll0_sysclk2, NULL, 0),
	LPSC(11, 0, i2c1,   pll0_sysclk4, NULL, 0),
	LPSC(12, 0, uart1,  pll0_sysclk2, NULL, 0),
	LPSC(13, 0, uart2,  pll0_sysclk2, NULL, 0),
	LPSC(16, 0, lcdc,   pll0_sysclk2, NULL, 0),
	LPSC(17, 0, pwm,    pll0_sysclk2, NULL, 0),
	LPSC(20, 0, ecap,   pll0_sysclk2, NULL, 0),
	LPSC(21, 0, eqep,   pll0_sysclk2, NULL, 0),
	{ }
};

static int of_da830_psc1_init(struct device *dev, void __iomem *base)
{
	return of_davinci_psc_clk_init(dev, da830_psc1_info, 32, base);
}

static struct clk_bulk_data da830_psc1_parent_clks[] = {
	{ .id = "pll0_sysclk2" },
	{ .id = "pll0_sysclk4" },
	{ .id = "pll0_sysclk5" },
};

const struct davinci_psc_init_data of_da830_psc1_init_data = {
	.parent_clks		= da830_psc1_parent_clks,
	.num_parent_clks	= ARRAY_SIZE(da830_psc1_parent_clks),
	.psc_init		= &of_da830_psc1_init,
};
