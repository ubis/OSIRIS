/*
 * TI DA830/OMAP L137 EVM board
 *
 * Author: Mark A. Greer <mgreer@mvista.com>
 * Derived from: arch/arm/mach-davinci/board-dm644x-evm.c
 *
 * 2007, 2009 (c) MontaVista Software, Inc. This file is licensed under
 * the terms of the GNU General Public License version 2. This program
 * is licensed "as is" without any warranty of any kind, whether express
 * or implied.
 */
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/console.h>
#include <linux/interrupt.h>
#include <linux/gpio.h>
#include <linux/platform_device.h>
#include <linux/i2c.h>
#include <linux/i2c/pcf857x.h>
#include <linux/i2c/at24.h>
#include <linux/i2c/tsc2004.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>
#include <linux/spi/spi.h>
#include <linux/spi/flash.h>
#include <linux/usb/musb.h>

#include <asm/mach-types.h>
#include <asm/mach/arch.h>

#include <mach/cp_intc.h>
#include <mach/mux.h>
#include <mach/nand.h>
#include <mach/da8xx.h>
#include <mach/usb.h>

#define DA830_EVM_PHY_MASK		0x0
#define DA830_EVM_MDIO_FREQUENCY	2200000	/* PHY bus frequency */

#define DA830_EMIF25_ASYNC_DATA_CE3_BASE	0x62000000
#define DA830_EMIF25_CONTROL_BASE		0x68000000

/*
 * USB1 (the rear port) VBUS is switched by GPIO5[13], over-current is reported
 * on GPIO5[12]; both active low.
 */
#define ON_BD_USB_DRV	GPIO_TO_PIN(5, 13)
#define ON_BD_USB_OVC	GPIO_TO_PIN(5, 12)

static const short da830_evm_usb11_pins[] = {
	DA830_GPIO5_13, DA830_GPIO5_12,
	-1
};

static da8xx_ocic_handler_t da830_evm_usb_ocic_handler;

static int da830_evm_usb_set_power(unsigned port, int on)
{
	gpio_set_value(ON_BD_USB_DRV, !on);
	return 0;
}

static int da830_evm_usb_get_power(unsigned port)
{
	return !gpio_get_value(ON_BD_USB_DRV);
}

static int da830_evm_usb_get_oci(unsigned port)
{
	return !gpio_get_value(ON_BD_USB_OVC);
}

static irqreturn_t da830_evm_usb_ocic_irq(int, void *);

static int da830_evm_usb_ocic_notify(da8xx_ocic_handler_t handler)
{
	int irq 	= gpio_to_irq(ON_BD_USB_OVC);
	int error	= 0;

	if (handler != NULL) {
		da830_evm_usb_ocic_handler = handler;

		error = request_irq(irq, da830_evm_usb_ocic_irq, IRQF_DISABLED |
				    IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
				    "OHCI over-current indicator", NULL);
		if (error)
			printk(KERN_ERR "%s: could not request IRQ to watch "
			       "over-current indicator changes\n", __func__);
	} else
		free_irq(irq, NULL);

	return error;
}

static struct da8xx_ohci_root_hub da830_evm_usb11_pdata = {
	.set_power	= da830_evm_usb_set_power,
	.get_power	= da830_evm_usb_get_power,
	.get_oci	= da830_evm_usb_get_oci,
	.ocic_notify	= da830_evm_usb_ocic_notify,

	/* TPS2065 switch @ 5V */
	.potpgt		= (3 + 1) / 2,	/* 3 ms max */
};

static irqreturn_t da830_evm_usb_ocic_irq(int irq, void *dev_id)
{
	da830_evm_usb_ocic_handler(&da830_evm_usb11_pdata, 1);
	return IRQ_HANDLED;
}

static struct musb_hdrc_platform_data usb_evm_data[] = {
	{
#ifdef CONFIG_USB_MUSB_OTG
		.mode = MUSB_OTG,
#elif defined(CONFIG_USB_MUSB_DUAL_ROLE)
		.mode = MUSB_DUAL_ROLE,
#elif defined(CONFIG_USB_MUSB_PERIPHERAL)
		.mode =  MUSB_PERIPHERAL,
#elif defined(CONFIG_USB_MUSB_HOST)
		.mode = MUSB_HOST,
#endif
		.power = 255,
		.potpgt = 8,
		.set_vbus = NULL, /* VBUs is directly controlled by the IP */
	}
};

