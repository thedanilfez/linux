// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2012-2017, The Linux Foundation. All rights reserved.
// Copyright (c) 2020, Stephan Gerhold

#define CREATE_TRACE_POINTS
#include "q6voice-common.h"

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/kref.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>

#define Q6VOICE_TIMEOUT_MS	1000

/* Largest response payload we need to buffer (VSS version string) */
#define Q6VOICE_REQ_RESP_MAX	64

/* Upper bound for the optional dynamic-debug packet hexdump */
#define Q6VOICE_HEXDUMP_MAX	256

/* ADSP voice service status codes (shared with the APR error namespace) */
#define Q6VOICE_ADSP_EOK		0x00000000
#define Q6VOICE_ADSP_EFAILED		0x00000001
#define Q6VOICE_ADSP_EBADPARAM		0x00000002
#define Q6VOICE_ADSP_EUNSUPPORTED	0x00000003
#define Q6VOICE_ADSP_EVERSION		0x00000004
#define Q6VOICE_ADSP_EUNEXPECTED	0x00000005
#define Q6VOICE_ADSP_EPANIC		0x00000006
#define Q6VOICE_ADSP_ENORESOURCE	0x00000007
#define Q6VOICE_ADSP_EHANDLE		0x00000008
#define Q6VOICE_ADSP_EALREADY		0x00000009
#define Q6VOICE_ADSP_ENOTREADY		0x0000000A
#define Q6VOICE_ADSP_EPENDING		0x0000000B
#define Q6VOICE_ADSP_EBUSY		0x0000000C
#define Q6VOICE_ADSP_EABORTED		0x0000000D
#define Q6VOICE_ADSP_EPREEMPTED		0x0000000E
#define Q6VOICE_ADSP_ECONTINUE		0x0000000F
#define Q6VOICE_ADSP_EIMMEDIATE		0x00000010
#define Q6VOICE_ADSP_ENOTIMPL		0x00000011
#define Q6VOICE_ADSP_ENEEDMORE		0x00000012
#define Q6VOICE_ADSP_ENOMEMORY		0x00000014
#define Q6VOICE_ADSP_ENOTEXIST		0x00000015

static const char * const q6voice_service_name[] = {
	[Q6VOICE_SERVICE_MVM] = "MVM",
	[Q6VOICE_SERVICE_CVP] = "CVP",
	[Q6VOICE_SERVICE_CVS] = "CVS",
};

/**
 * struct q6voice_service - an APR service instance (MVM, CVP or CVS)
 *
 * @adev: backing APR device
 * @type: which voice service this is
 * @present: false once the protection domain went away
 * @refcount: lifetime; the service outlives its APR device while sessions
 *	created on it are still held by the upper layers
 * @lock: protects the session table and the service-level request state
 * @sessions: per-path session, indexed by &enum q6voice_path_type
 * @req_lock: serialises service-level (sessionless) requests
 * @req_pending: a service-level request is in flight
 * @req_done: the in-flight service-level request received its response
 * @req_opcode: request opcode (matched against a basic result payload)
 * @req_rsp_opcode: expected command response opcode, 0 for a basic result
 * @req_src_port: ``src_port`` the request was sent from
 * @req_status: DSP status of the completed service-level request
 * @req_size: number of valid bytes in @req_resp
 * @req_resp: payload of the completed service-level response
 * @req_wait: wait queue completed on @req_done
 */
struct q6voice_service {
	struct apr_device *adev;
	enum q6voice_service_type type;
	bool present;
	struct kref refcount;

	spinlock_t lock;
	struct q6voice_session *sessions[Q6VOICE_PATH_COUNT];

	struct mutex req_lock;
	bool req_pending;
	bool req_done;
	u32 req_opcode;
	u32 req_rsp_opcode;
	u16 req_src_port;
	u32 req_status;
	size_t req_size;
	u8 req_resp[Q6VOICE_REQ_RESP_MAX];
	wait_queue_head_t req_wait;
};

