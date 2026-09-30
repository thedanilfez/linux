// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2022, The Linux Foundation. All rights reserved.

#include <linux/export.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/notifier.h>
#include <linux/regmap.h>
#include <linux/remoteproc/qcom_rproc.h>

#include "lpass-macro-common.h"

static DEFINE_MUTEX(lpass_codec_mutex);
static enum lpass_codec_version lpass_codec_version;

struct lpass_macro_ssr {
	struct mutex lock;
	struct notifier_block nb;
	struct regmap *regmap;
	void *cookie;
	bool offline;
};

static int lpass_macro_ssr_notify(struct notifier_block *nb,
				  unsigned long event, void *data)
{
	struct lpass_macro_ssr *ssr = container_of(nb, struct lpass_macro_ssr, nb);

	if (event == QCOM_SSR_BEFORE_SHUTDOWN) {
		mutex_lock(&ssr->lock);
		/* DSP reset removes the hardware vote before consumer teardown. */
		WRITE_ONCE(ssr->offline, true);
		regcache_cache_only(ssr->regmap, true);
		regcache_mark_dirty(ssr->regmap);
		mutex_unlock(&ssr->lock);
	}
	return NOTIFY_OK;
}

static void lpass_macro_ssr_unregister(void *data)
{
	struct lpass_macro_ssr *ssr = data;

	qcom_unregister_ssr_notifier(ssr->cookie, &ssr->nb);
}

struct lpass_macro_ssr *
lpass_macro_regmap_register_ssr(struct device *dev, struct regmap *regmap)
{
	struct lpass_macro_ssr *ssr;
	struct of_phandle_args args;
	bool adsp = false;
	int i, ret;

	/* Native-clock/Audioreach platforms do not use this ADSP clock provider. */
	for (i = 0; !of_parse_phandle_with_args(dev->of_node, "clocks",
					     "#clock-cells", i, &args); i++) {
		adsp = of_device_is_compatible(args.np, "qcom,q6afe-clocks");
		of_node_put(args.np);
		if (adsp)
			break;
	}
	if (!adsp || !IS_REACHABLE(CONFIG_QCOM_RPROC_COMMON))
		return NULL;
	ssr = devm_kzalloc(dev, sizeof(*ssr), GFP_KERNEL);
	if (!ssr)
		return ERR_PTR(-ENOMEM);
	ssr->regmap = regmap;
	mutex_init(&ssr->lock);
	ssr->nb.notifier_call = lpass_macro_ssr_notify;
	ssr->cookie = qcom_register_ssr_notifier("adsp", &ssr->nb);
	if (IS_ERR(ssr->cookie))
		return ERR_CAST(ssr->cookie);
	ret = devm_add_action_or_reset(dev, lpass_macro_ssr_unregister, ssr);
	return ret ? ERR_PTR(ret) : ssr;
}
EXPORT_SYMBOL_GPL(lpass_macro_regmap_register_ssr);

bool lpass_macro_is_ssr_down(struct lpass_macro_ssr *ssr)
{
	return ssr && READ_ONCE(ssr->offline);
}
EXPORT_SYMBOL_GPL(lpass_macro_is_ssr_down);

int lpass_macro_ssr_lock(struct lpass_macro_ssr *ssr)
{
	if (!ssr)
		return 0;
	mutex_lock(&ssr->lock);
	if (ssr->offline) {
		mutex_unlock(&ssr->lock);
		return -EHOSTDOWN;
	}
	return 0;
}
EXPORT_SYMBOL_GPL(lpass_macro_ssr_lock);

void lpass_macro_ssr_unlock(struct lpass_macro_ssr *ssr)
{
	if (ssr)
		mutex_unlock(&ssr->lock);
}
EXPORT_SYMBOL_GPL(lpass_macro_ssr_unlock);