static __init void da830_evm_usb_init(void)
{
	u32 cfgchip2;
	int ret;

	/* PHY PLL runs off the 20 MHz crystal; USB1 gets 48 MHz from it */
	cfgchip2 = __raw_readl(DA8XX_SYSCFG0_VIRT(DA8XX_CFGCHIP2_REG));
	cfgchip2 &= ~CFGCHIP2_REFFREQ;
	cfgchip2 |=  CFGCHIP2_REFFREQ_20MHZ;
	__raw_writel(cfgchip2, DA8XX_SYSCFG0_VIRT(DA8XX_CFGCHIP2_REG));

	/* USB_REFCLKIN is not used. */
	ret = davinci_cfg_reg(DA830_USB0_DRVVBUS);
	if (ret)
		pr_warning("%s: USB 2.0 PinMux setup failed: %d\n",
			   __func__, ret);

	da8xx_usb20_configure(usb_evm_data, ARRAY_SIZE(usb_evm_data));

	ret = da8xx_pinmux_setup(da830_evm_usb11_pins);
	if (ret) {
		pr_warning("%s: USB 1.1 PinMux setup failed: %d\n",
			   __func__, ret);
		return;
	}

	ret = gpio_request(ON_BD_USB_DRV, "ON_BD_USB_DRV");
	if (ret) {
		printk(KERN_ERR "%s: failed to request GPIO for USB 1.1 port "
		       "power control: %d\n", __func__, ret);
		return;
	}
	gpio_direction_output(ON_BD_USB_DRV, 1);

	ret = gpio_request(ON_BD_USB_OVC, "ON_BD_USB_OVC");
	if (ret) {
		printk(KERN_ERR "%s: failed to request GPIO for USB 1.1 port "
		       "over-current indicator: %d\n", __func__, ret);
		return;
	}
	gpio_direction_input(ON_BD_USB_OVC);

	ret = da8xx_register_usb11(&da830_evm_usb11_pdata);
	if (ret)
		pr_warning("%s: USB 1.1 registration failed: %d\n",
			   __func__, ret);
}

static struct davinci_uart_config da830_evm_uart_config __initdata = {
	.enabled_uarts = 0x7,
};

static const short da830_evm_mcasp1_pins[] = {
	/* TX to PCM1690: AHCLKX1 = MCLK in from PCM9211, ACLKX1/AFSX1 = BCK/LRCK out,
	 * AXR1_0..3 = FL/FR, C/SW, SL/SR, SBL/SBR.  RX from PCM9211/DIR: ACLKR1/AFSR1 +
	 * AXR1_4 (SDOUTDIR, main stereo); AXR1_5/7/8/9 = HDMI SLR/SPDIF/CSW/SBLR.
	 * NR515: AMUTE1's pin is GPIO4_14 = ETH_RESET, don't mux it. */
	DA830_AHCLKX1, DA830_ACLKX1, DA830_AFSX1, DA830_ACLKR1, DA830_AFSR1,
	DA830_AXR1_0, DA830_AXR1_1, DA830_AXR1_2, DA830_AXR1_3, DA830_AXR1_4,
	DA830_AXR1_5,
	-1
};

/* control lines whose pins come up as EMIFA, SPI or McASP */
static const short onkyo_gpio_pins[] = {
	DA830_GPIO1_0,		/* DIGMUT */
	DA830_GPIO1_8,		/* ARMZ2MUT */
	DA830_GPIO1_13,		/* DBG/AUDIO */
	DA830_GPIO2_9,		/* HDMIINT */
	DA830_GPIO4_11,		/* AFE_RESETX */
	DA830_GPIO5_3,		/* DIG_TUNER */
	DA830_GPIO2_15,		/* USB0_OC_DET */
	DA830_GPIO2_8,		/* DATA_NET */
	DA830_GPIO1_9,		/* DSP2_IRQ: second DSP, not fitted */
	DA830_GPIO1_10,		/* DSP2_RST */
	DA830_GPIO1_11,		/* DSP2_CS */
	DA830_GPIO1_12,		/* DSP2_BSY */
	DA830_GPIO2_0,		/* DSP2SDO */
	DA830_GPIO2_1,		/* DSP2SDI */
	-1
};