/* Protect q6voice_services[] */
static DEFINE_SPINLOCK(q6voice_services_lock);
static struct q6voice_service *q6voice_services[Q6VOICE_SERVICE_COUNT];

static const char *q6voice_service_str(enum q6voice_service_type type)
{
	if (type >= Q6VOICE_SERVICE_COUNT)
		return "?";
	return q6voice_service_name[type];
}

const char *q6voice_status_str(u32 status)
{
	switch (status) {
	case Q6VOICE_ADSP_EOK:
		return "ADSP_EOK";
	case Q6VOICE_ADSP_EFAILED:
		return "ADSP_EFAILED";
	case Q6VOICE_ADSP_EBADPARAM:
		return "ADSP_EBADPARAM";
	case Q6VOICE_ADSP_EUNSUPPORTED:
		return "ADSP_EUNSUPPORTED";
	case Q6VOICE_ADSP_EVERSION:
		return "ADSP_EVERSION";
	case Q6VOICE_ADSP_EUNEXPECTED:
		return "ADSP_EUNEXPECTED";
	case Q6VOICE_ADSP_EPANIC:
		return "ADSP_EPANIC";
	case Q6VOICE_ADSP_ENORESOURCE:
		return "ADSP_ENORESOURCE";
	case Q6VOICE_ADSP_EHANDLE:
		return "ADSP_EHANDLE";
	case Q6VOICE_ADSP_EALREADY:
		return "ADSP_EALREADY";
	case Q6VOICE_ADSP_ENOTREADY:
		return "ADSP_ENOTREADY";
	case Q6VOICE_ADSP_EPENDING:
		return "ADSP_EPENDING";
	case Q6VOICE_ADSP_EBUSY:
		return "ADSP_EBUSY";
	case Q6VOICE_ADSP_EABORTED:
		return "ADSP_EABORTED";
	case Q6VOICE_ADSP_EPREEMPTED:
		return "ADSP_EPREEMPTED";
	case Q6VOICE_ADSP_ECONTINUE:
		return "ADSP_ECONTINUE";
	case Q6VOICE_ADSP_EIMMEDIATE:
		return "ADSP_EIMMEDIATE";
	case Q6VOICE_ADSP_ENOTIMPL:
		return "ADSP_ENOTIMPL";
	case Q6VOICE_ADSP_ENEEDMORE:
		return "ADSP_ENEEDMORE";
	case Q6VOICE_ADSP_ENOMEMORY:
		return "ADSP_ENOMEMORY";
	case Q6VOICE_ADSP_ENOTEXIST:
		return "ADSP_ENOTEXIST";
	default:
		return "ADSP_ERR_UNKNOWN";
	}
}
EXPORT_SYMBOL_GPL(q6voice_status_str);

int q6voice_status_to_errno(u32 status)
{
	switch (status) {
	case Q6VOICE_ADSP_EOK:
		return 0;
	case Q6VOICE_ADSP_EBADPARAM:
		return -EINVAL;
	case Q6VOICE_ADSP_EUNSUPPORTED:
		return -EOPNOTSUPP;
	case Q6VOICE_ADSP_EVERSION:
		return -ENOPROTOOPT;
	case Q6VOICE_ADSP_ENORESOURCE:
		return -ENOSPC;
	case Q6VOICE_ADSP_EHANDLE:
		return -EBADR;
	case Q6VOICE_ADSP_EALREADY:
		return -EALREADY;
	case Q6VOICE_ADSP_ENOTREADY:
		return -EPERM;
	case Q6VOICE_ADSP_EPENDING:
		return -EINPROGRESS;
	case Q6VOICE_ADSP_EBUSY:
		return -EBUSY;
	case Q6VOICE_ADSP_EABORTED:
		return -ECANCELED;
	case Q6VOICE_ADSP_EPREEMPTED:
	case Q6VOICE_ADSP_ECONTINUE:
	case Q6VOICE_ADSP_EIMMEDIATE:
	case Q6VOICE_ADSP_ENOTIMPL:
		return -EAGAIN;
	case Q6VOICE_ADSP_ENEEDMORE:
		return -ENODATA;
	case Q6VOICE_ADSP_ENOMEMORY:
		return -ENOMEM;
	case Q6VOICE_ADSP_ENOTEXIST:
		return -ENODEV;
	default:
		return -EREMOTEIO;
	}
}
EXPORT_SYMBOL_GPL(q6voice_status_to_errno);

