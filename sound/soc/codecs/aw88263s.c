// SPDX-License-Identifier: GPL-2.0-only
//
// ASoC driver for the Awinic AW88263S smart speaker amplifier.
//
// The register layout and power sequence follow the AW88263S datasheet
// (revision 1.3) and the vendor AW882XX PID 2032 driver.  The ACF container
// parser is shared with the other Awinic drivers in this directory.

#include <linux/bitfield.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/unaligned.h>

#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/tlv.h>

#include "aw88263s.h"
#include "aw88395/aw88395_data_type.h"
#include "aw88395/aw88395_device.h"

static const struct regmap_config aw88263s_regmap_config = {
	.reg_bits = 8,
	.val_bits = 16,
	.max_register = AW88263S_REG_MAX,
	.reg_format_endian = REGMAP_ENDIAN_LITTLE,
	.val_format_endian = REGMAP_ENDIAN_BIG,
};

static unsigned int aw88263s_db_to_reg(unsigned int value)
{
	value = min_t(unsigned int, value, AW88263S_MUTE_VOL);

	return ((((value / AW88263S_VOLUME_STEP) << 6) +
		 (value % AW88263S_VOLUME_STEP)) << 6);
}

static unsigned int aw88263s_reg_to_db(unsigned int value)
{
	value = FIELD_GET(AW88263S_SYSCTRL2_VOL, value);

	return ((value >> 6) * AW88263S_VOLUME_STEP) + (value & 0x3f);
}

static int aw88263s_set_volume(struct aw88263s *aw88263s, unsigned int value)
{
	return regmap_update_bits(aw88263s->regmap, AW88263S_SYSCTRL2_REG,
				 AW88263S_SYSCTRL2_VOL, aw88263s_db_to_reg(value));
}

static int aw88263s_set_mute(struct aw88263s *aw88263s, bool mute)
{
	int ret;

	if (mute) {
		ret = aw88263s_set_volume(aw88263s, AW88263S_MUTE_VOL);
		if (ret)
			return ret;

		return regmap_update_bits(aw88263s->regmap,
					  AW88263S_SYSCTRL2_REG,
					  AW88263S_SYSCTRL2_HMUTE,
					  AW88263S_SYSCTRL2_HMUTE);
	}

	ret = regmap_update_bits(aw88263s->regmap, AW88263S_SYSCTRL2_REG,
					AW88263S_SYSCTRL2_HMUTE, 0);
	if (ret)
		return ret;

	return aw88263s_set_volume(aw88263s,
				   aw88263s->aw_pa->volume_desc.ctl_volume);
}

static int aw88263s_set_power_down(struct aw88263s *aw88263s, bool power_down)
{
	return regmap_update_bits(aw88263s->regmap, AW88263S_SYSCTRL_REG,
				 AW88263S_SYSCTRL_PWDN,
				 power_down ? AW88263S_SYSCTRL_PWDN : 0);
}

static int aw88263s_set_amp_power_down(struct aw88263s *aw88263s,
					       bool power_down)
{
	return regmap_update_bits(aw88263s->regmap, AW88263S_SYSCTRL_REG,
				 AW88263S_SYSCTRL_AMPPD,
				 power_down ? AW88263S_SYSCTRL_AMPPD : 0);
}

static int aw88263s_set_i2s_tx(struct aw88263s *aw88263s, bool enable)
{
	return regmap_update_bits(aw88263s->regmap, AW88263S_I2SCFG1_REG,
				 AW88263S_I2SCFG1_I2STXEN,
				 enable ? AW88263S_I2SCFG1_I2STXEN : 0);
}

static void aw88263s_clear_int_status(struct aw88263s *aw88263s)
{
	unsigned int status;

	/* SYSINT is read-to-clear. Read twice, as done by the vendor driver. */
	regmap_read(aw88263s->regmap, AW88263S_SYSINT_REG, &status);
	regmap_read(aw88263s->regmap, AW88263S_SYSINT_REG, &status);
}

static int aw88263s_check_sysst(struct aw88263s *aw88263s)
{
	unsigned int status;
	int ret, i;

	for (i = 0; i < AW88263S_SYSST_RETRIES; i++) {
		ret = regmap_read(aw88263s->regmap, AW88263S_SYSST_REG, &status);
		if (ret)
			return ret;

		if (status & AW88263S_SYSST_FAULTS)
			dev_dbg(aw88263s->aw_pa->dev,
				"SYSST fault status: 0x%04x\n", status);

		if ((status & AW88263S_SYSST_CHECK) == AW88263S_SYSST_CHECK)
			return 0;

		usleep_range(2000, 2010);
	}

	return -ETIMEDOUT;
}

