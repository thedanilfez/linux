// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026 thedanilfez <thedanilfezlol@gmail.com>
 *
 * Qualcomm PM6150 QG fuel gauge driver.
 *
 */

#include <linux/iio/consumer.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/pm_wakeirq.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/rtc.h>
#include <linux/unaligned.h>

#include "qcom_qg_math.h"

/* offsets from downstream */
#define QG_SUBTYPE		0x05
#define QG_STATUS1		0x08
#define QG_OK			BIT(7)
#define QG_BATTERY_PRESENT	BIT(0)
#define QG_STATUS2		0x09
#define QG_GOOD_OCV		BIT(1)
#define QG_STATUS3		0x0a
#define QG_MASTER_CTL		0x41
#define QG_MASTER_HOLD		BIT(0)
#define QG_S2_CTL2		0x51
#define QG_S2_CTL3		0x52
#define QG_S3_IBAT_CTL1		0x5d
#define QG_PON_OCV_V		0x70
#define QG_GOOD_OCV_V		0x74
#define QG_AVG_V		0x80
#define QG_AVG_I		0x82
#define QG_ACCUM_V		0x88
#define QG_ACCUM_I		0x8b
#define QG_ACCUM_COUNT		0x8e
#define QG_FIFO_V		0x90
#define QG_FIFO_I		0xa0
#define QG_LAST_V		0xc0
#define QG_LAST_I		0xc2
#define QG_LAST_SLEEP_OCV	0xcc
#define QG_RESET_SAMPLE		0x8000

/* sdam */
#define QG_SDAM_VALID		0x46
#define QG_SDAM_SOC		0x47
#define QG_SDAM_TEMP		0x48
#define QG_SDAM_OCV		0x4c
#define QG_SDAM_TIME		0x54
#define QG_SDAM_FCC		0x68

#define QG_RECENT_STATE_SECONDS	360
#define QG_RECENT_TEMP_TOLERANCE	50

struct qcom_qg {
	struct device *dev;
	struct regmap *regmap;
	struct nvmem_device *sdam;
	struct iio_channel *therm;
	struct power_supply *psy;
	struct power_supply *charger;
	struct power_supply_battery_info *info;
	struct mutex lock;
	u32 base;
	int current_lsb; /* microamps * 1000 per raw code */
	int fifo_irq;
	int capacity;
	int cutoff_bp; /* physical SOC at displayed 0%, in 0.01% units */
	int saved_capacity;
	int full_uah;
	int fifo_period_ms;
	ktime_t last_fifo;
	int notified_voltage;
	int notified_current;
	int notified_temp;
	int notified_status;
	int notified_charge_uah;
	s64 charge_nah;
	s64 saved_charge_nah;
	bool full_latched;
	bool learned_full;
	bool lost_samples;
	bool notified;
	bool ocv_dirty;
};

static int qg_read_u16(struct qcom_qg *qg, unsigned int reg, u16 *value)
{
	u8 data[2];
	int ret;

	ret = regmap_bulk_read(qg->regmap, qg->base + reg, data, 2);
	if (!ret)
		*value = get_unaligned_le16(data);
	return ret;
}

static int qg_voltage(struct qcom_qg *qg, unsigned int reg, int *uv)
{
	u16 raw;
	int ret = qg_read_u16(qg, reg, &raw);

	if (ret)
		return ret;
	if (!raw || raw == QG_RESET_SAMPLE)
		return -ENODATA;
	*uv = div_u64((u64)raw * 194637, 1000);
	if (*uv < 2000000 || *uv > 5000000)
		return -ERANGE;
	return 0;
}

static int qg_current(struct qcom_qg *qg, unsigned int reg, int *ua)
{
	u16 raw;
	int ret = qg_read_u16(qg, reg, &raw);

	if (ret)
		return ret;
	if (raw == QG_RESET_SAMPLE)
		return -ENODATA;
	/* hardware current is negative while charging. */
	*ua = -div_s64((s64)(s16)raw * qg->current_lsb, 1000);
	return 0;
}

static int qg_temperature(struct qcom_qg *qg, int *deci_c)
{
	int milli_c, ret;

	ret = iio_read_channel_processed(qg->therm, &milli_c);
	if (!ret)
		*deci_c = DIV_ROUND_CLOSEST(milli_c, 100);
	return ret;
}

