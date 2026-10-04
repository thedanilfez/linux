// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026 thedanilfez <thedanilfezlol@gmail.com>
 *
 * Qualcomm PM6150 QG fuel gauge driver.
 */

#include <linux/bitfield.h>
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
#define QG_SUBTYPE		0x05
#define QG_SUBTYPE_5A		0x03
#define QG_SUBTYPE_10A		0x04
#define QG_STATUS1		0x08
#define QG_OK			BIT(7)
#define QG_BATTERY_PRESENT	BIT(0)
#define QG_STATUS2		0x09
#define QG_GOOD_OCV		BIT(1)
#define QG_STATUS3		0x0a
#define QG_MASTER_CTL		0x41
#define QG_MASTER_HOLD		BIT(0)
#define QG_DATA_CTL2		0x42
#define QG_BURST_HOLD		BIT(0)
#define QG_S2_CTL2		0x51
#define QG_FIFO_LENGTH_MASK	GENMASK(5, 3)
#define QG_SAMPLE_COUNT_MASK	GENMASK(2, 0)
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
#define QG_LAST_BURST_I		0xc6
#define QG_RESET_SAMPLE		0x8000

#define QG_SDAM_VALID		0x46
#define QG_SDAM_SOC		0x47
#define QG_SDAM_TEMP		0x48
#define QG_SDAM_OCV		0x4c
#define QG_SDAM_TIME		0x54
#define QG_SDAM_CYCLES		0x58
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
	struct mutex lock; /* Serializes measurements and gauge state. */
	u32 base;
	int current_lsb; /* microamps * 1000 per raw code */
	int fifo_irq;
	int ocv_irq;
	int capacity;
	int cutoff_bp; /* physical SOC at displayed 0%, in 0.01% units */
	int saved_capacity;
	int full_uah;
	int saved_full_uah;
	int ocv_uv;
	int learning_start_bp;
	int fifo_period_ms;
	ktime_t last_fifo;
	int notified_voltage;
	int notified_current;
	int notified_temp;
	int notified_status;
	int notified_charge_uah;
	s64 charge_nah;
	s64 counter_nah;
	s64 learning_nah;
	s64 saved_charge_nah;
	struct qg_cycle_counter cycles;
	bool full_latched;
	bool cycles_dirty;
	bool notified;
	bool ocv_dirty;
};

static void qg_reset_tracking(struct qcom_qg *qg)
{
	int i;

	qg->learning_start_bp = -1;
	qg->learning_nah = 0;
	for (i = 0; i < QG_CYCLE_BUCKETS; i++)
		qg->cycles.start_soc[i] = -1;
}

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

