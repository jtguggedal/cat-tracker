/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#include <zephyr.h>
#include <stdio.h>
#include <drivers/sensor.h>
#include <event_manager.h>

#if defined(CONFIG_EXTERNAL_SENSORS)
#include "ext_sensors.h"
#endif

#define MODULE sensor_module

#include "modules_common.h"
#include "events/app_module_event.h"
#include "events/data_module_event.h"
#include "events/sensor_module_event.h"
#include "events/util_module_event.h"

#include <logging/log.h>
LOG_MODULE_REGISTER(sensor_module, CONFIG_SENSOR_MODULE_LOG_LEVEL);

struct sensor_msg_data {
	union {
		struct app_module_event app;
		struct data_module_event data;
		struct util_module_event util;
	} module;
};

/* Sensor module super states. */
enum sensor_module_state {
	SENSOR_STATE_INIT,
	SENSOR_STATE_RUNNING
} sensor_state;

K_MSGQ_DEFINE(msgq_sensor, sizeof(struct sensor_msg_data), 10, 4);

static struct module_data self = {
	.name = "sensor",
	.msg_q = &msgq_sensor,
};

#if defined(CONFIG_EXTERNAL_SENSORS)
static void movement_data_send(const struct ext_sensor_evt *const acc_data)
{
	static int buf_entry_try_again_timeout;

	/* Only populate accelerometer buffer if a configurable amount of time
	 * has passed since the last accelerometer buffer entry was filled.
	 */
	if (k_uptime_get() - buf_entry_try_again_timeout >
	    1000 * CONFIG_TIME_BETWEEN_ACCELEROMETER_BUFFER_STORE_SEC) {

		struct sensor_module_event *sensor_module_event =
				new_sensor_module_event();

		sensor_module_event->data.accel.values[0] =
				acc_data->value_array[0];
		sensor_module_event->data.accel.values[1] =
				acc_data->value_array[1];
		sensor_module_event->data.accel.values[2] =
				acc_data->value_array[2];
		sensor_module_event->data.accel.ts = k_uptime_get();
		sensor_module_event->data.accel.queued = true;
		sensor_module_event->type = SENSOR_EVT_MOVEMENT_DATA_READY;

		EVENT_SUBMIT(sensor_module_event);

		buf_entry_try_again_timeout = k_uptime_get();
	}
}

static void ext_sensor_handler(const struct ext_sensor_evt *const evt)
{
	switch (evt->type) {
	case EXT_SENSOR_EVT_ACCELEROMETER_TRIGGER:
		movement_data_send(evt);
		break;
	default:
		break;
	}
}
#endif

static int environmental_data_get(void)
{
	struct sensor_module_event *sensor_module_event;
#if defined(CONFIG_EXTERNAL_SENSORS)
	int err;
	double temp, hum;

	/* Request data from external sensors. */
	err = ext_sensors_temperature_get(&temp);
	if (err) {
		LOG_ERR("temperature_get, error: %d", err);
		return err;
	}

	err = ext_sensors_humidity_get(&hum);
	if (err) {
		LOG_ERR("temperature_get, error: %d", err);
		return err;
	}

	sensor_module_event = new_sensor_module_event();
	sensor_module_event->data.sensors.env_ts = k_uptime_get();
	sensor_module_event->data.sensors.temp = temp;
	sensor_module_event->data.sensors.hum = hum;
	sensor_module_event->data.sensors.queued = true;
	sensor_module_event->type = SENSOR_EVT_ENVIRONMENTAL_DATA_READY;
#else

	/* This event must be sent even though environmental sensors are not
	 * available on the nRF9160DK. This is because the Data module expects
	 * responses from the different modules within a certain amounf of time
	 * after the APP_EVT_DATA_GET event has been emitted.
	 */
	LOG_DBG("No external sensors, submitting dummy sensor data");

	/* Set this entry to false signifying that the event carries no data.
	 * This makes sure that the entry is not stored in the circular buffer.
	 */
	sensor_module_event = new_sensor_module_event();
	sensor_module_event->data.sensors.queued = false;
	sensor_module_event->type = SENSOR_EVT_ENVIRONMENTAL_DATA_READY;
#endif
	EVENT_SUBMIT(sensor_module_event);

	return 0;
}

