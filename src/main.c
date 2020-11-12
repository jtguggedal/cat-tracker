/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#include <zephyr.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <event_manager.h>

#if defined(CONFIG_WATCHDOG)
#include "watchdog.h"
#endif

#define MODULE app_manager

#include "modules_common.h"
#include "events/app_mgr_event.h"
#include "events/cloud_mgr_event.h"
#include "events/data_mgr_event.h"
#include "events/sensor_mgr_event.h"
#include "events/ui_mgr_event.h"
#include "events/util_mgr_event.h"
#include "events/modem_mgr_event.h"
#include "events/output_mgr_event.h"

#include <logging/log.h>
#include <logging/log_ctrl.h>
LOG_MODULE_REGISTER(MODULE, CONFIG_CAT_TRACKER_LOG_LEVEL);

extern atomic_t manager_count;

extern void cJSON_Init(void);

struct app_msg_data {
	union {
		struct cloud_mgr_event cloud;
		struct ui_mgr_event ui;
		struct sensor_mgr_event sensor;
		struct data_mgr_event data;
		struct util_mgr_event util;
		struct modem_mgr_event modem;
		struct app_mgr_event app;
	} manager;
};

/* Expose external manager threads. */
#if defined(CONFIG_DATA_MANAGER)
extern const k_tid_t data_manager_thread;
#endif
#if defined(CONFIG_SENSOR_MANAGER)
extern const k_tid_t sensor_manager_thread;
#endif

/* Application manager super states. */
enum app_state_type {
	APP_MGR_STATE_INIT,
	APP_MGR_STATE_RUNNING
} app_state;

/* Application sub states. */
enum app_sub_state_type {
	APP_SUB_STATE_ACTIVE_MODE,
	APP_SUB_STATE_PASSIVE_MODE,
} app_sub_state;

/* Internal copy of the device configuration. */
static struct cloud_data_cfg app_cfg;

static void data_sample_timer_handler(struct k_timer *dummy);

/* Application manager message queue. */
K_MSGQ_DEFINE(msgq_app, sizeof(struct app_msg_data), 10, 4);

/* Timers used by the application manager */
K_TIMER_DEFINE(data_sample_timer, data_sample_timer_handler, NULL);
K_TIMER_DEFINE(movement_timeout_timer, data_sample_timer_handler, NULL);
K_TIMER_DEFINE(movement_resolution_timer, NULL, NULL);

static struct module_data self = {
	.msg_q = &msgq_app,
};

static void state_set(enum app_state_type new_state)
{
	app_state = new_state;
}

static void sub_state_set(enum app_sub_state_type new_state)
{
	app_sub_state = new_state;
}

static void signal_error(int err)
{
	struct app_mgr_event *app_mgr_event = new_app_mgr_event();

	app_mgr_event->err = err;
	app_mgr_event->type = APP_MGR_EVT_ERROR;

	EVENT_SUBMIT(app_mgr_event);
}

static void config_send(void)
{
	struct app_mgr_event *app_mgr_event = new_app_mgr_event();

	app_mgr_event->type = APP_MGR_EVT_CONFIG_SEND;

	EVENT_SUBMIT(app_mgr_event);
}

static void data_get_all(void)
{
	struct app_mgr_event *app_mgr_event = new_app_mgr_event();

	/* Specify which data that is to be included in the transmission. */
	app_mgr_event->data_list[0] = APP_DATA_MODEM;
	app_mgr_event->data_list[1] = APP_DATA_BATTERY;
	app_mgr_event->data_list[2] = APP_DATA_ENVIRONMENTALS;
	app_mgr_event->data_list[3] = APP_DATA_GNSS;

	/* Set list count to number of data types passed in app_mgr_event. */
	app_mgr_event->count = 4;
	app_mgr_event->type = APP_MGR_EVT_DATA_GET;

	/* Specify a timeout that each manager has to fetch data. If data is not
	 * fetched within this timeout, the data that is available is sent.
	 */
	app_mgr_event->timeout = app_cfg.gpst + 60;

	EVENT_SUBMIT(app_mgr_event);
}

static void data_get_init(void)
{
	struct app_mgr_event *app_mgr_event = new_app_mgr_event();

	/* Specify which data that is to be included in the transmission. */
	app_mgr_event->data_list[0] = APP_DATA_MODEM;
	app_mgr_event->data_list[1] = APP_DATA_BATTERY;
	app_mgr_event->data_list[2] = APP_DATA_ENVIRONMENTALS;

	/* Set list count to number of data types passed in app_mgr_event. */
	app_mgr_event->count = 3;
	app_mgr_event->type = APP_MGR_EVT_DATA_GET;

	/* Specify a timeout that each manager has to fetch data. If data is not
	 * fetched within this timeout, the data that is available is sent.
	 */
	app_mgr_event->timeout = 10;

	EVENT_SUBMIT(app_mgr_event);
}

