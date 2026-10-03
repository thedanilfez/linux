// SPDX-License-Identifier: GPL-2.0-only
/*
 * Awinic AW8624 LRA haptics driver
 *
 * Based on the AWINIC driver, Copyright (c) 2018 AWINIC Technology CO., LTD.
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>

#define AW8624_ID			0x00
#define AW8624_CHIP_ID			0x24
#define AW8624_RESET			0xaa
#define AW8624_SYSINT			0x02
#define AW8624_SYSINT_FAULTS		(BIT(6) | BIT(5) | BIT(2) | BIT(1))
#define AW8624_SYSINTM			0x03
#define AW8624_SYSINTM_UVLO		BIT(5)
#define AW8624_SYSCTRL			0x04
#define AW8624_SYSCTRL_PLAY_MODE		GENMASK(3, 2)
#define AW8624_SYSCTRL_CONT		BIT(3)
#define AW8624_SYSCTRL_STANDBY		BIT(0)
#define AW8624_GO			0x05
#define AW8624_GO_ENABLE			BIT(0)
#define AW8624_DBGCTRL			0x20
#define AW8624_DBGCTRL_INT_MODE		GENMASK(3, 2)
#define AW8624_DBGCTRL_INT_EDGE		BIT(2)
#define AW8624_DBGCTRL_INT_ENABLE	BIT(5)
#define AW8624_DATCTRL			0x2b
#define AW8624_DATCTRL_LPF		BIT(5)
#define AW8624_DATCTRL_FC		GENMASK(7, 6)
#define AW8624_PWMPRC			0x2d
#define AW8624_PWMPRC_ENABLE		BIT(7)
#define AW8624_PWMDBG			0x2e
#define AW8624_PWMDBG_MODE		GENMASK(6, 5)
#define AW8624_PWMDBG_24K		BIT(6)
#define AW8624_WAVECTRL			0x31
#define AW8624_WAVECTRL_OVERDRIVE	GENMASK(7, 4)
#define AW8624_ANACTRL			0x38
#define AW8624_ANACTRL_LRA_REG		BIT(5)
#define AW8624_SW_BRAKE			0x39
#define AW8624_DATDBG			0x3b
#define AW8624_PRLVL			0x3e
#define AW8624_PRLVL_ENABLE		BIT(7)
#define AW8624_GLB_STATE			0x47
#define AW8624_GLB_STATE_MASK		GENMASK(3, 0)
#define AW8624_CONT_CTRL			0x48
#define AW8624_CONT_CTRL_ZC		BIT(7)
#define AW8624_CONT_CTRL_CLOSED		BIT(3)
#define AW8624_CONT_CTRL_F0_DETECT	BIT(2)
#define AW8624_CONT_CTRL_AUTO_BRAKE	BIT(0)
#define AW8624_F_PRE_H			0x49
#define AW8624_F_PRE_L			0x4a
#define AW8624_TD_H			0x4b
#define AW8624_TD_L			0x4c
#define AW8624_TSET			0x4d
#define AW8624_THRS_BRA_END		0x4f
#define AW8624_EF_RDATAH			0x55
#define AW8624_TRIM_LRA			0x5b
#define AW8624_R_SPARE			0x5d
#define AW8624_R_SPARE_ENABLE		BIT(7)
#define AW8624_D2SCFG			0x5e
#define AW8624_D2SCFG_GAIN		GENMASK(2, 0)
#define AW8624_DETCTRL			0x5f
#define AW8624_DETCTRL_PROTECT_OFF	BIT(5)
#define AW8624_ADCTEST			0x66
#define AW8624_ADCTEST_VBAT_COMP		BIT(6)
#define AW8624_F_LRA_F0_H		0x68
#define AW8624_ZC_THRSH_H		0x72
#define AW8624_ZC_THRSH_L		0x73
#define AW8624_BEMF_VTHH_H		0x74
#define AW8624_BEMF_VTHH_L		0x75
#define AW8624_BEMF_VTHL_H		0x76
#define AW8624_BEMF_VTHL_L		0x77
#define AW8624_BEMF_NUM			0x78
#define AW8624_BEMF_NUM_BRAKE		GENMASK(3, 0)
#define AW8624_TIME_NZC			0x7a
#define AW8624_DRV_LVL			0x7b
#define AW8624_DRV_LVL_OV		0x7c
#define AW8624_NUM_F0_1			0x7d
#define AW8624_NUM_F0_2			0x7e
#define AW8624_NUM_F0_3			0x7f

struct aw8624 {
	struct device *dev;
	struct regmap *regmap;
	struct gpio_desc *reset_gpio;
	/* Serialize playback, interrupt handling and power transitions. */
	struct mutex lock;
	struct work_struct work;
	unsigned int magnitude;
	bool suspended;
	u32 f0;
	u32 calibration_percent;
	u32 drive_level;
	u32 overdrive_level;
	u32 drive_time;
	u32 zero_cross_threshold;
	u32 brake_cycles;
	u32 tset;
	u32 bemf_upper;
	u32 bemf_lower;
	u32 f0_trace[4];
};

