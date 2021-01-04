/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#include <zephyr.h>
#include <drivers/gps.h>
#include <stdio.h>
#include <date_time.h>
#include <event_manager.h>
#include <drivers/gps.h>

#define MODULE gps_module

#include "modules_common.h"
#include "events/app_module_event.h"
#include "events/gps_module_event.h"
#include "events/data_module_event.h"
#include "events/util_module_event.h"

#include <logging/log.h>
LOG_MODULE_REGISTER(MODULE, CONFIG_GPS_MODULE_LOG_LEVEL);

/* Maximum GPS interval value. Dummy value, will not be used. Starting
 * and stopping of GPS is done by the application.
 */
#define GPS_INTERVAL_MAX 1800

static struct module_data self = {
	.name = "gps",
};

struct gps_msg_data {
	union {
		struct app_module_event app;
		struct data_module_event data;
		struct util_module_event util;
		struct gps_module_event gps;
	} module;
};

/* GPS module super states. */
static enum gps_module_state_type {
	GPS_STATE_INIT,
	GPS_STATE_RUNNING
} gps_state;

static enum gps_module_sub_state_type {
	GPS_SUB_STATE_IDLE,
	GPS_SUB_STATE_SEARCH
} gps_sub_state;

static void message_handler(struct gps_msg_data *data);

static void state_set(enum gps_module_state_type new_state)
{
	gps_state = new_state;
}

static void sub_state_set(enum gps_module_sub_state_type new_state)
{
	gps_sub_state = new_state;
}

/* GPS device. Used to identify the GPS driver in the sensor API. */
static const struct device *gps_dev;

/* nRF9160 GPS driver configuration. */
static struct gps_config gps_cfg = {
	.nav_mode = GPS_NAV_MODE_PERIODIC,
	.power_mode = GPS_POWER_MODE_DISABLED,
	.interval = GPS_INTERVAL_MAX
};

static void gps_data_send(struct gps_pvt *gps_data)
{
	struct gps_module_event *gps_module_event = new_gps_module_event();

	gps_module_event->data.gps.longi = gps_data->longitude;
	gps_module_event->data.gps.lat = gps_data->latitude;
	gps_module_event->data.gps.alt = gps_data->altitude;
	gps_module_event->data.gps.acc = gps_data->accuracy;
	gps_module_event->data.gps.spd = gps_data->speed;
	gps_module_event->data.gps.hdg = gps_data->heading;
	gps_module_event->data.gps.gps_ts = k_uptime_get();
	gps_module_event->data.gps.queued = true;
	gps_module_event->type = GPS_EVT_DATA_READY;

	EVENT_SUBMIT(gps_module_event);
}

static void gps_search_start(void)
{
	int err;

	/* Do not initiate GPS search if timeout is 0. */
	if (gps_cfg.timeout == 0) {
		LOG_WRN("GPS search disabled");
		return;
	}

	err = gps_start(gps_dev, &gps_cfg);
	if (err) {
		LOG_WRN("Failed to start GPS, error: %d", err);
		return;
	}

	SEND_EVENT(gps, GPS_EVT_ACTIVE);
}

static void gps_search_stop(void)
{
	int err;

	err = gps_stop(gps_dev);
	if (err) {
		LOG_WRN("Failed to stop GPS, error: %d", err);
		return;
	}

	SEND_EVENT(gps, GPS_EVT_INACTIVE);
}

static void gps_time_set(struct gps_pvt *gps_data)
{
	/* Change datetime.year and datetime.month to accommodate the
	 * correct input format.
	 */
	struct tm gps_time = {
		.tm_year = gps_data->datetime.year - 1900,
		.tm_mon = gps_data->datetime.month - 1,
		.tm_mday = gps_data->datetime.day,
		.tm_hour = gps_data->datetime.hour,
		.tm_min = gps_data->datetime.minute,
		.tm_sec = gps_data->datetime.seconds,
	};

	date_time_set(&gps_time);
}

static void gps_event_handler(const struct device *dev, struct gps_event *evt)
{
	switch (evt->type) {
	case GPS_EVT_SEARCH_STARTED:
		LOG_DBG("GPS_EVT_SEARCH_STARTED");
		break;
	case GPS_EVT_SEARCH_STOPPED:
		LOG_DBG("GPS_EVT_SEARCH_STOPPED");
		break;
	case GPS_EVT_SEARCH_TIMEOUT:
		LOG_DBG("GPS_EVT_SEARCH_TIMEOUT");
		SEND_EVENT(gps, GPS_EVT_TIMEOUT);
		gps_search_stop();
		break;
	case GPS_EVT_PVT:
		/* Don't spam logs */
		break;
	case GPS_EVT_PVT_FIX:
		LOG_DBG("GPS_EVT_PVT_FIX");
		gps_time_set(&evt->pvt);
		gps_data_send(&evt->pvt);
		gps_search_stop();
		break;
	case GPS_EVT_NMEA:
		/* Don't spam logs */
		break;
	case GPS_EVT_NMEA_FIX:
		LOG_DBG("Position fix with NMEA data");
		break;
	case GPS_EVT_OPERATION_BLOCKED:
		LOG_DBG("GPS_EVT_OPERATION_BLOCKED");
		break;
	case GPS_EVT_OPERATION_UNBLOCKED:
		LOG_DBG("GPS_EVT_OPERATION_UNBLOCKED");
		break;
	case GPS_EVT_AGPS_DATA_NEEDED:
		LOG_DBG("GPS_EVT_AGPS_DATA_NEEDED");
		struct gps_module_event *gps_module_event =
				new_gps_module_event();

		gps_module_event->data.agps_request = evt->agps_request;
		gps_module_event->type = GPS_EVT_AGPS_NEEDED;
		EVENT_SUBMIT(gps_module_event);
		break;
	case GPS_EVT_ERROR:
		LOG_DBG("GPS_EVT_ERROR\n");
		break;
	default:
		break;
	}
}

