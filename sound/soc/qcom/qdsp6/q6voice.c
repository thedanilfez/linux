// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2012-2017, The Linux Foundation. All rights reserved.
// Copyright (c) 2020, Stephan Gerhold

#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <sound/pcm.h>
#include "q6afe.h"
#include "q6core.h"
#include "q6cvp.h"
#include "q6cvs.h"
#include "q6mvm.h"
#include "q6voice-common.h"

/*
 * AFE port indices are configured by the ASoC/DAPM layer; the DSP port IDs are
 * resolved with q6afe_get_port_id() right before a command is built.
 */

/* Both PCM substreams must be open (and prepared) before voice can start */
#define Q6VOICE_BOTH_STREAMS	(BIT(SNDRV_PCM_STREAM_PLAYBACK) | \
				 BIT(SNDRV_PCM_STREAM_CAPTURE))

/* Defaults for debugfs bring-up; the ASoC path supplies real BE parameters. */
#define Q6VOICE_DEVICE_RATE		48000
#define Q6VOICE_DEVICE_BITS		16
#define Q6VOICE_DEVICE_CHANNELS		2

/* Vendor voice_get_topology() fallback when ACDB calibration is absent. */
#define Q6VOICE_TX_TOPOLOGY		VSS_IVOCPROC_TOPOLOGY_ID_NONE
#define Q6VOICE_RX_TOPOLOGY		VSS_IVOCPROC_TOPOLOGY_ID_RX_DEFAULT

/* CVD service versions that change the command set */
#define Q6VOICE_CVD_2_2			0x0202
#define Q6VOICE_CVD_2_3			0x0203

/* CVP AVCS service version that introduced SET_PARAM channel info */
#define Q6VOICE_CVP_SERVICE_V2		2

#define Q6VOICE_MUTE_RAMP_MS		500
#define Q6VOICE_VOLUME_RAMP_MS		20

enum q6voice_path_state {
	Q6VOICE_STATE_IDLE = 0,
	Q6VOICE_STATE_STARTING,
	Q6VOICE_STATE_RUNNING,
	Q6VOICE_STATE_PAUSED,
	Q6VOICE_STATE_STOPPING,
	Q6VOICE_STATE_ERROR,
};

static const char * const q6voice_state_name[] = {
	[Q6VOICE_STATE_IDLE] = "IDLE",
	[Q6VOICE_STATE_STARTING] = "STARTING",
	[Q6VOICE_STATE_RUNNING] = "RUNNING",
	[Q6VOICE_STATE_PAUSED] = "PAUSED",
	[Q6VOICE_STATE_STOPPING] = "STOPPING",
	[Q6VOICE_STATE_ERROR] = "ERROR",
};

/**
 * struct q6voice_caps - firmware capabilities, discovered once per boot
 *
 * @detected: the caps below have been populated from the firmware
 * @cvd_version: raw CVD version string reported by MVM
 * @cvd_int: CVD version as ``major << 8 | minor`` for easy comparison
 * @cvp_service_version: AVCS CVP service API version (0 if unknown)
 * @cvp_v3: use the V3 vocproc create/set-device commands
 * @channel_info: send vocproc channel info with SET_PARAM_V2
 * @dev_channels: send the legacy TOPOLOGY_SET_DEV_CHANNELS instead
 * @endpoint_media_info: send the per-port endpoint media info parameters
 * @topology_commit: send VSS_IVOCPROC_CMD_TOPOLOGY_COMMIT
 * @instance_id: SET_PARAM_V3 carries an instance id
 */
struct q6voice_caps {
	bool detected;
	char cvd_version[Q6VOICE_CVD_VERSION_MAX];
	unsigned int cvd_int;
	int cvp_service_version;
	bool cvp_v3;
	bool channel_info;
	bool dev_channels;
	bool endpoint_media_info;
	bool topology_commit;
	bool instance_id;
};

struct q6voice_path_runtime {
	struct q6voice_session *sessions[Q6VOICE_SERVICE_COUNT];
	enum q6voice_path_state state;

	unsigned int opened;	/* BIT(SNDRV_PCM_STREAM_*) */
	unsigned int prepared;	/* BIT(SNDRV_PCM_STREAM_*) */

	struct q6voice_device_cfg cfg;

	/* Cached stream controls, applied to the DSP once the path runs */
	bool tx_muted;
	u16 rx_volume;
};

struct q6voice_path {
	struct q6voice *v;

	enum q6voice_path_type type;
	/*
	 * Currently selected AFE port indices. These are read without the
	 * state mutex from the DAPM kcontrol getter, so they are accessed with
	 * READ_ONCE()/WRITE_ONCE().
	 */
	int tx_port_index;
	int rx_port_index;
	/* Serialize access to the voice path session and its state */
	struct mutex lock;
	struct q6voice_path_runtime *runtime;
};

struct q6voice {
	struct device *dev;
	struct q6voice_path paths[Q6VOICE_PATH_COUNT];

	struct mutex caps_lock;
	struct q6voice_caps caps;

#ifdef CONFIG_DEBUG_FS
	struct dentry *debugfs_dir;
#endif
};

/* ------------------------------------------------------- capability probe */

static bool q6voice_path_is_ap_owned(enum q6voice_path_type path)
{
	return path == Q6VOICE_PATH_VOIP;
}

/*
 * The AP destroys the sessions it owns. For CS voice the modem owns the MVM
 * and CVS sessions, so the passive handles must not be destroyed and are kept
 * across calls; the DSP keeps the underlying modem session alive. VoLTE and
 * the AP-owned VoIP path are torn down by the AP.
 */
static bool q6voice_session_needs_destroy(enum q6voice_path_type path,
					  enum q6voice_service_type svc)
{
	if (svc == Q6VOICE_SERVICE_CVP)
		return true;
	return q6voice_path_is_ap_owned(path) || path == Q6VOICE_PATH_VOLTE;
}

static int q6voice_parse_cvd_version(const char *version, unsigned int *major,
				     unsigned int *minor)
{
	if (sscanf(version, "%u.%u", major, minor) != 2)
		return -EINVAL;

	return 0;
}