static int aw8624_stop(struct aw8624 *aw)
{
	unsigned int state;
	int ret;

	ret = regmap_write(aw->regmap, AW8624_GO, 0);
	if (ret)
		return ret;

	ret = regmap_read_poll_timeout(aw->regmap, AW8624_GLB_STATE, state,
				       !(state & AW8624_GLB_STATE_MASK),
				       2000, 200000);
	if (ret)
		return ret;

	ret = regmap_update_bits(aw->regmap, AW8624_SYSINTM,
				 AW8624_SYSINTM_UVLO, AW8624_SYSINTM_UVLO);
	if (ret)
		return ret;

	return regmap_update_bits(aw->regmap, AW8624_SYSCTRL,
				  AW8624_SYSCTRL_STANDBY, AW8624_SYSCTRL_STANDBY);
}

static int aw8624_active(struct aw8624 *aw)
{
	unsigned int status;
	int ret;

	ret = regmap_update_bits(aw->regmap, AW8624_SYSCTRL,
				 AW8624_SYSCTRL_PLAY_MODE | AW8624_SYSCTRL_STANDBY,
				 AW8624_SYSCTRL_CONT);
	if (ret)
		return ret;

	/* Clear the power-on undervoltage interrupt before unmasking it. */
	ret = regmap_read(aw->regmap, AW8624_SYSINT, &status);
	if (ret)
		return ret;

	return regmap_update_bits(aw->regmap, AW8624_SYSINTM,
				  AW8624_SYSINTM_UVLO, 0);
}

