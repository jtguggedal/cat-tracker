/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#ifndef _MODULES_COMMON_H_
#define _MODULES_COMMON_H_

#define IS_EVENT(_ptr, _mgr, _evt) \
		is_ ## _mgr ## _mgr_event(&_ptr->manager._mgr.header) && \
		_ptr->manager._mgr.type == _evt


struct module_data {
	// Storing only thread ID here. Assume stack is kept static in module.
	k_tid_t thread_id;

	// Queue is kept outside of struct
	struct k_msgq *msg_q;
};

int module_get_next_msg(struct module_data *module, void *msg);

void module_enqueue_msg(struct module_data *module, void *msg);

#endif /* _MODULES_COMMON_H_ */
