// SPDX-License-Identifier: GPL-2.0 */
// Copyright (c) 2020 Stephan Gerhold

#ifndef _Q6_CVS_H
#define _Q6_CVS_H

#include "q6voice.h"

struct q6voice_session;

struct q6voice_session *q6cvs_session_create(enum q6voice_path_type path);
bool q6cvs_path_is_passive(enum q6voice_path_type path);

int q6cvs_set_stream_mute(struct q6voice_session *cvs, u16 direction, bool mute,
			  u16 ramp_duration_ms);

#endif /* _Q6_CVS_H */