static int aw88263s_check_pll(struct aw88263s *aw88263s)
{
	unsigned int status;
	int ret, i;

	for (i = 0; i < AW88263S_SYSST_RETRIES; i++) {
		ret = regmap_read(aw88263s->regmap, AW88263S_SYSST_REG, &status);
		if (ret)
			return ret;

		if ((status & (AW88263S_SYSST_CLKS | AW88263S_SYSST_PLLS)) ==
		    (AW88263S_SYSST_CLKS | AW88263S_SYSST_PLLS))
			return 0;

		usleep_range(2000, 2010);
	}

	return -ETIMEDOUT;
}

static int aw88263s_configure_i2s(struct aw88263s *aw88263s)
{
	int ret;

	ret = regmap_update_bits(aw88263s->regmap, AW88263S_I2SCFG2_REG,
				 AW88263S_I2SCFG2_SLOT_NUM,
				 aw88263s->slot_num_value);
	if (ret)
		return ret;

	ret = regmap_update_bits(aw88263s->regmap, AW88263S_I2SCFG1_REG,
				 AW88263S_I2SCFG1_TX_SLOT,
				 aw88263s->tx_slot_value);
	if (ret)
		return ret;

	ret = regmap_update_bits(aw88263s->regmap, AW88263S_I2SCFG1_REG,
				 AW88263S_I2SCFG1_RXL_SLOT,
				 aw88263s->rxl_slot_value);
	if (ret)
		return ret;

	ret = regmap_update_bits(aw88263s->regmap, AW88263S_I2SCFG2_REG,
				 AW88263S_I2SCFG2_RXR_SLOT,
				 aw88263s->rxr_slot_value);
	if (ret)
		return ret;

	ret = regmap_update_bits(aw88263s->regmap, AW88263S_PLLCTRL1_REG,
				 AW88263S_PLLCTRL1_CCO_MUX,
				 aw88263s->cco_mux_value);
	if (ret)
		return ret;

	ret = regmap_update_bits(aw88263s->regmap, AW88263S_I2SCTRL_REG,
				 AW88263S_I2SCTRL_I2SSR,
				 aw88263s->sr_value);
	if (ret)
		return ret;

	ret = regmap_update_bits(aw88263s->regmap, AW88263S_I2SCTRL_REG,
				 AW88263S_I2SCTRL_I2SBCK,
				 aw88263s->tdm_bck_value != UINT_MAX ?
				 aw88263s->tdm_bck_value : aw88263s->bck_value);
	if (ret)
		return ret;

	ret = regmap_update_bits(aw88263s->regmap, AW88263S_I2SCTRL_REG,
				 AW88263S_I2SCTRL_I2SFS, aw88263s->fs_value);
	if (ret)
		return ret;

	ret = regmap_update_bits(aw88263s->regmap, AW88263S_I2SCTRL_REG,
				 AW88263S_I2SCTRL_I2SMD, aw88263s->md_value);
	if (ret)
		return ret;

	ret = regmap_update_bits(aw88263s->regmap, AW88263S_SYSCTRL_REG,
				 AW88263S_SYSCTRL_BCKINV,
				 aw88263s->bck_inv_value);
	if (ret)
		return ret;

	return regmap_update_bits(aw88263s->regmap, AW88263S_SYSCTRL_REG,
				 AW88263S_SYSCTRL_I2SEN,
				 AW88263S_SYSCTRL_I2SEN);
}

static int aw88263s_load_profile(struct aw88263s *aw88263s);