static int lpass_macro_link_clock_suppliers(struct device *dev)
{
	struct of_phandle_args args;
	struct platform_device *supplier;
	struct device_link *link;
	u32 flags;
	int i, ret;

	for (i = 0; ; i++) {
		ret = of_parse_phandle_with_args(dev->of_node, "clocks",
						"#clock-cells", i, &args);
		if (ret == -ENOENT)
			return 0;
		if (ret)
			return ret;

		/* APR clock devices disappear on an audio protection-domain restart. */
		if (of_device_is_compatible(args.np, "qcom,q6afe-clocks")) {
			flags = DL_FLAG_AUTOREMOVE_CONSUMER;
		} else if (of_property_present(args.np, "#sound-dai-cells")) {
			/* The fsgen gate is in another macro's register/power domain. */
			flags = DL_FLAG_AUTOREMOVE_CONSUMER | DL_FLAG_PM_RUNTIME |
				DL_FLAG_RPM_ACTIVE;
		} else {
			of_node_put(args.np);
			continue;
		}

		supplier = of_find_device_by_node(args.np);
		of_node_put(args.np);
		if (!supplier)
			return -EPROBE_DEFER;
		if (!device_is_bound(&supplier->dev)) {
			put_device(&supplier->dev);
			return -EPROBE_DEFER;
		}
		link = device_link_add(dev, &supplier->dev, flags);
		put_device(&supplier->dev);
		if (!link)
			return -EINVAL;
	}
}

struct lpass_macro *lpass_macro_pds_init(struct device *dev)
{
	struct lpass_macro *l_pds;
	int ret;

	ret = lpass_macro_link_clock_suppliers(dev);
	if (ret)
		return ERR_PTR(ret);

	if (!of_property_present(dev->of_node, "power-domains"))
		return NULL;

	l_pds = devm_kzalloc(dev, sizeof(*l_pds), GFP_KERNEL);
	if (!l_pds)
		return ERR_PTR(-ENOMEM);

	l_pds->macro_pd = dev_pm_domain_attach_by_name(dev, "macro");
	if (IS_ERR_OR_NULL(l_pds->macro_pd)) {
		ret = l_pds->macro_pd ? PTR_ERR(l_pds->macro_pd) : -ENODATA;
		goto macro_err;
	}

	ret = pm_runtime_resume_and_get(l_pds->macro_pd);
	if (ret < 0)
		goto macro_sync_err;

	l_pds->dcodec_pd = dev_pm_domain_attach_by_name(dev, "dcodec");
	if (IS_ERR_OR_NULL(l_pds->dcodec_pd)) {
		ret = l_pds->dcodec_pd ? PTR_ERR(l_pds->dcodec_pd) : -ENODATA;
		goto dcodec_err;
	}

	ret = pm_runtime_resume_and_get(l_pds->dcodec_pd);
	if (ret < 0)
		goto dcodec_sync_err;
	return l_pds;

dcodec_sync_err:
	dev_pm_domain_detach(l_pds->dcodec_pd, false);
dcodec_err:
	pm_runtime_put(l_pds->macro_pd);
macro_sync_err:
	dev_pm_domain_detach(l_pds->macro_pd, false);
macro_err:
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(lpass_macro_pds_init);

void lpass_macro_pds_exit(struct lpass_macro *pds)
{
	if (pds) {
		pm_runtime_put(pds->macro_pd);
		dev_pm_domain_detach(pds->macro_pd, false);
		pm_runtime_put(pds->dcodec_pd);
		dev_pm_domain_detach(pds->dcodec_pd, false);
	}
}
EXPORT_SYMBOL_GPL(lpass_macro_pds_exit);

void lpass_macro_set_codec_version(enum lpass_codec_version version)
{
	mutex_lock(&lpass_codec_mutex);
	lpass_codec_version = version;
	mutex_unlock(&lpass_codec_mutex);
}
EXPORT_SYMBOL_GPL(lpass_macro_set_codec_version);

enum lpass_codec_version lpass_macro_get_codec_version(void)
{
	enum lpass_codec_version ver;

	mutex_lock(&lpass_codec_mutex);
	ver = lpass_codec_version;
	mutex_unlock(&lpass_codec_mutex);

	return ver;
}
EXPORT_SYMBOL_GPL(lpass_macro_get_codec_version);

MODULE_DESCRIPTION("Common macro driver");
MODULE_LICENSE("GPL");
