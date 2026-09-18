/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Awinic AW88263S register definitions
 *
 * The register layout is based on the AW88263S datasheet, revision 1.3,
 * and the vendor AW_PID_2032_REG_H definitions.
 */

#ifndef __AW88263S_H__
#define __AW88263S_H__

#include <linux/bits.h>

#define AW88263S_CHIP_ID			0x2032

#define AW88263S_ID_REG			0x00
#define AW88263S_SYSST_REG			0x01
#define AW88263S_SYSINT_REG			0x02
#define AW88263S_SYSINTM_REG			0x03
#define AW88263S_SYSCTRL_REG			0x04
#define AW88263S_SYSCTRL2_REG			0x05
#define AW88263S_I2SCTRL_REG			0x06
#define AW88263S_I2SCFG1_REG			0x07
#define AW88263S_I2SCFG2_REG			0x08
#define AW88263S_VBAT_REG			0x12
#define AW88263S_TEMP_REG			0x13
#define AW88263S_PVDD_REG			0x14
#define AW88263S_PLLCTRL1_REG			0x66
#define AW88263S_VTMCTRL3_REG			0x56
#define AW88263S_EFRH_REG			0x78
#define AW88263S_EFRM1_REG			0x7a
#define AW88263S_REG_MAX			0x7e

#define AW88263S_SYSST_BSTS			BIT(9)
#define AW88263S_SYSST_SWS			BIT(8)
#define AW88263S_SYSST_NOCLKS			BIT(5)
#define AW88263S_SYSST_CLKS			BIT(4)
#define AW88263S_SYSST_OCDS			BIT(3)
#define AW88263S_SYSST_OTHS			BIT(1)
#define AW88263S_SYSST_PLLS			BIT(0)
#define AW88263S_SYSST_CHECK			(AW88263S_SYSST_BSTS | \
						 AW88263S_SYSST_SWS | \
						 AW88263S_SYSST_CLKS | \
						 AW88263S_SYSST_PLLS)
#define AW88263S_SYSST_FAULTS			(BIT(15) | BIT(14) | BIT(11) | \
						 BIT(10) | AW88263S_SYSST_NOCLKS | \
						 AW88263S_SYSST_OCDS | AW88263S_SYSST_OTHS)

#define AW88263S_SYSCTRL_ULS_MODE		BIT(15)
#define AW88263S_SYSCTRL_INTMODE		BIT(9)
#define AW88263S_SYSCTRL_INTN			BIT(8)
#define AW88263S_SYSCTRL_RCV_MODE		BIT(7)
#define AW88263S_SYSCTRL_I2SEN			BIT(6)
#define AW88263S_SYSCTRL_WSINV			BIT(5)
#define AW88263S_SYSCTRL_BCKINV			BIT(4)
#define AW88263S_SYSCTRL_IPLL			BIT(3)
#define AW88263S_SYSCTRL_AMPPD			BIT(1)
#define AW88263S_SYSCTRL_PWDN			BIT(0)

#define AW88263S_SYSCTRL2_VOL			GENMASK(15, 6)
#define AW88263S_SYSCTRL2_HDCCE			BIT(5)
#define AW88263S_SYSCTRL2_HMUTE			BIT(4)

#define AW88263S_I2SCTRL_INPLEV			BIT(13)
#define AW88263S_I2SCTRL_I2SRXEN		BIT(12)
#define AW88263S_I2SCTRL_CHSEL			GENMASK(11, 10)
#define AW88263S_I2SCTRL_I2SMD			GENMASK(9, 8)
#define AW88263S_I2SCTRL_I2SFS			GENMASK(7, 6)
#define AW88263S_I2SCTRL_I2SBCK			GENMASK(5, 4)
#define AW88263S_I2SCTRL_I2SSR			GENMASK(3, 0)

#define AW88263S_I2SCFG1_TX_SLOT			GENMASK(15, 12)
#define AW88263S_I2SCFG1_RXL_SLOT		GENMASK(11, 8)
#define AW88263S_I2SCFG1_TX_DATA			GENMASK(7, 6)
#define AW88263S_I2SCFG1_DRVSTREN		BIT(5)
#define AW88263S_I2SCFG1_DOHZ			BIT(4)
#define AW88263S_I2SCFG1_FSYNC_TYPE		BIT(3)
#define AW88263S_I2SCFG1_I2SCHS			BIT(1)
#define AW88263S_I2SCFG1_I2STXEN			BIT(0)

#define AW88263S_I2SCFG2_RXR_SLOT		GENMASK(15, 12)
#define AW88263S_I2SCFG2_SLOT_NUM		GENMASK(7, 5)

#define AW88263S_PLLCTRL1_CCO_MUX		BIT(14)

#define AW88263S_MUTE_VOL			(90 * 8)
#define AW88263S_VOLUME_STEP			(6 * 8)
#define AW88263S_CTL_MIN_ATTEN			(60 * 8)
#define AW88263S_CTL_MAX_VOL			(AW88263S_CTL_MIN_ATTEN / 2)
#define AW88263S_CTL_DEFAULT_VOL		(AW88263S_CTL_MIN_ATTEN / 2)

#define AW88263S_SOFT_RESET			0x55aa
#define AW88263S_ACF_FILE			"aw882xx_pid_2032_acf.bin"
#define AW88263S_SYSST_RETRIES			10

#define AW88263S_RATES				(SNDRV_PCM_RATE_8000_48000 | \
						 SNDRV_PCM_RATE_96000)
#define AW88263S_FORMATS			(SNDRV_PCM_FMTBIT_S16_LE | \
						 SNDRV_PCM_FMTBIT_S24_LE | \
						 SNDRV_PCM_FMTBIT_S32_LE)

#define AW88263S_PROFILE_EXT(xname, profile_info, profile_get, profile_set) \
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,					\
	.name = xname,								\
	.info = profile_info,								\
	.get = profile_get,								\
	.put = profile_set,								\
}

enum aw88263s_i2s_mode {
	AW88263S_I2S_MODE_PHILIPS,
	AW88263S_I2S_MODE_MSB,
	AW88263S_I2S_MODE_LSB,
};

struct aw88263s {
	struct aw_device *aw_pa;
	struct mutex lock;
	struct gpio_desc *reset_gpio;
	struct regmap *regmap;
	struct aw_container *aw_cfg;

	unsigned int sr_value;
	unsigned int cco_mux_value;
	unsigned int fs_value;
	unsigned int bck_value;
	unsigned int bck_inv_value;
	unsigned int tdm_bck_value;
	unsigned int md_value;

	unsigned int slot_num_value;
	unsigned int tx_slot_value;
	unsigned int rxl_slot_value;
	unsigned int rxr_slot_value;
};

#endif /* __AW88263S_H__ */