static int q6voice_query_cvd_version(struct q6voice *v)
{
	char version[Q6VOICE_CVD_VERSION_MAX] = {};
	unsigned int major, minor;
	u32 status;
	int ret;

	/*
	 * The CVD version is a command response (VSS_IVERSION_RSP_GET), not an
	 * APR basic result, and only the MVM service implements it. The query
	 * must be sent with a valid source port; the DSP echoes it into
	 * dest_port of the response.
	 */
	ret = q6voice_service_request(Q6VOICE_SERVICE_MVM, VSS_IVERSION_CMD_GET,
				      VSS_IVERSION_RSP_GET, Q6VOICE_PATH_VOICE,
				      version, sizeof(version), &status);
	if (ret) {
		dev_err(v->dev, "CVD version query failed: %d\n", ret);
		return ret;
	}

	version[sizeof(version) - 1] = '\0';
	if (q6voice_parse_cvd_version(version, &major, &minor)) {
		dev_err(v->dev, "invalid CVD version string \"%s\"\n", version);
		return -EINVAL;
	}

	strscpy(v->caps.cvd_version, version, sizeof(v->caps.cvd_version));
	v->caps.cvd_int = (major << 8) | minor;

	return 0;
}

static int q6voice_query_cvp_service_version(struct q6voice *v)
{
	struct q6core_svc_api_info info = {};
	int ret;

	ret = q6core_get_svc_api_info(APR_SVC_ADSP_CVP, &info);
	if (ret < 0 || !info.api_version) {
		dev_err(v->dev, "CVP service version query failed: %d\n",
			ret < 0 ? ret : -ENODATA);
		return ret < 0 ? ret : -ENODATA;
	}

	v->caps.cvp_service_version = info.api_version;

	return 0;
}

static void q6voice_derive_caps(struct q6voice *v)
{
	struct q6voice_caps *caps = &v->caps;
	bool cvd_2_2 = caps->cvd_int >= Q6VOICE_CVD_2_2;
	bool cvd_2_3 = caps->cvd_int >= Q6VOICE_CVD_2_3;

	caps->cvp_v3 = cvd_2_2;
	caps->topology_commit = cvd_2_2;
	caps->endpoint_media_info = cvd_2_3;
	caps->instance_id = true;

	if (caps->cvp_service_version >= Q6VOICE_CVP_SERVICE_V2) {
		caps->channel_info = true;
		caps->dev_channels = false;
	} else {
		/*
		 * The pre-2.0 vocproc only understood TOPOLOGY_SET_DEV_CHANNELS,
		 * and the vendor only sends it on CVD 2.2+ firmware.
		 */
		caps->channel_info = false;
		caps->dev_channels = cvd_2_2;
	}
}

static int q6voice_detect_caps(struct q6voice *v)
{
	int ret;

	guard(mutex)(&v->caps_lock);

	if (v->caps.detected)
		return 0;

	ret = q6voice_query_cvd_version(v);
	if (ret)
		return ret;

	ret = q6voice_query_cvp_service_version(v);
	if (ret)
		return ret;

	q6voice_derive_caps(v);
	v->caps.detected = true;

	return 0;
}

/* ------------------------------------------------------------- helpers */

static struct q6voice_session *
q6voice_path_session(struct q6voice_path_runtime *rt,
		     enum q6voice_service_type svc)
{
	struct q6voice_session *s = rt->sessions[svc];

	if (s && !q6voice_session_is_live(s)) {
		/* Stale handle, e.g. after an ADSP restart */
		rt->sessions[svc] = NULL;
		q6voice_session_release(s);
		s = NULL;
	}

	return s;
}

static void q6voice_fill_channel_map(u8 *map, unsigned int channels)
{
	memset(map, 0, Q6CVP_CHANNEL_MAP_SIZE);
	map[0] = Q6CVP_PCM_CHANNEL_FL;
	if (channels > 1)
		map[1] = Q6CVP_PCM_CHANNEL_FR;
}

static int q6voice_build_cvp_config(struct q6voice_path *p,
				    const struct q6voice_device_cfg *cfg,
				    struct q6cvp_config *cvp_cfg)
{
	int tx_port_id, rx_port_id;

	tx_port_id = q6afe_get_port_id(cfg->tx_port_index);
	if (tx_port_id < 0) {
		dev_err(p->v->dev, "invalid TX AFE port index %d\n",
			cfg->tx_port_index);
		return -EINVAL;
	}

	rx_port_id = q6afe_get_port_id(cfg->rx_port_index);
	if (rx_port_id < 0) {
		dev_err(p->v->dev, "invalid RX AFE port index %d\n",
			cfg->rx_port_index);
		return -EINVAL;
	}

	memset(cvp_cfg, 0, sizeof(*cvp_cfg));
	cvp_cfg->tx_port_id = tx_port_id;
	cvp_cfg->rx_port_id = rx_port_id;
	cvp_cfg->tx_topology_id = cfg->tx_topology_id ? :
				  Q6VOICE_TX_TOPOLOGY;
	cvp_cfg->rx_topology_id = cfg->rx_topology_id ? :
				  Q6VOICE_RX_TOPOLOGY;
	cvp_cfg->vocproc_mode = cfg->external_ec ?
		VSS_IVOCPROC_VOCPROC_MODE_EC_EXT_MIXING :
		VSS_IVOCPROC_VOCPROC_MODE_EC_INT_MIXING;

	if (cfg->external_ec && cfg->ec_ref_index >= 0) {
		int ec_port_id = q6afe_get_port_id(cfg->ec_ref_index);

		if (ec_port_id < 0) {
			dev_err(p->v->dev, "invalid EC ref port index %d\n",
				cfg->ec_ref_index);
			return -EINVAL;
		}
		cvp_cfg->ec_ref_port_id = ec_port_id;
	} else {
		cvp_cfg->ec_ref_port_id = VSS_IVOCPROC_PORT_ID_NONE;
	}

