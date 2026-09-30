// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2026 thedanilfez <thedanilfezlol@gmail.com>
 * Copyright (c) 2012-2017, The Linux Foundation. All rights reserved.
 * Copyright (c) 2020, Stephan Gerhold
 *
 * CVS - Core Voice Stream (ADSP voice service 0x0A).
 *
 * For modem-controlled calls (CS voice, VoLTE) the vocoder stream itself is
 * created by the modem. The AP only requests a passive-control handle so that
 * the DSP ties the vocproc attached through MVM to the modem-owned stream.
 * Only the AP-owned VoIP path creates a full-control stream.
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/soc/qcom/apr.h>
#include "q6cvs.h"
#include "q6voice-common.h"

#define VSS_ISTREAM_CMD_CREATE_PASSIVE_CONTROL_SESSION	0x00011140
#define VSS_ISTREAM_CMD_CREATE_FULL_CONTROL_SESSION	0x000110F7
#define VSS_IVOLUME_CMD_MUTE_V2				0x0001138B

#define VSS_ISTREAM_DIRECTION_RX_TX			2

#define VSS_MEDIA_ID_PCM_16_KHZ				0x00010FCC
#define VSS_NETWORK_ID_VOIP				0x00011362

struct q6cvs_create_passive_cmd {
	struct apr_hdr hdr;
	char name[Q6VOICE_SESSION_NAME_LEN];
} __packed;

struct q6cvs_create_full_cmd {
	struct apr_hdr hdr;
	u16 direction;
	u32 enc_media_type;
	u32 dec_media_type;
	u32 network_id;
	char name[Q6VOICE_SESSION_NAME_LEN];
} __packed;

struct q6cvs_mute_cmd {
	struct apr_hdr hdr;
	u16 direction;
	u16 mute_flag;
	u16 ramp_duration_ms;
} __packed;

/* Wire-format size checks against the vendor q6voice.h structures */
static_assert(sizeof(struct q6cvs_create_passive_cmd) == 40);
static_assert(sizeof(struct q6cvs_create_full_cmd) == 54);
static_assert(sizeof(struct q6cvs_mute_cmd) == 26);

static const char *q6cvs_session_name(enum q6voice_path_type path)
{
	switch (path) {
	case Q6VOICE_PATH_VOICE:
		return "default modem voice";
	case Q6VOICE_PATH_VOLTE:
		return "default volte voice";
	case Q6VOICE_PATH_VOIP:
		return "default q6 voice";
	case Q6VOICE_PATH_VOICEMMODE1:
		return "11C05000";
	case Q6VOICE_PATH_VOICEMMODE2:
		return "11DC5000";
	default:
		return NULL;
	}
}

bool q6cvs_path_is_passive(enum q6voice_path_type path)
{
	return path != Q6VOICE_PATH_VOIP;
}
EXPORT_SYMBOL_GPL(q6cvs_path_is_passive);

struct q6voice_session *q6cvs_session_create(enum q6voice_path_type path)
{
	const char *name = q6cvs_session_name(path);

	if (!name)
		return ERR_PTR(-EINVAL);

	if (q6cvs_path_is_passive(path)) {
		struct q6cvs_create_passive_cmd cmd = {};

		cmd.hdr.pkt_size = sizeof(cmd);
		cmd.hdr.opcode = VSS_ISTREAM_CMD_CREATE_PASSIVE_CONTROL_SESSION;
		strscpy(cmd.name, name, sizeof(cmd.name));

		return q6voice_session_create(Q6VOICE_SERVICE_CVS, path,
					      &cmd.hdr);
	} else {
		struct q6cvs_create_full_cmd cmd = {};

		cmd.hdr.pkt_size = sizeof(cmd);
		cmd.hdr.opcode = VSS_ISTREAM_CMD_CREATE_FULL_CONTROL_SESSION;
		cmd.direction = VSS_ISTREAM_DIRECTION_RX_TX;
		cmd.enc_media_type = VSS_MEDIA_ID_PCM_16_KHZ;
		cmd.dec_media_type = VSS_MEDIA_ID_PCM_16_KHZ;
		cmd.network_id = VSS_NETWORK_ID_VOIP;
		strscpy(cmd.name, name, sizeof(cmd.name));

		return q6voice_session_create(Q6VOICE_SERVICE_CVS, path,
					      &cmd.hdr);
	}
}
EXPORT_SYMBOL_GPL(q6cvs_session_create);

int q6cvs_set_stream_mute(struct q6voice_session *cvs, u16 direction, bool mute,
			  u16 ramp_duration_ms)
{
	struct q6cvs_mute_cmd cmd = {};

	cmd.hdr.pkt_size = sizeof(cmd);
	cmd.hdr.opcode = VSS_IVOLUME_CMD_MUTE_V2;
	cmd.direction = direction;
	cmd.mute_flag = mute;
	cmd.ramp_duration_ms = ramp_duration_ms;

	return q6voice_common_send(cvs, &cmd.hdr);
}
EXPORT_SYMBOL_GPL(q6cvs_set_stream_mute);

static int q6cvs_probe(struct apr_device *adev)
{
	return q6voice_common_probe(adev, Q6VOICE_SERVICE_CVS);
}

static const struct of_device_id q6cvs_device_id[] = {
	{ .compatible = "qcom,q6cvs" },
	{}
};
MODULE_DEVICE_TABLE(of, q6cvs_device_id);

static struct apr_driver qcom_q6cvs_driver = {
	.probe = q6cvs_probe,
	.remove = q6voice_common_remove,
	.callback = q6voice_common_callback,
	.driver = {
		.name = "qcom-q6cvs",
		.of_match_table = of_match_ptr(q6cvs_device_id),
	},
};

module_apr_driver(qcom_q6cvs_driver);

MODULE_AUTHOR("Stephan Gerhold <stephan@gerhold.net>");
MODULE_DESCRIPTION("Q6 Core Voice Stream");
MODULE_LICENSE("GPL v2");