/*
 * Look up a service and take a reference on it. The caller owns the reference
 * and must drop it with kref_put(&svc->refcount, q6voice_service_free). This
 * makes the lookup safe against a concurrent protection-domain restart.
 */
static struct q6voice_service *
q6voice_service_get(enum q6voice_service_type type)
{
	struct q6voice_service *svc;
	unsigned long flags;

	if (type >= Q6VOICE_SERVICE_COUNT)
		return NULL;

	spin_lock_irqsave(&q6voice_services_lock, flags);
	svc = q6voice_services[type];
	if (svc)
		kref_get(&svc->refcount);
	spin_unlock_irqrestore(&q6voice_services_lock, flags);

	return svc;
}

static void q6voice_service_free(struct kref *ref)
{
	struct q6voice_service *svc = container_of(ref, struct q6voice_service,
						   refcount);

	mutex_destroy(&svc->req_lock);
	put_device(&svc->adev->dev);
	kfree(svc);
}

int q6voice_common_probe(struct apr_device *adev, enum q6voice_service_type type)
{
	struct device *dev = &adev->dev;
	struct q6voice_service *svc, *existing;
	unsigned long flags;

	if (type >= Q6VOICE_SERVICE_COUNT)
		return -EINVAL;

	/*
	 * Not devm-managed: sessions created on the service may outlive the
	 * APR device (a protection-domain restart happens while the AP still
	 * holds passive MVM/CVS handles), so the service is freed when the last
	 * session is released instead.
	 */
	svc = kzalloc_obj(*svc);
	if (!svc)
		return -ENOMEM;

	svc->adev = adev;
	get_device(&adev->dev);
	svc->type = type;
	svc->present = true;
	kref_init(&svc->refcount);
	spin_lock_init(&svc->lock);
	mutex_init(&svc->req_lock);
	init_waitqueue_head(&svc->req_wait);

	dev_set_drvdata(dev, svc);

	spin_lock_irqsave(&q6voice_services_lock, flags);
	existing = q6voice_services[type];
	if (!existing)
		q6voice_services[type] = svc;
	spin_unlock_irqrestore(&q6voice_services_lock, flags);

	if (existing) {
		dev_warn(dev, "duplicate %s APR service, ignoring\n",
			 q6voice_service_str(type));
		dev_set_drvdata(dev, NULL);
		kref_put(&svc->refcount, q6voice_service_free);
		return -EEXIST;
	}

	dev_dbg(dev, "%s APR service registered\n", q6voice_service_str(type));

	return 0;
}
EXPORT_SYMBOL_GPL(q6voice_common_probe);

static void q6voice_session_free(struct kref *ref);