static int setup(void)
{
#if defined(CONFIG_EXTERNAL_SENSORS)
	int err;

	err = ext_sensors_init(ext_sensor_handler);
	if (err) {
		LOG_ERR("ext_sensors_init, error: %d", err);
		return err;
	}
#endif
	return 0;
}

static bool event_handler(const struct event_header *eh)
{
	if (is_app_module_event(eh)) {
		struct app_module_event *event = cast_app_module_event(eh);
		struct sensor_msg_data sensor_msg = {
			.module.app = *event
		};

		module_enqueue_msg(&self, &sensor_msg);
	}

	if (is_data_module_event(eh)) {
		struct data_module_event *event = cast_data_module_event(eh);
		struct sensor_msg_data sensor_msg = {
			.module.data = *event
		};

		module_enqueue_msg(&self, &sensor_msg);
	}

	if (is_util_module_event(eh)) {
		struct util_module_event *event = cast_util_module_event(eh);
		struct sensor_msg_data sensor_msg = {
			.module.util = *event
		};

		module_enqueue_msg(&self, &sensor_msg);
	}

	return false;
}

static bool environmental_data_requested(enum app_module_data_type *data_list,
					 size_t count)
{
	for (size_t i = 0; i < count; i++) {
		if (data_list[i] == APP_DATA_ENVIRONMENTAL) {
			return true;
		}
	}

	return false;
}

static void on_state_init(struct sensor_msg_data *msg)
{
	if (IS_EVENT(msg, data, DATA_EVT_CONFIG_INIT)) {
#if defined(CONFIG_EXTERNAL_SENSORS)
		int movement_threshold = msg->module.data.data.cfg.acct;

		ext_sensors_mov_thres_set(movement_threshold);
#endif
		sensor_state = SENSOR_STATE_RUNNING;
	}
}

static void on_state_running(struct sensor_msg_data *msg)
{
	if (IS_EVENT(msg, data, DATA_EVT_CONFIG_READY)) {

#if defined(CONFIG_EXTERNAL_SENSORS)
		int movement_threshold = msg->module.data.data.cfg.acct;

		ext_sensors_mov_thres_set(movement_threshold);
#endif
	}

	if (IS_EVENT(msg, app, APP_EVT_DATA_GET)) {
		if (!environmental_data_requested(
			msg->module.app.data_list,
			msg->module.app.count)) {
			return;
		}

		int err;

		err = environmental_data_get();
		if (err) {
			LOG_ERR("environmental_data_get, error: %d", err);
			SEND_ERROR(sensor, SENSOR_EVT_ERROR, err);
		}
	}
}

static void on_all_states(struct sensor_msg_data *msg)
{
	if (IS_EVENT(msg, util, UTIL_EVT_SHUTDOWN_REQUEST)) {
		SEND_EVENT(sensor, SENSOR_EVT_SHUTDOWN_READY);
	}
}

static void sensor_module(void)
{
	int err;

	struct sensor_msg_data msg;

	self.thread_id = k_current_get();

	module_start(&self);

	err = setup();
	if (err) {
		LOG_ERR("setup, error: %d", err);
		SEND_ERROR(sensor, SENSOR_EVT_ERROR, err);
	}

	while (true) {
		module_get_next_msg(&self, &msg);

		switch (sensor_state) {
		case SENSOR_STATE_INIT:
			on_state_init(&msg);
			break;
		case SENSOR_STATE_RUNNING:
			on_state_running(&msg);
			break;
		default:
			LOG_WRN("Unknown sensor module state.");
			break;
		}

		on_all_states(&msg);
	}
}

K_THREAD_DEFINE(sensor_module_thread, CONFIG_SENSOR_THREAD_STACK_SIZE,
		sensor_module, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);

EVENT_LISTENER(MODULE, event_handler);
EVENT_SUBSCRIBE(MODULE, app_module_event);
EVENT_SUBSCRIBE(MODULE, data_module_event);
EVENT_SUBSCRIBE(MODULE, util_module_event);