static int qg_ocv_capacity(struct qcom_qg *qg, int uv, int *capacity)
{
	int temp, ret;

	ret = qg_temperature(qg, &temp);
	if (ret)
		return ret;
	ret = power_supply_batinfo_ocv2cap(qg->info, uv,
						  DIV_ROUND_CLOSEST(temp, 10));
	if (ret < 0)
		return ret;
	*capacity = clamp(ret, 0, 100);
	return 0;
}

static int qg_capacity_ocv(struct qcom_qg *qg, int capacity, int *uv)
{
	const struct power_supply_battery_ocv_table *table;
	int temp, len, i, ret;

	if (capacity < 0 || capacity > 100)
		return -ERANGE;

	ret = qg_temperature(qg, &temp);
	if (ret)
		return ret;
	table = power_supply_find_ocv2cap_table(qg->info,
					DIV_ROUND_CLOSEST(temp, 10), &len);
	if (!table || len < 2)
		return -EINVAL;
	for (i = 1; i < len - 1 && capacity < table[i].capacity; i++)
		;
	if (table[i - 1].capacity == table[i].capacity) {
		*uv = table[i].ocv;
		return *uv >= 2000000 && *uv <= 5000000 ? 0 : -ERANGE;
	}
	*uv = table[i].ocv + div_s64((s64)(capacity - table[i].capacity) *
				      (table[i - 1].ocv - table[i].ocv),
				      table[i - 1].capacity - table[i].capacity);
	return *uv >= 2000000 && *uv <= 5000000 ? 0 : -ERANGE;
}

static int qg_rtc_seconds(u32 *seconds)
{
	struct rtc_device *rtc;
	struct rtc_time tm;
	time64_t time;
	int ret;

	rtc = rtc_class_open(CONFIG_RTC_HCTOSYS_DEVICE);
	if (!rtc)
		return -EPROBE_DEFER;
	ret = rtc_read_time(rtc, &tm);
	if (!ret)
		ret = rtc_valid_tm(&tm);
	rtc_class_close(rtc);
	if (ret)
		return ret;
	time = rtc_tm_to_time64(&tm);
	if (time < 0 || time > U32_MAX)
		return -ERANGE;
	*seconds = time;
	return 0;
}

static int qg_charger_property(struct qcom_qg *qg,
			       enum power_supply_property prop, int *value)
{
	union power_supply_propval val;
	int ret;

	ret = power_supply_get_property(qg->charger, prop, &val);
	if (!ret)
		*value = val.intval;
	return ret;
}

static int qg_status(struct qcom_qg *qg, int *status)
{
	int current_ua, ret;
	unsigned int qg_status1;

	ret = regmap_read(qg->regmap, qg->base + QG_STATUS1, &qg_status1);
	if (ret)
		return ret;
	if (!(qg_status1 & QG_BATTERY_PRESENT)) {
		*status = POWER_SUPPLY_STATUS_UNKNOWN;
		return 0;
	}

	ret = qg_charger_property(qg, POWER_SUPPLY_PROP_STATUS, status);
	if (ret)
		return ret;
	ret = qg_current(qg, QG_LAST_I, &current_ua);
	if (ret)
		return ret;
	if (current_ua < -20000)
		*status = POWER_SUPPLY_STATUS_DISCHARGING;
	else if (current_ua > 20000)
		*status = POWER_SUPPLY_STATUS_CHARGING;
	return 0;
}

static void qg_update_capacity(struct qcom_qg *qg)
{
	s64 full_nah = (s64)qg->full_uah * 1000;
	int raw_bp;

	qg->charge_nah = clamp_t(s64, qg->charge_nah, 0, full_nah);
	if (qg->full_latched && qg->charge_nah < full_nah * 99 / 100)
		qg->full_latched = false;
	raw_bp = qg_raw_soc_bp(qg->charge_nah, full_nah);
	qg->capacity = qg_usable_capacity(raw_bp, qg->cutoff_bp,
					 qg->full_latched);
}

