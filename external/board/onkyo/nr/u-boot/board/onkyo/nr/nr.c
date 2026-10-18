// SPDX-License-Identifier: GPL-2.0+
/*
 * Onkyo DSP board: DA830 / OMAP-L137, 64 MiB SDRAM, NAND on
 * EMIFA CS3, RMII Ethernet, console on UART1.
 *
 * dspboot (the first stage on the DSP) has set up the PLL, the SDRAM and
 * the NAND pins, given the ARM the PSCs and started this from RAM.
 */

#include <config.h>
#include <clock_legacy.h>
#include <fdt_support.h>
#include <init.h>
#include <mtd_node.h>
#include <nand.h>
#include <jffs2/load_kernel.h>
#include <asm/arch/hardware.h>
#include <asm/arch/davinci_misc.h>
#include <asm/io.h>
#include <asm/ti-common/davinci_nand.h>

/* drivers/net/ti/davinci_emac.c */
int davinci_eth_phy_read(u8 phy_addr, u8 reg_num, u16 *data);
int davinci_eth_phy_write(u8 phy_addr, u8 reg_num, u16 data);

/*
 * CS3 timing as Linux derives it at the 89 MHz EMIFA clock: read setup 2,
 * strobe 5, hold 1; write setup 3, strobe 3, hold 2; turnaround 2; 8 bit.
 * dspboot leaves the slowest timing, which doubles the time to attach UBI
 * and read the kernel.
 */
#define NAND_CS3_TIMING		0x08222204

#define GPIO_BANK23_DIR		0x01e26038
#define GPIO_BANK45_DIR		0x01e26060
#define GPIO_BANK45_SET		0x01e26068
#define PHY_INT_GPIO		BIT(28)	/* GP3[12], bank 2/3 */
#define DSD_SW_GPIO		BIT(14)	/* GP4[14], bank 4/5 */

static const struct pinmux_config uart1_pins[] = {
	{ pinmux(11), 1, 3 },	/* UART1_TXD */
	{ pinmux(11), 1, 2 },	/* UART1_RXD */
};

static const struct pinmux_config emac_pins[] = {
	{ pinmux(9), 2, 5 },	/* RMII_MHZ_50_CLK */
	{ pinmux(10), 2, 1 },	/* RMII_TXD[0] */
	{ pinmux(10), 2, 2 },	/* RMII_TXD[1] */
	{ pinmux(10), 2, 3 },	/* RMII_TXEN */
	{ pinmux(10), 2, 4 },	/* RMII_CRS_DV */
	{ pinmux(10), 2, 5 },	/* RMII_RXD[0] */
	{ pinmux(10), 2, 6 },	/* RMII_RXD[1] */
	{ pinmux(10), 2, 7 },	/* RMII_RXER */
	{ pinmux(11), 2, 0 },	/* MDIO_CLK */
	{ pinmux(11), 2, 1 },	/* MDIO_D */
	{ pinmux(9), 8, 7 },	/* GP3[12]: PHY interrupt */
};

/* GPIOs */
static const struct pinmux_config gpio_pins[] = {
	{ pinmux(9), 8, 0 },	/* GP5[13]: rear USB VBUS, active low */
	{ pinmux(7), 8, 6 },	/* GP5[3]: DIG_TUNER */
	{ pinmux(12), 8, 4 },	/* GP4[0] */
	{ pinmux(13), 8, 0 },	/* GP4[4] */
	{ pinmux(11), 8, 7 },	/* GP4[10] */
	{ pinmux(12), 8, 1 },	/* GP4[12] */
	{ pinmux(12), 8, 2 },	/* GP4[13] */
	{ pinmux(11), 8, 6 },	/* GP3[15] */
	{ pinmux(12), 8, 3 },	/* GP4[14]: DSD_SW (717), open (515) */
};

static const struct pinmux_resource pinmuxes[] = {
	PINMUX_ITEM(uart1_pins),
	PINMUX_ITEM(emac_pins),
	PINMUX_ITEM(gpio_pins),
};

static const struct lpsc_resource lpsc[] = {
	{ DAVINCI_LPSC_AEMIF },
	{ DAVINCI_LPSC_EMAC },
	{ DAVINCI_LPSC_UART1 },
	{ DAVINCI_LPSC_GPIO },
};

int board_early_init_f(void)
{
	if (da8xx_configure_lpsc_items(lpsc, ARRAY_SIZE(lpsc)))
		return 1;
	if (davinci_configure_pin_mux_items(pinmuxes, ARRAY_SIZE(pinmuxes)))
		return 1;

	writel(DAVINCI_UART_PWREMU_MGMT_FREE | DAVINCI_UART_PWREMU_MGMT_URRST |
	       DAVINCI_UART_PWREMU_MGMT_UTRST,
	       &davinci_uart1_ctrl_regs->pwremu_mgmt);

	writel(NAND_CS3_TIMING, &davinci_emif_regs->ab2cr);

	return 0;
}

/* the PHY takes the 50 MHz RMII reference clock */
void davinci_eth_board_phy_init(u8 phy_addr)
{
	u16 val;

	if (davinci_eth_phy_read(phy_addr, 0x1f, &val))
		davinci_eth_phy_write(phy_addr, 0x1f, val | BIT(7));
}

/*
 * The environment takes block 0 and dspboot block 1 up to 1 MiB, as the
 * boot ROM reads it from block 1; the rest are at offsets dspboot and the
 * environment use, block aligned for blocks of up to 256 KiB.
 */
void board_mtdparts_default(const char **mtdids, const char **mtdparts)
{
	static char parts[128];
	struct mtd_info *mtd = get_nand_dev_by_index(0);
	unsigned int block = mtd ? mtd->erasesize >> 10 : 16;

	snprintf(parts, sizeof(parts),
		 "mtdparts=davinci_nand.1:%uk(env),%uk(dspboot),256k(env2),"
		 "512k(uboot_a),512k(uboot_b),-(ubi)", block, 1024 - block);
	*mtdids = "nand0=davinci_nand.1";
	*mtdparts = parts;
}

/* the kernel's device tree gets the same partitions */
int ft_board_setup(void *blob, struct bd_info *bd)
{
	static const struct node_info nodes[] = {
		{ "ti,davinci-nand", MTD_DEV_TYPE_NAND },
	};

	fdt_fixup_mtdparts(blob, nodes, ARRAY_SIZE(nodes));
	return 0;
}

int print_cpuinfo(void)
{
	printf("CPU:   DA830/OMAP-L137, ARM %d MHz\n",
	       clk_get(DAVINCI_ARM_CLKID) / 1000000);
	return 0;
}

int board_init(void)
{
	irq_init();

	writel(readl(&davinci_syscfg_regs->suspsrc) &
	       ~(DAVINCI_SYSCFG_SUSPSRC_EMAC | DAVINCI_SYSCFG_SUSPSRC_TIMER0 |
		 DAVINCI_SYSCFG_SUSPSRC_UART1),
	       &davinci_syscfg_regs->suspsrc);

	/* PHY interrupt in; DSD_SW high */
	setbits_le32(GPIO_BANK23_DIR, PHY_INT_GPIO);
	writel(DSD_SW_GPIO, GPIO_BANK45_SET);
	clrbits_le32(GPIO_BANK45_DIR, DSD_SW_GPIO);

	return 0;
}
