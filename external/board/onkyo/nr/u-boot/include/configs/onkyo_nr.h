/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Onkyo DSP board (DA830 / OMAP-L137)
 */

#ifndef __CONFIG_H
#define __CONFIG_H

#define CFG_SYS_EXCEPTION_VECTORS_HIGH

#define CFG_SYS_OSCIN_FREQ		20000000
#define CFG_SYS_TIMERBASE		DAVINCI_TIMER0_BASE
#define CFG_SYS_HZ_CLOCK		clk_get(DAVINCI_AUXCLK_CLKID)

#define CFG_SYS_SDRAM_BASE		0xc0000000
#define PHYS_SDRAM_1			DAVINCI_DDR_EMIF_DATA_BASE
#define PHYS_SDRAM_1_SIZE		(64 << 20)
#define CFG_MAX_RAM_BANK_SIZE		(64 << 20)

#define CFG_SYS_NS16550_CLK		clk_get(DAVINCI_UART2_CLKID)

#define CFG_SYS_NAND_CS			3
#define CFG_SYS_NAND_BASE		DAVINCI_ASYNC_EMIF_DATA_CE3_BASE
#define CFG_SYS_NAND_MASK_CLE		0x10
#define CFG_SYS_NAND_MASK_ALE		0x8

#define LINUX_BOOT_PARAM_ADDR		(PHYS_SDRAM_1 + 0x100)

/* the PHY negotiates by itself after reset; restarting it costs seconds */
#define CONFIG_DRIVER_TI_EMAC_RMII_NO_NEGOTIATE

/*
 * The OSIRIS boot: the slot "bootslot" (a/b) names, or once the one in
 * "upgrade_slot" after osiris-upgrade, cleared before it boots; then the
 * other slot. UBI volumes are created in the order kernel_a, kernel_b,
 * rootfs_a, rootfs_b, data, so behind mtd0-5 gluebi makes rootfs_a mtd8
 * and rootfs_b mtd9. This is what older systems saved in the environment,
 * so both U-Boots boot both kinds of slot the same way.
 */
#define CFG_EXTRA_ENV_SETTINGS \
	"bootslot=a\0" \
	"bootargs_base=console=ttyS1,115200n8 mem=56M panic=5 " \
		"ubi.mtd=5 rootfstype=squashfs ro\0" \
	"bootargs_extra=\0" \
	"bootslot_root=if test \"${slot}\" = b; then setenv rootdev /dev/mtdblock9; " \
		"else setenv rootdev /dev/mtdblock8; fi\0" \
	"bootslot_one=run bootslot_root; ubi read ${loadaddr} kernel_${slot} && " \
		"setenv bootargs ${bootargs_base} root=${rootdev} osiris.slot=${slot} " \
		"${bootargs_extra} && bootm ${loadaddr}\0"

#include <asm/arch/hardware.h>

#endif /* __CONFIG_H */