	dev_dbg(p->v->dev,
		"path %d: tx_port=%#06x rx_port=%#06x tx_topo=%#x rx_topo=%#x mode=%#x ec_ref=%#06x\n",
		p->type, cvp_cfg->tx_port_id, cvp_cfg->rx_port_id,
		cvp_cfg->tx_topology_id, cvp_cfg->rx_topology_id,
		cvp_cfg->vocproc_mode, cvp_cfg->ec_ref_port_id);

	return 0;
}

/*
 * Configure the vocproc device channels and media format. This is the part of
 * the vendor voice_setup_vocproc() sequence that sits between CVP create and
 * TOPOLOGY_COMMIT; the firmware rejects the commit (which then invalidates the
 * session) if the device channels are not described correctly.
 */
static int q6voice_configure_media(struct q6voice_path *p,
				   const struct q6cvp_config *cvp_cfg)
{
	struct q6voice *v = p->v;
	struct q6voice_caps *caps = &v->caps;
	struct q6voice_session *cvp = p->runtime->sessions[Q6VOICE_SERVICE_CVP];
	const struct q6voice_device_cfg *cfg = &p->runtime->cfg;
	u8 rx_map[Q6CVP_CHANNEL_MAP_SIZE], tx_map[Q6CVP_CHANNEL_MAP_SIZE];
	unsigned int rx_channels = cfg->rx_channels ?: Q6VOICE_DEVICE_CHANNELS;
	unsigned int tx_channels = cfg->tx_channels ?: Q6VOICE_DEVICE_CHANNELS;
	unsigned int rx_bits = cfg->rx_bits ?: Q6VOICE_DEVICE_BITS;
	unsigned int tx_bits = cfg->tx_bits ?: Q6VOICE_DEVICE_BITS;
	unsigned int rx_rate = cfg->rx_rate ?: Q6VOICE_DEVICE_RATE;
	unsigned int tx_rate = cfg->tx_rate ?: Q6VOICE_DEVICE_RATE;
	int ret;

	q6voice_fill_channel_map(rx_map, rx_channels);
	q6voice_fill_channel_map(tx_map, tx_channels);

	if (caps->channel_info) {
		ret = q6cvp_set_channel_info(cvp,
				VSS_PARAM_VOCPROC_RX_CHANNEL_INFO,
				rx_channels, rx_bits, rx_map);
		if (ret)
			goto err;

		ret = q6cvp_set_channel_info(cvp,
				VSS_PARAM_VOCPROC_TX_CHANNEL_INFO,
				tx_channels, tx_bits, tx_map);
		if (ret)
			goto err;

		ret = q6cvp_set_channel_info(cvp,
				VSS_PARAM_VOCPROC_EC_REF_CHANNEL_INFO,
				rx_channels, rx_bits, rx_map);
		if (ret)
			goto err;
	} else if (caps->dev_channels) {
		ret = q6cvp_set_dev_channels(cvp, tx_channels, rx_channels);
		if (ret)
			goto err;
	}

	if (caps->endpoint_media_info) {
		ret = q6cvp_set_endpoint_media_info(cvp,
				VSS_PARAM_RX_PORT_ENDPOINT_MEDIA_INFO,
				caps->instance_id, cvp_cfg->rx_port_id,
				rx_channels, rx_bits, rx_rate, rx_map);
		if (ret)
			goto err;

		ret = q6cvp_set_endpoint_media_info(cvp,
				VSS_PARAM_TX_PORT_ENDPOINT_MEDIA_INFO,
				caps->instance_id, cvp_cfg->tx_port_id,
				tx_channels, tx_bits, tx_rate, tx_map);
		if (ret)
			goto err;

		if (cvp_cfg->ec_ref_port_id != VSS_IVOCPROC_PORT_ID_NONE) {
			ret = q6cvp_set_endpoint_media_info(cvp,
				VSS_PARAM_EC_REF_PORT_ENDPOINT_MEDIA_INFO,
				caps->instance_id, cvp_cfg->ec_ref_port_id,
				rx_channels, rx_bits, rx_rate, rx_map);
			if (ret)
				goto err;
		}
	}

	return 0;

err:
	dev_err(v->dev, "path %d: media configuration failed: %d\n",
		p->type, ret);
	return ret;
}

static int q6voice_configure_rx_mfc(struct q6voice_path *p)
{
	struct q6voice_session *cvp = p->runtime->sessions[Q6VOICE_SERVICE_CVP];
	const struct q6voice_device_cfg *cfg = &p->runtime->cfg;
	unsigned int channels = cfg->rx_channels ?: Q6VOICE_DEVICE_CHANNELS;
	unsigned int rate = cfg->rx_rate ?: Q6VOICE_DEVICE_RATE;
	u8 map[Q6CVP_CHANNEL_MAP_SIZE];
	int ret;

	/*
	 * The vocproc RX output is mono; the channel mixer and media format
	 * converter fan it out to the stereo device. The vendor only sends
	 * these when the RX device has more than one channel.
	 */
	if (channels <= 1)
		return 0;

	q6voice_fill_channel_map(map, channels);

	ret = q6cvp_set_rx_channel_mixer(cvp, channels);
	if (ret) {
		dev_warn(p->v->dev, "path %d: RX channel mixer failed: %d\n",
			 p->type, ret);
		return ret;
	}

	ret = q6cvp_set_rx_mfc_config(cvp, rate, channels, map);
	if (ret)
		dev_warn(p->v->dev, "path %d: RX MFC config failed: %d\n",
			 p->type, ret);

	return ret;
}

/* ------------------------------------------------------------- lifecycle */

/*
 * Destroy the sessions this path owns and drop the ones it does not. When
 * @release_all is false the modem-owned MVM/CVS sessions are kept (with their
 * DSP handles) so that the next call can reuse them, matching the vendor
 * stack. When it is true (device removal) everything is released, but the
 * modem-owned sessions are still not destroyed.
 */