void q6voice_common_remove(struct apr_device *adev)
{
	struct q6voice_service *svc = dev_get_drvdata(&adev->dev);
	enum q6voice_service_type type;
	struct q6voice_session *sessions[Q6VOICE_PATH_COUNT];
	unsigned long flags;
	int i;

	if (!svc)
		return;

	type = svc->type;

	spin_lock_irqsave(&q6voice_services_lock, flags);
	if (q6voice_services[type] == svc)
		q6voice_services[type] = NULL;
	spin_unlock_irqrestore(&q6voice_services_lock, flags);

	/*
	 * The protection domain is gone: every handle our sessions hold is
	 * stale. Mark them invalid and wake any waiter so a command in flight
	 * fails immediately instead of waiting for the full timeout. The
	 * sessions themselves are owned by the upper layers and are released
	 * later; they keep the service (and thus its locks) alive until then.
	 */
	spin_lock_irqsave(&svc->lock, flags);
	WRITE_ONCE(svc->present, false);
	if (svc->req_pending && !svc->req_done) {
		svc->req_status = Q6VOICE_ADSP_EFAILED;
		svc->req_size = 0;
		svc->req_pending = false;
		svc->req_done = true;
	}
	for (i = 0; i < Q6VOICE_PATH_COUNT; i++) {
		sessions[i] = svc->sessions[i];
		if (sessions[i]) {
			kref_get(&sessions[i]->refcount);
			WRITE_ONCE(sessions[i]->handle, 0);
		}
	}
	spin_unlock_irqrestore(&svc->lock, flags);

	wake_up(&svc->req_wait);
	for (i = 0; i < Q6VOICE_PATH_COUNT; i++) {
		if (sessions[i]) {
			WRITE_ONCE(sessions[i]->expected_opcode, 0);
			wake_up(&sessions[i]->wait);
			kref_put(&sessions[i]->refcount, q6voice_session_free);
		}
	}

	/* Drop the reference taken in q6voice_common_probe() */
	kref_put(&svc->refcount, q6voice_service_free);

	dev_set_drvdata(&adev->dev, NULL);
}
EXPORT_SYMBOL_GPL(q6voice_common_remove);

static void q6voice_session_free(struct kref *ref)
{
	struct q6voice_session *s = container_of(ref, struct q6voice_session,
						 refcount);

	kfree(s);
}

/*
 * Send one command on a session and wait for its basic result.
 *
 * @creating selects a create-session transaction: the request is sent to the
 * service port (dest_port 0) and a successful reply assigns the session
 * handle. For an established session the command is addressed to the session
 * handle and the reply must come from that same handle.
 */
static int q6voice_session_transaction(struct q6voice_session *s,
				       struct apr_hdr *hdr, bool creating)
{
	unsigned long flags;
	int ret;

	if (!READ_ONCE(s->svc->present))
		return -ENODEV;
	if (!creating && !s->handle)
		return -ENXIO;

	hdr->hdr_field = APR_SEQ_CMD_HDR_FIELD;
	hdr->src_port = s->path;
	hdr->dest_port = creating ? 0 : s->handle;
	hdr->token = 0;
	if (!hdr->pkt_size)
		hdr->pkt_size = APR_HDR_SIZE;

	trace_q6voice_command(q6voice_service_str(s->svc->type), s->path,
			      hdr->src_port, hdr->dest_port, hdr->opcode,
			      hdr->pkt_size);
	print_hex_dump_debug("q6voice tx: ", DUMP_PREFIX_OFFSET, 16, 1, hdr,
			     min_t(u32, hdr->pkt_size, Q6VOICE_HEXDUMP_MAX),
			     true);

	spin_lock_irqsave(&s->lock, flags);
	s->expected_opcode = hdr->opcode;
	s->result = 0;
	spin_unlock_irqrestore(&s->lock, flags);

	ret = apr_send_pkt(s->svc->adev, (struct apr_pkt *)hdr);
	if (ret < 0) {
		WRITE_ONCE(s->expected_opcode, 0);
		dev_err(s->dev, "%s opcode %#010x: send failed: %d\n",
			q6voice_service_str(s->svc->type), hdr->opcode, ret);
		return ret;
	}

	ret = wait_event_timeout(s->wait,
				 READ_ONCE(s->expected_opcode) == 0,
				 msecs_to_jiffies(Q6VOICE_TIMEOUT_MS));
	if (!ret) {
		WRITE_ONCE(s->expected_opcode, 0);
		dev_err(s->dev, "%s opcode %#010x (path %u, handle %#06x): timeout\n",
			q6voice_service_str(s->svc->type), hdr->opcode, s->path,
			s->handle);
		return -ETIMEDOUT;
	}

