// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>

#include "qcom_qg_math.h"

static void qg_large_pack_capacity_test(struct kunit *test)
{
	/* The full charge in nAh must not truncate to a 32-bit divisor. */
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

static void qg_cycle_count_test(struct kunit *test)
{
	struct qg_cycle_counter counter = {};
	int i;

	for (i = 0; i < QG_CYCLE_BUCKETS; i++)
		counter.start_soc[i] = -1;
	for (i = 0; i < 10000; i += 100)
		KUNIT_EXPECT_FALSE(test, qg_cycle_update(&counter, i, true));
	KUNIT_EXPECT_TRUE(test, qg_cycle_update(&counter, 10000, false));
	for (i = 0; i < QG_CYCLE_BUCKETS; i++)
		KUNIT_EXPECT_EQ(test, counter.count[i], 1);
	KUNIT_EXPECT_FALSE(test, qg_cycle_update(&counter, 10000, false));
}

static void qg_partial_cycle_test(struct kunit *test)
{
	struct qg_cycle_counter counter = {};
	int i;

	for (i = 0; i < QG_CYCLE_BUCKETS; i++)
		counter.start_soc[i] = -1;
	qg_cycle_update(&counter, 2000, true);
	KUNIT_EXPECT_FALSE(test, qg_cycle_update(&counter, 2500, false));
	qg_cycle_update(&counter, 2000, true);
	KUNIT_EXPECT_TRUE(test, qg_cycle_update(&counter, 2700, false));
	KUNIT_EXPECT_EQ(test, counter.count[1], 1);

	counter.count[1] = U16_MAX;
	qg_cycle_update(&counter, 2000, true);
	KUNIT_EXPECT_FALSE(test, qg_cycle_update(&counter, 2700, false));
	KUNIT_EXPECT_EQ(test, counter.count[1], U16_MAX);
}

static void qg_capacity_learning_test(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, qg_learned_capacity(4500000000LL, 1000,
						  5000000, 5000000), 5000000);
	KUNIT_EXPECT_EQ(test, qg_learned_capacity(4250000000LL, 1500,
						  5000000, 5000000), 5000000);
	KUNIT_EXPECT_EQ(test, qg_learned_capacity(4590000000LL, 1000,
						  5000000, 5000000), 5050000);
	KUNIT_EXPECT_EQ(test, qg_learned_capacity(4320000000LL, 1000,
						  5000000, 5000000), 4900000);
	KUNIT_EXPECT_EQ(test, qg_learned_capacity(54000000000LL, 1000,
						  60000000, 60000000), 60000000);
}

static void qg_invalid_learning_test(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, qg_learned_capacity(-1, 1000,
						  5000000, 5000000), 0);
	KUNIT_EXPECT_EQ(test, qg_learned_capacity(4500000000LL, 0,
						  5000000, 5000000), 0);
	KUNIT_EXPECT_EQ(test, qg_learned_capacity(4500000000LL, 10000,
						  5000000, 5000000), 0);
	KUNIT_EXPECT_EQ(test, qg_learned_capacity(1000000000LL, 1000,
						  5000000, 5000000), 0);
	KUNIT_EXPECT_EQ(test, qg_learned_capacity(6000000000LL, 1000,
						  5000000, 5000000), 0);
	KUNIT_EXPECT_EQ(test, qg_learned_capacity(59400000000LL, 1000,
						  65000000, 65000000), 0);
}

static struct kunit_case qg_math_cases[] = {
	KUNIT_CASE(qg_large_pack_capacity_test),
	KUNIT_CASE(qg_usable_window_test),
	KUNIT_CASE(qg_cycle_count_test),
	KUNIT_CASE(qg_partial_cycle_test),
	KUNIT_CASE(qg_capacity_learning_test),
	KUNIT_CASE(qg_invalid_learning_test),
	{}
};

static struct kunit_suite qg_math_suite = {
	.name = "qcom-qg-math",
	.test_cases = qg_math_cases,
};

kunit_test_suite(qg_math_suite);

MODULE_LICENSE("GPL");
