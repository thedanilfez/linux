/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef QCOM_QG_MATH_H
#define QCOM_QG_MATH_H

#include <linux/math64.h>
#include <linux/minmax.h>

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

#endif