static int aw88263s_start(struct aw88263s *aw88263s)
{
	int ret;

	if (aw88263s->aw_pa->status == AW88395_DEV_PW_ON)
		return 0;

	if (aw88263s->aw_pa->prof_cur != aw88263s->aw_pa->prof_index) {
		ret = aw88263s_load_profile(aw88263s);
		if (ret)
			return ret;
	}

	ret = aw88263s_set_i2s_tx(aw88263s, false);
	if (ret)
		return ret;

	ret = aw88263s_set_power_down(aw88263s, false);
	if (ret)
		return ret;
	usleep_range(2000, 2010);

	ret = aw88263s_configure_i2s(aw88263s);
	if (ret)
		goto power_down;

	ret = aw88263s_check_pll(aw88263s);
	if (ret)
		goto power_down;

	ret = aw88263s_set_amp_power_down(aw88263s, false);
	if (ret)
		goto power_down;
	usleep_range(1000, 1100);

	ret = aw88263s_check_sysst(aw88263s);
	if (ret)
		goto amp_power_down;

	ret = aw88263s_set_i2s_tx(aw88263s, true);
	if (ret)
		goto amp_power_down;

	ret = aw88263s_set_mute(aw88263s, false);
	if (ret)
		goto tx_disable;

	aw88263s->aw_pa->status = AW88395_DEV_PW_ON;
	return 0;

tx_disable:
	aw88263s_set_i2s_tx(aw88263s, false);
amp_power_down:
	aw88263s_set_amp_power_down(aw88263s, true);
power_down:
	aw88263s_set_power_down(aw88263s, true);
	return ret;
}

static int aw88263s_stop(struct aw88263s *aw88263s)
{
	int ret;

	if (aw88263s->aw_pa->status == AW88395_DEV_PW_OFF)
		return 0;

	ret = aw88263s_set_mute(aw88263s, true);
	if (ret)
		return ret;

	aw88263s_clear_int_status(aw88263s);
	ret = aw88263s_set_i2s_tx(aw88263s, false);
	if (ret)
		return ret;

	ret = aw88263s_set_amp_power_down(aw88263s, true);
	if (ret)
		return ret;
	usleep_range(1000, 1100);

	ret = aw88263s_set_power_down(aw88263s, true);
	if (!ret)
		aw88263s->aw_pa->status = AW88395_DEV_PW_OFF;

	return ret;
}

static int aw88263s_set_vcalb(struct aw88263s *aw88263s)
{
	unsigned int icalk_raw, vcalk_raw;
	int icalk, vcalk, vcalb;
	int ret;

	ret = regmap_read(aw88263s->regmap, AW88263S_EFRM1_REG, &icalk_raw);
	if (ret)
		return ret;

	ret = regmap_read(aw88263s->regmap, AW88263S_EFRH_REG, &vcalk_raw);
	if (ret)
		return ret;

	icalk = sign_extend32(icalk_raw & GENMASK(9, 0), 9) + 1000;
	vcalk = sign_extend32(vcalk_raw & GENMASK(9, 0), 9) + 1000;
	if (!vcalk)
		return -EINVAL;

	vcalb = (8192 * icalk) / vcalk;
	return regmap_write(aw88263s->regmap, AW88263S_VTMCTRL3_REG, vcalb);
}

static int aw88263s_load_profile(struct aw88263s *aw88263s)
{
	struct aw_device *aw_dev = aw88263s->aw_pa;
	struct aw_prof_desc *profile;
	struct aw_sec_data_desc *reg_data;
	unsigned int read_val;
	unsigned int init_volume;
	unsigned int reg, value;
	int ret, i;

	ret = aw88395_dev_get_prof_data(aw_dev, aw_dev->prof_index, &profile);
	if (ret)
		return ret;

	reg_data = &profile->sec_desc[AW88395_DATA_TYPE_REG];
	if (!reg_data->data || !reg_data->len || reg_data->len % 4)
		return -EINVAL;

	for (i = 0; i < reg_data->len; i += 4) {
		reg = get_unaligned_le16(reg_data->data + i);
		value = get_unaligned_le16(reg_data->data + i + 2);

		if (reg == AW88263S_SYSCTRL_REG) {
			ret = regmap_read(aw88263s->regmap, reg, &read_val);
			if (ret)
				return ret;

			value = (value & ~(AW88263S_SYSCTRL_PWDN |
					   AW88263S_SYSCTRL_AMPPD)) |
				 (read_val & (AW88263S_SYSCTRL_PWDN |
					      AW88263S_SYSCTRL_AMPPD));
		} else if (reg == AW88263S_SYSCTRL2_REG) {
			ret = regmap_read(aw88263s->regmap, reg, &read_val);
			if (ret)
				return ret;

			value = (value & ~AW88263S_SYSCTRL2_HMUTE) |
				(read_val & AW88263S_SYSCTRL2_HMUTE);
		} else if (reg == AW88263S_VTMCTRL3_REG) {
			/* This is derived from the per-chip calibration eFuse values. */
			continue;
		}

		ret = regmap_write(aw88263s->regmap, reg, value);
		if (ret)
			return ret;
	}

	ret = aw88263s_set_vcalb(aw88263s);
	if (ret)
		return ret;

	ret = regmap_read(aw88263s->regmap, AW88263S_SYSCTRL2_REG, &read_val);
	if (ret)
		return ret;

	init_volume = aw88263s_reg_to_db(read_val);
	aw_dev->volume_desc.init_volume = init_volume;
	ret = aw88263s_set_volume(aw88263s, aw_dev->volume_desc.mute_volume);
	if (ret)
		return ret;

	aw_dev->prof_cur = aw_dev->prof_index;
	return 0;
}