static void q6voice_path_teardown(struct q6voice_path *p, bool release_all)
{
	struct q6voice_path_runtime *rt = p->runtime;
	enum q6voice_service_type svc;

	for (svc = 0; svc < Q6VOICE_SERVICE_COUNT; svc++) {
		struct q6voice_session *s = rt->sessions[svc];
		bool owned;

		if (!s)
			continue;

		owned = q6voice_session_needs_destroy(p->type, svc);
		if (!owned && !release_all)
			continue;

		if (owned) {
			int ret = q6voice_session_destroy(s);

			if (ret)
				dev_warn(p->v->dev,
					 "path %d: failed to destroy service %d session: %d\n",
					 p->type, svc, ret);
		}

		rt->sessions[svc] = NULL;
		q6voice_session_release(s);
	}
}

static int q6voice_path_start(struct q6voice_path *p)
{
	struct q6voice *v = p->v;
	struct q6voice_path_runtime *rt = p->runtime;
	struct q6voice_session *mvm, *cvs = NULL, *cvp = NULL;
	struct q6cvp_config cvp_cfg;
	enum q6cvp_create_version version;
	int ret;

	ret = q6voice_detect_caps(v);
	if (ret)
		return ret;

	ret = q6voice_build_cvp_config(p, &rt->cfg, &cvp_cfg);
	if (ret)
		return ret;

	rt->state = Q6VOICE_STATE_STARTING;

	/*
	 * 1. Control sessions. For modem-controlled calls the MVM/CVS sessions
	 * are created once and then reused for every subsequent call, because
	 * the modem keeps the underlying session alive.
	 */
	mvm = q6voice_path_session(rt, Q6VOICE_SERVICE_MVM);
	if (!mvm) {
		mvm = q6mvm_session_create(p->type);
		if (IS_ERR(mvm)) {
			ret = PTR_ERR(mvm);
			dev_err(v->dev,
				"voice setup: path %d create MVM session failed: %d\n",
				p->type, ret);
			goto err;
		}
		rt->sessions[Q6VOICE_SERVICE_MVM] = mvm;
	}

	cvs = q6voice_path_session(rt, Q6VOICE_SERVICE_CVS);
	if (!cvs) {
		cvs = q6cvs_session_create(p->type);
		if (IS_ERR(cvs)) {
			ret = PTR_ERR(cvs);
			cvs = NULL;
			dev_err(v->dev,
				"voice setup: path %d create CVS session failed: %d\n",
				p->type, ret);
			goto err;
		}
		rt->sessions[Q6VOICE_SERVICE_CVS] = cvs;

		if (q6voice_path_is_ap_owned(p->type)) {
			ret = q6mvm_attach_stream(mvm, cvs, true);
			if (ret) {
				dev_err(v->dev,
					"voice setup: path %d attach stream failed: %d\n",
					p->type, ret);
				goto err;
			}
		}
	}

	/* 2. Let MVM know the modem drives the state machine */
	if (!q6voice_path_is_ap_owned(p->type)) {
		ret = q6mvm_set_dual_control(mvm, true);
		if (ret) {
			dev_err(v->dev,
				"voice setup: path %d dual control failed: %d\n",
				p->type, ret);
			goto err;
		}
	}

	/* 3. Full-control vocproc for the AP */
	version = v->caps.cvp_v3 ? Q6CVP_CREATE_V3 : Q6CVP_CREATE_V2;
	cvp = q6cvp_session_create(p->type, &cvp_cfg, version);
	if (IS_ERR(cvp)) {
		ret = PTR_ERR(cvp);
		dev_err(v->dev,
			"voice setup: path %d create vocproc (%s) failed: %d\n",
			p->type, v->caps.cvp_v3 ? "V3" : "V2", ret);
		goto err;
	}
	rt->sessions[Q6VOICE_SERVICE_CVP] = cvp;

	/*
	 * 4. Device channels / media format and topology commit. A failure
	 * here invalidates the vocproc, so it aborts the call setup instead of
	 * being ignored.
	 */
	ret = q6voice_configure_media(p, &cvp_cfg);
	if (ret)
		goto err_unwind;

	if (v->caps.topology_commit) {
		ret = q6cvp_topology_commit(cvp);
		if (ret) {
			dev_err(v->dev,
				"voice setup: path %d topology commit failed: %d\n",
				p->type, ret);
			goto err_unwind;
		}
	}

	ret = q6voice_configure_rx_mfc(p);
	if (ret)
		dev_warn(v->dev,
			 "voice setup: path %d RX MFC configuration failed: %d (continuing)\n",
			 p->type, ret);

	/* 5. Enable the vocproc before attaching it */
	ret = q6cvp_enable(cvp, true);
	if (ret) {
		dev_err(v->dev,
			"voice setup: path %d enable vocproc failed: %d\n",
			p->type, ret);
		goto err_unwind;
	}

	/* 6. Attach the vocproc to the modem-owned stream graph */
	ret = q6mvm_attach(mvm, cvp, true);
	if (ret) {
		dev_err(v->dev,
			"voice setup: path %d attach vocproc failed: %d\n",
			p->type, ret);
		goto err_unwind;
	}

	/* 7. Apply cached stream controls; failures here are not fatal */
	if (cvs) {
		ret = q6cvs_set_stream_mute(cvs, Q6VOICE_DIRECTION_TX,
					    rt->tx_muted, Q6VOICE_MUTE_RAMP_MS);
		if (ret)
			dev_warn(v->dev, "path %d: TX mute failed: %d\n",
				 p->type, ret);
	}

	if (rt->rx_volume) {
		ret = q6cvp_set_volume_step(cvp, Q6VOICE_DIRECTION_RX,
					    rt->rx_volume,
					    Q6VOICE_VOLUME_RAMP_MS);
		if (ret)
			dev_warn(v->dev, "path %d: RX volume failed: %d\n",
				 p->type, ret);
	}

	/* 8. Start the voice path */
	ret = q6mvm_start(mvm, true);
	if (ret) {
		dev_err(v->dev,
			"voice setup: path %d start voice failed: %d\n",
			p->type, ret);
		goto err_unwind;
	}

	rt->state = Q6VOICE_STATE_RUNNING;
	return 0;

err_unwind:
	if (cvp) {
		q6mvm_attach(mvm, cvp, false);
		q6cvp_enable(cvp, false);
	}
err:
	rt->state = Q6VOICE_STATE_ERROR;
	q6voice_path_teardown(p, false);
	return ret;
}

