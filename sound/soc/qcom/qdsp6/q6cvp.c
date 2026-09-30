// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2026 thedanilfez <thedanilfezlol@gmail.com>
 * Copyright (c) 2012-2017, The Linux Foundation. All rights reserved.
 * Copyright (c) 2020, Stephan Gerhold
 *
 * CVP - Core Voice Processor / vocproc (ADSP voice service 0x0B).
 *
 * The vocproc is the processing element that bridges the modem-owned vocoder
 * stream to the AFE ports. Its create/set-device commands exist in several
 * revisions; which one the firmware accepts depends on the CVD version, so
 * the caller passes the revision it negotiated with the DSP.
 *
 * All wire structures are packed and match the vendor
 * (techpack/audio/include/dsp/q6voice.h) byte for byte.
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/soc/qcom/apr.h>
#include "q6cvp.h"
#include "q6voice-common.h"

#define VSS_IVOCPROC_CMD_CREATE_FULL_CONTROL_SESSION_V2	0x000112BF
#define VSS_IVOCPROC_CMD_CREATE_FULL_CONTROL_SESSION_V3	0x00013169
#define VSS_IVOCPROC_CMD_SET_DEVICE_V2			0x000112C6
#define VSS_IVOCPROC_CMD_SET_DEVICE_V3			0x0001316A
#define VSS_IVOCPROC_CMD_TOPOLOGY_SET_DEV_CHANNELS	0x00013199
#define VSS_IVOCPROC_CMD_TOPOLOGY_COMMIT		0x00013198
#define VSS_IVOCPROC_CMD_ENABLE				0x000100C6
#define VSS_IVOCPROC_CMD_DISABLE			0x000110E1
#define VSS_IVOLUME_CMD_SET_STEP			0x000112C2

#define VSS_ICOMMON_CMD_SET_PARAM_V2			0x0001133D
#define VSS_ICOMMON_CMD_SET_PARAM_V3			0x00013245

#define INSTANCE_ID_0					0

#define Q6CVP_DEFAULT_RAMP_MS				20

/* Coefficient for unity gain in Q14 */
#define Q6CVP_GAIN_Q14_UNITY				(1 << 14)

/* ------------------------------------------------------- create / device */

struct q6cvp_create_cmd {
	struct apr_hdr hdr;
	u16 direction;
	u16 tx_port_id;
	u32 tx_topology_id;
	u16 rx_port_id;
	u32 rx_topology_id;
	u32 profile_id;
	u32 vocproc_mode;
	u16 ec_ref_port_id;
	char name[Q6VOICE_SESSION_NAME_LEN];
} __packed;

struct q6cvp_set_device_cmd {
	struct apr_hdr hdr;
	u16 tx_port_id;
	u32 tx_topology_id;
	u16 rx_port_id;
	u32 rx_topology_id;
	u32 vocproc_mode;
	u16 ec_ref_port_id;
} __packed;

struct q6cvp_dev_channels_cmd {
	struct apr_hdr hdr;
	u16 tx_num_channels;
	u16 rx_num_channels;
} __packed;

struct q6cvp_volume_step_cmd {
	struct apr_hdr hdr;
	u16 direction;
	u32 value;
	u16 ramp_duration_ms;
} __packed;

/*
 * VSS parameter payloads. The SET_PARAM_V2 layout is
 *	{ module_id, param_id, param_size, reserved, <payload> }
 * and the SET_PARAM_V3 layout replaces the flat header with
 *	{ module_id, instance_id, reserved, param_id, param_size }.
 */
struct q6cvp_param_hdr_v1 {
	u32 module_id;
	u32 param_id;
	u16 param_size;
	u16 reserved;
} __packed;

struct q6cvp_param_hdr_v3 {
	u32 module_id;
	u16 instance_id;
	u16 reserved;
	u32 param_id;
	u32 param_size;
} __packed;

struct q6cvp_set_param_v2_cmd {
	struct apr_hdr hdr;
	u32 mem_handle;
	u64 mem_address;
	u32 mem_size;
	struct q6cvp_param_hdr_v1 param;
	union {
		struct {
			u32 num_channels;
			u32 bits_per_sample;
			u8 channel_mapping[Q6CVP_CHANNEL_MAP_SIZE];
		} __packed channel_info;
		struct {
			u32 index;
			u16 num_output_channels;
			u16 num_input_channels;
			u16 out_channel_map[2];
			u16 in_channel_map[1];
			u16 channel_weight_coeff[2][1];
			u16 reserved;
		} __packed ch_mixer;
		struct {
			u32 sample_rate;
			u16 bits_per_sample;
			u16 num_channels;
			u16 channel_type[Q6CVP_CHANNEL_MAP_SIZE];
		} __packed mfc;
	} data;
} __packed;