static int aw88263s_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct aw88263s *aw88263s = snd_soc_component_get_drvdata(dai->component);

	switch (fmt & SND_SOC_DAIFMT_INV_MASK) {
	case SND_SOC_DAIFMT_NB_NF:
		aw88263s->bck_inv_value = 0;
		break;
	case SND_SOC_DAIFMT_IB_NF:
		aw88263s->bck_inv_value = AW88263S_SYSCTRL_BCKINV;
		break;
	default:
		dev_err(aw88263s->aw_pa->dev, "unsupported clock inversion 0x%x\n",
			fmt & SND_SOC_DAIFMT_INV_MASK);
		return -EINVAL;
	}

	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
	case SND_SOC_DAIFMT_DSP_A:
		aw88263s->md_value = FIELD_PREP(AW88263S_I2SCTRL_I2SMD,
						AW88263S_I2S_MODE_PHILIPS);
		break;
	case SND_SOC_DAIFMT_MSB:
	case SND_SOC_DAIFMT_DSP_B:
		aw88263s->md_value = FIELD_PREP(AW88263S_I2SCTRL_I2SMD,
						AW88263S_I2S_MODE_MSB);
		break;
	case SND_SOC_DAIFMT_LSB:
		aw88263s->md_value = FIELD_PREP(AW88263S_I2SCTRL_I2SMD,
						AW88263S_I2S_MODE_LSB);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int aw88263s_hw_params(struct snd_pcm_substream *substream,
				      struct snd_pcm_hw_params *params,
				      struct snd_soc_dai *dai)
{
	struct aw88263s *aw88263s = snd_soc_component_get_drvdata(dai->component);

	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
		return 0;

	switch (params_rate(params)) {
	case 8000:
		aw88263s->sr_value = FIELD_PREP(AW88263S_I2SCTRL_I2SSR, 0);
		aw88263s->cco_mux_value = 0;
		break;
	case 11025:
		aw88263s->sr_value = FIELD_PREP(AW88263S_I2SCTRL_I2SSR, 1);
		aw88263s->cco_mux_value = AW88263S_PLLCTRL1_CCO_MUX;
		break;
	case 12000:
		aw88263s->sr_value = FIELD_PREP(AW88263S_I2SCTRL_I2SSR, 2);
		aw88263s->cco_mux_value = AW88263S_PLLCTRL1_CCO_MUX;
		break;
	case 16000:
		aw88263s->sr_value = FIELD_PREP(AW88263S_I2SCTRL_I2SSR, 3);
		aw88263s->cco_mux_value = 0;
		break;
	case 22050:
		aw88263s->sr_value = FIELD_PREP(AW88263S_I2SCTRL_I2SSR, 4);
		aw88263s->cco_mux_value = AW88263S_PLLCTRL1_CCO_MUX;
		break;
	case 24000:
		aw88263s->sr_value = FIELD_PREP(AW88263S_I2SCTRL_I2SSR, 5);
		aw88263s->cco_mux_value = AW88263S_PLLCTRL1_CCO_MUX;
		break;
	case 32000:
		aw88263s->sr_value = FIELD_PREP(AW88263S_I2SCTRL_I2SSR, 6);
		aw88263s->cco_mux_value = 0;
		break;
	case 44100:
		aw88263s->sr_value = FIELD_PREP(AW88263S_I2SCTRL_I2SSR, 7);
		aw88263s->cco_mux_value = AW88263S_PLLCTRL1_CCO_MUX;
		break;
	case 48000:
		aw88263s->sr_value = FIELD_PREP(AW88263S_I2SCTRL_I2SSR, 8);
		aw88263s->cco_mux_value = AW88263S_PLLCTRL1_CCO_MUX;
		break;
	case 96000:
		aw88263s->sr_value = FIELD_PREP(AW88263S_I2SCTRL_I2SSR, 9);
		aw88263s->cco_mux_value = AW88263S_PLLCTRL1_CCO_MUX;
		break;
	default:
		return -EINVAL;
	}