static int aw8624_calibrate(struct aw8624 *aw)
{
	unsigned int revision, d2scfg, state, f0, timeout;
	u8 data[2];
	int step, ret;

	ret = regmap_read(aw->regmap, AW8624_EF_RDATAH, &revision);
	if (ret)
		return ret;

	ret = regmap_read(aw->regmap, AW8624_D2SCFG, &d2scfg);
	if (ret)
		return ret;

	ret = regmap_update_bits(aw->regmap, AW8624_D2SCFG,
				 AW8624_D2SCFG_GAIN, AW8624_D2SCFG_GAIN);
	if (ret)
		return ret;

	ret = regmap_write(aw->regmap, AW8624_TRIM_LRA, 0);
	if (ret)
		return ret;

	ret = regmap_write(aw->regmap, AW8624_CONT_CTRL,
			   AW8624_CONT_CTRL_F0_DETECT);
	if (ret)
		return ret;

	ret = regmap_write(aw->regmap, AW8624_NUM_F0_1,
			   aw->f0_trace[0] << 4 | aw->f0_trace[1]);
	if (ret)
		return ret;

	ret = regmap_write(aw->regmap, AW8624_NUM_F0_2, aw->f0_trace[2]);
	if (ret)
		return ret;

	ret = regmap_write(aw->regmap, AW8624_NUM_F0_3, aw->f0_trace[3]);
	if (ret)
		return ret;

	ret = aw8624_active(aw);
	if (ret)
		return ret;

	ret = regmap_write(aw->regmap, AW8624_GO, AW8624_GO_ENABLE);
	if (ret)
		return ret;

	/* Allow the detection sequence to start before polling standby. */
	usleep_range(10000, 11000);
	timeout = DIV_ROUND_UP(10000000, aw->f0) *
		  (aw->f0_trace[0] + aw->f0_trace[1] +
		   (aw->f0_trace[3] + aw->f0_trace[1]) * aw->f0_trace[2]);
	ret = regmap_read_poll_timeout(aw->regmap, AW8624_GLB_STATE, state,
				       !(state & AW8624_GLB_STATE_MASK),
				       10000, timeout + 500000);
	if (ret)
		return ret;

	ret = regmap_bulk_read(aw->regmap, AW8624_F_LRA_F0_H, data, sizeof(data));
	if (ret)
		return ret;

	f0 = data[0] << 8 | data[1];
	if (!f0)
		return -EINVAL;

	/* The resonance period uses a fixed coefficient of 260. */
	f0 = 1000000000 / (f0 * 260);
	if (f0 * 100 < aw->f0 * (100 - aw->calibration_percent) ||
	    f0 * 100 > aw->f0 * (100 + aw->calibration_percent))
		return dev_err_probe(aw->dev, -ERANGE,
				     "Resonance outside calibration range: %u\n", f0);

	step = DIV_ROUND_CLOSEST(100000 * ((int)f0 - (int)aw->f0), (int)f0 * 250);
	if (revision & BIT(0)) {
		step &= 0x3f;
	} else {
		/* Older revisions encode the trim around a different midpoint. */
		step += 16;
		if (step < 16 || (step > 31 && step < 48))
			step += 16;
		else
			step -= 16;
	}

	ret = regmap_write(aw->regmap, AW8624_TRIM_LRA, step);
	if (ret)
		return ret;

	ret = regmap_write(aw->regmap, AW8624_D2SCFG, d2scfg);
	if (ret)
		return ret;

	return aw8624_stop(aw);
}

