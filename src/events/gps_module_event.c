/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#include <stdio.h>

#include "gps_module_event.h"

static int log_gps_module_event(const struct event_header *eh, char *buf,
			 size_t buf_len)
{
	const struct gps_module_event *event = cast_gps_module_event(eh);
	char event_name[50] = "\0";

	switch (event->type) {
	case GPS_EVT_DATA_READY:
		strcpy(event_name, "GPS_EVT_DATA_READY");
		break;
	case GPS_EVT_TIMEOUT:
		strcpy(event_name, "GPS_EVT_TIMEOUT");
		break;
	case GPS_EVT_ACTIVE:
		strcpy(event_name, "GPS_EVT_ACTIVE");
		break;
	case GPS_EVT_INACTIVE:
		strcpy(event_name, "GPS_EVT_INACTIVE");
		break;
	case GPS_EVT_SHUTDOWN_READY:
		strcpy(event_name, "GPS_EVT_SHUTDOWN_READY");
		break;
	case GPS_EVT_AGPS_NEEDED:
		strcpy(event_name, "GPS_EVT_AGPS_NEEDED");
		break;
	case GPS_EVT_ERROR_CODE:
		strcpy(event_name, "GPS_EVT_ERROR_CODE");
		return snprintf(buf, buf_len, "%s - Error code %d",
				event_name, event->data.err);
	default:
		strcpy(event_name, "Unknown event");
		break;
	}

	return snprintf(buf, buf_len, "%s", event_name);
}

EVENT_TYPE_DEFINE(gps_module_event,
		  CONFIG_GPS_EVENTS_LOG,
		  log_gps_module_event,
		  NULL);