	switch (params_width(params)) {
	case 16:
		aw88263s->fs_value = FIELD_PREP(AW88263S_I2SCTRL_I2SFS, 0);
		break;
	case 20:
		aw88263s->fs_value = FIELD_PREP(AW88263S_I2SCTRL_I2SFS, 1);
		break;
	case 24:
		aw88263s->fs_value = FIELD_PREP(AW88263S_I2SCTRL_I2SFS, 2);
		break;
	case 32:
		aw88263s->fs_value = FIELD_PREP(AW88263S_I2SCTRL_I2SFS, 3);
		break;
	default:
		return -EINVAL;
	}

	switch (params_physical_width(params)) {
	case 16:
		aw88263s->bck_value = FIELD_PREP(AW88263S_I2SCTRL_I2SBCK, 0);
		break;
	case 20:
	case 24:
		aw88263s->bck_value = FIELD_PREP(AW88263S_I2SCTRL_I2SBCK, 1);
		break;
	case 32:
		aw88263s->bck_value = FIELD_PREP(AW88263S_I2SCTRL_I2SBCK, 2);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int aw88263s_set_tdm_slot(struct snd_soc_dai *dai,
					unsigned int tx_mask, unsigned int rx_mask,
					int slots, int slot_width)
{
	struct aw88263s *aw88263s = snd_soc_component_get_drvdata(dai->component);
	int tx_slot, rx_slot;

	if (!slots) {
		aw88263s->slot_num_value = FIELD_PREP(AW88263S_I2SCFG2_SLOT_NUM, 0);
		aw88263s->tdm_bck_value = UINT_MAX;
		aw88263s->tx_slot_value = FIELD_PREP(AW88263S_I2SCFG1_TX_SLOT, 0);
		aw88263s->rxl_slot_value = FIELD_PREP(AW88263S_I2SCFG1_RXL_SLOT, 0);
		aw88263s->rxr_slot_value = FIELD_PREP(AW88263S_I2SCFG2_RXR_SLOT, 1);
		return 0;
	}

	switch (slots) {
	case 1:
	case 2:
	case 4:
	case 6:
	case 8:
		aw88263s->slot_num_value = FIELD_PREP(AW88263S_I2SCFG2_SLOT_NUM,
				slots == 1 ? 1 : slots == 2 ? 2 : slots == 4 ? 3 :
				slots == 6 ? 4 : 5);
		break;
	default:
		return -EINVAL;
	}

	switch (slot_width) {
	case 16:
		aw88263s->tdm_bck_value = FIELD_PREP(AW88263S_I2SCTRL_I2SBCK, 0);
		break;
	case 20:
	case 24:
		aw88263s->tdm_bck_value = FIELD_PREP(AW88263S_I2SCTRL_I2SBCK, 1);
		break;
	case 32:
		aw88263s->tdm_bck_value = FIELD_PREP(AW88263S_I2SCTRL_I2SBCK, 2);
		break;
	default:
		return -EINVAL;
	}

	if (tx_mask) {
		tx_slot = __ffs(tx_mask);
		if (tx_slot >= slots || tx_slot > 7)
			return -EINVAL;
		aw88263s->tx_slot_value = FIELD_PREP(AW88263S_I2SCFG1_TX_SLOT,
				tx_slot);
	}

	if (rx_mask) {
		rx_slot = __ffs(rx_mask);
		if (rx_slot >= slots || rx_slot > 7)
			return -EINVAL;
		aw88263s->rxl_slot_value = FIELD_PREP(AW88263S_I2SCFG1_RXL_SLOT,
				rx_slot);

		if (rx_mask & ~BIT(rx_slot)) {
			rx_slot = __ffs(rx_mask & ~BIT(rx_slot));
			if (rx_slot >= slots || rx_slot > 7)
				return -EINVAL;
			aw88263s->rxr_slot_value = FIELD_PREP(AW88263S_I2SCFG2_RXR_SLOT,
					rx_slot);
		}
	}

	return 0;
}

static const struct snd_soc_dai_ops aw88263s_dai_ops = {
	.set_fmt = aw88263s_set_fmt,
	.hw_params = aw88263s_hw_params,
	.set_tdm_slot = aw88263s_set_tdm_slot,
};

static struct snd_soc_dai_driver aw88263s_dai = {
	.name = "aw88263s-aif",
	.playback = {
		.stream_name = "Speaker_Playback",
		.channels_min = 1,
		.channels_max = 2,
		.rates = AW88263S_RATES,
		.formats = AW88263S_FORMATS,
	},
	.capture = {
		.stream_name = "Speaker_Capture",
		.channels_min = 1,
		.channels_max = 2,
		.rates = AW88263S_RATES,
		.formats = AW88263S_FORMATS,
	},
	.ops = &aw88263s_dai_ops,
};

static int aw88263s_profile_info(struct snd_kcontrol *kcontrol,
				 struct snd_ctl_elem_info *uinfo)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct aw88263s *aw88263s = snd_soc_component_get_drvdata(component);
	char *name;
	int count, ret;

	uinfo->type = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
	uinfo->count = 1;
	count = aw88263s->aw_pa->prof_info.count;
	uinfo->value.enumerated.items = count;
	if (!count)
		return 0;
	if (uinfo->value.enumerated.item >= count)
		uinfo->value.enumerated.item = count - 1;

	ret = aw88395_dev_get_prof_name(aw88263s->aw_pa,
				uinfo->value.enumerated.item, &name);
	if (ret)
		return ret;

	strscpy(uinfo->value.enumerated.name, name);
	return 0;
}

static int aw88263s_profile_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct aw88263s *aw88263s = snd_soc_component_get_drvdata(component);

