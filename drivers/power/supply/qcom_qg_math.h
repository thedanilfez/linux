/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef QCOM_QG_MATH_H
#define QCOM_QG_MATH_H

#include <linux/limits.h>
#include <linux/math64.h>
#include <linux/minmax.h>
#include <linux/types.h>

#define QG_CYCLE_BUCKETS	8

struct qg_cycle_counter {
	u16 count[QG_CYCLE_BUCKETS];
	int start_soc[QG_CYCLE_BUCKETS];
};

/* full_nah exceeds U32_MAX on a 5 Ah battery. Use a 64-bit divisor. */
static inline int qg_raw_soc_bp(u64 charge_nah, u64 full_nah)
{
	if (!full_nah)
		return 0;
	return div64_u64(charge_nah * 10000 + full_nah / 2, full_nah);
}

static inline int qg_raw_soc_percent(u64 charge_nah, u64 full_nah)
{
	if (!full_nah)
		return 0;
	return div64_u64(charge_nah * 100 + full_nah / 2, full_nah);
}

static inline int qg_usable_capacity(int raw_bp, int cutoff_bp, bool full)
{
	int percent;

	percent = raw_bp <= cutoff_bp ? 0 :
		DIV_ROUND_CLOSEST((raw_bp - cutoff_bp) * 100,
				  10000 - cutoff_bp);
	return clamp(percent, 0, full ? 100 : 99);
}

static inline bool qg_cycle_update(struct qg_cycle_counter *counter, int soc,
				   bool charging)
{
	int i, bucket = min(soc * QG_CYCLE_BUCKETS / 10000,
			    QG_CYCLE_BUCKETS - 1);
	bool changed = false;

	if (charging) {
		if (counter->start_soc[bucket] < 0)
			counter->start_soc[bucket] = soc;
		return false;
	}

	for (i = 0; i < QG_CYCLE_BUCKETS; i++) {
		if (counter->start_soc[i] >= 0 &&
		    soc - counter->start_soc[i] >= 10000 / QG_CYCLE_BUCKETS / 2 &&
		    counter->count[i] < U16_MAX) {
			counter->count[i]++;
			changed = true;
		}
		counter->start_soc[i] = -1;
	}
	return changed;
}

static inline int qg_learned_capacity(s64 charge_nah, int start_bp,
				      int full_uah, int design_uah)
{
	s64 learned;

	if (charge_nah <= 0 || start_bp < 1000 || start_bp > 1500)
		return 0;
	learned = div_s64(charge_nah * 10, 10000 - start_bp);
	if (learned < design_uah / 2 || learned > (s64)design_uah * 110 / 100 ||
	    learned > U16_MAX * 1000)
		return 0;

	/* Limit changes to 1% up and 2% down per complete measurement. */
	return clamp_t(s64, learned, (s64)full_uah * 98 / 100,
		       (s64)full_uah * 101 / 100);
}

#endif
