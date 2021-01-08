/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#include <stdio.h>

#include "app_module_event.h"

static char *type2str(enum app_module_data_type type)
{
	switch (type) {
	case APP_DATA_ENVIRONMENTAL:
		return "ENV";
	case APP_DATA_MOVEMENT:
		return "MOVE";
	case APP_DATA_MODEM_STATIC:
		return "MOD_STAT";
	case APP_DATA_MODEM_DYNAMIC:
		return "MOD_DYN";
	case APP_DATA_BATTERY:
		return "BAT";
	case APP_DATA_GNSS:
		return "GNSS";
	default:
		return "Unknown type";
	}
}

static int log_app_module_event(const struct event_header *eh, char *buf,
			  size_t buf_len)
{
	const struct app_module_event *event = cast_app_module_event(eh);
	char event_name[50] = "\0";
	char data_types[50] = "\0";

	switch (event->type) {
	case APP_EVT_DATA_GET:
		strcpy(event_name, "APP_EVT_DATA_GET");

		for (int i = 0; i < event->count; i++) {
			strcat(data_types, type2str(event->data_list[i]));

			if (i == event->count - 1) {
				break;
			}

			strcat(data_types, ", ");
		}

		return snprintf(buf, buf_len, "%s - Requested data types (%s)",
				event_name, data_types);
	case APP_EVT_CONFIG_GET:
		strcpy(event_name, "APP_EVT_CONFIG_GET");
		break;
	case APP_EVT_DATA_GET_ALL:
		strcpy(event_name, "APP_EVT_DATA_GET_ALL");
		break;
	case APP_EVT_START:
		strcpy(event_name, "APP_EVT_START");
		break;
	case APP_EVT_LTE_CONNECT:
		strcpy(event_name, "APP_EVT_LTE_CONNECT");
		break;
	case APP_EVT_LTE_DISCONNECT:
		strcpy(event_name, "APP_EVT_LTE_DISCONNECT");
		break;
	case APP_EVT_CONFIG_SEND:
		strcpy(event_name, "APP_EVT_CONFIG_SEND");
		break;
	case APP_EVT_SHUTDOWN_READY:
		strcpy(event_name, "APP_EVT_SHUTDOWN_READY");
		break;
	case APP_EVT_ERROR:
		strcpy(event_name, "APP_EVT_ERROR");
		return snprintf(buf, buf_len, "%s - Error code %d",
				event_name, event->data.err);
	default:
		strcpy(event_name, "Unknown event");
		break;
	}

	return snprintf(buf, buf_len, "%s", event_name);
}

EVENT_TYPE_DEFINE(app_module_event,
		  CONFIG_APP_EVENTS_LOG,
		  log_app_module_event,
		  NULL);
