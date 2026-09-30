// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2012-2017, The Linux Foundation. All rights reserved.
// Copyright (c) 2020, Stephan Gerhold

#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <sound/soc.h>
#include <sound/pcm_params.h>
#include <dt-bindings/sound/qcom,q6dsp-lpass-ports.h>
#include <dt-bindings/sound/qcom,q6voice.h>
#include "q6voice.h"
#include "q6voice-common.h"

#define DRV_NAME	"q6voice-dai"

struct q6voice_dai {
	struct q6voice *voice;
	enum q6voice_path_type path;
	unsigned int opened;
	struct snd_soc_pcm_runtime *rtd;
	bool tx_muted;
	unsigned int rx_volume;
	u32 tx_topology;
	u32 rx_topology;
};

#define Q6VOICE_EC_REF_INTERNAL		(-1)

struct q6voice_rx_port_desc {
	unsigned int index;
	int ec_ref_index;
};

static const struct q6voice_rx_port_desc q6voice_rx_ports[] = {
	{ RX_CODEC_DMA_RX_0, Q6VOICE_EC_REF_INTERNAL },
	{ WSA_CODEC_DMA_RX_0, Q6VOICE_EC_REF_INTERNAL },
	{ PRIMARY_MI2S_RX, Q6VOICE_EC_REF_INTERNAL },
};

static int q6voice_rx_ec_ref_port(int index)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(q6voice_rx_ports); i++) {
		if (q6voice_rx_ports[i].index == index)
			return q6voice_rx_ports[i].ec_ref_index;
	}

	return Q6VOICE_EC_REF_INTERNAL;
}

static int q6voice_dai_apply(struct q6voice_dai *priv);

static int q6voice_dai_startup(struct snd_pcm_substream *substream,
			       struct snd_soc_dai *dai)
{
	struct q6voice_dai *priv = snd_soc_dai_get_drvdata(dai);
	int ret;

	priv->rtd = snd_soc_substream_to_rtd(substream);
	ret = q6voice_start(priv->voice, priv->path, substream->stream);
	if (!ret)
		priv->opened |= BIT(substream->stream);
	return ret;
}

static int q6voice_dai_prepare(struct snd_pcm_substream *substream,
			       struct snd_soc_dai *dai)
{
	struct q6voice_dai *priv = snd_soc_dai_get_drvdata(dai);
	int ret;

	ret = q6voice_dai_apply(priv);
	if (!ret)
		ret = q6voice_prepare(priv->voice, priv->path, substream->stream);
	return ret;
}

static void q6voice_dai_shutdown(struct snd_pcm_substream *substream,
				 struct snd_soc_dai *dai)
{
	struct q6voice_dai *priv = snd_soc_dai_get_drvdata(dai);

	q6voice_stop(priv->voice, priv->path, substream->stream);
	priv->opened &= ~BIT(substream->stream);
}

static int q6voice_dai_hw_params(struct snd_pcm_substream *substream,
				 struct snd_pcm_hw_params *params,
				 struct snd_soc_dai *dai)
{
	return 0;
}

static const struct snd_soc_dai_ops q6voice_dai_ops = {
	.startup = q6voice_dai_startup,
	.hw_params = q6voice_dai_hw_params,
	.prepare = q6voice_dai_prepare,
	.shutdown = q6voice_dai_shutdown,
};

static struct snd_soc_dai_driver q6voice_dais[] = {
	{
		.id = CS_VOICE,
		.name = "CS-VOICE",
		.playback = {
			.stream_name = "CS-VOICE Playback",
			.formats = SNDRV_PCM_FMTBIT_S16_LE |
				   SNDRV_PCM_FMTBIT_S32_LE,
			.rates = SNDRV_PCM_RATE_CONTINUOUS,
			.rate_min = 8000,
			.rate_max = 48000,
			.channels_min = 1,
			.channels_max = 2,
		},
		.capture = {
			.stream_name = "CS-VOICE Capture",
			.formats = SNDRV_PCM_FMTBIT_S16_LE |
				   SNDRV_PCM_FMTBIT_S32_LE,
			.rates = SNDRV_PCM_RATE_CONTINUOUS,
			.rate_min = 8000,
			.rate_max = 48000,
			.channels_min = 1,
			.channels_max = 2,
		},
		.ops = &q6voice_dai_ops,
	},
};