struct q6cvp_media_format_info {
	u32 port_id;
	u16 num_channels;
	u16 bits_per_sample;
	u32 sample_rate;
	u8 channel_mapping[Q6CVP_CHANNEL_MAP_SIZE];
} __packed;

struct q6cvp_set_param_v3_cmd {
	struct apr_hdr hdr;
	u32 mem_handle;
	u64 mem_address;
	u32 payload_size;
	struct q6cvp_param_hdr_v3 param;
	struct q6cvp_media_format_info info;
} __packed;

struct q6cvp_set_param_v2_endpoint_cmd {
	struct apr_hdr hdr;
	u32 mem_handle;
	u64 mem_address;
	u32 payload_size;
	struct q6cvp_param_hdr_v1 param;
	struct q6cvp_media_format_info info;
} __packed;

/*
 * Wire-format size checks. These mirror the vendor structures in
 * techpack/audio/include/dsp/q6voice.h; a size change here means the DSP
 * packet layout changed and must be re-validated against the firmware.
 */
static_assert(sizeof(struct q6cvp_create_cmd) == 64);
static_assert(sizeof(struct q6cvp_set_device_cmd) == 38);
static_assert(sizeof(struct q6cvp_dev_channels_cmd) == 24);
static_assert(sizeof(struct q6cvp_volume_step_cmd) == 28);
static_assert(sizeof(struct q6cvp_param_hdr_v1) == 12);
static_assert(sizeof(struct q6cvp_param_hdr_v3) == 16);
static_assert(sizeof(struct q6cvp_media_format_info) == 44);
static_assert(sizeof(struct q6cvp_set_param_v3_cmd) == 96);
static_assert(sizeof(struct q6cvp_set_param_v2_endpoint_cmd) == 92);
static_assert(sizeof(((struct q6cvp_set_param_v2_cmd *)0)->data) == 72);

static void q6cvp_fill_create_cmd(struct q6cvp_create_cmd *cmd,
				  const struct q6cvp_config *cfg,
				  enum q6cvp_create_version version)
{
	cmd->hdr.pkt_size = sizeof(*cmd);
	cmd->hdr.opcode = version == Q6CVP_CREATE_V3 ?
		VSS_IVOCPROC_CMD_CREATE_FULL_CONTROL_SESSION_V3 :
		VSS_IVOCPROC_CMD_CREATE_FULL_CONTROL_SESSION_V2;

	cmd->direction = VSS_IVOCPROC_DIRECTION_RX_TX;
	cmd->tx_port_id = cfg->tx_port_id;
	cmd->rx_port_id = cfg->rx_port_id;
	cmd->tx_topology_id = cfg->tx_topology_id;
	cmd->rx_topology_id = cfg->rx_topology_id;
	cmd->profile_id = VSS_ICOMMON_CAL_NETWORK_ID_NONE;
	cmd->vocproc_mode = cfg->vocproc_mode;
	cmd->ec_ref_port_id = cfg->ec_ref_port_id;
}

struct q6voice_session *
q6cvp_session_create(enum q6voice_path_type path,
		     const struct q6cvp_config *cfg,
		     enum q6cvp_create_version version)
{
	struct q6cvp_create_cmd cmd = {};

	q6cvp_fill_create_cmd(&cmd, cfg, version);

	return q6voice_session_create(Q6VOICE_SERVICE_CVP, path, &cmd.hdr);
}
EXPORT_SYMBOL_GPL(q6cvp_session_create);

int q6cvp_set_device(struct q6voice_session *cvp,
		     const struct q6cvp_config *cfg,
		     enum q6cvp_create_version version)
{
	struct q6cvp_set_device_cmd cmd = {};

	cmd.hdr.pkt_size = sizeof(cmd);
	cmd.hdr.opcode = version == Q6CVP_CREATE_V3 ?
		VSS_IVOCPROC_CMD_SET_DEVICE_V3 : VSS_IVOCPROC_CMD_SET_DEVICE_V2;

	cmd.tx_port_id = cfg->tx_port_id;
	cmd.rx_port_id = cfg->rx_port_id;
	cmd.tx_topology_id = cfg->tx_topology_id;
	cmd.rx_topology_id = cfg->rx_topology_id;
	cmd.vocproc_mode = cfg->vocproc_mode;
	cmd.ec_ref_port_id = cfg->ec_ref_port_id;

	return q6voice_common_send(cvp, &cmd.hdr);
}
EXPORT_SYMBOL_GPL(q6cvp_set_device);

/* ------------------------------------------------------------ enable etc. */

