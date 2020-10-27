/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#include <zephyr.h>

#include "modules_common.h"

#include <logging/log.h>

LOG_MODULE_REGISTER(modules_common, CONFIG_CAT_TRACKER_LOG_LEVEL);

static atomic_t active_module_count;

int module_get_next_msg(struct module_data *module, void *msg)
{
	return k_msgq_get(module->msg_q, msg, K_FOREVER);
}

void module_enqueue_msg(struct module_data *module, void *msg)
{
	while (k_msgq_put(module->msg_q, msg, K_NO_WAIT) != 0) {
		/* Message queue is full: purge old data & try again */
		k_msgq_purge(module->msg_q);
		LOG_WRN("Message queue full, queue purged");
	}
}

void module_start(struct module_data *module)
{
	atomic_inc(&active_module_count);

	if (module->name) {
		LOG_DBG("Module \"%s\" started", module->name);
	} else if (module->thread_id) {
		LOG_DBG("Module with thread ID %p started", module->thread_id);
	}
}

uint32_t module_active_count_get(void)
{
	return atomic_get(&active_module_count);
}