static void data_sample_timer_handler(struct k_timer *dummy)
{
	struct app_mgr_event *app_mgr_event = new_app_mgr_event();

	app_mgr_event->type = APP_MGR_EVT_DATA_GET_ALL;

	EVENT_SUBMIT(app_mgr_event);
}

static bool event_handler(const struct event_header *eh)
{
	if (is_cloud_mgr_event(eh)) {
		struct cloud_mgr_event *event = cast_cloud_mgr_event(eh);
		struct app_msg_data app_msg = {
			.manager.cloud = *event
		};

		module_enqueue_msg(&self, &app_msg);
	}

	if (is_app_mgr_event(eh)) {
		struct app_mgr_event *event = cast_app_mgr_event(eh);
		struct app_msg_data app_msg = {
			.manager.app = *event
		};

		module_enqueue_msg(&self, &app_msg);
	}

	if (is_data_mgr_event(eh)) {
		struct data_mgr_event *event = cast_data_mgr_event(eh);
		struct app_msg_data app_msg = {
			.manager.data = *event
		};

		module_enqueue_msg(&self, &app_msg);
	}

	if (is_sensor_mgr_event(eh)) {
		struct sensor_mgr_event *event = cast_sensor_mgr_event(eh);
		struct app_msg_data app_msg = {
			.manager.sensor = *event
		};

		module_enqueue_msg(&self, &app_msg);
	}

	if (is_util_mgr_event(eh)) {
		struct util_mgr_event *event = cast_util_mgr_event(eh);
		struct app_msg_data app_msg = {
			.manager.util = *event
		};

		module_enqueue_msg(&self, &app_msg);
	}

	if (is_modem_mgr_event(eh)) {
		struct modem_mgr_event *event = cast_modem_mgr_event(eh);
		struct app_msg_data app_msg = {
			.manager.modem = *event
		};

		module_enqueue_msg(&self, &app_msg);
	}

	return false;
}

static void on_state_init(struct app_msg_data *msg)
{
	if (IS_EVENT(msg, data, DATA_MGR_EVT_CONFIG_INIT)) {
		/* Keep a copy of the new configuration. */
		app_cfg = msg->manager.data.data.cfg;

		if (app_cfg.act) {
			LOG_INF("Device mode: Active");
			LOG_INF("Start data sample timer: %d seconds interval",
				app_cfg.actw);
			k_timer_start(&data_sample_timer,
				      K_SECONDS(app_cfg.actw),
				      K_SECONDS(app_cfg.actw));
		} else {
			LOG_INF("Device mode: Passive");
			LOG_INF("Start movement timeout: %d seconds interval",
				app_cfg.movt);

			k_timer_start(&movement_timeout_timer,
				K_SECONDS(app_cfg.movt),
				K_SECONDS(app_cfg.movt));
		}

		state_set(APP_MGR_STATE_RUNNING);
		sub_state_set(app_cfg.act ? APP_SUB_STATE_ACTIVE_MODE :
					    APP_SUB_STATE_PASSIVE_MODE);
	}
}

void on_sub_state_passive(struct app_msg_data *msg)
{
	if (IS_EVENT(msg, data, DATA_MGR_EVT_CONFIG_READY)) {
		/* Keep a copy of the new configuration. */
		app_cfg = msg->manager.data.data.cfg;

		/*Acknowledge configuration to cloud. */
		config_send();

		if (app_cfg.act) {
			LOG_INF("Device mode: Active");
			LOG_INF("Start data sample timer: %d seconds interval",
				app_cfg.actw);
			k_timer_start(&data_sample_timer,
				      K_SECONDS(app_cfg.actw),
				      K_SECONDS(app_cfg.actw));
			k_timer_stop(&movement_timeout_timer);
			sub_state_set(APP_SUB_STATE_ACTIVE_MODE);
			return;
		}

		LOG_INF("Device mode: Passive");
		LOG_INF("Start movement timeout: %d seconds interval",
			app_cfg.movt);

		k_timer_start(&movement_timeout_timer,
			      K_SECONDS(app_cfg.movt),
			      K_SECONDS(app_cfg.movt));
		k_timer_stop(&data_sample_timer);
	}

	if (IS_EVENT(msg, sensor, SENSOR_MGR_EVT_MOVEMENT_DATA_READY)) {
		if (k_timer_remaining_get(&movement_resolution_timer) == 0) {
			/* Do an initial data sample. */
			data_sample_timer_handler(NULL);

			LOG_INF("%d seconds until movement can trigger",
				app_cfg.pasw);
			LOG_INF("a new data sample/publication");

			/* Start one shot timer. After the timer has expired,
			 * movement is the only event that triggers a new
			 * one shot timer.
			 */
			k_timer_start(&movement_resolution_timer,
				      K_SECONDS(app_cfg.pasw),
				      K_SECONDS(0));
		}
	}
}

