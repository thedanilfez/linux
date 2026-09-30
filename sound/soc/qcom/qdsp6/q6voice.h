/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2026 thedanilfez <thedanilfezlol@gmail.com>
 * Copyright (c) 2020 Stephan Gerhold
 */

#ifndef _Q6_VOICE_H
#define _Q6_VOICE_H

#include <linux/types.h>

struct device;

/*
 * VSS volume/mute direction. Shared by the CVP volume step and the CVS
 * stream mute commands.
 */
#define Q6VOICE_DIRECTION_TX	0
#define Q6VOICE_DIRECTION_RX	1

/**
 * enum q6voice_path_type - logical voice paths handled by the Q6 voice stack
 *
 * @Q6VOICE_PATH_VOICE: Circuit-switched cellular voice. The vocoder stream is
 *	created and owned by the modem; Linux only attaches a vocproc to it.
 * @Q6VOICE_PATH_VOLTE: VoLTE/IMS voice. Same model as @Q6VOICE_PATH_VOICE but
 *	a different session name/VSID and the AP cleans up the sessions itself.
 * @Q6VOICE_PATH_VOICEMMODE1: First multi-mode modem session, named "11C05000".
 * @Q6VOICE_PATH_VOICEMMODE2: Second multi-mode modem session, named "11DC5000".
 * @Q6VOICE_PATH_VOIP: Application/AP-owned VoIP session, where Linux creates
 *	the full-control MVM/CVS sessions.
 */
enum q6voice_path_type {
	Q6VOICE_PATH_VOICE = 0,
	Q6VOICE_PATH_VOLTE,
	Q6VOICE_PATH_VOIP,
	Q6VOICE_PATH_VOICEMMODE1,
	Q6VOICE_PATH_VOICEMMODE2,
	Q6VOICE_PATH_COUNT
};

/**
 * struct q6voice_device_cfg - AFE device configuration for a voice path
 *
 * @tx_port_index: AFE port index carrying the uplink (microphone) samples
 * @rx_port_index: AFE port index carrying the downlink (earpiece/speaker)
 * @ec_ref_index: AFE port index used as external echo-cancellation reference,
 *	or -1 to derive the reference from the RX path (internal mixing)
 * @tx_topology_id: uplink processing topology presented to the vocproc
 * @rx_topology_id: downlink processing topology presented to the vocproc
 * @external_ec: select external EC mixing instead of internal mixing
 * @tx_rate: uplink backend sample rate
 * @rx_rate: downlink backend sample rate
 * @tx_channels: uplink backend channel count
 * @rx_channels: downlink backend channel count
 * @tx_bits: uplink backend sample width
 * @rx_bits: downlink backend sample width
 * @tx_topology_id / @rx_topology_id are DSP constants; see q6cvp.h.
 */
struct q6voice_device_cfg {
	int tx_port_index;
	int rx_port_index;
	int ec_ref_index;
	u32 tx_topology_id;
	u32 rx_topology_id;
	bool external_ec;
	/* Actual DPCM backend media; zero retains the debugfs default. */
	unsigned int tx_rate, rx_rate;
	unsigned int tx_channels, rx_channels;
	unsigned int tx_bits, rx_bits;
};

struct q6voice;

struct q6voice *q6voice_create(struct device *dev);

int q6voice_start(struct q6voice *v, enum q6voice_path_type path, bool capture);
int q6voice_prepare(struct q6voice *v, enum q6voice_path_type path, bool capture);
int q6voice_stop(struct q6voice *v, enum q6voice_path_type path, bool capture);
int q6voice_pause(struct q6voice *v, enum q6voice_path_type path);

int q6voice_set_device(struct q6voice *v, enum q6voice_path_type path,
		       const struct q6voice_device_cfg *cfg);

int q6voice_set_tx_mute(struct q6voice *v, enum q6voice_path_type path,
			bool mute);
int q6voice_set_rx_volume(struct q6voice *v, enum q6voice_path_type path,
			  u16 volume);

int q6voice_get_port(struct q6voice *v, enum q6voice_path_type path,
		     bool capture);
void q6voice_set_port(struct q6voice *v, enum q6voice_path_type path,
		      bool capture, int index);

/*
 * Developer bring-up interface. These entry points let a debugfs file drive
 * and inspect the DSP session lifecycle without going through ALSA/UCM. They
 * are explicitly NOT a stable userspace ABI and may be removed once call audio
 * is fully supported from the ASoC path.
 */
int q6voice_debug_start(struct q6voice *v, enum q6voice_path_type path,
			int tx_index, int rx_index, bool external_ec,
			int ec_ref_index);
int q6voice_debug_stop(struct q6voice *v, enum q6voice_path_type path);
int q6voice_debug_status(struct q6voice *v, enum q6voice_path_type path,
			 char *buf, size_t len);

#endif /* _Q6_VOICE_H */