/* SPI0 to the MCU; NSPI0_ENA is DIG_TUNER here */
static const short onkyo_spi0_pins[] = {
	DA830_SPI0_SOMI_0, DA830_SPI0_SIMO_0, DA830_SPI0_CLK, DA830_NSPI0_SCS_0,
	-1
};

static u8 da830_iis_serializer_direction[] = {
	TX_MODE,	TX_MODE,	TX_MODE,	TX_MODE,	/* AXR1_0-3 -> PCM1690 DIN4..DIN1 */
	RX_MODE,	INACTIVE_MODE,	INACTIVE_MODE,	INACTIVE_MODE,	/* AXR1_4 <- PCM9211 SDOUTDIR (main stereo) */
	INACTIVE_MODE,	INACTIVE_MODE,	INACTIVE_MODE,	INACTIVE_MODE,
};

static struct snd_platform_data da830_evm_snd_data = {
	.tx_dma_offset  = 0x2000,
	.rx_dma_offset  = 0x2000,
	.op_mode        = DAVINCI_MCASP_IIS_MODE,
	.num_serializer = ARRAY_SIZE(da830_iis_serializer_direction),
	.tdm_slots      = 2,	/* Standard stereo I2S - proper 50% duty LRCK */
	.serial_dir     = da830_iis_serializer_direction,
	.eventq_no      = EVENTQ_0,
	.version	= MCASP_VERSION_2,
	.txnumevt	= 8,	/* FIFO slack: 8 x 4 serializers = 32 words */
	.rxnumevt	= 1,
};

/*
 * GPIO2[1] is used as MMC_SD_WP and GPIO2[2] as MMC_SD_INS.
 */
static const short da830_evm_mmc_sd_pins[] = {
	DA830_MMCSD_DAT_0, DA830_MMCSD_DAT_1, DA830_MMCSD_DAT_2,
	DA830_MMCSD_DAT_3, DA830_MMCSD_DAT_4, DA830_MMCSD_DAT_5,
	DA830_MMCSD_DAT_6, DA830_MMCSD_DAT_7, DA830_MMCSD_CLK,
	DA830_MMCSD_CMD,   DA830_GPIO2_1,     DA830_GPIO2_2,
	-1
};

#define DA830_MMCSD_WP_PIN		GPIO_TO_PIN(2, 1)
#define DA830_MMCSD_CD_PIN		GPIO_TO_PIN(2, 2)

static int da830_evm_mmc_get_ro(int index)
{
	return gpio_get_value(DA830_MMCSD_WP_PIN);
}

static int da830_evm_mmc_get_cd(int index)
{
	return !gpio_get_value(DA830_MMCSD_CD_PIN);
}

static struct davinci_mmc_config da830_evm_mmc_config = {
	.get_ro			= da830_evm_mmc_get_ro,
	.get_cd			= da830_evm_mmc_get_cd,
	.wires			= 4,
	.max_freq		= 50000000,
	.caps			= MMC_CAP_MMC_HIGHSPEED | MMC_CAP_SD_HIGHSPEED,
	.version		= MMC_CTLR_VERSION_2,
};