static int qg_sdam_save(struct qcom_qg *qg)
{
	u8 data[4];
	int ret, model_ocv, temp;
	u32 seconds;

	if (qg->lost_samples ||
	    (qg->capacity == qg->saved_capacity && !qg->ocv_dirty &&
	     qg->charge_nah - qg->saved_charge_nah > -5000000 &&
	     qg->charge_nah - qg->saved_charge_nah < 5000000))
		return 0;
	ret = qg_capacity_ocv(qg,
		qg_raw_soc_percent(qg->charge_nah,
				   (u64)qg->full_uah * 1000), &model_ocv);
	if (ret)
		return ret;
	ret = qg_temperature(qg, &temp);
	if (ret)
		return ret;
	ret = qg_rtc_seconds(&seconds);
	if (ret)
		return ret;
	data[0] = 0;
	ret = nvmem_device_write(qg->sdam, QG_SDAM_VALID, 1, data);
	if (ret < 0)
		return ret;
	data[0] = qg->capacity;
	ret = nvmem_device_write(qg->sdam, QG_SDAM_SOC, 1, data);
	if (ret < 0)
		return ret;
	put_unaligned_le16((s16)temp, data);
	ret = nvmem_device_write(qg->sdam, QG_SDAM_TEMP, 2, data);
	if (ret < 0)
		return ret;
	put_unaligned_le32(model_ocv, data);
	ret = nvmem_device_write(qg->sdam, QG_SDAM_OCV, 4, data);
	if (ret < 0)
		return ret;
	put_unaligned_le32(seconds, data);
	ret = nvmem_device_write(qg->sdam, QG_SDAM_TIME, 4, data);
	if (ret < 0)
		return ret;
	data[0] = 1;
	ret = nvmem_device_write(qg->sdam, QG_SDAM_VALID, 1, data);
	if (ret >= 0) {
		qg->saved_capacity = qg->capacity;
		qg->saved_charge_nah = qg->charge_nah;
		qg->ocv_dirty = false;
	}
	return ret < 0 ? ret : 0;
}

static int qg_master_hold(struct qcom_qg *qg, bool hold)
{
	int ret;

	/* A 0->1 transition holds; clearing restarts the FIFO. */
	ret = regmap_update_bits(qg->regmap, qg->base + QG_MASTER_CTL,
				 QG_MASTER_HOLD, 0);
	if (!ret && hold)
		ret = regmap_update_bits(qg->regmap, qg->base + QG_MASTER_CTL,
					 QG_MASTER_HOLD, QG_MASTER_HOLD);
	return ret;
}

static int qg_sample_config(struct qcom_qg *qg, int *count, int *interval)
{
	unsigned int ctl2, ctl3;
	int ret;

	ret = regmap_read(qg->regmap, qg->base + QG_S2_CTL2, &ctl2);
	if (ret)
		return ret;
	ret = regmap_read(qg->regmap, qg->base + QG_S2_CTL3, &ctl3);
	if (ret)
		return ret;
	*count = 1 << ((ctl2 & 7) + 1);
	/* PM6150 nominal 32 kHz clock actually runs at 32764 Hz. */
	*interval = DIV_ROUND_CLOSEST(ctl3 * 10 * 32000, 32764);
	return *interval ? 0 : -EINVAL;
}

static int qg_read_fifo(struct qcom_qg *qg, bool realtime, bool *changed)
{
	unsigned int reg, entries, i;
	int count, interval, current_ua, voltage, ret, old = qg->capacity;
	s64 delta_nah = 0;
	u16 raw;

	ret = regmap_read(qg->regmap,
			  qg->base + (realtime ? QG_STATUS3 : QG_S2_CTL2), &reg);
	if (ret)
		return ret;
	entries = realtime ? reg & 0xf : ((reg >> 3) & 7) + 1;
	if (entries > 8)
		return -EINVAL;
	ret = qg_sample_config(qg, &count, &interval);
	if (ret)
		return ret;
	for (i = 0; i < entries; i++) {
		ret = qg_read_u16(qg, QG_FIFO_V + i * 2, &raw);
		if (ret)
			return ret;
		if (!raw || raw == QG_RESET_SAMPLE)
			return -ENODATA;
		voltage = div_u64((u64)raw * 194637, 1000);
		ret = qg_current(qg, QG_FIFO_I + i * 2, &current_ua);
		if (ret)
			return ret;
		if (voltage < 2000000 || voltage > 5000000)
			return -ERANGE;
		/* uA * ms / 3600 = nAh. Defer state update until all reads pass. */
		delta_nah += div_s64((s64)current_ua * count * interval, 3600);
	}
	if (realtime) {
		/* RT accumulation is the partial next FIFO entry. Read it only
		 * under master hold, immediately before the FIFO is restarted.
		 */
		u8 data[3];
		unsigned int n, raw_i;

		ret = regmap_read(qg->regmap, qg->base + QG_ACCUM_COUNT, &n);
		if (ret)
			return ret;
		if (n >= 10) {
			ret = regmap_bulk_read(qg->regmap, qg->base + QG_ACCUM_V,
					       data, 3);
			if (ret)
				return ret;
			raw = (data[0] | data[1] << 8 | data[2] << 16) / n;
			if (!raw || raw == QG_RESET_SAMPLE)
				return -ENODATA;
			ret = regmap_bulk_read(qg->regmap, qg->base + QG_ACCUM_I,
					       data, 3);
			if (ret)
				return ret;
			raw_i = data[0] | data[1] << 8 | data[2] << 16;
			current_ua = -div_s64((s64)sign_extend32(raw_i, 23) *
						qg->current_lsb, n * 1000);
			delta_nah += div_s64((s64)current_ua * n * interval,
						3600);
		}
	}
	qg->charge_nah += delta_nah;
	qg_update_capacity(qg);
	*changed = qg->capacity != old;
	return 0;
}

