// SPDX-License-Identifier: GPL-2.0-only
/*
 * Will Semiconductor WL2866D 4-channel camera PMIC regulator driver
 *
 * The WL2866D provides two DVDD LDOs (N-MOS pass device, powered from VIN1
 * with VIN2 as bias) and two AVDD LDOs (P-MOS pass device, powered from VIN2).
 * The voltage selectors, the per-rail enable bits and the active discharge
 * configuration are all programmed over I2C.
 *
 * The device also has an active-high EN pin that force-enables every output at
 * its default voltage; it is held deasserted so that the rails can be
 * controlled individually through the I2C enable register.
 *
 * Copyright (c) 2026 thedanilfez <thedanilfezlol@gmail.com>
 */

#include <linux/bitops.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>

#define WL2866D_REG_REV			0x00
#define WL2866D_REG_DISCHARGE		0x02
#define WL2866D_REG_DVDD1_VOUT		0x03
#define WL2866D_REG_DVDD2_VOUT		0x04
#define WL2866D_REG_AVDD1_VOUT		0x05
#define WL2866D_REG_AVDD2_VOUT		0x06
#define WL2866D_REG_ENABLE		0x0e
#define WL2866D_REG_SEQ_CTRL		0x0f	/* last defined register */

/* 0x02: per-output discharge resistors, gated by a shared master bit. */
#define WL2866D_DISCHARGE_MASTER	BIT(7)
#define WL2866D_DISCHARGE_MASK(_id)	BIT(_id)

/* 0x0e: one software enable bit per output. */
#define WL2866D_ENABLE_MASK(_id)	BIT(_id)

#define WL2866D_DVDD_N_VOLTAGES		201	/* 0.6 .. 1.8 V */
#define WL2866D_DVDD_MIN_UV		600000
#define WL2866D_DVDD_STEP_UV		6000

#define WL2866D_AVDD_N_VOLTAGES		249	/* 1.2 .. 4.3 V */
#define WL2866D_AVDD_MIN_UV		1200000
#define WL2866D_AVDD_STEP_UV		12500

#define WL2866D_DVDD_ENABLE_TIME_US	500
#define WL2866D_AVDD_ENABLE_TIME_US	800

enum wl2866d_reg_id {
	WL2866D_ID_DVDD1,
	WL2866D_ID_DVDD2,
	WL2866D_ID_AVDD1,
	WL2866D_ID_AVDD2,
};

/*
 * 0x02[7] is a discharge master enable that gates every per-output bit, so it
 * cannot be expressed with the generic regulator_set_active_discharge_regmap()
 * helper. Set it together with the requesting rail; clearing a rail must leave
 * it alone because it is shared with the other outputs.
 */
static int wl2866d_set_active_discharge(struct regulator_dev *rdev, bool enable)
{
	unsigned int mask = WL2866D_DISCHARGE_MASK(rdev_get_id(rdev));

	if (enable)
		mask |= WL2866D_DISCHARGE_MASTER;

	return regmap_update_bits(rdev->regmap, WL2866D_REG_DISCHARGE, mask,
				  enable ? mask : 0);
}

static const struct regulator_ops wl2866d_regulator_ops = {
	.list_voltage		= regulator_list_voltage_linear,
	.map_voltage		= regulator_map_voltage_linear,
	.get_voltage_sel	= regulator_get_voltage_sel_regmap,
	.set_voltage_sel	= regulator_set_voltage_sel_regmap,
	.enable			= regulator_enable_regmap,
	.disable		= regulator_disable_regmap,
	.is_enabled		= regulator_is_enabled_regmap,
	.set_active_discharge	= wl2866d_set_active_discharge,
};