static void q6voice_path_stop(struct q6voice_path *p)
{
	struct q6voice *v = p->v;
	struct q6voice_path_runtime *rt = p->runtime;
	struct q6voice_session *mvm = rt->sessions[Q6VOICE_SERVICE_MVM];
	struct q6voice_session *cvp = rt->sessions[Q6VOICE_SERVICE_CVP];
	int ret;

	rt->state = Q6VOICE_STATE_STOPPING;

	if (mvm && cvp && cvp->handle) {
		ret = q6mvm_start(mvm, false);
		if (ret)
			dev_warn(v->dev, "path %d: stop voice failed: %d\n",
				 p->type, ret);

		ret = q6mvm_attach(mvm, cvp, false);
		if (ret)
			dev_warn(v->dev, "path %d: detach vocproc failed: %d\n",
				 p->type, ret);
	}

	/* Destroys (or drops) the sessions owned by this path */
	q6voice_path_teardown(p, false);
	rt->state = Q6VOICE_STATE_IDLE;
}

static bool q6voice_path_ready(struct q6voice_path *p)
{
	struct q6voice_path_runtime *rt = p->runtime;

	if (rt->opened == Q6VOICE_BOTH_STREAMS &&
	    rt->prepared == Q6VOICE_BOTH_STREAMS)
		return true;
	return false;
}

static int q6voice_try_start(struct q6voice_path *p)
{
	struct q6voice *v = p->v;
	struct q6voice_path_runtime *rt = p->runtime;
	int ret;

	if (rt->state != Q6VOICE_STATE_IDLE)
		return 0;

	if (!q6voice_path_ready(p)) {
		dev_dbg(v->dev,
			"path %d: deferring start (opened=%#x prepared=%#x)\n",
			p->type, rt->opened, rt->prepared);
		return 0;
	}

	if (rt->cfg.tx_port_index < 0 || rt->cfg.rx_port_index < 0) {
		dev_dbg(v->dev,
			"path %d: deferring start (tx_port=%d rx_port=%d, set the Voice Mixer controls)\n",
			p->type, rt->cfg.tx_port_index, rt->cfg.rx_port_index);
		return 0;
	}

	ret = q6voice_path_start(p);
	return ret;
}

static int q6voice_path_get_or_create(struct q6voice_path *p)
{
	if (!p->runtime) {
		p->runtime = kzalloc_obj(*p->runtime);
		if (!p->runtime)
			return -ENOMEM;
		p->runtime->cfg.tx_port_index = -1;
		p->runtime->cfg.rx_port_index = -1;
		p->runtime->cfg.ec_ref_index = -1;
		p->runtime->state = Q6VOICE_STATE_IDLE;
	}
	return 0;
}

int q6voice_start(struct q6voice *v, enum q6voice_path_type path, bool capture)
{
	struct q6voice_path *p;
	int ret;

	if (path >= Q6VOICE_PATH_COUNT)
		return -EINVAL;

	p = &v->paths[path];

	guard(mutex)(&p->lock);

	ret = q6voice_path_get_or_create(p);
	if (ret)
		return ret;

	if (p->runtime->opened & BIT(capture))
		return -EALREADY;

	p->runtime->opened |= BIT(capture);
	return 0;
}
EXPORT_SYMBOL_GPL(q6voice_start);

int q6voice_prepare(struct q6voice *v, enum q6voice_path_type path, bool capture)
{
	struct q6voice_path *p;
	int ret;

	if (path >= Q6VOICE_PATH_COUNT)
		return -EINVAL;

	p = &v->paths[path];

	guard(mutex)(&p->lock);

	if (!p->runtime || !(p->runtime->opened & BIT(capture)))
		return -EINVAL;

	p->runtime->prepared |= BIT(capture);

	ret = q6voice_try_start(p);
	if (ret)
		return ret;

	return 0;
}
EXPORT_SYMBOL_GPL(q6voice_prepare);

int q6voice_stop(struct q6voice *v, enum q6voice_path_type path, bool capture)
{
	struct q6voice_path *p;

	if (path >= Q6VOICE_PATH_COUNT)
		return -EINVAL;

	p = &v->paths[path];

	guard(mutex)(&p->lock);

	if (!p->runtime || !(p->runtime->opened & BIT(capture)))
		return 0;

	p->runtime->opened &= ~BIT(capture);
	p->runtime->prepared &= ~BIT(capture);

	if (p->runtime->state == Q6VOICE_STATE_RUNNING ||
	    p->runtime->state == Q6VOICE_STATE_PAUSED ||
	    p->runtime->state == Q6VOICE_STATE_ERROR)
		q6voice_path_stop(p);

	return 0;
}
EXPORT_SYMBOL_GPL(q6voice_stop);

int q6voice_pause(struct q6voice *v, enum q6voice_path_type path)
{
	struct q6voice_path *p;
	int ret;

	if (path >= Q6VOICE_PATH_COUNT)
		return -EINVAL;
	p = &v->paths[path];
	guard(mutex)(&p->lock);
	if (!p->runtime || p->runtime->state != Q6VOICE_STATE_RUNNING)
		return 0;
	ret = q6mvm_pause(p->runtime->sessions[Q6VOICE_SERVICE_MVM]);
	if (!ret)
		p->runtime->state = Q6VOICE_STATE_PAUSED;
	return ret;
}
EXPORT_SYMBOL_GPL(q6voice_pause);

