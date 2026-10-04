/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __VENUS_SECURE_H__
#define __VENUS_SECURE_H__

#include <linux/types.h>

struct venus_core;
struct venus_secure_buffer;

int venus_secure_init(struct venus_core *core);
void venus_secure_deinit(struct venus_core *core, bool quiesced);
void venus_secure_reclaim(struct venus_core *core);
struct venus_secure_buffer *venus_secure_alloc(struct venus_core *core,
					      size_t size, u32 alignment,
					      dma_addr_t *iova);
int venus_secure_free(struct venus_core *core, struct venus_secure_buffer *buf,
		      bool released);

#endif