/* fixme: need to use codec-to-codec */
static const struct snd_pcm_hardware q6voice_dai_hardware = {
	.info =			SNDRV_PCM_INFO_INTERLEAVED,
	.formats =		SNDRV_PCM_FMTBIT_S16_LE |
				SNDRV_PCM_FMTBIT_S32_LE,
	.rates =		SNDRV_PCM_RATE_CONTINUOUS,
	.rate_min =		8000,
	.rate_max =		48000,
	.channels_min =		1,
	.channels_max =		2,
	.buffer_bytes_max =	64 * 1024,
	.period_bytes_min =	64,
	.period_bytes_max =	64 * 1024,
	.periods_min =		1,
	.periods_max =		1024,
	.fifo_size =		0,
};

static int q6voice_dai_open(struct snd_soc_component *component,
			    struct snd_pcm_substream *substream)
{
	substream->runtime->hw = q6voice_dai_hardware;
	return 0;
}

static int q6voice_dai_copy(struct snd_soc_component *component,
			    struct snd_pcm_substream *substream, int channel,
			    unsigned long pos, struct iov_iter *iter,
			    unsigned long bytes)
{
	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE) {
		if (iov_iter_zero(bytes, iter) != bytes)
			return -EFAULT;
	} else {
		iov_iter_advance(iter, bytes);
	}

	return 0;
}

static snd_pcm_uframes_t
q6voice_dai_pointer(struct snd_soc_component *component,
		    struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	snd_pcm_uframes_t pos = READ_ONCE(runtime->control->appl_ptr);

	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
		pos += runtime->period_size;

	return pos % runtime->buffer_size;
}

static int q6voice_get_mixer(struct snd_kcontrol *kcontrol,
			     struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_dapm_context *dapm = snd_soc_dapm_kcontrol_to_dapm(kcontrol);
	struct snd_soc_component *c = snd_soc_dapm_to_component(dapm);
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	struct q6voice_dai *priv = snd_soc_component_get_drvdata(c);
	struct q6voice *v = priv->voice;
	bool capture = !!mc->shift;

	ucontrol->value.integer.value[0] =
		q6voice_get_port(v, priv->path, capture) == mc->reg;
	return 0;
}

static int q6voice_dai_apply(struct q6voice_dai *priv)
{
	struct q6voice_device_cfg cfg = {};
	struct snd_soc_dpcm *dpcm;
	int stream;

	cfg.tx_port_index = q6voice_get_port(priv->voice, priv->path, true);
	cfg.rx_port_index = q6voice_get_port(priv->voice, priv->path, false);
	cfg.ec_ref_index = q6voice_rx_ec_ref_port(cfg.rx_port_index);
	cfg.external_ec = cfg.ec_ref_index >= 0;
	cfg.tx_topology_id = priv->tx_topology;
	cfg.rx_topology_id = priv->rx_topology;

	if (priv->rtd) {
		for (stream = 0; stream < 2; stream++) {
			int index = stream ? cfg.tx_port_index : cfg.rx_port_index;
			bool found = false;

			for_each_dpcm_be(priv->rtd, stream, dpcm) {
				struct snd_soc_pcm_runtime *be = dpcm->be;
				struct snd_pcm_hw_params *params = &be->dpcm[stream].hw_params;

				if (dpcm->state == SND_SOC_DPCM_LINK_STATE_FREE ||
				    snd_soc_rtd_to_cpu(be, 0)->id != index ||
				    be->dpcm[stream].state < SND_SOC_DPCM_STATE_PREPARE ||
				    be->dpcm[stream].state >= SND_SOC_DPCM_STATE_HW_FREE)
					continue;
				found = true;
				if (stream) {
					cfg.tx_rate = params_rate(params);
					cfg.tx_channels = params_channels(params);
					cfg.tx_bits = params_width(params);
				} else {
					cfg.rx_rate = params_rate(params);
					cfg.rx_channels = params_channels(params);
					cfg.rx_bits = params_width(params);
				}
			}
			if (index >= 0 && !found &&
			    priv->rtd->dpcm[stream].state >= SND_SOC_DPCM_STATE_HW_PARAMS &&
			    priv->rtd->dpcm[stream].state < SND_SOC_DPCM_STATE_HW_FREE)
				return -ENODEV;
		}
	}

	return q6voice_set_device(priv->voice, priv->path, &cfg);
}