static int aw8624_init(struct aw8624 *aw)
{
	const struct reg_sequence config[] = {
		{ AW8624_SYSINTM, 0xff },
		{ AW8624_DATCTRL, AW8624_DATCTRL_FC | AW8624_DATCTRL_LPF },
		{ AW8624_DATDBG, 0x80 },
		{ AW8624_F_PRE_H, (1000000000 / (aw->f0 * 260)) >> 8 },
		{ AW8624_F_PRE_L, (1000000000 / (aw->f0 * 260)) & 0xff },
		{ AW8624_TD_H, aw->drive_time >> 8 },
		{ AW8624_TD_L, aw->drive_time & 0xff },
		{ AW8624_TSET, aw->tset },
		{ AW8624_ZC_THRSH_H, aw->zero_cross_threshold >> 8 },
		{ AW8624_ZC_THRSH_L, aw->zero_cross_threshold & 0xff },
		{ AW8624_TIME_NZC, 0x23 },
		{ AW8624_DRV_LVL, aw->drive_level },
		{ AW8624_DRV_LVL_OV, aw->overdrive_level },
		{ AW8624_SW_BRAKE, 0x2c },
		{ AW8624_THRS_BRA_END, 0 },
		{ AW8624_BEMF_VTHH_H, aw->bemf_upper >> 8 },
		{ AW8624_BEMF_VTHH_L, aw->bemf_upper & 0xff },
		{ AW8624_BEMF_VTHL_H, aw->bemf_lower >> 8 },
		{ AW8624_BEMF_VTHL_L, aw->bemf_lower & 0xff },
	};
	unsigned int id;
	int ret;

	gpiod_set_value_cansleep(aw->reset_gpio, 1);
	usleep_range(1000, 2000);
	gpiod_set_value_cansleep(aw->reset_gpio, 0);
	usleep_range(3500, 4000);

	ret = regmap_read(aw->regmap, AW8624_ID, &id);
	if (ret)
		return ret;
	if (id != AW8624_CHIP_ID)
		return -ENODEV;

	ret = regmap_write(aw->regmap, AW8624_ID, AW8624_RESET);
	if (ret)
		return ret;
	usleep_range(1000, 1500);

	ret = regmap_multi_reg_write(aw->regmap, config, ARRAY_SIZE(config));
	if (ret)
		return ret;

	ret = regmap_update_bits(aw->regmap, AW8624_PWMDBG,
				 AW8624_PWMDBG_MODE, AW8624_PWMDBG_24K);
	if (ret)
		return ret;

	ret = regmap_update_bits(aw->regmap, AW8624_DETCTRL,
				 AW8624_DETCTRL_PROTECT_OFF, AW8624_DETCTRL_PROTECT_OFF);
	if (ret)
		return ret;

	ret = regmap_update_bits(aw->regmap, AW8624_PWMPRC, AW8624_PWMPRC_ENABLE, 0);
	if (ret)
		return ret;

	ret = regmap_update_bits(aw->regmap, AW8624_PRLVL, AW8624_PRLVL_ENABLE, 0);
	if (ret)
		return ret;

	ret = regmap_update_bits(aw->regmap, AW8624_WAVECTRL,
				 AW8624_WAVECTRL_OVERDRIVE, 0);
	if (ret)
		return ret;

	ret = regmap_update_bits(aw->regmap, AW8624_BEMF_NUM,
				 AW8624_BEMF_NUM_BRAKE, aw->brake_cycles);
	if (ret)
		return ret;

	ret = regmap_update_bits(aw->regmap, AW8624_ADCTEST,
				 AW8624_ADCTEST_VBAT_COMP, AW8624_ADCTEST_VBAT_COMP);
	if (ret)
		return ret;

	ret = regmap_update_bits(aw->regmap, AW8624_ANACTRL,
				 AW8624_ANACTRL_LRA_REG, AW8624_ANACTRL_LRA_REG);
	if (ret)
		return ret;

	ret = regmap_update_bits(aw->regmap, AW8624_R_SPARE,
				 AW8624_R_SPARE_ENABLE, AW8624_R_SPARE_ENABLE);
	if (ret)
		return ret;

	ret = aw8624_calibrate(aw);
	if (ret)
		return ret;

	return regmap_write(aw->regmap, AW8624_CONT_CTRL,
			    AW8624_CONT_CTRL_ZC | AW8624_CONT_CTRL_CLOSED |
			    AW8624_CONT_CTRL_AUTO_BRAKE);
}

static void aw8624_work(struct work_struct *work)
{
	struct aw8624 *aw = container_of(work, struct aw8624, work);
	unsigned int magnitude, go;
	int ret;

	mutex_lock(&aw->lock);
	if (aw->suspended)
		goto out;

	magnitude = READ_ONCE(aw->magnitude);
	if (!magnitude) {
		ret = aw8624_stop(aw);
		if (ret)
			goto err;
		goto out;
	}

	ret = regmap_read(aw->regmap, AW8624_GO, &go);
	if (ret)
		goto err;

	ret = regmap_write(aw->regmap, AW8624_DRV_LVL,
			   DIV_ROUND_CLOSEST(magnitude * aw->drive_level, 0xffff));
	if (ret)
		goto err;

	ret = regmap_write(aw->regmap, AW8624_DRV_LVL_OV,
			   DIV_ROUND_CLOSEST(magnitude * aw->overdrive_level, 0xffff));
	if (ret)
		goto err;

	/* Strength updates must not retrigger startup and braking. */
	if (go & AW8624_GO_ENABLE)
		goto out;

	ret = aw8624_active(aw);
	if (ret)
		goto err;

	ret = regmap_write(aw->regmap, AW8624_GO, AW8624_GO_ENABLE);
	if (ret)
		goto err;
	goto out;

err:
	dev_err(aw->dev, "Failed to play effect: %d\n", ret);
out:
	mutex_unlock(&aw->lock);
}

static int aw8624_play(struct input_dev *input, void *data,
		       struct ff_effect *effect)
{
	struct aw8624 *aw = input_get_drvdata(input);