static int qg_drain(struct qcom_qg *qg, bool *changed)
{
	int ret, release;
	s64 old_charge = qg->charge_nah;
	int old_capacity = qg->capacity;
	bool old_full_latched = qg->full_latched;

	ret = qg_master_hold(qg, true);
	if (ret)
		return ret;
	ret = qg_read_fifo(qg, true, changed);
	release = qg_master_hold(qg, false);
	if (release) {
		qg->charge_nah = old_charge;
		qg->capacity = old_capacity;
		qg->full_latched = old_full_latched;
		*changed = false;
	}
	return ret ?: release;
}

static int qg_configure(struct qcom_qg *qg, int fifo, int count, int ms)
{
	int ret, release;

	ret = qg_master_hold(qg, true);
	if (ret)
		return ret;
	ret = regmap_update_bits(qg->regmap, qg->base + QG_S2_CTL2,
				 GENMASK(5, 0), ((fifo - 1) << 3) |
				 (ilog2(count) - 1));
	/* Downstream couples S3 entry qualification to S2 FIFO length. */
	if (!ret)
		ret = regmap_update_bits(qg->regmap,
					 qg->base + QG_S3_IBAT_CTL1,
					 GENMASK(2, 0), fifo > 3 ? 2 : 0);
	if (!ret)
		ret = regmap_write(qg->regmap, qg->base + QG_S2_CTL3, ms / 10);
	release = qg_master_hold(qg, false);
	if (ret || release)
		return ret ?: release;
	qg->fifo_period_ms = div_u64((u64)fifo * count * ms * 32000 +
					   32764 / 2, 32764);
	qg->last_fifo = ktime_get_boottime();
	return 0;
}

static void qg_anchor_full(struct qcom_qg *qg)
{
	int status;

	if (!qg_status(qg, &status) && status == POWER_SUPPLY_STATUS_FULL) {
		qg->full_latched = true;
		qg->lost_samples = false;
		qg->charge_nah = (s64)qg->full_uah * 1000;
		qg_update_capacity(qg);
	}
}

static bool qg_measurements_changed(struct qcom_qg *qg)
{
	int voltage, current_ua, temp, status;
	int charge_uah;
	bool changed;

	if (qg_voltage(qg, QG_AVG_V, &voltage) ||
	    qg_current(qg, QG_AVG_I, &current_ua) ||
	    qg_temperature(qg, &temp) || qg_status(qg, &status))
		return false;
	charge_uah = div_s64(qg->charge_nah, 1000);
	changed = !qg->notified ||
		  abs(voltage - qg->notified_voltage) >= 20000 ||
		  abs(current_ua - qg->notified_current) >= 100000 ||
		  abs(temp - qg->notified_temp) >= 10 ||
		  abs(charge_uah - qg->notified_charge_uah) >= 5000 ||
		  status != qg->notified_status;
	if (changed) {
		qg->notified_voltage = voltage;
		qg->notified_current = current_ua;
		qg->notified_temp = temp;
		qg->notified_status = status;
		qg->notified_charge_uah = charge_uah;
		qg->notified = true;
	}
	return changed;
}