static int q6voice_put_mixer(struct snd_kcontrol *kcontrol,
			     struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_dapm_context *dapm = snd_soc_dapm_kcontrol_to_dapm(kcontrol);
	struct snd_soc_component *c = snd_soc_dapm_to_component(dapm);
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	struct q6voice_dai *priv = snd_soc_component_get_drvdata(c);
	struct q6voice *v = priv->voice;
	bool val = !!ucontrol->value.integer.value[0];
	bool capture = !!mc->shift;
	int old_port = q6voice_get_port(v, priv->path, capture);
	int ret;

	if (val == (old_port == mc->reg))
		return 0;
	if (val && old_port >= 0)
		return -EBUSY;

	ret = q6voice_pause(v, priv->path);
	if (ret)
		return ret;
	q6voice_set_port(v, priv->path, capture, val ? mc->reg : -1);

	ret = snd_soc_dapm_mixer_update_power(dapm, kcontrol, val, NULL);
	if (ret < 0)
		return ret;

	snd_soc_dpcm_mutex_lock(c->card);
	ret = q6voice_dai_apply(priv);
	snd_soc_dpcm_mutex_unlock(c->card);
	return ret ?: 1;
}

static int q6voice_dai_tx_mute_get(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *c = snd_kcontrol_chip(kcontrol);
	struct q6voice_dai *priv = snd_soc_component_get_drvdata(c);

	ucontrol->value.integer.value[0] = priv->tx_muted;
	return 0;
}

static int q6voice_dai_tx_mute_put(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *c = snd_kcontrol_chip(kcontrol);
	struct q6voice_dai *priv = snd_soc_component_get_drvdata(c);
	bool val = !!ucontrol->value.integer.value[0];
	int ret;

	priv->tx_muted = val;
	ret = q6voice_set_tx_mute(priv->voice, priv->path, val);
	if (ret)
		return ret;

	return 1;
}

static int q6voice_dai_rx_volume_get(struct snd_kcontrol *kcontrol,
				     struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *c = snd_kcontrol_chip(kcontrol);
	struct q6voice_dai *priv = snd_soc_component_get_drvdata(c);

	ucontrol->value.integer.value[0] = priv->rx_volume;
	return 0;
}

static int q6voice_dai_rx_volume_put(struct snd_kcontrol *kcontrol,
				     struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *c = snd_kcontrol_chip(kcontrol);
	struct q6voice_dai *priv = snd_soc_component_get_drvdata(c);
	unsigned int val = ucontrol->value.integer.value[0];
	int ret;

	if (val > 100)
		return -EINVAL;

	priv->rx_volume = val;
	ret = q6voice_set_rx_volume(priv->voice, priv->path, val);
	if (ret)
		return ret;

	return 1;
}

static int q6voice_dai_topology_get(struct snd_kcontrol *kcontrol,
				    struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *c = snd_kcontrol_chip(kcontrol);
	struct q6voice_dai *priv = snd_soc_component_get_drvdata(c);
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;

	ucontrol->value.integer.value[0] = mc->shift ? priv->rx_topology :
						       priv->tx_topology;
	return 0;
}

static int q6voice_dai_topology_put(struct snd_kcontrol *kcontrol,
				    struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *c = snd_kcontrol_chip(kcontrol);
	struct q6voice_dai *priv = snd_soc_component_get_drvdata(c);
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	u32 val = ucontrol->value.integer.value[0];
	int ret;

	if (mc->shift) {
		if (val == priv->rx_topology)
			return 0;
		priv->rx_topology = val;
	} else {
		if (val == priv->tx_topology)
			return 0;
		priv->tx_topology = val;
	}

	snd_soc_dpcm_mutex_lock(c->card);
	ret = q6voice_dai_apply(priv);
	snd_soc_dpcm_mutex_unlock(c->card);
	return ret ?: 1;
}

static const struct snd_kcontrol_new voice_tx_mixer_controls[] = {
	SOC_SINGLE_EXT("TX_CODEC_DMA_TX_0", TX_CODEC_DMA_TX_0, 1, 1, 0,
		       q6voice_get_mixer, q6voice_put_mixer),
	SOC_SINGLE_EXT("TX_CODEC_DMA_TX_3", TX_CODEC_DMA_TX_3, 1, 1, 0,
		       q6voice_get_mixer, q6voice_put_mixer),
};

static const struct snd_kcontrol_new wcd_rx_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", RX_CODEC_DMA_RX_0, 0, 1, 0,
		       q6voice_get_mixer, q6voice_put_mixer),
};