	ucontrol->value.integer.value[0] = aw88263s->aw_pa->prof_index;
	return 0;
}

static int aw88263s_profile_set(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct aw88263s *aw88263s = snd_soc_component_get_drvdata(component);
	int ret;

	mutex_lock(&aw88263s->lock);
	ret = aw88395_dev_set_profile_index(aw88263s->aw_pa,
			ucontrol->value.integer.value[0]);
	if (ret == -EPERM)
		ret = 0;
	else if (!ret && aw88263s->aw_pa->status == AW88395_DEV_PW_ON) {
		ret = aw88263s_stop(aw88263s);
		if (!ret)
			ret = aw88263s_start(aw88263s);
	}
	mutex_unlock(&aw88263s->lock);

	return ret ? ret : 1;
}

static int aw88263s_volume_get(struct snd_kcontrol *kcontrol,
				       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct aw88263s *aw88263s = snd_soc_component_get_drvdata(component);
	unsigned int attenuation = aw88263s->aw_pa->volume_desc.ctl_volume;

	if (attenuation >= AW88263S_CTL_MIN_ATTEN)
		ucontrol->value.integer.value[0] = 0;
	else
		ucontrol->value.integer.value[0] =
		(AW88263S_CTL_MIN_ATTEN - attenuation) / 2;
	return 0;
}

static int aw88263s_volume_set(struct snd_kcontrol *kcontrol,
				       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct aw88263s *aw88263s = snd_soc_component_get_drvdata(component);
	struct soc_mixer_control *mc = (struct soc_mixer_control *)
			kcontrol->private_value;
	unsigned int value = ucontrol->value.integer.value[0];

	if (value < mc->min || value > mc->max)
		return -EINVAL;

	value = AW88263S_CTL_MIN_ATTEN - (value * 2);
	if (value == aw88263s->aw_pa->volume_desc.ctl_volume)
		return 0;

	aw88263s->aw_pa->volume_desc.ctl_volume = value;
	return aw88263s_set_volume(aw88263s, value) ?: 1;
}

static const DECLARE_TLV_DB_SCALE(aw88263s_volume_tlv, -6000, 25, 0);

static const struct snd_kcontrol_new aw88263s_controls[] = {
	SOC_SINGLE_EXT_TLV("PCM Playback Volume", AW88263S_SYSCTRL2_REG,
			   6, AW88263S_CTL_MAX_VOL, 1,
			   aw88263s_volume_get, aw88263s_volume_set,
			   aw88263s_volume_tlv),
	AW88263S_PROFILE_EXT("Profile Set", aw88263s_profile_info,
			     aw88263s_profile_get, aw88263s_profile_set),
};