static int qg_read_current(struct qcom_qg *qg, int *ua)
{
	int ret, release;

	ret = regmap_update_bits(qg->regmap, qg->base + QG_DATA_CTL2,
				 QG_BURST_HOLD, QG_BURST_HOLD);
	if (ret)
		return ret;
	ret = qg_current(qg, QG_LAST_BURST_I, ua);
	release = regmap_update_bits(qg->regmap, qg->base + QG_DATA_CTL2,
				     QG_BURST_HOLD, 0);
	return ret ?: release;
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
	ret = qg_read_current(qg, &current_ua);
	if (ret)
		return ret;
	if (current_ua < -20000)
		*status = POWER_SUPPLY_STATUS_DISCHARGING;
	else if (current_ua > 20000 && *status != POWER_SUPPLY_STATUS_FULL)
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
	u8 data[QG_CYCLE_BUCKETS * 2];
	int i, ret, model_ocv, raw_capacity, temp;
	u32 seconds;

	if (qg->cycles_dirty) {
		for (i = 0; i < QG_CYCLE_BUCKETS; i++)
			put_unaligned_le16(qg->cycles.count[i], data + i * 2);
		ret = nvmem_device_write(qg->sdam, QG_SDAM_CYCLES,
					 sizeof(data), data);
		if (ret < 0)
			return ret;
		qg->cycles_dirty = false;
	}
	if (qg->full_uah != qg->saved_full_uah) {
		put_unaligned_le16(DIV_ROUND_CLOSEST(qg->full_uah, 1000), data);
		ret = nvmem_device_write(qg->sdam, QG_SDAM_FCC, 2, data);
		if (ret < 0)
			return ret;
		qg->saved_full_uah = qg->full_uah;
	}
	if (qg->capacity == qg->saved_capacity && !qg->ocv_dirty &&
	    qg->charge_nah - qg->saved_charge_nah > -5000000 &&
	    qg->charge_nah - qg->saved_charge_nah < 5000000)
		return 0;
	raw_capacity = qg_raw_soc_percent(qg->charge_nah, (u64)qg->full_uah * 1000);
	ret = qg_capacity_ocv(qg, raw_capacity, &model_ocv);
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

static void qg_account_charge(struct qcom_qg *qg, s64 delta_nah)
{
	qg->counter_nah += delta_nah;
	qg->charge_nah += delta_nah;
	if (delta_nah < 0)
		qg->learning_start_bp = -1;
	else if (qg->learning_start_bp >= 0)
		qg->learning_nah += delta_nah;
	qg_update_capacity(qg);
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
	*count = 1 << (FIELD_GET(QG_SAMPLE_COUNT_MASK, ctl2) + 1);
	/* PM6150 nominal 32 kHz clock actually runs at 32764 Hz. */
	*interval = DIV_ROUND_CLOSEST(ctl3 * 10 * 32000, 32764);
	return *interval ? 0 : -EINVAL;
}

static int qg_read_fifo(struct qcom_qg *qg, bool realtime, s64 *charge_nah)
{
	unsigned int reg, entries, i;
	int count, interval, current_ua, voltage, ret;
	s64 delta_nah = 0;
	u16 raw;

	ret = regmap_read(qg->regmap,
			  qg->base + (realtime ? QG_STATUS3 : QG_S2_CTL2), &reg);
	if (ret)
		return ret;
	entries = realtime ? reg & 0xf : FIELD_GET(QG_FIFO_LENGTH_MASK, reg) + 1;
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
		/* uA * ms / 3600 = nAh. */
		delta_nah += div_s64((s64)current_ua * count * interval, 3600);
	}
	if (realtime) {
		/* Master hold keeps the partial FIFO entry stable. */
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
			voltage = div_u64((u64)raw * 194637, 1000);
			if (voltage < 2000000 || voltage > 5000000)
				return -ERANGE;
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
	*charge_nah = delta_nah;
	return 0;
}

static int qg_drain(struct qcom_qg *qg, bool *changed)
{
	int ret, release;
	s64 delta_nah;
	int old_capacity = qg->capacity;

	if (ktime_ms_delta(ktime_get_boottime(), qg->last_fifo) >
	    qg->fifo_period_ms * 3 / 2)
		qg_reset_tracking(qg);
	ret = qg_master_hold(qg, true);
	if (ret) {
		qg_reset_tracking(qg);
		return ret;
	}
	ret = qg_read_fifo(qg, true, &delta_nah);
	release = qg_master_hold(qg, false);
	if (ret || release) {
		qg_reset_tracking(qg);
		return ret ?: release;
	}
	qg_account_charge(qg, delta_nah);
	qg->last_fifo = ktime_get_boottime();
	*changed |= qg->capacity != old_capacity;
	return 0;
}

static int qg_configure(struct qcom_qg *qg, int fifo, int count, int ms)
{
	int ret, release;

	ret = qg_master_hold(qg, true);
	if (ret)
		return ret;
	ret = regmap_update_bits(qg->regmap, qg->base + QG_S2_CTL2,
				 QG_FIFO_LENGTH_MASK | QG_SAMPLE_COUNT_MASK,
				 FIELD_PREP(QG_FIFO_LENGTH_MASK, fifo - 1) |
				 FIELD_PREP(QG_SAMPLE_COUNT_MASK, ilog2(count) - 1));
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

static int qg_update_status(struct qcom_qg *qg)
{
	int status, temp, learned, ret;

	ret = qg_status(qg, &status);
	if (ret) {
		qg_reset_tracking(qg);
		return ret;
	}
	qg->cycles_dirty |= qg_cycle_update(&qg->cycles,
		qg_raw_soc_bp(qg->charge_nah, (u64)qg->full_uah * 1000),
		status == POWER_SUPPLY_STATUS_CHARGING);

	ret = qg_temperature(qg, &temp);
	if (ret || temp < 150 || temp > 500 ||
	    (status != POWER_SUPPLY_STATUS_CHARGING &&
	     status != POWER_SUPPLY_STATUS_FULL))
		qg->learning_start_bp = -1;
	if (status != POWER_SUPPLY_STATUS_FULL || qg->full_latched)
		return ret;

	learned = qg_learned_capacity(qg->learning_nah, qg->learning_start_bp,
				      qg->full_uah, qg->info->charge_full_design_uah);
	if (learned) {
		dev_dbg(qg->dev, "Learned capacity %d -> %d uAh\n", qg->full_uah, learned);
		qg->full_uah = learned;
	}
	qg->learning_start_bp = -1;
	qg->full_latched = true;
	qg->charge_nah = (s64)qg->full_uah * 1000;
	qg->ocv_dirty = true;
	qg_update_capacity(qg);
	return ret;
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
	int ret, old;
	s64 elapsed, delta_nah;

	mutex_lock(&qg->lock);
	old = qg->capacity;
	elapsed = ktime_ms_delta(ktime_get_boottime(), qg->last_fifo);
	/* A hold/restart can leave a latched FIFO interrupt behind. */
	if (elapsed < qg->fifo_period_ms / 2) {
		mutex_unlock(&qg->lock);
		return IRQ_HANDLED;
	}
	/* Missing FIFO data invalidates learning, not the SOC estimate. */
	if (elapsed > qg->fifo_period_ms * 3 / 2)
		qg_reset_tracking(qg);
	ret = qg_read_fifo(qg, false, &delta_nah);
	if (ret) {
		qg_reset_tracking(qg);
		goto out;
	}
	qg_account_charge(qg, delta_nah);
	ret = qg_update_status(qg);
	if (!ret)
		ret = qg_sdam_save(qg);
out:
	changed |= qg_measurements_changed(qg);
	qg->last_fifo = ktime_get_boottime();
	changed |= qg->capacity != old;
	mutex_unlock(&qg->lock);
	if (ret)
		dev_warn_ratelimited(qg->dev, "FIFO update failed: %d\n", ret);
	if (changed)
		power_supply_changed(qg->psy);
	return IRQ_HANDLED;
}

static int qg_update_ocv(struct qcom_qg *qg, bool *changed)
{
	unsigned int status;
	int uv, capacity, temp, ret;

	ret = regmap_read(qg->regmap, qg->base + QG_STATUS2, &status);
	if (ret)
		return ret;
	if (!(status & QG_GOOD_OCV))
		return 0;

	/* Do not integrate pre-OCV samples after re-anchoring the estimate. */
	ret = qg_drain(qg, changed);
	if (ret)
		dev_warn_ratelimited(qg->dev, "Failed to drain OCV samples: %d\n", ret);
	ret = qg_voltage(qg, QG_GOOD_OCV_V, &uv);
	if (ret)
		return ret;
	ret = qg_ocv_capacity(qg, uv, &capacity);
	if (ret)
		return ret;
	*changed = true;
	qg->ocv_uv = uv;
	qg->full_latched = capacity == 100;
	qg->ocv_dirty = true;
	qg->charge_nah = div_s64((s64)qg->full_uah * 1000 * capacity, 100);
	qg_update_capacity(qg);

	/* OCV corrections are not measured charge cycles. */
	qg_reset_tracking(qg);
	ret = qg_temperature(qg, &temp);
	if (!ret && temp >= 150 && temp <= 500 && capacity >= 10 && capacity <= 15) {
		qg->learning_start_bp = capacity * 100;
		dev_dbg(qg->dev, "Capacity learning started at %d%%\n", capacity);
	}
	return ret;
}

static irqreturn_t qg_ocv_irq(int irq, void *data)
{
	struct qcom_qg *qg = data;
	bool changed = false;
	int ret;

	mutex_lock(&qg->lock);
	ret = qg_update_ocv(qg, &changed);
	if (!ret && changed)
		ret = qg_sdam_save(qg);
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
	bool changed = false;
	int ret = 0;

	mutex_lock(&qg->lock);
	if (qg->full_uah) {
		ret = qg_drain(qg, &changed);
		if (!ret)
			ret = qg_update_status(qg);
		if (!ret)
			ret = qg_sdam_save(qg);
	}
	mutex_unlock(&qg->lock);
	if (ret)
		dev_warn_ratelimited(qg->dev, "SDAM update failed: %d\n", ret);
	power_supply_changed(psy);
}

static const enum power_supply_property qg_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN,
	POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_AVG,
	POWER_SUPPLY_PROP_VOLTAGE_OCV,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CURRENT_AVG,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_CHARGE_COUNTER,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CYCLE_COUNT,
	POWER_SUPPLY_PROP_SCOPE,
	POWER_SUPPLY_PROP_TEMP,
};

static int qg_get_property(struct power_supply *psy,
			   enum power_supply_property prop,
			   union power_supply_propval *val)
{
	struct qcom_qg *qg = power_supply_get_drvdata(psy);
	unsigned int status;
	int i, ret = 0;

	mutex_lock(&qg->lock);
	if (!qg->full_uah) {
		ret = -ENODATA;
		goto out;
	}
	switch (prop) {
	case POWER_SUPPLY_PROP_STATUS:
		ret = qg_status(qg, &val->intval);
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		ret = qg_charger_property(qg, prop, &val->intval);
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		ret = regmap_read(qg->regmap, qg->base + QG_STATUS1, &status);
		if (!ret)
			val->intval = !!(status & QG_BATTERY_PRESENT);
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
	case POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN:
	case POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN:
		ret = power_supply_battery_info_get_prop(qg->info, prop, val);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		ret = qg_voltage(qg, QG_LAST_V, &val->intval);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_AVG:
		ret = qg_voltage(qg, QG_AVG_V, &val->intval);
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		ret = qg_read_current(qg, &val->intval);
		break;
	case POWER_SUPPLY_PROP_CURRENT_AVG:
		ret = qg_current(qg, QG_AVG_I, &val->intval);
		break;
	case POWER_SUPPLY_PROP_TEMP:
		ret = qg_temperature(qg, &val->intval);
		break;
	case POWER_SUPPLY_PROP_SCOPE:
		val->intval = POWER_SUPPLY_SCOPE_SYSTEM;
		break;
	case POWER_SUPPLY_PROP_CHARGE_COUNTER:
		val->intval = div_s64(qg->counter_nah, 1000);
		break;
	case POWER_SUPPLY_PROP_CHARGE_NOW:
		val->intval = div_s64(qg->charge_nah, 1000);
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		val->intval = qg->full_uah;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_OCV:
		val->intval = qg->ocv_uv;
		break;
	case POWER_SUPPLY_PROP_CYCLE_COUNT:
		val->intval = 0;
		for (i = 0; i < QG_CYCLE_BUCKETS; i++)
			val->intval += qg->cycles.count[i];
		val->intval /= QG_CYCLE_BUCKETS;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = qg->capacity;
		break;
	default:
		ret = -EINVAL;
		break;
	}
out:
	mutex_unlock(&qg->lock);
	return ret;
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

	if (info->charge_full_design_uah < 100000 ||
	    info->charge_full_design_uah > U16_MAX * 1000 ||
	    info->voltage_min_design_uv < 2000000 ||
	    info->voltage_max_design_uv > 5000000 ||
	    info->voltage_max_design_uv <= info->voltage_min_design_uv ||
	    !info->ocv_table[0])
		return false;
	for (i = 0; i < POWER_SUPPLY_OCV_TEMP_MAX && info->ocv_table[i]; i++) {
		const struct power_supply_battery_ocv_table *table = info->ocv_table[i];

		if (info->ocv_table_size[i] < 2)
			return false;
		if (table[0].capacity != 100 ||
		    table[info->ocv_table_size[i] - 1].capacity != 0)
			return false;
		for (j = 1; j < info->ocv_table_size[i]; j++) {
			if (table[j - 1].ocv <= table[j].ocv ||
			    table[j - 1].capacity < table[j].capacity ||
			    table[j].capacity < 0 || table[j].capacity > 100)
				return false;
		}
	}
	return true;
}

static int qg_seed(struct qcom_qg *qg)
{
	u8 data[QG_SDAM_FCC + 2 - QG_SDAM_VALID];
	unsigned int status;
	u32 now, shutdown_time;
	int i, uv, capacity, raw_bp, fcc, temp, saved_soc, saved_ocv, ret;
	bool valid, recent;

	ret = qg_rtc_seconds(&now);
	if (ret)
		return ret;
	ret = qg_temperature(qg, &temp);
	if (ret)
		return ret;
	ret = nvmem_device_read(qg->sdam, QG_SDAM_VALID, sizeof(data), data);
	if (ret < 0)
		return ret;

	saved_soc = data[QG_SDAM_SOC - QG_SDAM_VALID];
	saved_ocv = get_unaligned_le32(data + QG_SDAM_OCV - QG_SDAM_VALID);
	shutdown_time = get_unaligned_le32(data + QG_SDAM_TIME - QG_SDAM_VALID);
	valid = data[0] == 1 && saved_soc <= 100 &&
		saved_ocv >= 2000000 && saved_ocv <= 5000000;
	recent = valid && now >= shutdown_time &&
		 now - shutdown_time <= QG_RECENT_STATE_SECONDS &&
		 abs(temp - (s16)get_unaligned_le16(data + QG_SDAM_TEMP -
						  QG_SDAM_VALID)) <=
		 QG_RECENT_TEMP_TOLERANCE;

	ret = qg_ocv_capacity(qg, qg->info->voltage_min_design_uv, &capacity);
	if (ret)
		return ret;
	qg->cutoff_bp = capacity * 100;
	if (qg->cutoff_bp >= 10000)
		return -EINVAL;
	ret = regmap_read(qg->regmap, qg->base + QG_STATUS2, &status);
	if (ret)
		return ret;

	/* A recent saved estimate avoids the voltage drop from boot load. */
	if (recent) {
		uv = saved_ocv;
		capacity = saved_soc;
		raw_bp = qg->cutoff_bp +
			 DIV_ROUND_CLOSEST(capacity * (10000 - qg->cutoff_bp), 100);
	} else {
		ret = qg_voltage(qg, status & QG_GOOD_OCV ?
				 QG_GOOD_OCV_V : QG_PON_OCV_V, &uv);
		if (ret)
			return ret;
		ret = qg_ocv_capacity(qg, uv, &capacity);
		if (ret)
			return ret;
		raw_bp = capacity * 100;
		capacity = qg_usable_capacity(raw_bp, qg->cutoff_bp, true);
		if (valid && abs(saved_soc - capacity) <= 10) {
			capacity = saved_soc;
			raw_bp = qg->cutoff_bp +
				 DIV_ROUND_CLOSEST(capacity *
						   (10000 - qg->cutoff_bp), 100);
		}
	}
	qg->ocv_uv = uv;
	qg->full_uah = qg->info->charge_full_design_uah;
	fcc = get_unaligned_le16(data + QG_SDAM_FCC - QG_SDAM_VALID) * 1000;
	if (fcc >= qg->full_uah / 2 && fcc <= (s64)qg->full_uah * 110 / 100)
		qg->full_uah = fcc;
	qg->saved_full_uah = qg->full_uah;
	for (i = 0; i < QG_CYCLE_BUCKETS; i++) {
		qg->cycles.count[i] = get_unaligned_le16(data + QG_SDAM_CYCLES -
						      QG_SDAM_VALID + i * 2);
		qg->cycles.start_soc[i] = -1;
	}
	qg->learning_start_bp = -1;
	qg->full_latched = capacity == 100;
	qg->charge_nah = div_s64((s64)qg->full_uah * 1000 * raw_bp, 10000);
	qg_update_capacity(qg);
	qg->ocv_dirty = true;
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
	int ret;

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
	if (subtype != QG_SUBTYPE_5A && subtype != QG_SUBTYPE_10A)
		return dev_err_probe(dev, -ENODEV, "unsupported QG subtype %u\n",
				     subtype);
	qg->current_lsb = subtype == QG_SUBTYPE_5A ? 152588 : 305176;
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
	qg->info = qg->psy->battery_info;
	if (!qg->info || !qg_profile_valid(qg->info))
		return dev_err_probe(dev, -EINVAL, "invalid battery profile\n");
	mutex_lock(&qg->lock);
	ret = qg_seed(qg);
	if (!ret)
		ret = qg_configure(qg, 5, 128, 100);
	if (ret)
		qg->full_uah = 0;
	mutex_unlock(&qg->lock);
	if (ret)
		return dev_err_probe(dev, ret, "gauge initialization failed\n");
	qg->fifo_irq = platform_get_irq_byname(pdev, "fifo-done");
	if (qg->fifo_irq < 0)
		return qg->fifo_irq;
	qg->ocv_irq = platform_get_irq_byname(pdev, "good-ocv");
	if (qg->ocv_irq < 0)
		return qg->ocv_irq;
	ret = devm_request_threaded_irq(dev, qg->fifo_irq, NULL, qg_fifo_irq,
					IRQF_ONESHOT, "qcom-qg-fifo", qg);
	if (ret)
		return dev_err_probe(dev, ret, "FIFO IRQ\n");
	ret = devm_request_threaded_irq(dev, qg->ocv_irq, NULL, qg_ocv_irq,
					IRQF_ONESHOT, "qcom-qg-ocv", qg);
	if (ret)
		return dev_err_probe(dev, ret, "OCV IRQ\n");
	ret = device_init_wakeup(dev, true);
	if (ret)
		return ret;
	ret = dev_pm_set_wake_irq(dev, qg->fifo_irq);
	if (ret) {
		device_init_wakeup(dev, false);
		return dev_err_probe(dev, ret, "FIFO wake IRQ\n");
	}
	platform_set_drvdata(pdev, qg);
	dev_info(dev, "PM6150 QG initialized at %d%%\n", qg->capacity);
	return 0;
}

static void qg_shutdown(struct platform_device *pdev)
{
	struct qcom_qg *qg = platform_get_drvdata(pdev);
	bool changed = false;
	int ret;

	disable_irq(qg->fifo_irq);
	disable_irq(qg->ocv_irq);
	mutex_lock(&qg->lock);
	ret = qg_drain(qg, &changed);
	if (!ret)
		ret = qg_sdam_save(qg);
	mutex_unlock(&qg->lock);
	if (ret)
		dev_warn(qg->dev, "Failed to save shutdown state: %d\n", ret);
}

static void qg_remove(struct platform_device *pdev)
{
	qg_shutdown(pdev);
	dev_pm_clear_wake_irq(&pdev->dev);
	device_init_wakeup(&pdev->dev, false);
}

static int qg_suspend(struct device *dev)
{
	struct qcom_qg *qg = dev_get_drvdata(dev);
	bool changed = false;
	int ret, status, save_ret = 0;

	disable_irq(qg->fifo_irq);
	disable_irq(qg->ocv_irq);
	mutex_lock(&qg->lock);
	ret = qg_status(qg, &status);
	if (ret)
		goto out;
	ret = qg_drain(qg, &changed);
	if (ret)
		goto out;
	if (status != POWER_SUPPLY_STATUS_CHARGING)
		ret = qg_configure(qg, 8, 256, 200);
	if (!ret)
		save_ret = qg_sdam_save(qg);
out:
	mutex_unlock(&qg->lock);
	enable_irq(qg->ocv_irq);
	enable_irq(qg->fifo_irq);
	if (changed || ret)
		power_supply_changed(qg->psy);
	if (save_ret)
		dev_warn_ratelimited(dev, "Failed to save suspend state: %d\n", save_ret);
	return ret;
}

static int qg_resume(struct device *dev)
{
	struct qcom_qg *qg = dev_get_drvdata(dev);
	bool changed = false;
	int ret, save_ret = 0;

	disable_irq(qg->fifo_irq);
	disable_irq(qg->ocv_irq);
	mutex_lock(&qg->lock);
	ret = qg_drain(qg, &changed);
	if (ret)
		dev_warn_ratelimited(dev, "Failed to read suspend samples: %d\n", ret);
	ret = qg_configure(qg, 5, 128, 100);
	if (!ret)
		ret = qg_update_ocv(qg, &changed);
	if (!ret)
		ret = qg_update_status(qg);
	if (!ret)
		save_ret = qg_sdam_save(qg);
	mutex_unlock(&qg->lock);
	enable_irq(qg->ocv_irq);
	enable_irq(qg->fifo_irq);
	if (changed || ret)
		power_supply_changed(qg->psy);
	if (save_ret)
		dev_warn_ratelimited(dev, "Failed to save resume state: %d\n", save_ret);
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
	.shutdown = qg_shutdown,
};
module_platform_driver(qg_driver);

MODULE_DESCRIPTION("Qualcomm PM6150 QG fuel gauge");
MODULE_AUTHOR("thedanilfez <thedanilfezlol@gmail.com>");
MODULE_LICENSE("GPL");
