// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2026 thedanilfez <thedanilfezlol@gmail.com>
 * Copyright (c) 2012-2017, The Linux Foundation. All rights reserved.
 * Copyright (c) 2020, Stephan Gerhold
 *
 * MVM - Multimode Voice Manager (ADSP voice service 0x09).
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/soc/qcom/apr.h>
#include "q6mvm.h"
#include "q6voice-common.h"

#define VSS_IMVM_CMD_CREATE_PASSIVE_CONTROL_SESSION	0x000110FF
#define VSS_IMVM_CMD_CREATE_FULL_CONTROL_SESSION	0x000110FE
#define VSS_IMVM_CMD_SET_POLICY_DUAL_CONTROL		0x00011327
#define VSS_IMVM_CMD_ATTACH_STREAM			0x0001123C
#define VSS_IMVM_CMD_DETACH_STREAM			0x0001123D
#define VSS_IMVM_CMD_ATTACH_VOCPROC			0x0001123E
#define VSS_IMVM_CMD_DETACH_VOCPROC			0x0001123F
#define VSS_IMVM_CMD_START_VOICE			0x00011190
#define VSS_IMVM_CMD_STOP_VOICE				0x00011192
#define VSS_IMVM_CMD_STANDBY_VOICE			0x00011191
#define VSS_IMVM_CMD_PAUSE_VOICE			0x0001137D

struct q6mvm_create_session_cmd {
	struct apr_hdr hdr;
	char name[Q6VOICE_SESSION_NAME_LEN];
} __packed;

/*
 * This command is required to let MVM know who is in control of the session.
 * Enable is a single byte (bool) on the wire.
 */
struct q6mvm_dual_control_cmd {
	struct apr_hdr hdr;
	bool enable;
} __packed;

/*
 * Attach/detach a vocproc to/from the MVM. The MVM symmetrically
 * connects/disconnects this vocproc to/from all attached streams.
 */
struct q6mvm_attach_vocproc_cmd {
	struct apr_hdr hdr;
	u16 handle;
} __packed;

/* Attach/detach a stream (full-control MVM sessions only, i.e. VoIP) */
struct q6mvm_attach_stream_cmd {
	struct apr_hdr hdr;
	u16 handle;
} __packed;

/* Wire-format size checks against the vendor q6voice.h structures */
static_assert(sizeof(struct q6mvm_create_session_cmd) == 40);
static_assert(sizeof(struct q6mvm_dual_control_cmd) == 21);
static_assert(sizeof(struct q6mvm_attach_vocproc_cmd) == 22);
static_assert(sizeof(struct q6mvm_attach_stream_cmd) == 22);

static const char *q6mvm_session_name(enum q6voice_path_type path)
{
	switch (path) {
	case Q6VOICE_PATH_VOICE:
		return "default modem voice";
	case Q6VOICE_PATH_VOLTE:
		return "default volte voice";
	case Q6VOICE_PATH_VOIP:
		return "default voip";
	case Q6VOICE_PATH_VOICEMMODE1:
		return "11C05000";
	case Q6VOICE_PATH_VOICEMMODE2:
		return "11DC5000";
	default:
		return NULL;
	}
}

/*
 * CS voice and VoLTE sessions are created and driven by the modem; the AP only
 * requests a passive-control handle. The AP-owned VoIP path uses full control.
 */
bool q6mvm_path_is_passive(enum q6voice_path_type path)
{
	return path != Q6VOICE_PATH_VOIP;
}
EXPORT_SYMBOL_GPL(q6mvm_path_is_passive);

struct q6voice_session *q6mvm_session_create(enum q6voice_path_type path)
{
	struct q6mvm_create_session_cmd cmd = {};
	const char *name = q6mvm_session_name(path);

	if (!name)
		return ERR_PTR(-EINVAL);

	cmd.hdr.pkt_size = sizeof(cmd);
	cmd.hdr.opcode = q6mvm_path_is_passive(path) ?
		VSS_IMVM_CMD_CREATE_PASSIVE_CONTROL_SESSION :
		VSS_IMVM_CMD_CREATE_FULL_CONTROL_SESSION;
	strscpy(cmd.name, name, sizeof(cmd.name));

	return q6voice_session_create(Q6VOICE_SERVICE_MVM, path, &cmd.hdr);
}
EXPORT_SYMBOL_GPL(q6mvm_session_create);