	if (!READ_ONCE(s->svc->present)) {
		dev_err(s->dev, "%s opcode %#010x: service went away\n",
			q6voice_service_str(s->svc->type), hdr->opcode);
		return -ENODEV;
	}

	if (s->result) {
		dev_err(s->dev, "%s opcode %#010x (path %u, handle %#06x) failed: %s (%#x), errno %d\n",
			q6voice_service_str(s->svc->type), hdr->opcode, s->path,
			s->handle, q6voice_status_str(s->result), s->result,
			q6voice_status_to_errno(s->result));
		return q6voice_status_to_errno(s->result);
	}

	return 0;
}

int q6voice_common_send(struct q6voice_session *s, struct apr_hdr *hdr)
{
	return q6voice_session_transaction(s, hdr, false);
}
EXPORT_SYMBOL_GPL(q6voice_common_send);

/**
 * q6voice_service_request() - send a service-level (sessionless) command
 *
 * @type: voice service to send the command to
 * @opcode: command opcode
 * @rsp_opcode: exact response opcode to wait for, or 0 to wait for an
 *	APR_BASIC_RSP_RESULT carrying @opcode
 * @src_port: ``src_port`` to send the request from (the DSP echoes it back in
 *	``dest_port`` of the response)
 * @resp: optional buffer receiving the response payload
 * @resp_size: size of @resp
 * @resp_status: optional output for the DSP status
 *
 * Only a response matching @rsp_opcode (or the basic result for @opcode when
 * @rsp_opcode is 0) completes the request. Anything else is left for the
 * session dispatch path.
 */
int q6voice_service_request(enum q6voice_service_type type, u32 opcode,
			    u32 rsp_opcode, u16 src_port, void *resp,
			    size_t resp_size, u32 *resp_status)
{
	struct q6voice_service *svc = q6voice_service_get(type);
	struct apr_pkt cmd = {};
	unsigned long flags;
	u32 status;
	size_t len;
	int ret;

	if (!svc)
		return -ENODEV;

	mutex_lock(&svc->req_lock);

	if (!READ_ONCE(svc->present)) {
		ret = -ENODEV;
		goto out_unlock;
	}

	cmd.hdr.hdr_field = APR_SEQ_CMD_HDR_FIELD;
	cmd.hdr.pkt_size = APR_HDR_SIZE;
	cmd.hdr.src_port = src_port;
	cmd.hdr.dest_port = 0;
	cmd.hdr.token = 0;
	cmd.hdr.opcode = opcode;

	spin_lock_irqsave(&svc->lock, flags);
	svc->req_pending = true;
	svc->req_done = false;
	svc->req_opcode = opcode;
	svc->req_rsp_opcode = rsp_opcode;
	svc->req_src_port = src_port;
	svc->req_status = 0;
	svc->req_size = 0;
	spin_unlock_irqrestore(&svc->lock, flags);

	trace_q6voice_command(q6voice_service_str(type), src_port, src_port, 0,
			      opcode, APR_HDR_SIZE);
	print_hex_dump_debug("q6voice tx: ", DUMP_PREFIX_OFFSET, 16, 1, &cmd,
			     cmd.hdr.pkt_size, true);

	ret = apr_send_pkt(svc->adev, &cmd);
	if (ret < 0) {
		dev_err(&svc->adev->dev, "%s opcode %#010x: send failed: %d\n",
			q6voice_service_str(type), opcode, ret);
		goto out_clear;
	}

	ret = wait_event_timeout(svc->req_wait, READ_ONCE(svc->req_done),
				 msecs_to_jiffies(Q6VOICE_TIMEOUT_MS));
	if (!ret) {
		dev_err(&svc->adev->dev, "%s opcode %#010x: timeout\n",
			q6voice_service_str(type), opcode);
		ret = -ETIMEDOUT;
		goto out_clear;
	}

	spin_lock_irqsave(&svc->lock, flags);
	svc->req_pending = false;
	len = min(svc->req_size, resp_size);
	if (resp && len)
		memcpy(resp, svc->req_resp, len);
	status = svc->req_status;
	if (resp_status)
		*resp_status = status;
	ret = READ_ONCE(svc->present) ? 0 : -ENODEV;
	spin_unlock_irqrestore(&svc->lock, flags);

	if (ret)
		goto out_unlock;

	if (status) {
		dev_err(&svc->adev->dev, "%s opcode %#010x failed: %s (%#x), errno %d\n",
			q6voice_service_str(type), opcode,
			q6voice_status_str(status), status,
			q6voice_status_to_errno(status));
		ret = q6voice_status_to_errno(status);
	}

out_unlock:
	mutex_unlock(&svc->req_lock);
	kref_put(&svc->refcount, q6voice_service_free);
	return ret;

out_clear:
	spin_lock_irqsave(&svc->lock, flags);
	svc->req_pending = false;
	spin_unlock_irqrestore(&svc->lock, flags);
	mutex_unlock(&svc->req_lock);
	kref_put(&svc->refcount, q6voice_service_free);
	return ret;
}
EXPORT_SYMBOL_GPL(q6voice_service_request);