static int setup(void)
{
	int err;

	gps_dev = device_get_binding(CONFIG_GPS_DEV_NAME);
	if (gps_dev == NULL) {
		LOG_ERR("Could not get %s device",
			log_strdup(CONFIG_GPS_DEV_NAME));
		return -ENODEV;
	}

	err = gps_init(gps_dev, gps_event_handler);
	if (err) {
		LOG_ERR("Could not initialize GPS, error: %d", err);
		return err;
	}

	return 0;
}

static bool event_handler(const struct event_header *eh)
{
	if (is_app_module_event(eh)) {
		struct app_module_event *event = cast_app_module_event(eh);
		struct gps_msg_data msg = {
			.module.app = *event
		};

		message_handler(&msg);
	}

	if (is_data_module_event(eh)) {
		struct data_module_event *event = cast_data_module_event(eh);
		struct gps_msg_data msg = {
			.module.data = *event
		};

		message_handler(&msg);
	}

	if (is_util_module_event(eh)) {
		struct util_module_event *event = cast_util_module_event(eh);
		struct gps_msg_data msg = {
			.module.util = *event
		};

		message_handler(&msg);
	}

	if (is_gps_module_event(eh)) {
		struct gps_module_event *event = cast_gps_module_event(eh);
		struct gps_msg_data msg = {
			.module.gps = *event
		};

		message_handler(&msg);
	}

	return false;
}

static bool gps_data_requested(enum app_module_data_type *data_list,
			       size_t count)
{
	for (size_t i = 0; i < count; i++) {
		if (data_list[i] == APP_DATA_GNSS) {
			return true;
		}
	}

	return false;
}

static void on_state_init(struct gps_msg_data *msg)
{
	if (IS_EVENT(msg, data, DATA_EVT_CONFIG_INIT)) {
		gps_cfg.timeout = msg->module.data.data.cfg.gpst;
		state_set(GPS_STATE_RUNNING);
	}
}

static void on_state_running(struct gps_msg_data *msg)
{
	if (IS_EVENT(msg, data, DATA_EVT_CONFIG_READY)) {
		gps_cfg.timeout = msg->module.data.data.cfg.gpst;
	}
}

static void on_state_running_gps_search(struct gps_msg_data *msg)
{
	if (IS_EVENT(msg, gps, GPS_EVT_INACTIVE)) {
		sub_state_set(GPS_SUB_STATE_IDLE);
	}

	if (IS_EVENT(msg, app, APP_EVT_DATA_GET)) {
		if (!gps_data_requested(msg->module.app.data_list,
					msg->module.app.count)) {
			return;
		}

		LOG_WRN("GPS search already active and will not be restarted");
		LOG_WRN("Try setting a sample/publication interval greater");
		LOG_WRN("than the GPS search timeout.");
	}
}

static void on_state_running_gps_idle(struct gps_msg_data *msg)
{
	if (IS_EVENT(msg, gps, GPS_EVT_ACTIVE)) {
		sub_state_set(GPS_SUB_STATE_SEARCH);
	}

	if (IS_EVENT(msg, app, APP_EVT_DATA_GET)) {
		if (!gps_data_requested(msg->module.app.data_list,
					msg->module.app.count)) {
			return;
		}

		gps_search_start();
	}
}

static void on_all_states(struct gps_msg_data *msg)
{
	if (IS_EVENT(msg, app, APP_EVT_START)) {
		int err;

		state_set(GPS_STATE_INIT);
		module_start(&self);

		err = setup();
		if (err) {
			LOG_ERR("setup, error: %d", err);
			SEND_ERROR(gps, GPS_EVT_ERROR_CODE, err);
		}
	}

	if (IS_EVENT(msg, util, UTIL_EVT_SHUTDOWN_REQUEST)) {
		SEND_EVENT(gps, GPS_EVT_SHUTDOWN_READY);
	}
}

static void message_handler(struct gps_msg_data *msg)
{
	switch (gps_state) {
	case GPS_STATE_INIT:
		on_state_init(msg);
		break;
	case GPS_STATE_RUNNING:
		switch (gps_sub_state) {
		case GPS_SUB_STATE_SEARCH:
			on_state_running_gps_search(msg);
			break;
		case GPS_SUB_STATE_IDLE:
			on_state_running_gps_idle(msg);
			break;
		default:
			LOG_ERR("Unknown GPS module sub state.");
			break;
		}

		on_state_running(msg);
		break;
	default:
		LOG_ERR("Unknown GPS module state.");
		break;
	}

	on_all_states(msg);
}

EVENT_LISTENER(MODULE, event_handler);
EVENT_SUBSCRIBE(MODULE, app_module_event);
EVENT_SUBSCRIBE(MODULE, data_module_event);
EVENT_SUBSCRIBE(MODULE, util_module_event);
EVENT_SUBSCRIBE(MODULE, gps_module_event);