static irqreturn_t qg_fifo_irq(int irq, void *data)
{
	struct qcom_qg *qg = data;
	bool changed = false;
	bool lost;
	int ret, old;
	s64 elapsed;
	unsigned int realtime_count;

	mutex_lock(&qg->lock);
	old = qg->capacity;
	elapsed = ktime_ms_delta(ktime_get_boottime(), qg->last_fifo);
	/* A hold/restart can leave a latched FIFO interrupt behind. */
	if (elapsed < qg->fifo_period_ms / 2) {
		mutex_unlock(&qg->lock);
		return IRQ_HANDLED;
	}
	if (elapsed > qg->fifo_period_ms * 3 / 2)
		qg->lost_samples = true;
	ret = qg_read_fifo(qg, false, &changed);
	if (ret) {
		if (regmap_read(qg->regmap, qg->base + QG_STATUS3,
				&realtime_count) || !(realtime_count & 0xf))
			qg->lost_samples = true;
		ret = qg_drain(qg, &changed);
	}
	if (ret)
		qg->lost_samples = true;
	if (!ret) {
		qg_anchor_full(qg);
		ret = qg_sdam_save(qg);
	}
	changed |= qg_measurements_changed(qg);
	qg->last_fifo = ktime_get_boottime();
	changed |= qg->capacity != old;
	lost = qg->lost_samples;
	mutex_unlock(&qg->lock);
	if (ret)
		dev_warn_ratelimited(qg->dev, "FIFO update failed: %d\n", ret);
	if (changed || lost)
		power_supply_changed(qg->psy);
	return IRQ_HANDLED;
}

static irqreturn_t qg_ocv_irq(int irq, void *data)
{
	struct qcom_qg *qg = data;
	int uv, capacity, ret;
	bool changed = false;

	mutex_lock(&qg->lock);
	ret = qg_voltage(qg, QG_GOOD_OCV_V, &uv);
	if (!ret)
		ret = qg_ocv_capacity(qg, uv, &capacity);
	if (!ret) {
		changed = true; /* Charge counter may change without integer SOC. */
		qg->full_latched = capacity == 100;
		qg->lost_samples = false;
		qg->ocv_dirty = true;
		qg->charge_nah = div_s64((s64)qg->full_uah * 1000 * capacity,
					  100);
		qg_update_capacity(qg);
		ret = qg_sdam_save(qg);
	}
	mutex_unlock(&qg->lock);
	if (ret)
		dev_warn_ratelimited(qg->dev, "OCV update failed: %d\n", ret);
	if (changed)
		power_supply_changed(qg->psy);
	return IRQ_HANDLED;
}

static void qg_external_power_changed(struct power_supply *psy)
{
	struct qcom_qg *qg = power_supply_get_drvdata(psy);
	int ret = 0;

	mutex_lock(&qg->lock);
	if (qg->full_uah) {
		qg_anchor_full(qg);
		ret = qg_sdam_save(qg);
	}
	mutex_unlock(&qg->lock);
	if (ret)
		dev_warn_ratelimited(qg->dev, "SDAM update failed: %d\n", ret);
	power_supply_changed(psy);
}

static enum power_supply_property qg_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN,
	POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_AVG,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CURRENT_AVG,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_CHARGE_COUNTER,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_TEMP,
};

static int qg_get_property(struct power_supply *psy,
			   enum power_supply_property prop,
			   union power_supply_propval *val)
{
	struct qcom_qg *qg = power_supply_get_drvdata(psy);
	unsigned int status;
	int ret;

	switch (prop) {
	case POWER_SUPPLY_PROP_STATUS:
		return qg_status(qg, &val->intval);
	case POWER_SUPPLY_PROP_HEALTH:
		return qg_charger_property(qg, prop, &val->intval);
	case POWER_SUPPLY_PROP_PRESENT:
		ret = regmap_read(qg->regmap, qg->base + QG_STATUS1, &status);
		if (!ret)
			val->intval = !!(status & QG_BATTERY_PRESENT);
		return ret;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		if (!qg->info)
			return -ENODATA;
		val->intval = qg->info->technology;
		return 0;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		return qg_voltage(qg, QG_LAST_V, &val->intval);
	case POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN:
	case POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN:
		if (!qg->info)
			return -ENODATA;
		val->intval = prop == POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN ?
			qg->info->voltage_max_design_uv :
			qg->info->voltage_min_design_uv;
		return val->intval > 0 ? 0 : -ENODATA;
	case POWER_SUPPLY_PROP_VOLTAGE_AVG:
		return qg_voltage(qg, QG_AVG_V, &val->intval);
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		return qg_current(qg, QG_LAST_I, &val->intval);
	case POWER_SUPPLY_PROP_CURRENT_AVG:
		return qg_current(qg, QG_AVG_I, &val->intval);
	case POWER_SUPPLY_PROP_TEMP:
		return qg_temperature(qg, &val->intval);
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		if (!qg->info)
			return -ENODATA;
		val->intval = qg->info->charge_full_design_uah;
		return 0;
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		if (!qg->learned_full)
			return -ENODATA;
		val->intval = qg->full_uah;
		return 0;
	case POWER_SUPPLY_PROP_CHARGE_COUNTER:
	case POWER_SUPPLY_PROP_CHARGE_NOW:
	case POWER_SUPPLY_PROP_CAPACITY:
		mutex_lock(&qg->lock);
		if (!qg->full_uah || qg->lost_samples) {
			mutex_unlock(&qg->lock);
			return -ENODATA;
		}
		if (prop == POWER_SUPPLY_PROP_CAPACITY)
			val->intval = qg->capacity;
		else
			val->intval = div_s64(qg->charge_nah, 1000);
		mutex_unlock(&qg->lock);
		return 0;
	default:
		return -EINVAL;
	}
}