static inline void da830_evm_init_mmc(void)
{
	int ret;

	ret = da8xx_pinmux_setup(da830_evm_mmc_sd_pins);
	if (ret) {
		pr_warning("da830_evm_init: mmc/sd mux setup failed: %d\n",
				ret);
		return;
	}

	ret = gpio_request(DA830_MMCSD_WP_PIN, "MMC WP");
	if (ret) {
		pr_warning("da830_evm_init: can not open GPIO %d\n",
			   DA830_MMCSD_WP_PIN);
		return;
	}
	gpio_direction_input(DA830_MMCSD_WP_PIN);

	ret = gpio_request(DA830_MMCSD_CD_PIN, "MMC CD\n");
	if (ret) {
		pr_warning("da830_evm_init: can not open GPIO %d\n",
			   DA830_MMCSD_CD_PIN);
		gpio_free(DA830_MMCSD_WP_PIN);
		return;
	}
	gpio_direction_input(DA830_MMCSD_CD_PIN);

	ret = da8xx_register_mmcsd0(&da830_evm_mmc_config);
	if (ret) {
		pr_warning("da830_evm_init: mmc/sd registration failed: %d\n",
				ret);
		gpio_free(DA830_MMCSD_WP_PIN);
		gpio_free(DA830_MMCSD_CD_PIN);
	}
}

/* Onkyo NAND: 8-bit, ALE on EMA_A1, CLE on EMA_A2, CS3 */
static const short da830_evm_emif25_pins[] = {
	DA830_EMA_D_0, DA830_EMA_D_1, DA830_EMA_D_2, DA830_EMA_D_3,
	DA830_EMA_D_4, DA830_EMA_D_5, DA830_EMA_D_6, DA830_EMA_D_7,
	DA830_EMA_A_1, DA830_EMA_A_2, DA830_NEMA_WE, DA830_NEMA_CS_3,
	DA830_NEMA_OE, DA830_EMA_WAIT_0,
	-1
};

#if defined(CONFIG_MMC_DAVINCI) || defined(CONFIG_MMC_DAVINCI_MODULE)
#define HAS_MMC	1
#else
#define HAS_MMC	0
#endif

#ifdef CONFIG_DA830_UI_NAND
/*
 * OSIRIS flash layout, shared with dspboot and U-Boot (MTDPARTS_DEFAULT):
 * everything after block 1 is 256 KiB aligned. UBI holds the kernel and
 * rootfs volumes of both slots plus the data volume.
 */
static struct mtd_partition da830_evm_nand_partitions[] = {
	[0] = {
		.name		= "env",
		.offset		= 0,
		.size		= SZ_16K,
	},
	[1] = {
		.name		= "dspboot",	/* ROM boots it from block 1 */
		.offset		= MTDPART_OFS_APPEND,
		.size		= SZ_1M - SZ_16K,
		.mask_flags	= MTD_WRITEABLE,
	},
	[2] = {
		.name		= "env2",
		.offset		= SZ_1M,
		.size		= SZ_256K,
	},
	[3] = {
		.name		= "uboot_a",
		.offset		= MTDPART_OFS_APPEND,
		.size		= SZ_512K,
	},
	[4] = {
		.name		= "uboot_b",
		.offset		= MTDPART_OFS_APPEND,
		.size		= SZ_512K,
	},
	[5] = {
		.name		= "ubi",
		.offset		= MTDPART_OFS_APPEND,
		.size		= MTDPART_SIZ_FULL,
	},
};

/* Stock Onkyo firmware /proc/mtd, for reference:
dev:    size   erasesize  name
mtd0: 00004000 00004000 "params"
mtd1: 00010000 00004000 "2ndbl"
mtd2: 00040000 00004000 "bootloader"
mtd3: 00200000 00004000 "dsp1"
mtd4: 00200000 00004000 "dsp2"
mtd5: 00200000 00004000 "kernel"
mtd6: 00004000 00004000 "bootconf1"
mtd7: 00004000 00004000 "bootconf2"
mtd8: 01000000 00004000 "filesystem1"
mtd9: 01000000 00004000 "filesystem2"
mtd10: 01000000 00004000 "filesystem3"
mtd11: 00500000 00004000 "filesystem4"
mtd12: 004a4000 00004000 "filesystem5"
 */

/* flash bbt decriptors */
static uint8_t da830_evm_nand_bbt_pattern[] = { 'B', 'b', 't', '0' };
static uint8_t da830_evm_nand_mirror_pattern[] = { '1', 't', 'b', 'B' };

static struct nand_bbt_descr da830_evm_nand_bbt_main_descr = {
	.options	= NAND_BBT_LASTBLOCK | NAND_BBT_CREATE |
			  NAND_BBT_WRITE | NAND_BBT_2BIT |
			  NAND_BBT_VERSION | NAND_BBT_PERCHIP,
	.offs		= 0,
	.len		= 4,
	.veroffs	= 4,
	.maxblocks	= 4,
	.pattern	= da830_evm_nand_bbt_pattern
};