	WRITE_ONCE(aw->magnitude, max(effect->u.rumble.strong_magnitude,
				      effect->u.rumble.weak_magnitude));
	schedule_work(&aw->work);
	return 0;
}

static void aw8624_close(struct input_dev *input)
{
	struct aw8624 *aw = input_get_drvdata(input);
	int ret;

	cancel_work_sync(&aw->work);
	mutex_lock(&aw->lock);
	WRITE_ONCE(aw->magnitude, 0);
	ret = aw8624_stop(aw);
	mutex_unlock(&aw->lock);
	if (ret)
		dev_err(aw->dev, "Failed to stop haptics: %d\n", ret);
}

static irqreturn_t aw8624_irq(int irq, void *data)
{
	struct aw8624 *aw = data;
	unsigned int status;
	int ret;

	mutex_lock(&aw->lock);
	ret = regmap_read(aw->regmap, AW8624_SYSINT, &status);
	if (ret) {
		dev_err(aw->dev, "Failed to read interrupt status: %d\n", ret);
		goto out;
	}

	if (status & AW8624_SYSINT_FAULTS) {
		dev_err(aw->dev, "Haptic fault: %#x\n", status);
		ret = aw8624_stop(aw);
		if (ret)
			dev_err(aw->dev, "Failed to stop on fault: %d\n", ret);
	}
out:
	mutex_unlock(&aw->lock);
	return IRQ_HANDLED;
}

static int aw8624_read_properties(struct aw8624 *aw)
{
	struct device *dev = aw->dev;
	int ret;

	ret = device_property_read_u32(dev, "awinic,f0-preset", &aw->f0);
	if (ret)
		return ret;
	ret = device_property_read_u32(dev, "awinic,f0-calibration-percent",
				       &aw->calibration_percent);
	if (ret)
		return ret;
	ret = device_property_read_u32(dev, "awinic,drive-level", &aw->drive_level);
	if (ret)
		return ret;
	ret = device_property_read_u32(dev, "awinic,overdrive-level", &aw->overdrive_level);
	if (ret)
		return ret;
	ret = device_property_read_u32(dev, "awinic,drive-time", &aw->drive_time);
	if (ret)
		return ret;
	ret = device_property_read_u32(dev, "awinic,zero-cross-threshold",
				       &aw->zero_cross_threshold);
	if (ret)
		return ret;
	ret = device_property_read_u32(dev, "awinic,brake-cycles", &aw->brake_cycles);
	if (ret)
		return ret;
	ret = device_property_read_u32(dev, "awinic,tset", &aw->tset);
	if (ret)
		return ret;
	ret = device_property_read_u32(dev, "awinic,bemf-upper-threshold", &aw->bemf_upper);
	if (ret)
		return ret;
	ret = device_property_read_u32(dev, "awinic,bemf-lower-threshold", &aw->bemf_lower);
	if (ret)
		return ret;
	ret = device_property_read_u32_array(dev, "awinic,f0-trace-parameters",
					     aw->f0_trace, ARRAY_SIZE(aw->f0_trace));
	if (ret)
		return ret;

	if (aw->f0 < 1000 || aw->f0 > 3000 || aw->calibration_percent > 7 ||
	    aw->drive_level > 127 || aw->overdrive_level > 255 ||
	    aw->drive_time > 65535 || aw->zero_cross_threshold > 65535 ||
	    aw->brake_cycles > 7 || aw->tset > 255 ||
	    aw->bemf_upper > 65535 || aw->bemf_lower > 65535 ||
	    aw->f0_trace[0] > 15 || aw->f0_trace[1] > 15 ||
	    !aw->f0_trace[2] || aw->f0_trace[2] > 255 || aw->f0_trace[3] > 255)
		return -EINVAL;

	return 0;
}

static void aw8624_reset(void *data)
{
	struct aw8624 *aw = data;

	cancel_work_sync(&aw->work);
	gpiod_set_value_cansleep(aw->reset_gpio, 1);
}

static const struct regmap_config aw8624_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = AW8624_NUM_F0_3,
};