static const struct power_supply_desc qg_desc = {
	.name = "qcom_qg",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = qg_props,
	.num_properties = ARRAY_SIZE(qg_props),
	.get_property = qg_get_property,
	.external_power_changed = qg_external_power_changed,
};

static bool qg_profile_valid(struct power_supply_battery_info *info)
{
	int i, j;

	if (info->charge_full_design_uah < 100000 || !info->ocv_table[0])
		return false;
	for (i = 0; i < POWER_SUPPLY_OCV_TEMP_MAX && info->ocv_table[i]; i++) {
		const struct power_supply_battery_ocv_table *table = info->ocv_table[i];

		if (info->ocv_table_size[i] < 2)
			return false;
		for (j = 1; j < info->ocv_table_size[i]; j++) {
			if (table[j - 1].ocv <= table[j].ocv ||
			    table[j - 1].capacity < table[j].capacity)
				return false;
		}
	}
	return true;
}

static int qg_seed(struct qcom_qg *qg)
{
	u8 valid, saved_soc, saved_ocv[4], saved_fcc[2];
	u8 saved_temp[2] = {}, saved_time[4] = {};
	unsigned int status;
	u32 now, shutdown_time;
	int uv, capacity, raw_capacity, saved_raw, ret, fcc, temp = 0;
	bool recent;

	ret = qg_rtc_seconds(&now);
	if (ret)
		return ret;

	ret = regmap_read(qg->regmap, qg->base + QG_STATUS2, &status);
	if (ret)
		return ret;
	ret = qg_voltage(qg, status & QG_GOOD_OCV ?
				    QG_GOOD_OCV_V : QG_PON_OCV_V, &uv);
	if (ret)
		ret = qg_voltage(qg, QG_LAST_SLEEP_OCV, &uv);
	if (ret) {
		u8 persisted_ocv[4];
		u8 persisted_valid;

		ret = nvmem_device_read(qg->sdam, QG_SDAM_VALID, 1,
					&persisted_valid);
		if (ret >= 0 && persisted_valid)
			ret = nvmem_device_read(qg->sdam, QG_SDAM_OCV, 4,
						&persisted_ocv);
		if (ret >= 0 && persisted_valid) {
			uv = get_unaligned_le32(persisted_ocv);
			if (uv < 2000000 || uv > 5000000)
				ret = -ENODATA;
		} else if (ret >= 0) {
			ret = -ENODATA;
		}
	}
	if (ret < 0)
		return ret;
	ret = qg_ocv_capacity(qg, uv, &capacity);
	if (ret)
		return ret;
	raw_capacity = capacity;
	qg->full_uah = qg->info->charge_full_design_uah;
	ret = nvmem_device_read(qg->sdam, QG_SDAM_FCC, 2, saved_fcc);
	if (ret >= 0) {
		fcc = get_unaligned_le16(saved_fcc) * 1000;
		if (fcc >= qg->full_uah / 2 &&
		    fcc <= qg->full_uah + qg->full_uah / 2) {
			qg->full_uah = fcc;
			qg->learned_full = true;
		}
	}
	ret = nvmem_device_read(qg->sdam, QG_SDAM_VALID, 1, &valid);
	if (ret >= 0 && valid) {
		ret = nvmem_device_read(qg->sdam, QG_SDAM_SOC, 1, &saved_soc);
		if (ret >= 0)
			ret = nvmem_device_read(qg->sdam, QG_SDAM_TEMP, 2,
						&saved_temp);
		if (ret >= 0)
			ret = nvmem_device_read(qg->sdam, QG_SDAM_OCV, 4,
						&saved_ocv);
		if (ret >= 0)
			ret = nvmem_device_read(qg->sdam, QG_SDAM_TIME, 4,
						&saved_time);
		if (ret >= 0)
			ret = qg_temperature(qg, &temp);
		shutdown_time = ret >= 0 ? get_unaligned_le32(saved_time) : 0;
		recent = ret >= 0 && now >= shutdown_time &&
			 now - shutdown_time <= QG_RECENT_STATE_SECONDS &&
			 abs(temp - (s16)get_unaligned_le16(saved_temp)) <=
			 QG_RECENT_TEMP_TOLERANCE;
		/* Retain a recent gauge state even if power-on OCV is depressed
		 * by load. For older state use sweet's vendor 10% SOC gate.
		 */
		if (ret >= 0 && saved_soc <= 100 &&
		    get_unaligned_le32(saved_ocv) >= 2000000 &&
		    get_unaligned_le32(saved_ocv) <= 5000000 &&
		    (recent || abs((int)saved_soc - capacity) <= 10) &&
		    !qg_ocv_capacity(qg, get_unaligned_le32(saved_ocv),
					  &saved_raw)) {
			capacity = saved_soc;
			raw_capacity = max(saved_raw, capacity);
		}
	}
	qg->ocv_dirty = true;
	qg->full_latched = capacity == 100;
	/* Normalize physical charge to the usable window, as the vendor
	 * algorithm does with distinct CC_SOC and system SOC values.
	 */
	if (capacity < 100 && raw_capacity > capacity)
		qg->cutoff_bp = clamp(DIV_ROUND_CLOSEST((raw_capacity - capacity) *
						    10000, 100 - capacity),
				      0, 5000);
	qg->charge_nah = div_s64((s64)qg->full_uah * 1000 * raw_capacity,
				  100);
	qg_update_capacity(qg);
	qg->saved_capacity = -1;
	qg->saved_charge_nah = -1;
	return 0;
}

