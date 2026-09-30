/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2026 thedanilfez <thedanilfezlol@gmail.com>
 * Copyright (c) 2020 Stephan Gerhold
 */

#ifndef _Q6_VOICE_COMMON_H
#define _Q6_VOICE_COMMON_H

#include <linux/soc/qcom/apr.h>
#include "q6voice.h"
#include "q6voice-trace.h"

/* APRV2 common opcodes, valid for every voice service session */
#define APRV2_IBASIC_CMD_DESTROY_SESSION	0x0001003C

/*
 * VSS_IVERSION_CMD_GET is only supported by the MVM service and is answered
 * with a VSS_IVERSION_RSP_GET command response (not an APR basic result).
 */
#define VSS_IVERSION_CMD_GET			0x00011378
#define VSS_IVERSION_RSP_GET			0x00011379

#define Q6VOICE_CVD_VERSION_MAX			31

/* Maximum session name length accepted by the DSP (including terminator) */
#define Q6VOICE_SESSION_NAME_LEN		20

enum q6voice_service_type {
	Q6VOICE_SERVICE_MVM,
	Q6VOICE_SERVICE_CVP,
	Q6VOICE_SERVICE_CVS,
	Q6VOICE_SERVICE_COUNT
};

struct q6voice_service;

/**
 * struct q6voice_session - a single voice DSP session on one path
 *
 * @dev: device backing the owning APR service (for logging)
 * @svc: APR service the session belongs to
 * @refcount: lifetime; the session is freed once the last user drops it
 * @path: logical voice path this session was created for
 * @handle: DSP-assigned session handle (APR ``src_port`` of the create reply)
 * @wait: wait queue completed when the outstanding command is answered
 * @lock: protects @expected_opcode, @result and the last-command snapshot
 * @expected_opcode: opcode the outstanding request expects in the basic
 *	result payload, 0 while the session is idle
 * @result: DSP status of the last completed command
 * @last_opcode: opcode of the last completed command
 * @last_status: DSP status of the last completed command
 *
 * A session carries at most one outstanding transaction. Callers are
 * responsible for serialising access to a session (the q6voice path mutex
 * does this in practice).
 */
struct q6voice_session {
	struct device *dev;
	struct q6voice_service *svc;
	struct kref refcount;

	enum q6voice_path_type path;
	u16 handle;

	wait_queue_head_t wait;
	spinlock_t lock;
	u32 expected_opcode;
	u32 result;

	u32 last_opcode;
	u32 last_status;
};

int q6voice_common_probe(struct apr_device *adev, enum q6voice_service_type type);
void q6voice_common_remove(struct apr_device *adev);
int q6voice_common_callback(struct apr_device *adev, const struct apr_resp_pkt *data);

int q6voice_common_send(struct q6voice_session *s, struct apr_hdr *hdr);
int q6voice_service_request(enum q6voice_service_type type, u32 opcode,
			    u32 rsp_opcode, u16 src_port, void *resp,
			    size_t resp_size, u32 *resp_status);

struct q6voice_session *q6voice_session_create(enum q6voice_service_type type,
					       enum q6voice_path_type path,
					       struct apr_hdr *hdr);
int q6voice_session_destroy(struct q6voice_session *s);
void q6voice_session_release(struct q6voice_session *s);
bool q6voice_session_is_live(struct q6voice_session *s);

const char *q6voice_status_str(u32 status);
int q6voice_status_to_errno(u32 status);

#endif /* _Q6_VOICE_COMMON_H */