int q6cvp_enable(struct q6voice_session *cvp, bool enable)
{
	struct apr_pkt cmd = {};

	cmd.hdr.pkt_size = APR_HDR_SIZE;
	cmd.hdr.opcode = enable ? VSS_IVOCPROC_CMD_ENABLE :
				  VSS_IVOCPROC_CMD_DISABLE;

	return q6voice_common_send(cvp, &cmd.hdr);
}
EXPORT_SYMBOL_GPL(q6cvp_enable);

int q6cvp_topology_commit(struct q6voice_session *cvp)
{
	struct apr_pkt cmd = {};

	cmd.hdr.pkt_size = APR_HDR_SIZE;
	cmd.hdr.opcode = VSS_IVOCPROC_CMD_TOPOLOGY_COMMIT;

	return q6voice_common_send(cvp, &cmd.hdr);
}
EXPORT_SYMBOL_GPL(q6cvp_topology_commit);

/*
 * Legacy (pre-2.0 vocproc) way of describing the device channels. The RX leg
 * is mono inside the vocproc and the channel mixer/MFC below converts it to
 * the device format.
 */
int q6cvp_set_dev_channels(struct q6voice_session *cvp, u16 tx_channels,
			   u16 rx_channels)
{
	struct q6cvp_dev_channels_cmd cmd = {};

	cmd.hdr.pkt_size = sizeof(cmd);
	cmd.hdr.opcode = VSS_IVOCPROC_CMD_TOPOLOGY_SET_DEV_CHANNELS;
	cmd.tx_num_channels = tx_channels;
	cmd.rx_num_channels = rx_channels;

	return q6voice_common_send(cvp, &cmd.hdr);
}
EXPORT_SYMBOL_GPL(q6cvp_set_dev_channels);

/* ---------------------------------------------- SET_PARAM_V2 parameters */

static int q6cvp_send_param_v2(struct q6voice_session *cvp, u32 module_id,
			       u32 param_id, const void *data, size_t size)
{
	struct q6cvp_set_param_v2_cmd cmd = {};

	if (size > sizeof(cmd.data))
		return -EINVAL;

	cmd.hdr.pkt_size = sizeof(struct apr_hdr) + 4 + 8 + 4 +
			   sizeof(struct q6cvp_param_hdr_v1) + size;
	cmd.hdr.opcode = VSS_ICOMMON_CMD_SET_PARAM_V2;
	cmd.mem_size = sizeof(struct q6cvp_param_hdr_v1) + size;

	cmd.param.module_id = module_id;
	cmd.param.param_id = param_id;
	cmd.param.param_size = size;
	cmd.param.reserved = 0;

	memcpy(&cmd.data, data, size);

	return q6voice_common_send(cvp, &cmd.hdr);
}

int q6cvp_set_channel_info(struct q6voice_session *cvp, u32 param_id,
			   u32 channels, u32 bits_per_sample,
			   const u8 *channel_map)
{
	struct {
		u32 num_channels;
		u32 bits_per_sample;
		u8 channel_mapping[Q6CVP_CHANNEL_MAP_SIZE];
	} __packed info = {};

	info.num_channels = channels;
	info.bits_per_sample = bits_per_sample;
	if (channel_map)
		memcpy(info.channel_mapping, channel_map,
		       sizeof(info.channel_mapping));

	return q6cvp_send_param_v2(cvp, VSS_MODULE_CVD_GENERIC, param_id,
				   &info, sizeof(info));
}
EXPORT_SYMBOL_GPL(q6cvp_set_channel_info);

int q6cvp_set_rx_channel_mixer(struct q6voice_session *cvp, u16 channels)
{
	struct {
		u32 index;
		u16 num_output_channels;
		u16 num_input_channels;
		u16 out_channel_map[2];
		u16 in_channel_map[1];
		u16 channel_weight_coeff[2][1];
		u16 reserved;
	} __packed mixer = {
		.index = 0,
		.num_output_channels = channels,
		/* The vocproc output is mono; the mixer fans it out. */
		.num_input_channels = 1,
		.out_channel_map = { Q6CVP_PCM_CHANNEL_FL, Q6CVP_PCM_CHANNEL_FR },
		.in_channel_map = { Q6CVP_PCM_CHANNEL_FL },
		.channel_weight_coeff = {
			{ Q6CVP_GAIN_Q14_UNITY },
			{ Q6CVP_GAIN_Q14_UNITY },
		},
	};

	return q6cvp_send_param_v2(cvp, AUDPROC_MODULE_ID_MFC,
				   AUDPROC_CHMIXER_PARAM_ID_COEFF,
				   &mixer, sizeof(mixer));
}
EXPORT_SYMBOL_GPL(q6cvp_set_rx_channel_mixer);