static int qg_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct power_supply_config cfg = {};
	struct qcom_qg *qg;
	unsigned int subtype, status;
	int ret, ocv_irq;

	qg = devm_kzalloc(dev, sizeof(*qg), GFP_KERNEL);
	if (!qg)
		return -ENOMEM;
	qg->dev = dev;
	mutex_init(&qg->lock);
	qg->regmap = dev_get_regmap(dev->parent, NULL);
	if (!qg->regmap)
		return dev_err_probe(dev, -EPROBE_DEFER, "missing PMIC regmap\n");
	ret = device_property_read_u32(dev, "reg", &qg->base);
	if (ret)
		return dev_err_probe(dev, ret, "missing QG base\n");
	qg->therm = devm_iio_channel_get(dev, "batt-therm");
	if (IS_ERR(qg->therm))
		return dev_err_probe(dev, PTR_ERR(qg->therm), "battery thermistor\n");
	qg->sdam = devm_nvmem_device_get(dev, NULL);
	if (IS_ERR(qg->sdam))
		return dev_err_probe(dev, PTR_ERR(qg->sdam), "QG SDAM\n");
	qg->charger = devm_power_supply_get_by_reference(dev, "power-supplies");
	if (IS_ERR(qg->charger))
		return dev_err_probe(dev, PTR_ERR(qg->charger), "charger supply\n");
	if (!qg->charger)
		return dev_err_probe(dev, -EPROBE_DEFER, "waiting for charger\n");
	ret = regmap_read(qg->regmap, qg->base + QG_SUBTYPE, &subtype);
	if (ret)
		return ret;
	if (subtype != 3 && subtype != 4)
		return dev_err_probe(dev, -ENODEV, "unsupported QG subtype %u\n",
				     subtype);
	qg->current_lsb = subtype == 3 ? 152588 : 305176;
	ret = regmap_read_poll_timeout(qg->regmap, qg->base + QG_STATUS1,
				       status, status & QG_OK, 200000, 4000000);
	if (ret)
		return dev_err_probe(dev, ret, "QG hardware not ready\n");
	if (!(status & QG_BATTERY_PRESENT))
		return dev_err_probe(dev, -ENODEV, "battery absent\n");
	cfg.drv_data = qg;
	cfg.fwnode = dev_fwnode(dev);
	qg->psy = devm_power_supply_register(dev, &qg_desc, &cfg);
	if (IS_ERR(qg->psy))
		return dev_err_probe(dev, PTR_ERR(qg->psy), "battery supply\n");
	ret = power_supply_get_battery_info(qg->psy, &qg->info);
	if (ret)
		return dev_err_probe(dev, ret, "battery profile\n");
	if (!qg_profile_valid(qg->info)) {
		ret = dev_err_probe(dev, -EINVAL, "OCV profile and design capacity required\n");
		goto put_info;
	}
	ret = qg_seed(qg);
	if (ret)
		goto put_info;
	ret = qg_configure(qg, 5, 128, 100);
	if (ret)
		goto put_info;
	qg->fifo_irq = platform_get_irq_byname(pdev, "fifo-done");
	if (qg->fifo_irq < 0) {
		ret = qg->fifo_irq;
		goto put_info;
	}
	ocv_irq = platform_get_irq_byname(pdev, "good-ocv");
	if (ocv_irq < 0) {
		ret = ocv_irq;
		goto put_info;
	}
	ret = devm_request_threaded_irq(dev, qg->fifo_irq, NULL, qg_fifo_irq,
				IRQF_ONESHOT, "qcom-qg-fifo", qg);
	if (ret)
		goto put_info;
	ret = devm_request_threaded_irq(dev, ocv_irq, NULL, qg_ocv_irq,
				IRQF_ONESHOT, "qcom-qg-ocv", qg);
	if (ret)
		goto put_info;
	device_init_wakeup(dev, true);
	ret = dev_pm_set_wake_irq(dev, qg->fifo_irq);
	if (ret) {
		device_init_wakeup(dev, false);
		goto put_info;
	}
	platform_set_drvdata(pdev, qg);
	dev_info(dev, "PM6150 QG initialized at %d%%\n", qg->capacity);
	return 0;