static int q6voice_apply_device(struct q6voice_path *p)
{
	struct q6voice *v = p->v;
	struct q6voice_session *cvp = p->runtime->sessions[Q6VOICE_SERVICE_CVP];
	struct q6voice_session *mvm = p->runtime->sessions[Q6VOICE_SERVICE_MVM];
	struct q6cvp_config cvp_cfg;
	enum q6cvp_create_version version;
	int ret;

	/*
	 * A device switch reaches us one mixer at a time (UCM disables the old
	 * route before enabling the new one), so a port may temporarily be
	 * unset. Keep the current DSP configuration and wait for both ports.
	 */
	if (p->runtime->cfg.tx_port_index < 0 ||
	    p->runtime->cfg.rx_port_index < 0) {
		dev_dbg(v->dev,
			"path %d: deferring device switch (tx_port=%d rx_port=%d)\n",
			p->type, p->runtime->cfg.tx_port_index,
			p->runtime->cfg.rx_port_index);
		return 0;
	}

	if (!cvp || !cvp->handle)
		return 0;

	ret = q6voice_build_cvp_config(p, &p->runtime->cfg, &cvp_cfg);
	if (ret)
		return ret;

	version = v->caps.cvp_v3 ? Q6CVP_CREATE_V3 : Q6CVP_CREATE_V2;

	/*
	 * A device switch re-points the vocproc at different AFE ports while
	 * the graph is live. The vendor pauses the MVM voice path first
	 * (voc_disable_device() -> voice_pause_voice_call()), reprograms the
	 * device and resumes with MVM START_VOICE (below). Follow that order;
	 * the old route must stay attached if the pause fails.
	 */
	if (mvm && p->runtime->state == Q6VOICE_STATE_RUNNING) {
		ret = q6mvm_pause(mvm);
		if (ret)
			return ret;
		p->runtime->state = Q6VOICE_STATE_PAUSED;
	}

	ret = q6cvp_set_device(cvp, &cvp_cfg, version);
	if (ret) {
		dev_err(v->dev, "voice setup: path %d set device failed: %d\n",
			p->type, ret);
		return ret;
	}

	ret = q6voice_configure_media(p, &cvp_cfg);
	if (ret)
		return ret;

	if (v->caps.topology_commit) {
		ret = q6cvp_topology_commit(cvp);
		if (ret) {
			dev_err(v->dev,
				"voice setup: path %d topology commit failed: %d\n",
				p->type, ret);
			return ret;
		}
	}

	q6voice_configure_rx_mfc(p);

	/*
	 * The vendor stack restarts the voice path after a device change so
	 * the DSP re-evaluates the topology.
	 */
	if (mvm) {
		ret = q6mvm_start(mvm, true);
		if (ret) {
			dev_err(v->dev, "path %d: restart after switch failed: %d\n",
				p->type, ret);
			return ret;
		}
	}

	p->runtime->state = Q6VOICE_STATE_RUNNING;
	return 0;
}

static bool q6voice_device_equal(const struct q6voice_device_cfg *a,
				 const struct q6voice_device_cfg *b)
{
	return a->tx_port_index == b->tx_port_index &&
	       a->rx_port_index == b->rx_port_index &&
	       a->ec_ref_index == b->ec_ref_index &&
	       a->tx_topology_id == b->tx_topology_id &&
	       a->rx_topology_id == b->rx_topology_id &&
	       a->external_ec == b->external_ec &&
	       a->tx_rate == b->tx_rate &&
	       a->rx_rate == b->rx_rate &&
	       a->tx_channels == b->tx_channels &&
	       a->rx_channels == b->rx_channels &&
	       a->tx_bits == b->tx_bits &&
	       a->rx_bits == b->rx_bits;
}

int q6voice_set_device(struct q6voice *v, enum q6voice_path_type path,
		       const struct q6voice_device_cfg *cfg)
{
	struct q6voice_path *p;
	int ret;

	if (path >= Q6VOICE_PATH_COUNT)
		return -EINVAL;

	p = &v->paths[path];

	guard(mutex)(&p->lock);

	ret = q6voice_path_get_or_create(p);
	if (ret)
		return ret;

	if (q6voice_device_equal(&p->runtime->cfg, cfg) &&
	    p->runtime->state == Q6VOICE_STATE_RUNNING)
		return 0;

	WRITE_ONCE(p->tx_port_index, cfg->tx_port_index);
	WRITE_ONCE(p->rx_port_index, cfg->rx_port_index);
	p->runtime->cfg = *cfg;

	if (p->runtime->state == Q6VOICE_STATE_RUNNING ||
	    p->runtime->state == Q6VOICE_STATE_PAUSED) {
		ret = q6voice_apply_device(p);
		if (ret) {
			q6voice_path_stop(p);
			return ret;
		}
	}

	return q6voice_try_start(p);
}
EXPORT_SYMBOL_GPL(q6voice_set_device);

int q6voice_set_tx_mute(struct q6voice *v, enum q6voice_path_type path,
			bool mute)
{
	struct q6voice_path *p;
	struct q6voice_session *cvs;
	int ret;

	if (path >= Q6VOICE_PATH_COUNT)
		return -EINVAL;

	p = &v->paths[path];

	guard(mutex)(&p->lock);

	if (!p->runtime)
		return -EINVAL;

	p->runtime->tx_muted = mute;

	if (p->runtime->state != Q6VOICE_STATE_RUNNING)
		return 0;

	cvs = p->runtime->sessions[Q6VOICE_SERVICE_CVS];
	if (!cvs)
		return 0;

	ret = q6cvs_set_stream_mute(cvs, Q6VOICE_DIRECTION_TX, mute,
				    Q6VOICE_MUTE_RAMP_MS);
	if (ret)
		dev_err(v->dev, "path %d: TX mute failed: %d\n", p->type, ret);

	return ret;
}
EXPORT_SYMBOL_GPL(q6voice_set_tx_mute);

int q6voice_set_rx_volume(struct q6voice *v, enum q6voice_path_type path,
			  u16 volume)
{
	struct q6voice_path *p;
	struct q6voice_session *cvp;
	int ret;

	if (path >= Q6VOICE_PATH_COUNT)
		return -EINVAL;

	p = &v->paths[path];

	guard(mutex)(&p->lock);

	if (!p->runtime)
		return -EINVAL;

	p->runtime->rx_volume = volume;

	if (p->runtime->state != Q6VOICE_STATE_RUNNING)
		return 0;

	cvp = p->runtime->sessions[Q6VOICE_SERVICE_CVP];
	if (!cvp)
		return 0;

	ret = q6cvp_set_volume_step(cvp, Q6VOICE_DIRECTION_RX, volume,
				    Q6VOICE_VOLUME_RAMP_MS);
	if (ret)
		dev_err(v->dev, "path %d: RX volume failed: %d\n", p->type,
			ret);

	return ret;
}
EXPORT_SYMBOL_GPL(q6voice_set_rx_volume);

