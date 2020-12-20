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

#define SEND_EVENT(_mgr, _type)					      \
	struct _mgr ## _mgr_event *event = new_ ## _mgr ## _mgr_event();      \
	event->type = _type;						      \
	EVENT_SUBMIT(event)

#define SEND_ERROR(_mgr, _type, _error_code)				      \
	struct _mgr ## _mgr_event *event = new_ ## _mgr ## _mgr_event();      \
	event->type = _type;						      \
	event->data.err = _error_code;					      \
	EVENT_SUBMIT(event)

struct module_data {
	k_tid_t thread_id;
	char *name;
	struct k_msgq *msg_q;
};

void module_register(struct module_data *module);

void module_set_thread(struct module_data *module, const k_tid_t thread_id);

void module_set_name(struct module_data *module, char *name);

void module_set_queue(struct module_data *module,  struct k_msgq *msg_q);

int module_get_next_msg(struct module_data *module, void *msg);

void module_enqueue_msg(struct module_data *module, void *msg);

void module_start(struct module_data *module);

uint32_t module_active_count_get(void);

#endif /* _MODULES_COMMON_H_ */