put_info:
	/* battery_info uses devres attached to the power_supply device, and is
	 * released when devm_power_supply_register() unwinds.
	 */
	return ret;
}

static void qg_remove(struct platform_device *pdev)
{
	dev_pm_clear_wake_irq(&pdev->dev);
	device_init_wakeup(&pdev->dev, false);
}

static int qg_suspend(struct device *dev)
{
	struct qcom_qg *qg = dev_get_drvdata(dev);
	bool changed = false;
	int ret;

	disable_irq(qg->fifo_irq);
	mutex_lock(&qg->lock);
	if (ktime_ms_delta(ktime_get_boottime(), qg->last_fifo) >
	    qg->fifo_period_ms * 3 / 2)
		qg->lost_samples = true;
	ret = qg_drain(qg, &changed);
	if (!ret)
		ret = qg_configure(qg, 8, 256, 200);
	if (ret)
		qg->lost_samples = true;
	else
		ret = qg_sdam_save(qg);
	mutex_unlock(&qg->lock);
	enable_irq(qg->fifo_irq);
	if (changed || ret)
		power_supply_changed(qg->psy);
	return ret;
}

static int qg_resume(struct device *dev)
{
	struct qcom_qg *qg = dev_get_drvdata(dev);
	bool changed = false;
	int ret;

	disable_irq(qg->fifo_irq);
	mutex_lock(&qg->lock);
	if (ktime_ms_delta(ktime_get_boottime(), qg->last_fifo) >
	    qg->fifo_period_ms * 3 / 2)
		qg->lost_samples = true;
	ret = qg_drain(qg, &changed);
	if (!ret)
		ret = qg_configure(qg, 5, 128, 100);
	if (ret)
		qg->lost_samples = true;
	else {
		qg_anchor_full(qg);
		ret = qg_sdam_save(qg);
	}
	mutex_unlock(&qg->lock);
	enable_irq(qg->fifo_irq);
	if (changed || ret)
		power_supply_changed(qg->psy);
	return ret;
}

static DEFINE_SIMPLE_DEV_PM_OPS(qg_pm_ops, qg_suspend, qg_resume);

static const struct of_device_id qg_of_match[] = {
	{ .compatible = "qcom,pm6150-qg" },
	{ }
};
MODULE_DEVICE_TABLE(of, qg_of_match);

static struct platform_driver qg_driver = {
	.driver = {
		.name = "qcom-qg",
		.of_match_table = qg_of_match,
		.pm = pm_sleep_ptr(&qg_pm_ops),
	},
	.probe = qg_probe,
	.remove = qg_remove,
};
module_platform_driver(qg_driver);

MODULE_DESCRIPTION("Qualcomm PM6150 QG fuel gauge");
MODULE_AUTHOR("thedanilfez <thedanilfezlol@gmail.com>");
MODULE_LICENSE("GPL");