int q6voice_get_port(struct q6voice *v, enum q6voice_path_type path,
		     bool capture)
{
	if (path >= Q6VOICE_PATH_COUNT)
		return -1;

	return capture ? READ_ONCE(v->paths[path].tx_port_index) :
			 READ_ONCE(v->paths[path].rx_port_index);
}
EXPORT_SYMBOL_GPL(q6voice_get_port);

void q6voice_set_port(struct q6voice *v, enum q6voice_path_type path,
		      bool capture, int index)
{
	struct q6voice_path *p;

	if (path >= Q6VOICE_PATH_COUNT)
		return;

	p = &v->paths[path];
	if (capture)
		WRITE_ONCE(p->tx_port_index, index);
	else
		WRITE_ONCE(p->rx_port_index, index);
}
EXPORT_SYMBOL_GPL(q6voice_set_port);

/* -------------------------------------------------- developer interface */

int q6voice_debug_start(struct q6voice *v, enum q6voice_path_type path,
			int tx_index, int rx_index, bool external_ec,
			int ec_ref_index)
{
	struct q6voice_path *p;
	int ret;

	if (path >= Q6VOICE_PATH_COUNT)
		return -EINVAL;

	p = &v->paths[path];

	guard(mutex)(&p->lock);

	ret = q6voice_path_get_or_create(p);
	if (ret)
		return ret;

	if (p->runtime->state != Q6VOICE_STATE_IDLE)
		return -EBUSY;

	WRITE_ONCE(p->tx_port_index, tx_index);
	WRITE_ONCE(p->rx_port_index, rx_index);
	p->runtime->cfg.tx_port_index = tx_index;
	p->runtime->cfg.rx_port_index = rx_index;
	p->runtime->cfg.external_ec = external_ec;
	p->runtime->cfg.ec_ref_index = ec_ref_index;
	p->runtime->cfg.tx_topology_id = 0;
	p->runtime->cfg.rx_topology_id = 0;

	/* Pretend both substreams are open and prepared */
	p->runtime->opened = Q6VOICE_BOTH_STREAMS;
	p->runtime->prepared = Q6VOICE_BOTH_STREAMS;

	return q6voice_path_start(p);
}
EXPORT_SYMBOL_GPL(q6voice_debug_start);

int q6voice_debug_stop(struct q6voice *v, enum q6voice_path_type path)
{
	struct q6voice_path *p;

	if (path >= Q6VOICE_PATH_COUNT)
		return -EINVAL;

	p = &v->paths[path];

	guard(mutex)(&p->lock);

	if (!p->runtime)
		return 0;

	if (p->runtime->state == Q6VOICE_STATE_RUNNING ||
	    p->runtime->state == Q6VOICE_STATE_PAUSED ||
	    p->runtime->state == Q6VOICE_STATE_ERROR)
		q6voice_path_stop(p);

	p->runtime->opened = 0;
	p->runtime->prepared = 0;
	return 0;
}
EXPORT_SYMBOL_GPL(q6voice_debug_stop);

int q6voice_debug_status(struct q6voice *v, enum q6voice_path_type path,
			 char *buf, size_t len)
{
	struct q6voice_path_runtime *rt;
	struct q6voice_path *p;
	enum q6voice_service_type svc;
	int n = 0;

	if (path >= Q6VOICE_PATH_COUNT)
		return -EINVAL;

	p = &v->paths[path];

	guard(mutex)(&p->lock);
	rt = p->runtime;

	n += scnprintf(buf + n, len - n, "path %d (%s):\n", path,
		       q6voice_state_name[rt ? rt->state : Q6VOICE_STATE_IDLE]);

	scoped_guard(mutex, &v->caps_lock) {
		n += scnprintf(buf + n, len - n, "  cvd_version: %s\n",
			       v->caps.detected ? v->caps.cvd_version :
						  "<unqueried>");
		n += scnprintf(buf + n, len - n,
			       "  caps: cvp_v3=%d channel_info=%d dev_channels=%d topology_commit=%d media_info=%d\n",
			       v->caps.cvp_v3, v->caps.channel_info,
			       v->caps.dev_channels, v->caps.topology_commit,
			       v->caps.endpoint_media_info);
	}

	if (!rt) {
		n += scnprintf(buf + n, len - n, "  runtime: inactive\n");
		return n;
	}

	n += scnprintf(buf + n, len - n,
		       "  opened=%#x prepared=%#x tx_port=%d rx_port=%d ec_ref=%d external_ec=%d\n",
		       rt->opened, rt->prepared, rt->cfg.tx_port_index,
		       rt->cfg.rx_port_index, rt->cfg.ec_ref_index,
		       rt->cfg.external_ec);

	n += scnprintf(buf + n, len - n,
		       "  handles: mvm=%#06x cvs=%#06x cvp=%#06x\n",
		       rt->sessions[Q6VOICE_SERVICE_MVM] ?
				rt->sessions[Q6VOICE_SERVICE_MVM]->handle : 0,
		       rt->sessions[Q6VOICE_SERVICE_CVS] ?
				rt->sessions[Q6VOICE_SERVICE_CVS]->handle : 0,
		       rt->sessions[Q6VOICE_SERVICE_CVP] ?
				rt->sessions[Q6VOICE_SERVICE_CVP]->handle : 0);

	for (svc = 0; svc < Q6VOICE_SERVICE_COUNT; svc++) {
		struct q6voice_session *s = rt->sessions[svc];

		if (!s)
			continue;
		n += scnprintf(buf + n, len - n,
			       "  service %d: last opcode=%#010x status=%s (%#x)\n",
			       svc, s->last_opcode,
			       q6voice_status_str(s->last_status),
			       s->last_status);
	}

	return n;
}
EXPORT_SYMBOL_GPL(q6voice_debug_status);

