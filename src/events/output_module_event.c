/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#include <stdio.h>

#include "output_module_event.h"

static char *get_evt_type_str(enum output_module_event_type type)
{
	switch (type) {
	case OUTPUT_EVT_SHUTDOWN_READY:
		return "OUTPUT_EVT_SHUTDOWN_READY";
	case OUTPUT_EVT_ERROR:
		return "OUTPUT_EVT_ERROR";
	default:
		return "Unknown event";
	}
}

static int log_event(const struct event_header *eh, char *buf,
		     size_t buf_len)
{
	const struct output_module_event *event = cast_output_module_event(eh);

	if (event->type == OUTPUT_EVT_ERROR) {
		return snprintf(buf, buf_len, "%s - Error code %d",
				get_evt_type_str(event->type), event->data.err);
	}

	return snprintf(buf, buf_len, "%s", get_evt_type_str(event->type));
}

#if defined(CONFIG_PROFILER)

static void profile_event(struct log_event_buf *buf,
			 const struct event_header *eh)
{
	const struct output_module_event *event = cast_output_module_event(eh);

#if defined(CONFIG_PROFILER_EVENT_TYPE_STRING)
	profiler_log_encode_string(buf, get_evt_type_str(event->type),
		strlen(get_evt_type_str(event->type)));
#else
	profiler_log_encode_u32(buf, event->type);
#endif
}

EVENT_INFO_DEFINE(output_module_event,
#if defined(CONFIG_PROFILER_EVENT_TYPE_STRING)
		  ENCODE(PROFILER_ARG_STRING),
#else
		  ENCODE(PROFILER_ARG_U32),
#endif
		  ENCODE("type"),
		  profile_event);

#endif /* CONFIG_PROFILER */

EVENT_TYPE_DEFINE(output_module_event,
		  CONFIG_OUTPUT_EVENTS_LOG,
		  log_event,
#if defined(CONFIG_PROFILER)
		  &output_module_event_info);
#else
		  NULL);
#endif