int q6mvm_set_dual_control(struct q6voice_session *mvm, bool enable)
{
	struct q6mvm_dual_control_cmd cmd = {};

	cmd.hdr.pkt_size = sizeof(cmd);
	cmd.hdr.opcode = VSS_IMVM_CMD_SET_POLICY_DUAL_CONTROL;
	cmd.enable = enable;

	return q6voice_common_send(mvm, &cmd.hdr);
}
EXPORT_SYMBOL_GPL(q6mvm_set_dual_control);

int q6mvm_attach(struct q6voice_session *mvm, struct q6voice_session *cvp,
		 bool state)
{
	struct q6mvm_attach_vocproc_cmd cmd = {};

	cmd.hdr.pkt_size = sizeof(cmd);
	cmd.hdr.opcode = state ? VSS_IMVM_CMD_ATTACH_VOCPROC :
				 VSS_IMVM_CMD_DETACH_VOCPROC;
	cmd.handle = cvp->handle;

	return q6voice_common_send(mvm, &cmd.hdr);
}
EXPORT_SYMBOL_GPL(q6mvm_attach);

int q6mvm_attach_stream(struct q6voice_session *mvm,
			struct q6voice_session *cvs, bool state)
{
	struct q6mvm_attach_stream_cmd cmd = {};

	cmd.hdr.pkt_size = sizeof(cmd);
	cmd.hdr.opcode = state ? VSS_IMVM_CMD_ATTACH_STREAM :
				 VSS_IMVM_CMD_DETACH_STREAM;
	cmd.handle = cvs->handle;

	return q6voice_common_send(mvm, &cmd.hdr);
}
EXPORT_SYMBOL_GPL(q6mvm_attach_stream);

int q6mvm_start(struct q6voice_session *mvm, bool state)
{
	struct apr_pkt cmd;

	cmd.hdr.pkt_size = APR_HDR_SIZE;
	cmd.hdr.opcode = state ? VSS_IMVM_CMD_START_VOICE :
				 VSS_IMVM_CMD_STOP_VOICE;

	return q6voice_common_send(mvm, &cmd.hdr);
}
EXPORT_SYMBOL_GPL(q6mvm_start);

int q6mvm_pause(struct q6voice_session *mvm)
{
	struct apr_pkt cmd;

	cmd.hdr.pkt_size = APR_HDR_SIZE;
	cmd.hdr.opcode = VSS_IMVM_CMD_PAUSE_VOICE;

	return q6voice_common_send(mvm, &cmd.hdr);
}
EXPORT_SYMBOL_GPL(q6mvm_pause);

int q6mvm_standby(struct q6voice_session *mvm)
{
	struct apr_pkt cmd;

	cmd.hdr.pkt_size = APR_HDR_SIZE;
	cmd.hdr.opcode = VSS_IMVM_CMD_STANDBY_VOICE;

	return q6voice_common_send(mvm, &cmd.hdr);
}
EXPORT_SYMBOL_GPL(q6mvm_standby);

static int q6mvm_probe(struct apr_device *adev)
{
	int ret;

	ret = q6voice_common_probe(adev, Q6VOICE_SERVICE_MVM);
	if (ret)
		return ret;

	return of_platform_populate(adev->dev.of_node, NULL, NULL, &adev->dev);
}

static void q6mvm_remove(struct apr_device *adev)
{
	q6voice_common_remove(adev);
	of_platform_depopulate(&adev->dev);
}

static const struct of_device_id q6mvm_device_id[] = {
	{ .compatible = "qcom,q6mvm" },
	{}
};
MODULE_DEVICE_TABLE(of, q6mvm_device_id);

static struct apr_driver qcom_q6mvm_driver = {
	.probe = q6mvm_probe,
	.remove = q6mvm_remove,
	.callback = q6voice_common_callback,
	.driver = {
		.name = "qcom-q6mvm",
		.of_match_table = of_match_ptr(q6mvm_device_id),
	},
};

module_apr_driver(qcom_q6mvm_driver);

MODULE_AUTHOR("Stephan Gerhold <stephan@gerhold.net>");
MODULE_DESCRIPTION("Q6 Multimode Voice Manager");
MODULE_LICENSE("GPL v2");
