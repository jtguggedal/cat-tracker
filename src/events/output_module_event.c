/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#include <stdio.h>

#include "output_module_event.h"

static int log_output_module_event(const struct event_header *eh, char *buf,
				size_t buf_len)
{
	const struct output_module_event *event = cast_output_module_event(eh);
	char event_name[50] = "\0";

	switch (event->type) {
	case OUTPUT_EVT_SHUTDOWN_READY:
		strcpy(event_name, "OUTPUT_EVT_SHUTDOWN_READY");
		break;
	case OUTPUT_EVT_ERROR:
		strcpy(event_name, "OUTPUT_EVT_ERROR");
		return snprintf(buf, buf_len, "%s - Error code %d",
				event_name, event->data.err);
	default:
		strcpy(event_name, "Unknown event");
		break;
	}

	return snprintf(buf, buf_len, "%s", event_name);
}

EVENT_TYPE_DEFINE(output_module_event,
		  CONFIG_OUTPUT_EVENTS_LOG,
		  log_output_module_event,
		  NULL);
