// SPDX-License-Identifier: GPL-2.0-only
/*
 * Report the APPS suspend state to remote processors over SMP2P.
 */

#include <linux/bits.h>
#include <linux/interrupt.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeup.h>
#include <linux/soc/qcom/smem_state.h>
#include <linux/suspend.h>

/* Give userspace time to drain the data the remote woke us up for. */
#define SLEEPSTATE_WAKE_MS	200

struct smp2p_sleepstate {
	struct device *dev;
	struct qcom_smem_state *state;
	unsigned int bit;
	struct notifier_block pm_nb;
};

static int smp2p_sleepstate_pm_notify(struct notifier_block *nb,
				      unsigned long event, void *unused)
{
	struct smp2p_sleepstate *ss = container_of(nb, struct smp2p_sleepstate, pm_nb);

	switch (event) {
	case PM_SUSPEND_PREPARE:
		qcom_smem_state_update_bits(ss->state, BIT(ss->bit), 0);
		break;
	case PM_POST_SUSPEND:
		qcom_smem_state_update_bits(ss->state, BIT(ss->bit), BIT(ss->bit));
		break;
	}

	return NOTIFY_DONE;
}

static irqreturn_t smp2p_sleepstate_wake(int irq, void *data)
{
	struct smp2p_sleepstate *ss = data;

	pm_wakeup_dev_event(ss->dev, SLEEPSTATE_WAKE_MS, false);

	return IRQ_HANDLED;
}

static void smp2p_sleepstate_unregister(void *data)
{
	struct smp2p_sleepstate *ss = data;

	unregister_pm_notifier(&ss->pm_nb);
}

static int smp2p_sleepstate_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct smp2p_sleepstate *ss;
	int irq, ret;

	ss = devm_kzalloc(dev, sizeof(*ss), GFP_KERNEL);
	if (!ss)
		return -ENOMEM;

	ss->dev = dev;

	ss->state = devm_qcom_smem_state_get(dev, NULL, &ss->bit);
	if (IS_ERR(ss->state))
		return dev_err_probe(dev, PTR_ERR(ss->state),
				     "failed to get smem state\n");

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	ret = devm_device_init_wakeup(dev);
	if (ret)
		return ret;

	ret = devm_request_threaded_irq(dev, irq, NULL, smp2p_sleepstate_wake,
					IRQF_ONESHOT, "smp2p-sleepstate", ss);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request irq\n");

	qcom_smem_state_update_bits(ss->state, BIT(ss->bit), BIT(ss->bit));

	ss->pm_nb.notifier_call = smp2p_sleepstate_pm_notify;
	ret = register_pm_notifier(&ss->pm_nb);
	if (ret)
		return ret;

	return devm_add_action_or_reset(dev, smp2p_sleepstate_unregister, ss);
}

static const struct of_device_id smp2p_sleepstate_of_match[] = {
	{ .compatible = "qcom,smp2p-sleepstate" },
	{ }
};
MODULE_DEVICE_TABLE(of, smp2p_sleepstate_of_match);

static struct platform_driver smp2p_sleepstate_driver = {
	.probe = smp2p_sleepstate_probe,
	.driver = {
		.name = "qcom-smp2p-sleepstate",
		.of_match_table = smp2p_sleepstate_of_match,
	},
};
module_platform_driver(smp2p_sleepstate_driver);

MODULE_DESCRIPTION("Qualcomm SMP2P sleep state notifier");
MODULE_LICENSE("GPL");