static const struct snd_kcontrol_new wsa_rx_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", WSA_CODEC_DMA_RX_0, 0, 1, 0,
		       q6voice_get_mixer, q6voice_put_mixer),
};

static const struct snd_kcontrol_new mi2s_rx_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", PRIMARY_MI2S_RX, 0, 1, 0,
		       q6voice_get_mixer, q6voice_put_mixer),
};

static const char * const q6voice_session_text[] = {
	"CS Voice", "VoiceMMode1", "VoiceMMode2", "VoLTE",
};
static const enum q6voice_path_type q6voice_session_paths[] = {
	Q6VOICE_PATH_VOICE, Q6VOICE_PATH_VOICEMMODE1,
	Q6VOICE_PATH_VOICEMMODE2, Q6VOICE_PATH_VOLTE,
};
static SOC_ENUM_SINGLE_EXT_DECL(q6voice_session_enum, q6voice_session_text);

static int q6voice_dai_session_get(struct snd_kcontrol *kcontrol,
				 struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *c = snd_kcontrol_chip(kcontrol);
	struct q6voice_dai *priv = snd_soc_component_get_drvdata(c);
	int i;

	for (i = 0; i < ARRAY_SIZE(q6voice_session_paths); i++)
		if (q6voice_session_paths[i] == priv->path)
			ucontrol->value.enumerated.item[0] = i;
	return 0;
}

static int q6voice_dai_session_put(struct snd_kcontrol *kcontrol,
				 struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *c = snd_kcontrol_chip(kcontrol);
	struct q6voice_dai *priv = snd_soc_component_get_drvdata(c);
	unsigned int item = ucontrol->value.enumerated.item[0];
	int rx_port, tx_port, ret = 0;

	if (item >= ARRAY_SIZE(q6voice_session_paths))
		return -EINVAL;
	snd_soc_dpcm_mutex_lock(c->card);
	if (priv->path == q6voice_session_paths[item])
		goto out;
	if (priv->opened) {
		ret = -EBUSY;
		goto out;
	}
	rx_port = q6voice_get_port(priv->voice, priv->path, false);
	tx_port = q6voice_get_port(priv->voice, priv->path, true);
	q6voice_set_port(priv->voice, priv->path, false, -1);
	q6voice_set_port(priv->voice, priv->path, true, -1);
	priv->path = q6voice_session_paths[item];
	q6voice_set_port(priv->voice, priv->path, false, rx_port);
	q6voice_set_port(priv->voice, priv->path, true, tx_port);
	ret = q6voice_dai_apply(priv);
	if (!ret)
		ret = q6voice_set_tx_mute(priv->voice, priv->path, priv->tx_muted);
	if (!ret)
		ret = q6voice_set_rx_volume(priv->voice, priv->path, priv->rx_volume);
	ret = ret ?: 1;
out:
	snd_soc_dpcm_mutex_unlock(c->card);
	return ret;
}

static const struct snd_kcontrol_new q6voice_dai_controls[] = {
	SOC_ENUM_EXT("Voice Call Session", q6voice_session_enum,
		     q6voice_dai_session_get, q6voice_dai_session_put),
	SOC_SINGLE_BOOL_EXT("Voice Call TX Mute", 0, q6voice_dai_tx_mute_get,
			    q6voice_dai_tx_mute_put),
	SOC_SINGLE_EXT("Voice Call RX Volume", 0, 0, 100, 0,
		       q6voice_dai_rx_volume_get, q6voice_dai_rx_volume_put),
	/* Zero means "driver default" (see q6voice.c Q6VOICE_*_TOPOLOGY) */
	SOC_SINGLE_EXT("Voice Call TX Topology", 0, 0, S32_MAX, 0,
		       q6voice_dai_topology_get, q6voice_dai_topology_put),
	SOC_SINGLE_EXT("Voice Call RX Topology", 0, 1, S32_MAX, 0,
		       q6voice_dai_topology_get, q6voice_dai_topology_put),
};

