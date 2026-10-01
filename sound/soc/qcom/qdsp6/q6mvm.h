// SPDX-License-Identifier: GPL-2.0 */
// Copyright (c) 2020 Stephan Gerhold

#ifndef _Q6_MVM_H
#define _Q6_MVM_H

#include "q6voice.h"

struct q6voice_session;

struct q6voice_session *q6mvm_session_create(enum q6voice_path_type path);
bool q6mvm_path_is_passive(enum q6voice_path_type path);

int q6mvm_set_dual_control(struct q6voice_session *mvm, bool enable);
int q6mvm_attach(struct q6voice_session *mvm, struct q6voice_session *cvp,
		 bool state);
int q6mvm_attach_stream(struct q6voice_session *mvm,
			struct q6voice_session *cvs, bool state);
int q6mvm_start(struct q6voice_session *mvm, bool state);
int q6mvm_pause(struct q6voice_session *mvm);
int q6mvm_standby(struct q6voice_session *mvm);

#endif /* _Q6_MVM_H */