int q6cvp_set_rx_mfc_config(struct q6voice_session *cvp, u32 sample_rate,
			    u16 channels, const u8 *channel_map)
{
	struct {
		u32 sample_rate;
		u16 bits_per_sample;
		u16 num_channels;
		u16 channel_type[Q6CVP_CHANNEL_MAP_SIZE];
	} __packed mfc = {};
	int i;

	mfc.sample_rate = sample_rate;
	mfc.bits_per_sample = 16;
	mfc.num_channels = channels;
	if (channel_map) {
		for (i = 0; i < Q6CVP_CHANNEL_MAP_SIZE; i++)
			mfc.channel_type[i] = channel_map[i];
	}

	return q6cvp_send_param_v2(cvp, AUDPROC_MODULE_ID_MFC,
				   AUDPROC_PARAM_ID_MFC_OUTPUT_MEDIA_FORMAT,
				   &mfc, sizeof(mfc));
}
EXPORT_SYMBOL_GPL(q6cvp_set_rx_mfc_config);

/* ---------------------------------------------- endpoint media info (2.3+) */

int q6cvp_set_endpoint_media_info(struct q6voice_session *cvp, u32 param_id,
				  bool instance_id, u32 port_id, u16 channels,
				  u16 bits_per_sample, u32 sample_rate,
				  const u8 *channel_map)
{
	struct q6cvp_media_format_info info = {};

	info.port_id = port_id;
	info.num_channels = channels;
	info.bits_per_sample = bits_per_sample;
	info.sample_rate = sample_rate;
	if (channel_map)
		memcpy(info.channel_mapping, channel_map,
		       sizeof(info.channel_mapping));

	if (instance_id) {
		struct q6cvp_set_param_v3_cmd cmd = {};

		cmd.hdr.pkt_size = sizeof(cmd);
		cmd.hdr.opcode = VSS_ICOMMON_CMD_SET_PARAM_V3;
		cmd.payload_size = sizeof(cmd.param) + sizeof(cmd.info);
		cmd.param.module_id = VSS_MODULE_CVD_GENERIC;
		cmd.param.instance_id = INSTANCE_ID_0;
		cmd.param.param_id = param_id;
		cmd.param.param_size = sizeof(cmd.info);
		cmd.info = info;

		return q6voice_common_send(cvp, &cmd.hdr);
	} else {
		struct q6cvp_set_param_v2_endpoint_cmd cmd = {};

		cmd.hdr.pkt_size = sizeof(cmd);
		cmd.hdr.opcode = VSS_ICOMMON_CMD_SET_PARAM_V2;
		cmd.payload_size = sizeof(cmd.param) + sizeof(cmd.info);
		cmd.param.module_id = VSS_MODULE_CVD_GENERIC;
		cmd.param.param_id = param_id;
		cmd.param.param_size = sizeof(cmd.info);
		cmd.info = info;

		return q6voice_common_send(cvp, &cmd.hdr);
	}
}
EXPORT_SYMBOL_GPL(q6cvp_set_endpoint_media_info);

int q6cvp_set_volume_step(struct q6voice_session *cvp, u16 direction,
			  u32 value, u16 ramp_duration_ms)
{
	struct q6cvp_volume_step_cmd cmd = {};

	cmd.hdr.pkt_size = sizeof(cmd);
	cmd.hdr.opcode = VSS_IVOLUME_CMD_SET_STEP;
	cmd.direction = direction;
	cmd.value = value;
	cmd.ramp_duration_ms = ramp_duration_ms ? : Q6CVP_DEFAULT_RAMP_MS;

	return q6voice_common_send(cvp, &cmd.hdr);
}
EXPORT_SYMBOL_GPL(q6cvp_set_volume_step);

static int q6cvp_probe(struct apr_device *adev)
{
	return q6voice_common_probe(adev, Q6VOICE_SERVICE_CVP);
}

static const struct of_device_id q6cvp_device_id[] = {
	{ .compatible = "qcom,q6cvp" },
	{}
};
MODULE_DEVICE_TABLE(of, q6cvp_device_id);

static struct apr_driver qcom_q6cvp_driver = {
	.probe = q6cvp_probe,
	.remove = q6voice_common_remove,
	.callback = q6voice_common_callback,
	.driver = {
		.name = "qcom-q6cvp",
		.of_match_table = of_match_ptr(q6cvp_device_id),
	},
};

module_apr_driver(qcom_q6cvp_driver);

MODULE_AUTHOR("Stephan Gerhold <stephan@gerhold.net>");
MODULE_DESCRIPTION("Q6 Core Voice Processor");
MODULE_LICENSE("GPL v2");