static void on_sub_state_active(struct app_msg_data *msg)
{
	if (IS_EVENT(msg, data, DATA_MGR_EVT_CONFIG_READY)) {
		/* Keep a copy of the new configuration. */
		app_cfg = msg->manager.data.data.cfg;

		/* Acknowledge configuration to cloud. */
		config_send();

		if (!app_cfg.act) {
			LOG_INF("Device mode: Passive");
			LOG_INF("Start movement timeout: %d seconds interval",
				app_cfg.movt);
			k_timer_start(&movement_timeout_timer,
				      K_SECONDS(app_cfg.movt),
				      K_SECONDS(app_cfg.movt));
			k_timer_stop(&data_sample_timer);
			sub_state_set(APP_SUB_STATE_PASSIVE_MODE);
			return;
		}

		LOG_INF("Device mode: Active");
		LOG_INF("Start data sample timer: %d seconds interval",
			app_cfg.actw);

		k_timer_start(&data_sample_timer,
			      K_SECONDS(app_cfg.actw),
			      K_SECONDS(app_cfg.actw));
		k_timer_stop(&movement_timeout_timer);
	}
}

static void on_state_running(struct app_msg_data *msg)
{
	/* Always send the device configuration upon a
	 * established connection to cloud.
	 */
	if (IS_EVENT(msg, cloud, CLOUD_MGR_EVT_CONNECTED)) {
		config_send();
	}

	if (IS_EVENT(msg, modem, MODEM_MGR_EVT_DATE_TIME_OBTAINED)) {
		data_get_init();
	}

	if (IS_EVENT(msg, app, APP_MGR_EVT_DATA_GET_ALL)) {
		data_get_all();
	}
}

static void on_all_events(struct app_msg_data *msg)
{
	if (IS_EVENT(msg, util, UTIL_MGR_EVT_SHUTDOWN_REQUEST)) {
		k_timer_stop(&data_sample_timer);
		k_timer_stop(&movement_timeout_timer);
		k_timer_stop(&movement_resolution_timer);

		struct app_mgr_event *app_mgr_event = new_app_mgr_event();

		app_mgr_event->type = APP_MGR_EVT_SHUTDOWN_READY;
		EVENT_SUBMIT(app_mgr_event);
	}
}

static void signal_app_start(void)
{
	struct app_mgr_event *app_mgr_event = new_app_mgr_event();

	app_mgr_event->type = APP_MGR_EVT_START;
	EVENT_SUBMIT(app_mgr_event);
}

void main(void)
{
	int err;
	struct app_msg_data msg;

	self.thread_id = k_current_get();

	atomic_inc(&manager_count);

	if (event_manager_init()) {
		LOG_ERR("Event manager not initialized");
	}

#if defined(CONFIG_WATCHDOG)
	err = watchdog_init_and_start();
	if (err) {
		LOG_DBG("watchdog_init_and_start, error: %d", err);
		signal_error(err);
	}
#endif

	signal_app_start();

	while (true) {
		module_get_next_msg(&self, &msg);

		switch (app_state) {
		case APP_MGR_STATE_INIT:
			on_state_init(&msg);
			break;
		case APP_MGR_STATE_RUNNING:
			switch (app_sub_state) {
			case APP_SUB_STATE_ACTIVE_MODE:
				on_sub_state_active(&msg);
				break;
			case APP_SUB_STATE_PASSIVE_MODE:
				on_sub_state_passive(&msg);
				break;
			default:
				LOG_WRN("Unknown application sub state");
				break;
			}

			on_state_running(&msg);
			break;
		default:
			LOG_WRN("Unknown application state");
			break;
		}

		on_all_events(&msg);
	}
}

EVENT_LISTENER(MODULE, event_handler);
EVENT_SUBSCRIBE(MODULE, app_mgr_event);
EVENT_SUBSCRIBE(MODULE, data_mgr_event);
EVENT_SUBSCRIBE(MODULE, util_mgr_event);
EVENT_SUBSCRIBE_EARLY(MODULE, cloud_mgr_event);
EVENT_SUBSCRIBE_FINAL(MODULE, ui_mgr_event);
EVENT_SUBSCRIBE_FINAL(MODULE, sensor_mgr_event);
EVENT_SUBSCRIBE_FINAL(MODULE, modem_mgr_event);