static int aw88263s_playback_event(struct snd_soc_dapm_widget *widget,
					   struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(widget->dapm);
	struct aw88263s *aw88263s = snd_soc_component_get_drvdata(component);
	int ret = 0;

	mutex_lock(&aw88263s->lock);
	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		ret = aw88263s_start(aw88263s);
		break;
	case SND_SOC_DAPM_POST_PMD:
		ret = aw88263s_stop(aw88263s);
		break;
	default:
		break;
	}
	mutex_unlock(&aw88263s->lock);

	return ret;
}

static const struct snd_soc_dapm_widget aw88263s_dapm_widgets[] = {
	SND_SOC_DAPM_AIF_IN_E("AIF_RX", "Speaker_Playback", 0, 0, 0, 0,
				      aw88263s_playback_event,
				      SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_OUTPUT("DAC Output"),
	SND_SOC_DAPM_AIF_OUT("AIF_TX", "Speaker_Capture", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_INPUT("ADC Input"),
};

static const struct snd_soc_dapm_route aw88263s_audio_map[] = {
	{"DAC Output", NULL, "AIF_RX"},
	{"AIF_TX", NULL, "ADC Input"},
};

static int aw88263s_request_firmware(struct aw88263s *aw88263s)
{
	const struct firmware *firmware;
	struct aw_container *container;
	const char *name;
	int ret;

	ret = device_property_read_string(aw88263s->aw_pa->dev,
					  "firmware-name", &name);
	if (ret)
		name = AW88263S_ACF_FILE;

	ret = request_firmware(&firmware, name, aw88263s->aw_pa->dev);
	if (ret)
		return dev_err_probe(aw88263s->aw_pa->dev, ret,
				     "failed to load %s\n", name);

	container = devm_kzalloc(aw88263s->aw_pa->dev,
				 struct_size(container, data, firmware->size), GFP_KERNEL);
	if (!container) {
		release_firmware(firmware);
		return -ENOMEM;
	}

	container->len = firmware->size;
	memcpy(container->data, firmware->data, firmware->size);
	release_firmware(firmware);
	aw88263s->aw_cfg = container;

	ret = aw88395_dev_load_acf_check(aw88263s->aw_pa, container);
	if (ret)
		return ret;

	ret = aw88395_dev_cfg_load(aw88263s->aw_pa, container);
	if (ret)
		return ret;

	aw88263s->aw_pa->prof_index = 0;
	aw88263s->aw_pa->prof_cur = 0;
	return aw88263s_load_profile(aw88263s);
}

static int aw88263s_component_probe(struct snd_soc_component *component)
{
	struct snd_soc_dapm_context *dapm = snd_soc_component_to_dapm(component);
	struct aw88263s *aw88263s = snd_soc_component_get_drvdata(component);
	int ret;

	ret = aw88263s_request_firmware(aw88263s);
	if (ret)
		return ret;

	ret = snd_soc_dapm_new_controls(dapm, aw88263s_dapm_widgets,
					ARRAY_SIZE(aw88263s_dapm_widgets));
	if (ret)
		return ret;

	ret = snd_soc_dapm_add_routes(dapm, aw88263s_audio_map,
				      ARRAY_SIZE(aw88263s_audio_map));
	if (ret)
		return ret;

	return snd_soc_add_component_controls(component, aw88263s_controls,
					      ARRAY_SIZE(aw88263s_controls));
}

static const struct snd_soc_component_driver aw88263s_component_driver = {
	.probe = aw88263s_component_probe,
};

static void aw88263s_hw_reset(struct aw88263s *aw88263s)
{
	if (!aw88263s->reset_gpio)
		return;

	gpiod_set_value_cansleep(aw88263s->reset_gpio, 0);
	usleep_range(1000, 1010);
	gpiod_set_value_cansleep(aw88263s->reset_gpio, 1);
	usleep_range(1000, 1010);
}

static int aw88263s_i2c_probe(struct i2c_client *i2c)
{
	struct aw88263s *aw88263s;
	struct aw_device *aw_dev;
	unsigned int chip_id;
	int ret;

	if (!i2c_check_functionality(i2c->adapter, I2C_FUNC_I2C))
		return -ENXIO;

	aw88263s = devm_kzalloc(&i2c->dev, sizeof(*aw88263s), GFP_KERNEL);
	if (!aw88263s)
		return -ENOMEM;

	aw88263s->slot_num_value = FIELD_PREP(AW88263S_I2SCFG2_SLOT_NUM, 0);
	aw88263s->sr_value = FIELD_PREP(AW88263S_I2SCTRL_I2SSR, 8);
	aw88263s->cco_mux_value = AW88263S_PLLCTRL1_CCO_MUX;
	aw88263s->fs_value = FIELD_PREP(AW88263S_I2SCTRL_I2SFS, 2);
	aw88263s->bck_value = FIELD_PREP(AW88263S_I2SCTRL_I2SBCK, 2);
	aw88263s->tdm_bck_value = UINT_MAX;
	aw88263s->tx_slot_value = FIELD_PREP(AW88263S_I2SCFG1_TX_SLOT, 0);
	aw88263s->rxl_slot_value = FIELD_PREP(AW88263S_I2SCFG1_RXL_SLOT, 0);
	aw88263s->rxr_slot_value = FIELD_PREP(AW88263S_I2SCFG2_RXR_SLOT, 1);
	aw88263s->md_value = FIELD_PREP(AW88263S_I2SCTRL_I2SMD,
					AW88263S_I2S_MODE_PHILIPS);
	aw88263s->bck_inv_value = 0;
	mutex_init(&aw88263s->lock);
	i2c_set_clientdata(i2c, aw88263s);

	ret = devm_regulator_get_enable_optional(&i2c->dev, "dvdd");
	if (ret && ret != -ENODEV)
		return dev_err_probe(&i2c->dev, ret, "failed to enable dvdd\n");

	aw88263s->reset_gpio = devm_gpiod_get_optional(&i2c->dev, "reset",
							 GPIOD_OUT_LOW);
	if (IS_ERR(aw88263s->reset_gpio))
		return PTR_ERR(aw88263s->reset_gpio);
	aw88263s_hw_reset(aw88263s);

	aw88263s->regmap = devm_regmap_init_i2c(i2c, &aw88263s_regmap_config);
	if (IS_ERR(aw88263s->regmap))
		return PTR_ERR(aw88263s->regmap);

	ret = regmap_read(aw88263s->regmap, AW88263S_ID_REG, &chip_id);
	if (ret)
		return ret;
	if (chip_id != AW88263S_CHIP_ID)
		return dev_err_probe(&i2c->dev, -ENODEV,
				    "unexpected chip ID 0x%04x\n", chip_id);

	aw_dev = devm_kzalloc(&i2c->dev, sizeof(*aw_dev), GFP_KERNEL);
	if (!aw_dev)
		return -ENOMEM;

	aw_dev->dev = &i2c->dev;
	aw_dev->i2c = i2c;
	aw_dev->regmap = aw88263s->regmap;
	aw_dev->chip_id = chip_id;
	aw_dev->status = AW88395_DEV_PW_OFF;
	aw_dev->fw_status = AW88395_DEV_FW_FAILED;
	aw_dev->prof_info.prof_desc = NULL;
	aw_dev->prof_info.count = 0;
	aw_dev->prof_info.prof_type = AW88395_DEV_NONE_TYPE_ID;
	aw_dev->volume_desc.ctl_volume = AW88263S_CTL_DEFAULT_VOL;
	aw_dev->volume_desc.mute_volume = AW88263S_MUTE_VOL;
	aw_dev->channel = 0;
	of_property_read_u32(i2c->dev.of_node, "awinic,audio-channel",
			     &aw_dev->channel);
	aw88263s->aw_pa = aw_dev;

	ret = devm_snd_soc_register_component(&i2c->dev,
					      &aw88263s_component_driver,
					      &aw88263s_dai, 1);
	if (ret)
		dev_err(&i2c->dev, "failed to register AW88263S: %d\n", ret);

	return ret;
}

static const struct i2c_device_id aw88263s_i2c_id[] = {
	{ "aw88263s", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, aw88263s_i2c_id);

static const struct of_device_id aw88263s_of_match[] = {
	{ .compatible = "awinic,aw88263s" },
	{ }
};
MODULE_DEVICE_TABLE(of, aw88263s_of_match);

static struct i2c_driver aw88263s_i2c_driver = {
	.driver = {
		.name = "aw88263s",
		.of_match_table = aw88263s_of_match,
	},
	.probe = aw88263s_i2c_probe,
	.id_table = aw88263s_i2c_id,
};
module_i2c_driver(aw88263s_i2c_driver);

MODULE_DESCRIPTION("ASoC AW88263S Smart PA Driver");
MODULE_LICENSE("GPL v2");