static struct nand_bbt_descr da830_evm_nand_bbt_mirror_descr = {
	.options	= NAND_BBT_LASTBLOCK | NAND_BBT_CREATE |
			  NAND_BBT_WRITE | NAND_BBT_2BIT |
			  NAND_BBT_VERSION | NAND_BBT_PERCHIP,
	.offs		= 0,
	.len		= 4,
	.veroffs	= 4,
	.maxblocks	= 4,
	.pattern	= da830_evm_nand_mirror_pattern
};

static struct davinci_aemif_timing da830_evm_nandflash_timing = {
	.wsetup         = 24,
	.wstrobe        = 21,
	.whold          = 14,
	.rsetup         = 19,
	.rstrobe        = 50,
	.rhold          = 0,
	.ta             = 20,
};

static struct davinci_nand_pdata da830_evm_nand_pdata = {
	.parts		= da830_evm_nand_partitions,
	.nr_parts	= ARRAY_SIZE(da830_evm_nand_partitions),
	.ecc_mode	= NAND_ECC_HW,
	.ecc_bits	= 4,
	.options	= NAND_USE_FLASH_BBT,
	.bbt_td		= &da830_evm_nand_bbt_main_descr,
	.bbt_md		= &da830_evm_nand_bbt_mirror_descr,
	.timing         = &da830_evm_nandflash_timing,
};

static struct resource da830_evm_nand_resources[] = {
	[0] = {		/* First memory resource is NAND I/O window */
		.start	= DA830_EMIF25_ASYNC_DATA_CE3_BASE,
		.end	= DA830_EMIF25_ASYNC_DATA_CE3_BASE + PAGE_SIZE - 1,
		.flags	= IORESOURCE_MEM,
	},
	[1] = {		/* Second memory resource is AEMIF control registers */
		.start	= DA830_EMIF25_CONTROL_BASE,
		.end	= DA830_EMIF25_CONTROL_BASE + SZ_32K - 1,
		.flags	= IORESOURCE_MEM,
	},
};

static struct platform_device da830_evm_nand_device = {
	.name		= "davinci_nand",
	.id		= 1,
	.dev		= {
		.platform_data	= &da830_evm_nand_pdata,
	},
	.num_resources	= ARRAY_SIZE(da830_evm_nand_resources),
	.resource	= da830_evm_nand_resources,
};

static inline void da830_evm_init_nand(int mux_mode)
{
	int ret;

	if (HAS_MMC) {
		pr_warning("WARNING: both MMC/SD and NAND are "
				"enabled, but they share AEMIF pins.\n"
				"\tDisable MMC/SD for NAND support.\n");
		return;
	}

	ret = da8xx_pinmux_setup(da830_evm_emif25_pins);
	if (ret)
		pr_warning("da830_evm_init: emif25 mux setup failed: %d\n",
				ret);

	ret = platform_device_register(&da830_evm_nand_device);
	if (ret)
		pr_warning("da830_evm_init: NAND device not registered.\n");

}
#else
static inline void da830_evm_init_nand(int mux_mode) { }
#endif

static struct mtd_partition spi_flash_partitions[] = {
	[0] = {
		.name = "U-Boot",
		.offset = 0,
		.size = SZ_256K,
		.mask_flags = MTD_WRITEABLE,
	},
	[1] = {
		.name = "U-Boot Environment",
		.offset = MTDPART_OFS_APPEND,
		.size = SZ_16K,
		.mask_flags = MTD_WRITEABLE,
	},
	[2] = {
		.name = "Linux",
		.offset = MTDPART_OFS_NXTBLK,
		.size = MTDPART_SIZ_FULL,
		.mask_flags = 0,
	},
};

static struct flash_platform_data spi_flash_data = {
	.name = "m25p80",
	.parts = spi_flash_partitions,
	.nr_parts = ARRAY_SIZE(spi_flash_partitions),
	.type = "w25x32",
};