/* ------------------------------------- developer debugfs (bring-up only) */

/*
 * This lets a developer drive and observe the DSP session lifecycle without
 * going through ALSA/UCM/PipeWire. It is NOT a stable userspace ABI and can be
 * removed once call audio is driven entirely by the ASoC path.
 *
 *	cat  /sys/kernel/debug/q6voice/<dev>/status
 *	echo "start 0 <tx_port_index> <rx_port_index> [external_ec] [ec_ref]" \
 *		> /sys/kernel/debug/q6voice/<dev>/control
 *	echo "stop 0" > /sys/kernel/debug/q6voice/<dev>/control
 */
#ifdef CONFIG_DEBUG_FS

#define Q6VOICE_DEBUGFS_BUF_SIZE	2048


static int q6voice_status_show(struct seq_file *m, void *data)
{
	struct q6voice *v = m->private;
	enum q6voice_path_type path;
	char *buf;
	int ret;

	buf = kzalloc(Q6VOICE_DEBUGFS_BUF_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	for (path = 0; path < Q6VOICE_PATH_COUNT; path++) {
		ret = q6voice_debug_status(v, path, buf,
					   Q6VOICE_DEBUGFS_BUF_SIZE);
		if (ret > 0)
			seq_write(m, buf, ret);
	}

	kfree(buf);
	return 0;
}

static int q6voice_status_open(struct inode *inode, struct file *file)
{
	return single_open(file, q6voice_status_show, inode->i_private);
}

static ssize_t q6voice_control_write(struct file *file,
				     const char __user *ubuf, size_t count,
				     loff_t *ppos)
{
	struct q6voice *v = file_inode(file)->i_private;
	unsigned int path, tx, rx;
	int ext = 0, ec = -1;
	char *buf;
	int ret;

	buf = memdup_user_nul(ubuf, count);
	if (IS_ERR(buf))
		return PTR_ERR(buf);

	if (sscanf(buf, "start %u %u %u %d %d", &path, &tx, &rx, &ext,
		   &ec) >= 3) {
		if (path >= Q6VOICE_PATH_COUNT) {
			ret = -EINVAL;
			goto out;
		}
		ret = q6voice_debug_start(v, path, tx, rx, ext,
					  ext && ec < 0 ? (int)rx : ec);
	} else if (sscanf(buf, "stop %u", &path) == 1) {
		if (path >= Q6VOICE_PATH_COUNT) {
			ret = -EINVAL;
			goto out;
		}
		ret = q6voice_debug_stop(v, path);
	} else {
		ret = -EINVAL;
	}

out:
	kfree(buf);
	return ret ? ret : count;
}

static const struct file_operations q6voice_status_fops = {
	.owner = THIS_MODULE,
	.open = q6voice_status_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static const struct file_operations q6voice_control_fops = {
	.owner = THIS_MODULE,
	.write = q6voice_control_write,
	.llseek = noop_llseek,
};

static int q6voice_debugfs_init(struct q6voice *v)
{
	v->debugfs_dir = debugfs_create_dir(dev_name(v->dev), NULL);
	if (IS_ERR(v->debugfs_dir)) {
		int ret = PTR_ERR(v->debugfs_dir);

		v->debugfs_dir = NULL;
		return ret;
	}

	debugfs_create_file("status", 0444, v->debugfs_dir, v,
			    &q6voice_status_fops);
	debugfs_create_file("control", 0200, v->debugfs_dir, v,
			    &q6voice_control_fops);

	return 0;
}

static void q6voice_debugfs_exit(struct q6voice *v)
{
	debugfs_remove_recursive(v->debugfs_dir);
	v->debugfs_dir = NULL;
}

#else /* !CONFIG_DEBUG_FS */

static inline int q6voice_debugfs_init(struct q6voice *v) { return 0; }
static inline void q6voice_debugfs_exit(struct q6voice *v) { }

#endif /* CONFIG_DEBUG_FS */

/* ------------------------------------------------------------- device */

static void q6voice_free(void *data)
{
	struct q6voice *v = data;
	enum q6voice_path_type path;

	q6voice_debugfs_exit(v);

	for (path = 0; path < Q6VOICE_PATH_COUNT; path++) {
		struct q6voice_path *p = &v->paths[path];

		mutex_lock(&p->lock);
		if (p->runtime) {
			if (p->runtime->state == Q6VOICE_STATE_RUNNING ||
			    p->runtime->state == Q6VOICE_STATE_PAUSED ||
			    p->runtime->state == Q6VOICE_STATE_ERROR)
				q6voice_path_stop(p);
			/* Drop sessions kept across calls (modem-owned ones too) */
			q6voice_path_teardown(p, true);
			kfree(p->runtime);
			p->runtime = NULL;
		}
		mutex_unlock(&p->lock);
		mutex_destroy(&p->lock);
	}
}

struct q6voice *q6voice_create(struct device *dev)
{
	struct q6voice *v;
	enum q6voice_path_type path;
	int ret;

	v = devm_kzalloc(dev, sizeof(*v), GFP_KERNEL);
	if (!v)
		return ERR_PTR(-ENOMEM);

	v->dev = dev;
	mutex_init(&v->caps_lock);

	for (path = 0; path < Q6VOICE_PATH_COUNT; path++) {
		struct q6voice_path *p = &v->paths[path];

		p->v = v;
		p->type = path;
		p->tx_port_index = -1;
		p->rx_port_index = -1;
		mutex_init(&p->lock);
	}

	ret = devm_add_action(dev, q6voice_free, v);
	if (ret)
		return ERR_PTR(ret);

	ret = q6voice_debugfs_init(v);
	if (ret)
		dev_warn(dev, "failed to create debugfs interface: %d\n", ret);

	return v;
}
EXPORT_SYMBOL_GPL(q6voice_create);

MODULE_AUTHOR("Stephan Gerhold <stephan@gerhold.net>");
MODULE_DESCRIPTION("Q6Voice driver");
MODULE_LICENSE("GPL v2");