/**
 * q6voice_session_callback() - match a basic result against a session command
 * @s: session the packet was routed to
 * @data: received APR response
 *
 * Returns true if the packet completed the session's outstanding command.
 */
static bool q6voice_session_callback(struct q6voice_session *s,
				     const struct apr_resp_pkt *data)
{
	const struct aprv2_ibasic_rsp_result_t *result = data->payload;
	unsigned long flags;
	u32 status;
	int err;

	if (data->hdr.opcode != APR_BASIC_RSP_RESULT)
		return false;

	if (!data->payload || data->payload_size < sizeof(*result)) {
		dev_warn(s->dev, "short basic response (%d bytes)\n",
			 data->payload_size);
		return false;
	}

	status = result->status;

	spin_lock_irqsave(&s->lock, flags);
	if (result->opcode != s->expected_opcode) {
		spin_unlock_irqrestore(&s->lock, flags);
		dev_warn(s->dev, "unexpected basic response for opcode %#010x (status %s), expected %#010x\n",
			 result->opcode, q6voice_status_str(status),
			 s->expected_opcode);
		return false;
	}

	if (s->handle) {
		if (data->hdr.src_port != s->handle) {
			spin_unlock_irqrestore(&s->lock, flags);
			dev_warn(s->dev, "basic response from foreign session %#06x (handle %#06x)\n",
				 data->hdr.src_port, s->handle);
			return false;
		}
	} else if (!status) {
		/*
		 * Only a successful create reply assigns a handle. A NACK must
		 * not leave a bogus handle in the session.
		 */
		s->handle = data->hdr.src_port;
	}

	s->result = status;
	s->last_opcode = result->opcode;
	s->last_status = status;
	WRITE_ONCE(s->expected_opcode, 0);
	spin_unlock_irqrestore(&s->lock, flags);

	err = q6voice_status_to_errno(status);
	trace_q6voice_response(q6voice_service_str(s->svc->type), s->path,
			       data->hdr.src_port, data->hdr.dest_port,
			       result->opcode, status, err);

	wake_up(&s->wait);
	return true;
}

/**
 * q6voice_service_callback() - match a packet against a service-level request
 * @svc: service the packet was delivered to
 * @data: received APR response
 *
 * Returns true if the packet completed the outstanding sessionless request.
 */
