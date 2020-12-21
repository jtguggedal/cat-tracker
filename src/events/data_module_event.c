/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#include <stdio.h>

#include "data_module_event.h"

static int log_data_module_event(const struct event_header *eh, char *buf,
			  size_t buf_len)
{
	const struct data_module_event *event = cast_data_module_event(eh);
	char event_name[50] = "\0";

	switch (event->type) {
	case DATA_EVT_DATA_SEND:
		strcpy(event_name, "DATA_EVT_DATA_SEND");
		break;
	case DATA_EVT_DATA_READY:
		strcpy(event_name, "DATA_EVT_DATA_READY");
		break;
	case DATA_EVT_DATA_SEND_BATCH:
		strcpy(event_name, "DATA_EVT_DATA_SEND_BATCH");
		break;
	case DATA_EVT_UI_DATA_READY:
		strcpy(event_name, "DATA_EVT_UI_DATA_READY");
		break;
	case DATA_EVT_UI_DATA_SEND:
		strcpy(event_name, "DATA_EVT_UI_DATA_SEND");
		break;
	case DATA_EVT_CONFIG_INIT:
		strcpy(event_name, "DATA_EVT_CONFIG_INIT");
		break;
	case DATA_EVT_CONFIG_READY:
		strcpy(event_name, "DATA_EVT_CONFIG_READY");
		break;
	case DATA_EVT_CONFIG_GET:
		strcpy(event_name, "DATA_EVT_CONFIG_GET");
		break;
	case DATA_EVT_CONFIG_SEND:
		strcpy(event_name, "DATA_EVT_CONFIG_SEND");
		break;
	case DATA_EVT_SHUTDOWN_READY:
		strcpy(event_name, "DATA_EVT_SHUTDOWN_READY");
		break;
	case DATA_EVT_DATE_TIME_OBTAINED:
		strcpy(event_name, "DATA_EVT_DATE_TIME_OBTAINED");
		break;
	case DATA_EVT_ERROR:
		strcpy(event_name, "DATA_EVT_ERROR");
		return snprintf(buf, buf_len, "%s - Error code %d",
				event_name, event->data.err);
	default:
		strcpy(event_name, "Unknown event");
		break;
	}

	return snprintf(buf, buf_len, "%s", event_name);
}

EVENT_TYPE_DEFINE(data_module_event,
		  CONFIG_DATA_EVENTS_LOG,
		  log_data_module_event,
		  NULL);
