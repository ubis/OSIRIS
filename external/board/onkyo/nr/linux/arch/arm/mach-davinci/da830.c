// SPDX-License-Identifier: GPL-2.0-only
/*
 * TI DA830/OMAP L137 chip specific setup
 *
 * Author: Mark A. Greer <mgreer@mvista.com>
 *
 * 2009 (c) MontaVista Software, Inc.
 */

#include <linux/init.h>
#include <linux/io.h>

#include <asm/mach/arch.h>
#include <asm/mach/map.h>

#include "common.h"
#include "cputype.h"
#include "da8xx.h"
#include "irqs.h"

static struct map_desc da830_io_desc[] = {
	{
		.virtual	= IO_VIRT,
		.pfn		= __phys_to_pfn(IO_PHYS),
		.length		= IO_SIZE,
		.type		= MT_DEVICE
	},
	{
		.virtual	= DA8XX_CP_INTC_VIRT,
		.pfn		= __phys_to_pfn(DA8XX_CP_INTC_BASE),
		.length		= DA8XX_CP_INTC_SIZE,
		.type		= MT_DEVICE
	},
};

/* Contents of JTAG ID register used to identify exact cpu type */
static struct davinci_id da830_ids[] = {
	{
		.variant	= 0x0,
		.part_no	= 0xb7df,
		.manufacturer	= 0x017,	/* 0x02f >> 1 */
		.cpu_id		= DAVINCI_CPU_ID_DA830,
		.name		= "da830/omap-l137 rev1.0",
	},
	{
		.variant	= 0x8,
		.part_no	= 0xb7df,
		.manufacturer	= 0x017,
		.cpu_id		= DAVINCI_CPU_ID_DA830,
		.name		= "da830/omap-l137 rev1.1",
	},
	{
		.variant	= 0x9,
		.part_no	= 0xb7df,
		.manufacturer	= 0x017,
		.cpu_id		= DAVINCI_CPU_ID_DA830,
		.name		= "da830/omap-l137 rev2.0",
	},
};

static const struct davinci_soc_info davinci_soc_info_da830 = {
	.io_desc		= da830_io_desc,
	.io_desc_num		= ARRAY_SIZE(da830_io_desc),
	.jtag_id_reg		= DA8XX_SYSCFG0_BASE + DA8XX_JTAG_ID_REG,
	.ids			= da830_ids,
	.ids_num		= ARRAY_SIZE(da830_ids),
};

static void __init da830_init(void)
{
	davinci_common_init(&davinci_soc_info_da830);

	da8xx_syscfg0_base = ioremap(DA8XX_SYSCFG0_BASE, SZ_4K);
	WARN(!da8xx_syscfg0_base, "Unable to map syscfg0 module");
}

static const char *const da830_boards_compat[] __initconst = {
	"ti,da830",
	NULL,
};

DT_MACHINE_START(DA830_DT, "Generic DA830/OMAP-L137/AM17x")
	.map_io		= da830_init,
	.dt_compat	= da830_boards_compat,
	.init_late	= davinci_init_late,
MACHINE_END