static bool q6voice_service_callback(struct q6voice_service *svc,
				     const struct apr_resp_pkt *data)
{
	const struct aprv2_ibasic_rsp_result_t *result;
	unsigned long flags;
	bool handled = false;
	u32 status = 0;

	spin_lock_irqsave(&svc->lock, flags);
	if (!svc->req_pending || svc->req_done)
		goto out;

	if (svc->req_rsp_opcode) {
		if (data->hdr.opcode != svc->req_rsp_opcode)
			goto out;
	} else {
		if (data->hdr.opcode != APR_BASIC_RSP_RESULT)
			goto out;
		result = data->payload;
		if (!data->payload || data->payload_size < sizeof(*result) ||
		    result->opcode != svc->req_opcode)
			goto out;
		status = result->status;
	}

	svc->req_status = status;
	svc->req_size = min_t(size_t, data->payload_size,
			      sizeof(svc->req_resp));
	if (data->payload && svc->req_size)
		memcpy(svc->req_resp, data->payload, svc->req_size);
	svc->req_done = true;
	svc->req_pending = false;
	handled = true;

out:
	spin_unlock_irqrestore(&svc->lock, flags);

	if (handled) {
		trace_q6voice_response(q6voice_service_str(svc->type), 0,
				       data->hdr.src_port, data->hdr.dest_port,
				       data->hdr.opcode, status,
				       q6voice_status_to_errno(status));
		wake_up(&svc->req_wait);
	}

	return handled;
}

int q6voice_common_callback(struct apr_device *adev,
			    const struct apr_resp_pkt *data)
{
	struct device *dev = &adev->dev;
	struct q6voice_service *svc = dev_get_drvdata(dev);
	struct q6voice_session *s;
	const struct aprv2_ibasic_rsp_result_t *result;
	unsigned long flags;
	bool basic = data->hdr.opcode == APR_BASIC_RSP_RESULT;
	u32 status = 0;

	if (!svc)
		return 0;

	dev_dbg(dev, "%s callback: opcode %#010x dest %#06x src %#06x\n",
		q6voice_service_str(svc->type), data->hdr.opcode,
		data->hdr.dest_port, data->hdr.src_port);
	if (data->payload && data->payload_size)
		print_hex_dump_debug("q6voice rx: ", DUMP_PREFIX_OFFSET, 16, 1,
				     data->payload,
				     min_t(int, data->payload_size,
					   Q6VOICE_HEXDUMP_MAX), true);

	if (basic && data->payload &&
	    data->payload_size >= sizeof(*result)) {
		result = data->payload;
		status = result->status;
	}

	/* A sessionless request in flight takes priority: its expected
	 * response opcode is unambiguous. */
	if (q6voice_service_callback(svc, data))
		return 0;

	if (data->hdr.dest_port >= Q6VOICE_PATH_COUNT) {
		/* Asynchronous service events legitimately use a non-session
		 * destination port; only complain about unroutable results. */
		if (basic)
			dev_warn(dev, "basic result for invalid path %u (opcode %#010x, status %s)\n",
				 data->hdr.dest_port, data->hdr.opcode,
				 q6voice_status_str(status));
		return 0;
	}

	spin_lock_irqsave(&svc->lock, flags);
	s = svc->sessions[data->hdr.dest_port];
	if (s)
		kref_get(&s->refcount);
	spin_unlock_irqrestore(&svc->lock, flags);

	if (s) {
		q6voice_session_callback(s, data);
		kref_put(&s->refcount, q6voice_session_free);
	} else if (basic) {
		dev_warn(dev, "basic result for inactive path %u (opcode %#010x, status %s)\n",
			 data->hdr.dest_port, data->hdr.opcode,
			 q6voice_status_str(status));
	}

	return 0;
}
EXPORT_SYMBOL_GPL(q6voice_common_callback);

struct q6voice_session *
q6voice_session_create(enum q6voice_service_type type,
		       enum q6voice_path_type path, struct apr_hdr *hdr)
{
	struct q6voice_service *svc = q6voice_service_get(type);
	struct q6voice_session *s;
	unsigned long flags;
	int ret;

