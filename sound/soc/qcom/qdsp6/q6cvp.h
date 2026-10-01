// SPDX-License-Identifier: GPL-2.0 */
// Copyright (c) 2020 Stephan Gerhold

#ifndef _Q6_CVP_H
#define _Q6_CVP_H

#include "q6voice.h"

struct q6voice_session;

/* Vocproc direction */
#define VSS_IVOCPROC_DIRECTION_RX	0
#define VSS_IVOCPROC_DIRECTION_TX	1
#define VSS_IVOCPROC_DIRECTION_RX_TX	2

/* "no port" / "no EC reference" */
#define VSS_IVOCPROC_PORT_ID_NONE	0xFFFF

#define VSS_IVOCPROC_TOPOLOGY_ID_NONE			0x00010F70
#define VSS_IVOCPROC_TOPOLOGY_ID_TX_SM_ECNS		0x00010F71
#define VSS_IVOCPROC_TOPOLOGY_ID_TX_DM_FLUENCE		0x00010F72
#define VSS_IVOCPROC_TOPOLOGY_ID_TX_SM_ECNS_V2		0x00010F89
#define VSS_IVOCPROC_TOPOLOGY_ID_RX_DEFAULT		0x00010F77

/* Echo-cancellation reference source */
#define VSS_IVOCPROC_VOCPROC_MODE_EC_INT_MIXING		0x00010F7C
#define VSS_IVOCPROC_VOCPROC_MODE_EC_EXT_MIXING		0x00010F7D

/* Calibration profile */
#define VSS_ICOMMON_CAL_NETWORK_ID_NONE			0x0001135E

/* Channel layout of the endpoint media info parameter */
#define Q6CVP_CHANNEL_MAP_SIZE				32

/* PCM channel map values (VSS/PCM_CHANNEL_*) */
#define Q6CVP_PCM_CHANNEL_FL				1
#define Q6CVP_PCM_CHANNEL_FR				2
#define Q6CVP_PCM_CHANNEL_FC				3

/* Audio processing modules used by the vocproc */
#define VSS_MODULE_CVD_GENERIC				0x0001316E
#define AUDPROC_MODULE_ID_MFC				0x00010912
#define AUDPROC_PARAM_ID_MFC_OUTPUT_MEDIA_FORMAT	0x00010913
#define AUDPROC_CHMIXER_PARAM_ID_COEFF			0x00010342

/* Vocproc device channel info parameter IDs (module VSS_MODULE_CVD_GENERIC) */
#define VSS_PARAM_VOCPROC_TX_CHANNEL_INFO		0x0001328E
#define VSS_PARAM_VOCPROC_RX_CHANNEL_INFO		0x0001328F
#define VSS_PARAM_VOCPROC_EC_REF_CHANNEL_INFO		0x00013290

/* Endpoint media info parameter IDs (module VSS_MODULE_CVD_GENERIC) */
#define VSS_PARAM_TX_PORT_ENDPOINT_MEDIA_INFO		0x00013253
#define VSS_PARAM_RX_PORT_ENDPOINT_MEDIA_INFO		0x00013254
#define VSS_PARAM_EC_REF_PORT_ENDPOINT_MEDIA_INFO	0x00013255

enum q6cvp_create_version {
	Q6CVP_CREATE_V2,
	Q6CVP_CREATE_V3,
};

struct q6cvp_config {
	u16 tx_port_id;
	u16 rx_port_id;
	u32 tx_topology_id;
	u32 rx_topology_id;
	u32 vocproc_mode;
	u16 ec_ref_port_id;
};

struct q6voice_session *
q6cvp_session_create(enum q6voice_path_type path,
		     const struct q6cvp_config *cfg,
		     enum q6cvp_create_version version);

int q6cvp_set_device(struct q6voice_session *cvp,
		     const struct q6cvp_config *cfg,
		     enum q6cvp_create_version version);
int q6cvp_enable(struct q6voice_session *cvp, bool enable);
int q6cvp_topology_commit(struct q6voice_session *cvp);
int q6cvp_set_dev_channels(struct q6voice_session *cvp, u16 tx_channels,
			   u16 rx_channels);
int q6cvp_set_channel_info(struct q6voice_session *cvp, u32 param_id,
			   u32 channels, u32 bits_per_sample,
			   const u8 *channel_map);
int q6cvp_set_endpoint_media_info(struct q6voice_session *cvp, u32 param_id,
				  bool instance_id, u32 port_id, u16 channels,
				  u16 bits_per_sample, u32 sample_rate,
				  const u8 *channel_map);
int q6cvp_set_rx_channel_mixer(struct q6voice_session *cvp, u16 channels);
int q6cvp_set_rx_mfc_config(struct q6voice_session *cvp, u32 sample_rate,
			    u16 channels, const u8 *channel_map);
int q6cvp_set_volume_step(struct q6voice_session *cvp, u16 direction,
			  u32 value, u16 ramp_duration_ms);

#endif /* _Q6_CVP_H */
