/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#include <stdio.h>

#include "ui_module_event.h"

static int log_ui_module_event(const struct event_header *eh, char *buf,
			size_t buf_len)
{
	const struct ui_module_event *event = cast_ui_module_event(eh);
	char event_name[50] = "\0";

	switch (event->type) {
	case UI_EVT_BUTTON_DATA_READY:
		strcpy(event_name, "UI_EVT_BUTTON_DATA_READY");
		break;
	case UI_EVT_SHUTDOWN_READY:
		strcpy(event_name, "UI_EVT_SHUTDOWN_READY");
		break;
	case UI_EVT_ERROR:
		strcpy(event_name, "UI_EVT_ERROR");
		return snprintf(buf, buf_len, "%s - Error code %d",
				event_name, event->data.err);
	default:
		strcpy(event_name, "Unknown event");
		break;
	}

	return snprintf(buf, buf_len, "%s", event_name);
}

EVENT_TYPE_DEFINE(ui_module_event,
		  CONFIG_UI_EVENTS_LOG,
		  log_ui_module_event,
		  NULL);