#define WL2866D_REGULATOR_DESC(_id, _name, _vsel, _supply, _min, _step, _n, _time) \
	[WL2866D_ID_##_id] = {						\
		.name		= "wl2866d-" #_name,			\
		.supply_name	= _supply,				\
		.of_match	= of_match_ptr(#_name),			\
		.regulators_node = of_match_ptr("regulators"),		\
		.id		= WL2866D_ID_##_id,			\
		.ops		= &wl2866d_regulator_ops,		\
		.type		= REGULATOR_VOLTAGE,			\
		.owner		= THIS_MODULE,				\
		.n_voltages	= _n,					\
		.min_uV		= _min,					\
		.uV_step	= _step,				\
		.vsel_reg	= _vsel,				\
		.vsel_mask	= GENMASK(7, 0),			\
		.enable_reg	= WL2866D_REG_ENABLE,			\
		.enable_mask	= WL2866D_ENABLE_MASK(WL2866D_ID_##_id), \
		.enable_time	= _time,				\
	}

static const struct regulator_desc wl2866d_regulators[] = {
	WL2866D_REGULATOR_DESC(DVDD1, dvdd1, WL2866D_REG_DVDD1_VOUT, "vin1",
			       WL2866D_DVDD_MIN_UV, WL2866D_DVDD_STEP_UV,
			       WL2866D_DVDD_N_VOLTAGES,
			       WL2866D_DVDD_ENABLE_TIME_US),
	WL2866D_REGULATOR_DESC(DVDD2, dvdd2, WL2866D_REG_DVDD2_VOUT, "vin1",
			       WL2866D_DVDD_MIN_UV, WL2866D_DVDD_STEP_UV,
			       WL2866D_DVDD_N_VOLTAGES,
			       WL2866D_DVDD_ENABLE_TIME_US),
	WL2866D_REGULATOR_DESC(AVDD1, avdd1, WL2866D_REG_AVDD1_VOUT, "vin2",
			       WL2866D_AVDD_MIN_UV, WL2866D_AVDD_STEP_UV,
			       WL2866D_AVDD_N_VOLTAGES,
			       WL2866D_AVDD_ENABLE_TIME_US),
	WL2866D_REGULATOR_DESC(AVDD2, avdd2, WL2866D_REG_AVDD2_VOUT, "vin2",
			       WL2866D_AVDD_MIN_UV, WL2866D_AVDD_STEP_UV,
			       WL2866D_AVDD_N_VOLTAGES,
			       WL2866D_AVDD_ENABLE_TIME_US),
};

/*
 * The reserved registers have no known function, so keep the driver off them.
 * The sequencer registers (0x0a, 0x0b, 0x0f) are not blocked but are never
 * written: their reset value already leaves each output under its per-rail
 * enable bit, which is what the regulator core expects.
 */
static const struct regmap_range wl2866d_no_reg_ranges[] = {
	regmap_reg_range(0x01, 0x01),
	regmap_reg_range(0x07, 0x09),
	regmap_reg_range(0x0c, 0x0d),
};

static const struct regmap_access_table wl2866d_no_reg_table = {
	.no_ranges	= wl2866d_no_reg_ranges,
	.n_no_ranges	= ARRAY_SIZE(wl2866d_no_reg_ranges),
};

static const struct regmap_config wl2866d_regmap_config = {
	.reg_bits	= 8,
	.val_bits	= 8,
	.max_register	= WL2866D_REG_SEQ_CTRL,
	.rd_table	= &wl2866d_no_reg_table,
	.wr_table	= &wl2866d_no_reg_table,
};

static int wl2866d_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct regulator_config config = { };
	struct gpio_desc *en;
	struct regmap *regmap;
	unsigned int rev;
	int ret, i;

	/*
	 * VIN2 powers the control logic and the I2C interface and is the bias
	 * supply of the DVDD LDOs, so it has to be on before the chip can be
	 * accessed at all. VIN1, the DVDD power input, is enabled on demand by
	 * the regulator core through desc.supply_name.
	 */
	ret = devm_regulator_get_enable(dev, "vin2");
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable vin2 supply\n");

	regmap = devm_regmap_init_i2c(client, &wl2866d_regmap_config);
	if (IS_ERR(regmap))
		return dev_err_probe(dev, PTR_ERR(regmap),
				     "failed to init regmap\n");

	/*
	 * EN is active high and forces every output on at its default voltage.
	 * Request it deasserted so the chip is under I2C control, and never
	 * assert it: doing so would defeat per-rail control.
	 */
	en = devm_gpiod_get_optional(dev, "enable", GPIOD_OUT_LOW);
	if (IS_ERR(en))
		return dev_err_probe(dev, PTR_ERR(en),
				     "failed to get enable GPIO\n");

	/* Chip_REV is informational only: the datasheet defines no value. */
	ret = regmap_read(regmap, WL2866D_REG_REV, &rev);
	if (ret)
		dev_dbg(dev, "failed to read revision: %d\n", ret);
	else
		dev_info(dev, "chip revision 0x%02x\n", rev);

	config.dev = dev;
	config.regmap = regmap;

	for (i = 0; i < ARRAY_SIZE(wl2866d_regulators); i++) {
		struct regulator_dev *rdev;

		rdev = devm_regulator_register(dev, &wl2866d_regulators[i],
					       &config);
		if (IS_ERR(rdev))
			return dev_err_probe(dev, PTR_ERR(rdev),
					     "failed to register %s\n",
					     wl2866d_regulators[i].name);
	}

	return 0;
}

static const struct of_device_id wl2866d_of_match[] = {
	{ .compatible = "willsemi,wl2866d" },
	{ }
};
MODULE_DEVICE_TABLE(of, wl2866d_of_match);

static const struct i2c_device_id wl2866d_i2c_id[] = {
	{ .name = "wl2866d" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, wl2866d_i2c_id);

static struct i2c_driver wl2866d_driver = {
	.driver = {
		.name		= "wl2866d",
		.of_match_table	= wl2866d_of_match,
		.probe_type	= PROBE_PREFER_ASYNCHRONOUS,
	},
	.probe		= wl2866d_probe,
	.id_table	= wl2866d_i2c_id,
};
module_i2c_driver(wl2866d_driver);

MODULE_DESCRIPTION("Will Semiconductor WL2866D regulator driver");
MODULE_AUTHOR("thedanilfez <thedanilfezlol@gmail.com>");
MODULE_LICENSE("GPL");
