/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#include <zephyr.h>
#include <event_manager.h>
#include "modules_common.h"

#include <logging/log.h>

LOG_MODULE_REGISTER(modules_common, CONFIG_MODULES_COMMON_LOG_LEVEL);

struct event_prototype {
	struct event_header header;
	uint8_t event_id;
};

static atomic_t active_module_count;

int module_get_next_msg(struct module_data *module, void *msg)
{
	int err = k_msgq_get(module->msg_q, msg, K_FOREVER);

	if (err == 0 && IS_ENABLED(CONFIG_MODULES_COMMON_LOG_LEVEL_DBG)) {
		struct event_prototype *evt_proto =
			(struct event_prototype *)msg;
		struct event_type *event =
			(struct event_type *)evt_proto->header.type_id;
		char buf[50];

		event->log_event(&evt_proto->header, buf, sizeof(buf));

		LOG_DBG("%s module: Dequeued %s",
			log_strdup(module->name),
			log_strdup(buf));
	}

	return err;
}

void module_enqueue_msg(struct module_data *module, void *msg)
{
	while (k_msgq_put(module->msg_q, msg, K_NO_WAIT) != 0) {
		/* Message queue is full: purge old data & try again */
		k_msgq_purge(module->msg_q);
		LOG_WRN("%s: Message queue full, queue purged",
			log_strdup(module->name));
	}

	if (IS_ENABLED(CONFIG_MODULES_COMMON_LOG_LEVEL_DBG)) {
		struct event_prototype *evt_proto =
			(struct event_prototype *)msg;
		struct event_type *event =
			(struct event_type *)evt_proto->header.type_id;
		char buf[50];

		event->log_event(&evt_proto->header, buf, sizeof(buf));

		LOG_DBG("%s module: Enqueued: %s", log_strdup(module->name),
			log_strdup(buf));
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
