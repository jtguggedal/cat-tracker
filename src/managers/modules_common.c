/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#include <zephyr.h>

#include "modules_common.h"

inline int module_get_next_msg(struct module_data *module, void *msg)
{
	return k_msgq_get(module->msg_q, msg, K_FOREVER);
}

void module_enqueue_msg(struct module_data *module, void *msg)
{
	while (k_msgq_put(module->msg_q, msg, K_NO_WAIT) != 0) {
		/* Message queue is full: purge old data & try again */
		k_msgq_purge(module->msg_q);
		printk("Message queue full, queue purged\n");
	}
}

// void module_start()