static int aw8624_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct input_dev *input;
	struct aw8624 *aw;
	int ret;

	aw = devm_kzalloc(dev, sizeof(*aw), GFP_KERNEL);
	if (!aw)
		return -ENOMEM;

	aw->dev = dev;
	mutex_init(&aw->lock);
	INIT_WORK(&aw->work, aw8624_work);
	i2c_set_clientdata(client, aw);

	ret = aw8624_read_properties(aw);
	if (ret)
		return dev_err_probe(dev, ret, "Invalid motor properties\n");

	aw->regmap = devm_regmap_init_i2c(client, &aw8624_regmap_config);
	if (IS_ERR(aw->regmap))
		return dev_err_probe(dev, PTR_ERR(aw->regmap), "Failed to create regmap\n");

	aw->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(aw->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(aw->reset_gpio), "Failed to get reset GPIO\n");

	ret = devm_add_action_or_reset(dev, aw8624_reset, aw);
	if (ret)
		return ret;

	ret = aw8624_init(aw);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to initialize haptics\n");

	ret = regmap_update_bits(aw->regmap, AW8624_DBGCTRL,
				 AW8624_DBGCTRL_INT_ENABLE | AW8624_DBGCTRL_INT_MODE,
				 AW8624_DBGCTRL_INT_ENABLE | AW8624_DBGCTRL_INT_EDGE);
	if (ret)
		return ret;

	ret = devm_request_threaded_irq(dev, client->irq, NULL, aw8624_irq,
					IRQF_ONESHOT, dev_name(dev), aw);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to request interrupt\n");

	ret = regmap_write(aw->regmap, AW8624_SYSINTM,
			   0xff & ~(AW8624_SYSINT_FAULTS & ~AW8624_SYSINTM_UVLO));
	if (ret)
		return ret;

	input = devm_input_allocate_device(dev);
	if (!input)
		return -ENOMEM;

	input->name = "aw8624-haptics";
	input->id.bustype = BUS_I2C;
	input->close = aw8624_close;
	input_set_drvdata(input, aw);
	input_set_capability(input, EV_FF, FF_RUMBLE);

	ret = input_ff_create_memless(input, NULL, aw8624_play);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to create force feedback\n");

	ret = input_register_device(input);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to register input device\n");

	return 0;
}

static int aw8624_suspend(struct device *dev)
{
	struct aw8624 *aw = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&aw->lock);
	aw->suspended = true;
	mutex_unlock(&aw->lock);
	cancel_work_sync(&aw->work);

	mutex_lock(&aw->lock);
	ret = aw8624_stop(aw);
	if (ret)
		aw->suspended = false;
	mutex_unlock(&aw->lock);

	return ret;
}

static int aw8624_resume(struct device *dev)
{
	struct aw8624 *aw = dev_get_drvdata(dev);

	mutex_lock(&aw->lock);
	WRITE_ONCE(aw->magnitude, 0);
	aw->suspended = false;
	mutex_unlock(&aw->lock);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(aw8624_pm_ops, aw8624_suspend, aw8624_resume);

static void aw8624_shutdown(struct i2c_client *client)
{
	struct aw8624 *aw = i2c_get_clientdata(client);

	disable_irq(client->irq);
	mutex_lock(&aw->lock);
	aw->suspended = true;
	mutex_unlock(&aw->lock);
	aw8624_reset(aw);
}

static const struct of_device_id aw8624_of_match[] = {
	{ .compatible = "awinic,aw8624" },
	{ }
};
MODULE_DEVICE_TABLE(of, aw8624_of_match);

static struct i2c_driver aw8624_driver = {
	.probe = aw8624_probe,
	.shutdown = aw8624_shutdown,
	.driver = {
		.name = "aw8624",
		.of_match_table = aw8624_of_match,
		.pm = pm_sleep_ptr(&aw8624_pm_ops),
	},
};
module_i2c_driver(aw8624_driver);

MODULE_DESCRIPTION("Awinic AW8624 LRA haptics driver");
MODULE_LICENSE("GPL");