	if (!svc)
		return ERR_PTR(-ENODEV);
	if (path >= Q6VOICE_PATH_COUNT) {
		kref_put(&svc->refcount, q6voice_service_free);
		return ERR_PTR(-EINVAL);
	}

	s = kzalloc_obj(*s);
	if (!s) {
		kref_put(&svc->refcount, q6voice_service_free);
		return ERR_PTR(-ENOMEM);
	}

	s->dev = &svc->adev->dev;
	s->svc = svc;
	s->path = path;

	/*
	 * The reference taken by q6voice_service_get() is transferred to the
	 * session and released in q6voice_session_release(), keeping the
	 * service (and its locks) alive for as long as the session.
	 */

	kref_init(&s->refcount);
	spin_lock_init(&s->lock);
	init_waitqueue_head(&s->wait);

	spin_lock_irqsave(&svc->lock, flags);
	if (!READ_ONCE(svc->present) || svc->sessions[path]) {
		ret = READ_ONCE(svc->present) ? -EBUSY : -ENODEV;
		spin_unlock_irqrestore(&svc->lock, flags);
		kfree(s);
		kref_put(&svc->refcount, q6voice_service_free);
		return ERR_PTR(ret);
	}
	svc->sessions[path] = s;
	spin_unlock_irqrestore(&svc->lock, flags);

	ret = q6voice_session_transaction(s, hdr, true);
	if (ret)
		goto err;

	if (!s->handle) {
		dev_err(s->dev, "%s create on path %u returned no handle\n",
			q6voice_service_str(type), path);
		ret = -ENXIO;
		goto err;
	}

	dev_dbg(s->dev, "%s session path %u handle %#06x\n",
		q6voice_service_str(type), path, s->handle);

	return s;

err:
	q6voice_session_release(s);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(q6voice_session_create);

/**
 * q6voice_session_destroy() - destroy a DSP session, idempotently
 * @s: session to destroy
 *
 * A session that the DSP already discarded (ADSP_EHANDLE, or a handle
 * invalidated by a protection-domain restart) is treated as successfully
 * destroyed so that teardown can run unconditionally.
 */
int q6voice_session_destroy(struct q6voice_session *s)
{
	struct apr_pkt cmd;
	int ret;

	if (!s->handle)
		return 0;

	cmd.hdr.pkt_size = APR_HDR_SIZE;
	cmd.hdr.opcode = APRV2_IBASIC_CMD_DESTROY_SESSION;

	ret = q6voice_common_send(s, &cmd.hdr);
	if (ret && ret != -EBADR)
		return ret;

	s->handle = 0;
	return 0;
}
EXPORT_SYMBOL_GPL(q6voice_session_destroy);

void q6voice_session_release(struct q6voice_session *s)
{
	struct q6voice_service *svc = s->svc;
	unsigned long flags;

	spin_lock_irqsave(&svc->lock, flags);
	if (svc->sessions[s->path] == s)
		svc->sessions[s->path] = NULL;
	spin_unlock_irqrestore(&svc->lock, flags);

	kref_put(&s->refcount, q6voice_session_free);
	kref_put(&svc->refcount, q6voice_service_free);
}
EXPORT_SYMBOL_GPL(q6voice_session_release);

bool q6voice_session_is_live(struct q6voice_session *s)
{
	return s && READ_ONCE(s->svc->present) && READ_ONCE(s->handle);
}
EXPORT_SYMBOL_GPL(q6voice_session_is_live);

/*
 * The protocol wrappers (q6mvm/q6cvs/q6cvp) are separate modules, so the
 * transport tracepoints must be exported for them to record a transcript.
 */
EXPORT_TRACEPOINT_SYMBOL_GPL(q6voice_command);
EXPORT_TRACEPOINT_SYMBOL_GPL(q6voice_response);

MODULE_AUTHOR("Stephan Gerhold <stephan@gerhold.net>");
MODULE_DESCRIPTION("Q6Voice common session management");
MODULE_LICENSE("GPL v2");