static struct spi_board_info da830_spi_board_info[] = {
	[0] = {
		.modalias = "spidev",
		.platform_data = NULL,
		.mode = SPI_MODE_0 | SPI_NO_CS,  /* Manual CS via GPIO */
		.max_speed_hz = 1000000,        /* 1 MHz - very safe for reverse engineering */
		.bus_num = 0,
		.chip_select = 0,
	},
};

#ifdef CONFIG_DA830_UI_LCD
static int da830_lcd_hw_init(void)
{
	void __iomem *cfg_mstpri2_base;
	u32 val;

	/*
	 * Reconfigure the LCDC priority to the highest to ensure that
	 * the throughput/latency requirements for the LCDC are met.
	 */
	cfg_mstpri2_base = DA8XX_SYSCFG0_VIRT(DA8XX_MSTPRI2_REG);

	val = __raw_readl(cfg_mstpri2_base);
	val &= 0x0fffffff;
	__raw_writel(val, cfg_mstpri2_base);

	return 0;
}

static const short da830_evm_lcdc_pins[] = {
	DA830_NLCD_AC_ENB_CS,
	-1
};

static inline void da830_evm_init_lcdc(int mux_mode,
				struct da8xx_lcdc_platform_data *pdata)
{
	int ret;

	ret = da8xx_pinmux_setup(da830_lcdcntl_pins);
	if (ret)
		pr_warning("da830_evm_init: lcdcntl mux setup failed: %d\n",
				ret);

#if !defined(CONFIG_FB_DA8XX) && !defined(CONFIG_FB_DA8XX_MODULE)
	ret = da8xx_pinmux_setup(da830_evm_lcdc_pins);
	if (ret)
		pr_warning("da830_evm_init: evm lcd mux setup failed: %d\n",
				ret);
#endif

	ret = da830_lcd_hw_init();
	if (ret)
		pr_warning("da830_evm_init: lcd hw init failed: %d\n", ret);

	ret = da8xx_register_lcdc(pdata);
	if (ret)
		pr_warning("da830_evm_init: lcd setup failed: %d\n", ret);

	gpio_direction_output(mux_mode, 0);
}
#else
static inline void da830_evm_init_lcdc(int mux_mode,
				struct da8xx_lcdc_platform_data *pdata) {}
#endif

static struct at24_platform_data da830_evm_i2c_eeprom_info = {
	.byte_len	= SZ_256K / 8,
	.page_size	= 64,
	.flags		= AT24_FLAG_ADDR16,
	.setup		= davinci_get_mac_addr,
	.context	= (void *)0x7f00,
};

#define DA830_EVM_TSC2004_ADDRESS	0x49
#define DA830_LCD_BACKLIGHT_PIN		GPIO_TO_PIN(2, 12)
#define DA830_LCD_AC_ENB_CS		GPIO_TO_PIN(1, 11)

static int __init da830_evm_ui_expander_setup(struct i2c_client *client,
		int gpio, unsigned ngpio, void *context)
{
	struct da8xx_lcdc_platform_data *pdata = &sharp_lcd035q3dg01_pdata;
	struct i2c_msg tsc_detect = {
		.addr = DA830_EVM_TSC2004_ADDRESS,
	};
	int ret;

	ret = davinci_cfg_reg(DA830_GPIO1_11);
	if (ret) {
		pr_warning("%s: PinMux setup for GPIO %d failed: %d\n",
		   __func__, DA830_LCD_AC_ENB_CS, ret);
		return ret;
	}

	ret = gpio_request(DA830_LCD_AC_ENB_CS, "LCD CS\n");
	if (ret) {
		pr_warning("da830_evm_init: can not open GPIO %d\n",
		   DA830_LCD_AC_ENB_CS);
		return ret;
	}

	gpio_direction_output(DA830_LCD_AC_ENB_CS, 1);

	gpio_request(gpio + 6, "UI MUX_MODE");

	/* Drive mux mode low to match the default without UI card */
	gpio_direction_output(gpio + 6, 0);

	/* check if the new UI card with touchscreen LCD is connected */
	ret = i2c_transfer(client->adapter, &tsc_detect, 1);
	if (ret == 1) {
		ret = davinci_cfg_reg(DA830_GPIO2_12);
		if (ret) {
			pr_warning("%s: PinMux setup for GPIO %d failed: %d\n",
			   __func__, DA830_LCD_BACKLIGHT_PIN, ret);
			gpio_free(DA830_LCD_AC_ENB_CS);
			return ret;
		}

		ret = gpio_request(DA830_LCD_BACKLIGHT_PIN, "LCD BL\n");
		if (ret) {
			pr_warning("da830_evm_init: can not open GPIO %d\n",
			   DA830_LCD_BACKLIGHT_PIN);
			gpio_free(DA830_LCD_AC_ENB_CS);
			return ret;
		}

		gpio_direction_output(DA830_LCD_BACKLIGHT_PIN, 1);

		pdata = &sharp_lk043t1dg01_pdata;
	}

	da830_evm_init_lcdc(gpio + 6, pdata);

	da830_evm_init_nand(gpio + 6);

	return 0;
}

