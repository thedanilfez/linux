// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>

#include "qcom_qg_math.h"

static void qg_large_pack_capacity_test(struct kunit *test)
{
	/* Observed on sweet: 1,599,040 uAh remaining from a 4,997,000 uAh
	 * full pack. The old 32-bit divisor produced a bogus 99% result.
	 */
	KUNIT_EXPECT_EQ(test, qg_raw_soc_bp(1599040000ULL, 4997000000ULL),
			3200);
	KUNIT_EXPECT_EQ(test,
			qg_usable_capacity(3200, 0, false), 32);
	KUNIT_EXPECT_EQ(test,
			qg_raw_soc_percent(1599040000ULL, 4997000000ULL),
			32);
}

static void qg_usable_window_test(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, qg_usable_capacity(3200, 950, false), 25);
	KUNIT_EXPECT_EQ(test, qg_usable_capacity(950, 950, false), 0);
	KUNIT_EXPECT_EQ(test, qg_usable_capacity(10000, 950, false), 99);
	KUNIT_EXPECT_EQ(test, qg_usable_capacity(10000, 950, true), 100);
}

static struct kunit_case qg_math_cases[] = {
	KUNIT_CASE(qg_large_pack_capacity_test),
	KUNIT_CASE(qg_usable_window_test),
	{}
};

static struct kunit_suite qg_math_suite = {
	.name = "qcom-qg-math",
	.test_cases = qg_math_cases,
};
kunit_test_suite(qg_math_suite);

MODULE_LICENSE("GPL");
