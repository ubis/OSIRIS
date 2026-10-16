// SPDX-License-Identifier: GPL-2.0
/*
 * PLL clock descriptions for TI DA830/OMAP-L137/AM17XX
 *
 * Copyright (C) 2018 David Lechner <david@lechnology.com>
 */

#include <linux/bitops.h>
#include <linux/clk-provider.h>
#include <linux/init.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/types.h>

#include "pll.h"

static const struct davinci_pll_clk_info da830_pll_info = {
	.name = "pll0",
	.pllm_mask = GENMASK(4, 0),
	.pllm_min = 4,
	.pllm_max = 32,
	.pllout_min_rate = 300000000,
	.pllout_max_rate = 600000000,
	.flags = PLL_HAS_CLKMODE | PLL_HAS_PREDIV | PLL_HAS_POSTDIV,
};

/*
 * NB: Technically, the clocks flagged as SYSCLK_FIXED_DIV are "fixed ratio",
 * meaning that we could change the divider as long as we keep the correct
 * ratio between all of the clocks, but we don't support that because there is
 * currently not a need for it.
 */

SYSCLK(2, pll0_sysclk2, pll0_pllen, 5, SYSCLK_FIXED_DIV);
SYSCLK(3, pll0_sysclk3, pll0_pllen, 5, 0);
SYSCLK(4, pll0_sysclk4, pll0_pllen, 5, SYSCLK_FIXED_DIV);
SYSCLK(5, pll0_sysclk5, pll0_pllen, 5, 0);
SYSCLK(6, pll0_sysclk6, pll0_pllen, 5, SYSCLK_ARM_RATE | SYSCLK_FIXED_DIV);
/* the RMII reference clock out to the Ethernet PHY, with no consumer */
SYSCLK(7, pll0_sysclk7, pll0_pllen, 5, SYSCLK_ALWAYS_ENABLED);

static const struct davinci_pll_sysclk_info *da830_pll_sysclk_info[] = {
	&pll0_sysclk2,
	&pll0_sysclk3,
	&pll0_sysclk4,
	&pll0_sysclk5,
	&pll0_sysclk6,
	&pll0_sysclk7,
	NULL
};

static void __init of_da830_pll_init(struct device_node *node)
{
	void __iomem *base;

	base = of_iomap(node, 0);
	if (!base) {
		pr_err("%s: ioremap failed\n", __func__);
		return;
	}

	of_davinci_pll_init(NULL, node, &da830_pll_info, NULL,
			    da830_pll_sysclk_info, 7, base, NULL);
}
CLK_OF_DECLARE(da830_pll, "ti,da830-pll", of_da830_pll_init);