static int da830_evm_ui_expander_teardown(struct i2c_client *client, int gpio,
		unsigned ngpio, void *context)
{
	gpio_free(gpio + 6);
	return 0;
}

static struct pcf857x_platform_data __initdata da830_evm_ui_expander_info = {
	.gpio_base	= DAVINCI_N_GPIO,
	.setup		= da830_evm_ui_expander_setup,
	.teardown	= da830_evm_ui_expander_teardown,
};

/*
 * TSC 2004 Support
 */
#define TSC2004_GPIO_IRQ_PIN	GPIO_TO_PIN(2, 8)

static int tsc2004_init_irq(void)
{
	int ret = 0;

	ret = gpio_request(TSC2004_GPIO_IRQ_PIN, "tsc2004-irq");
	if (ret < 0) {
		pr_warning("%s: failed to TSC2004 IRQ GPIO: %d\n",
								__func__, ret);
		return ret;
	}

	ret = davinci_cfg_reg(DA830_GPIO2_8);
	if (ret) {
		pr_warning("%s: PinMux setup for GPIO %d failed: %d\n",
			   __func__, TSC2004_GPIO_IRQ_PIN, ret);
		return ret;
	}

	gpio_direction_input(TSC2004_GPIO_IRQ_PIN);

	return ret;
}

static void tsc2004_exit_irq(void)
{
	gpio_free(TSC2004_GPIO_IRQ_PIN);
}

static int tsc2004_get_irq_level(void)
{
	return gpio_get_value(TSC2004_GPIO_IRQ_PIN) ? 0 : 1;
}

struct tsc2004_platform_data da830evm_tsc2004data = {
	.model = 2004,
	.x_plate_ohms = 180,
	.get_pendown_state = tsc2004_get_irq_level,
	.init_platform_hw = tsc2004_init_irq,
	.exit_platform_hw = tsc2004_exit_irq,
};

static struct i2c_board_info __initdata da830_evm_i2c_devices[] = {
	{
		I2C_BOARD_INFO("24c256", 0x50),
		.platform_data	= &da830_evm_i2c_eeprom_info,
	},
	{
		I2C_BOARD_INFO("tlv320aic3x", 0x18),
	},
	{
		I2C_BOARD_INFO("pcf8574", 0x3f),
		.platform_data	= &da830_evm_ui_expander_info,
	},
};

static struct i2c_board_info __initdata da830_evm_tsc2004_dev = {
	I2C_BOARD_INFO("tsc2004", DA830_EVM_TSC2004_ADDRESS),
	.platform_data  = &da830evm_tsc2004data,
};

static struct davinci_i2c_platform_data da830_evm_i2c_0_pdata = {
	.bus_freq	= 100,	/* kHz */
	.bus_delay	= 0,	/* usec */
};

static struct platform_device da830_dit_device = {
	.name = "spdif-dit",
	.id   = -1,
};

