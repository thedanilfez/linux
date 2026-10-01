// SPDX-License-Identifier: GPL-2.0 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM q6voice

#if !defined(_Q6_VOICE_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _Q6_VOICE_TRACE_H

#include <linux/tracepoint.h>

TRACE_EVENT(q6voice_command,

	TP_PROTO(const char *service, u32 session, u16 src_port, u16 dest_port,
		 u32 opcode, size_t size),

	TP_ARGS(service, session, src_port, dest_port, opcode, size),

	TP_STRUCT__entry(
		__string(service, service)
		__field(u32, session)
		__field(u16, src_port)
		__field(u16, dest_port)
		__field(u32, opcode)
		__field(size_t, size)
	),

	TP_fast_assign(
		__assign_str(service);
		__entry->session = session;
		__entry->src_port = src_port;
		__entry->dest_port = dest_port;
		__entry->opcode = opcode;
		__entry->size = size;
	),

	TP_printk("%s path=%u opcode=%#010x src=%#06x dest=%#06x size=%zu",
		  __get_str(service), __entry->session, __entry->opcode,
		  __entry->src_port, __entry->dest_port, __entry->size)
);

TRACE_EVENT(q6voice_response,

	TP_PROTO(const char *service, u32 session, u16 src_port, u16 dest_port,
		 u32 opcode, u32 status, int errno),

	TP_ARGS(service, session, src_port, dest_port, opcode, status, errno),

	TP_STRUCT__entry(
		__string(service, service)
		__field(u32, session)
		__field(u16, src_port)
		__field(u16, dest_port)
		__field(u32, opcode)
		__field(u32, status)
		__field(int, errno)
	),

	TP_fast_assign(
		__assign_str(service);
		__entry->session = session;
		__entry->src_port = src_port;
		__entry->dest_port = dest_port;
		__entry->opcode = opcode;
		__entry->status = status;
		__entry->errno = errno;
	),

	TP_printk("%s path=%u opcode=%#010x src=%#06x dest=%#06x status=%#x (%d)",
		  __get_str(service), __entry->session, __entry->opcode,
		  __entry->src_port, __entry->dest_port, __entry->status,
		  __entry->errno)
);

#endif /* _Q6_VOICE_TRACE_H */

/* This part must be outside protection */
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE q6voice-trace
#include <trace/define_trace.h>