static const struct snd_soc_dapm_widget q6voice_dapm_widgets[] = {
	SND_SOC_DAPM_AIF_IN("CS-VOICE_DL1", "CS-VOICE Playback", 0,
			    SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_OUT("CS-VOICE_UL1", "CS-VOICE Capture", 0,
			     SND_SOC_NOPM, 0, 0),

	SND_SOC_DAPM_MIXER("CS-Voice Capture Mixer", SND_SOC_NOPM, 0, 0,
			   voice_tx_mixer_controls,
			   ARRAY_SIZE(voice_tx_mixer_controls)),

	SND_SOC_DAPM_MIXER("RX_CODEC_DMA_RX_0 Voice Mixer", SND_SOC_NOPM, 0, 0,
			   wcd_rx_mixer_controls,
			   ARRAY_SIZE(wcd_rx_mixer_controls)),
	SND_SOC_DAPM_MIXER("WSA_CODEC_DMA_RX_0 Voice Mixer", SND_SOC_NOPM, 0, 0,
			   wsa_rx_mixer_controls,
			   ARRAY_SIZE(wsa_rx_mixer_controls)),
	SND_SOC_DAPM_MIXER("PRIMARY_MI2S_RX Voice Mixer", SND_SOC_NOPM, 0, 0,
			   mi2s_rx_mixer_controls,
			   ARRAY_SIZE(mi2s_rx_mixer_controls)),
};

static const struct snd_soc_dapm_route q6voice_dapm_routes[] = {
	{ "CS-Voice Capture Mixer", "TX_CODEC_DMA_TX_0", "TX_CODEC_DMA_TX_0" },
	{ "CS-Voice Capture Mixer", "TX_CODEC_DMA_TX_3", "TX_CODEC_DMA_TX_3" },
	{ "CS-VOICE_UL1", NULL, "CS-Voice Capture Mixer" },

	{ "RX_CODEC_DMA_RX_0 Voice Mixer", "CS-Voice", "CS-VOICE_DL1" },
	{ "RX_CODEC_DMA_RX_0", NULL, "RX_CODEC_DMA_RX_0 Voice Mixer" },

	{ "WSA_CODEC_DMA_RX_0 Voice Mixer", "CS-Voice", "CS-VOICE_DL1" },
	{ "WSA_CODEC_DMA_RX_0", NULL, "WSA_CODEC_DMA_RX_0 Voice Mixer" },

	{ "PRIMARY_MI2S_RX Voice Mixer", "CS-Voice", "CS-VOICE_DL1" },
	{ "PRI_MI2S_RX", NULL, "PRIMARY_MI2S_RX Voice Mixer" },
};

static unsigned int q6voice_reg_read(struct snd_soc_component *component,
				     unsigned int reg)
{
	/* default value */
	return 0;
}

static int q6voice_reg_write(struct snd_soc_component *component,
			     unsigned int reg, unsigned int val)
{
	/* dummy */
	return 0;
}

static const struct snd_soc_component_driver q6voice_dai_component = {
	.name = DRV_NAME,
	.open = q6voice_dai_open,
	.copy = q6voice_dai_copy,
	.pointer = q6voice_dai_pointer,

	.controls = q6voice_dai_controls,
	.num_controls = ARRAY_SIZE(q6voice_dai_controls),

	.dapm_widgets = q6voice_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(q6voice_dapm_widgets),
	.dapm_routes = q6voice_dapm_routes,
	.num_dapm_routes = ARRAY_SIZE(q6voice_dapm_routes),
	.read = q6voice_reg_read,
	.write = q6voice_reg_write,

	/* Needs to probe after q6afe */
	.probe_order = SND_SOC_COMP_ORDER_LATE,
};

static int q6voice_dai_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct q6voice_dai *priv;
	struct q6voice *v;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	v = q6voice_create(dev);
	if (IS_ERR(v))
		return PTR_ERR(v);

	priv->voice = v;
	platform_set_drvdata(pdev, priv);

	return devm_snd_soc_register_component(dev, &q6voice_dai_component,
					       q6voice_dais,
					       ARRAY_SIZE(q6voice_dais));
}

static const struct of_device_id q6voice_dai_device_id[] = {
	{ .compatible = "qcom,q6voice-dais" },
	{}
};
MODULE_DEVICE_TABLE(of, q6voice_dai_device_id);

static struct platform_driver q6voice_dai_platform_driver = {
	.driver = {
		.name = "q6voice-dai",
		.of_match_table = of_match_ptr(q6voice_dai_device_id),
	},
	.probe = q6voice_dai_probe,
};
module_platform_driver(q6voice_dai_platform_driver);

MODULE_AUTHOR("Stephan Gerhold <stephan@gerhold.net>");
MODULE_DESCRIPTION("Q6Voice DAI driver");
MODULE_LICENSE("GPL v2");