static __init void da830_evm_init(void)
{
	struct davinci_soc_info *soc_info = &davinci_soc_info;
	int ret;

	ret = da8xx_register_edma();
	if (ret)
		pr_warning("da830_evm_init: edma registration failed: %d\n",
				ret);

	ret = da8xx_pinmux_setup(da830_i2c0_pins);
	if (ret)
		pr_warning("da830_evm_init: i2c0 mux setup failed: %d\n",
				ret);

	ret = da8xx_register_i2c(0, &da830_evm_i2c_0_pdata);
	if (ret)
		pr_warning("da830_evm_init: i2c0 registration failed: %d\n",
				ret);

	da830_evm_usb_init();

	soc_info->emac_pdata->phy_mask = DA830_EVM_PHY_MASK;
	soc_info->emac_pdata->mdio_max_freq = DA830_EVM_MDIO_FREQUENCY;
	soc_info->emac_pdata->rmii_en = 1;

	ret = da8xx_pinmux_setup(da830_cpgmac_pins);
	if (ret)
		pr_warning("da830_evm_init: cpgmac mux setup failed: %d\n",
				ret);

	ret = da8xx_register_emac();
	if (ret)
		pr_warning("da830_evm_init: emac registration failed: %d\n",
				ret);

	ret = da8xx_register_watchdog();
	if (ret)
		pr_warning("da830_evm_init: watchdog registration failed: %d\n",
				ret);

	davinci_serial_init(&da830_evm_uart_config);

	ret = da8xx_pinmux_setup(da830_evm_mcasp1_pins);
	if (ret)
		pr_warning("da830_evm_init: mcasp1 mux setup failed: %d\n",
				ret);

	da8xx_register_mcasp(1, &da830_evm_snd_data);

	ret = platform_device_register(&da830_dit_device);
	if (ret)
		pr_warning("da830_evm_init: dit device registration failed: %d\n",
				ret);


	ret = da8xx_register_rtc();
	if (ret)
		pr_warning("da830_evm_init: rtc setup failed: %d\n", ret);

	ret = da8xx_pinmux_setup(onkyo_spi0_pins);
	if (ret)
		pr_warning("da830_evm_init: spi0 mux setup failed: %d\n",
				ret);

	ret = da8xx_pinmux_setup(onkyo_gpio_pins);
	if (ret)
		pr_warning("da830_evm_init: control line mux setup failed: %d\n",
				ret);

	/* Configure GPIO1_14 (GPIO30) as GPIO for manual SPI CS (ARM_CS) */
	ret = davinci_cfg_reg(DA830_GPIO1_14);
	if (ret)
		pr_warning("da830_evm_init: GPIO1_14 (ARM_CS) setup failed: %d\n",
				ret);

	/* Configure GPIO2_2 (GPIO34) as GPIO for ARM_RDY input */
	ret = davinci_cfg_reg(DA830_GPIO2_2);
	if (ret)
		pr_warning("da830_evm_init: GPIO2_2 (ARM_RDY) setup failed: %d\n",
				ret);

	da830_init_spi0(BIT(0), da830_spi_board_info,
			ARRAY_SIZE(da830_spi_board_info));

	da830_evm_init_nand(0);
}

#ifdef CONFIG_SERIAL_8250_CONSOLE
static int __init da830_evm_console_init(void)
{
	return add_preferred_console("ttyS", 2, "115200");
}
console_initcall(da830_evm_console_init);
#endif

static __init void da830_evm_irq_init(void)
{
	struct davinci_soc_info *soc_info = &davinci_soc_info;

	cp_intc_init((void __iomem *)DA8XX_CP_INTC_VIRT, DA830_N_CP_INTC_IRQ,
			soc_info->intc_irq_prios);
}

static void __init da830_evm_map_io(void)
{
	da830_init();
}

MACHINE_START(DAVINCI_DA830_EVM, "DaVinci DA830/OMAP-L137/AM17xx EVM")
	.phys_io	= IO_PHYS,
	.io_pg_offst	= (__IO_ADDRESS(IO_PHYS) >> 18) & 0xfffc,
	.boot_params	= (DA8XX_DDR_BASE + 0x100),
	.map_io		= da830_evm_map_io,
	.init_irq	= da830_evm_irq_init,
	.timer		= &davinci_timer,
	.init_machine	= da830_evm_init,
MACHINE_END
