/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#include <zephyr.h>
#include <event_manager.h>
#include <settings/settings.h>

#include "cloud/cloud_codec/cloud_codec.h"

#define MODULE data_manager

#include "modules_common.h"
#include "events/app_mgr_event.h"
#include "events/cloud_mgr_event.h"
#include "events/data_mgr_event.h"
#include "events/gps_mgr_event.h"
#include "events/modem_mgr_event.h"
#include "events/sensor_mgr_event.h"
#include "events/ui_mgr_event.h"
#include "events/util_mgr_event.h"

#include <logging/log.h>
LOG_MODULE_REGISTER(MODULE, CONFIG_CAT_TRACKER_LOG_LEVEL);

extern atomic_t manager_count;

#define DEVICE_SETTINGS_KEY			"data_manager"
#define DEVICE_SETTINGS_CONFIG_KEY		"config"

/* Default device configuration values. */
#define DEFAULT_ACTIVE_TIMEOUT_SECONDS		120
#define DEFAULT_PASSIVE_TIMEOUT_SECONDS		120
#define DEFAULT_MOVEMENT_TIMEOUT_SECONDS	3600
#define DEFAULT_ACCELEROMETER_THRESHOLD		100
#define DEFAULT_GPS_TIMEOUT_SECONDS		60
#define DEFAULT_DEVICE_MODE			true

static void data_send_work_fn(struct k_work *work);

struct data_msg_data {
	union {
		struct modem_mgr_event modem;
		struct cloud_mgr_event cloud;
		struct gps_mgr_event gps;
		struct ui_mgr_event ui;
		struct sensor_mgr_event sensor;
		struct data_mgr_event data;
		struct app_mgr_event app;
		struct util_mgr_event util;
	} manager;
};

K_MSGQ_DEFINE(msgq_data, sizeof(struct data_msg_data), 10, 4);

static struct module_data self = {
	.msg_q = &msgq_data,
};
/* Ringbuffers. All data received by the data manager are stored in ringbuffers.
 * Upon a LTE connection loss the device will keep sampling/storing data in
 * the buffers, and empty the buffers in batches upon a reconnect.
 */
static struct cloud_data_gps gps_buf[CONFIG_GPS_BUFFER_MAX];
static struct cloud_data_sensors sensors_buf[CONFIG_SENSOR_BUFFER_MAX];
static struct cloud_data_modem modem_buf[CONFIG_MODEM_BUFFER_MAX];
static struct cloud_data_ui ui_buf[CONFIG_UI_BUFFER_MAX];
static struct cloud_data_accelerometer accel_buf[CONFIG_ACCEL_BUFFER_MAX];
static struct cloud_data_battery bat_buf[CONFIG_BAT_BUFFER_MAX];

/* Head of ringbuffers. */
static int head_gps_buf;
static int head_sensor_buf;
static int head_modem_buf;
static int head_ui_buf;
static int head_accel_buf;
static int head_bat_buf;

/* Default device configuration. */
static struct cloud_data_cfg current_cfg = {
	.gpst = DEFAULT_GPS_TIMEOUT_SECONDS,
	.act  = DEFAULT_DEVICE_MODE,
	.actw = DEFAULT_ACTIVE_TIMEOUT_SECONDS,
	.pasw = DEFAULT_PASSIVE_TIMEOUT_SECONDS,
	.movt = DEFAULT_MOVEMENT_TIMEOUT_SECONDS,
	.acct = DEFAULT_ACCELEROMETER_THRESHOLD
};

/* Cloud connection state. */
enum cloud_connection_state {
	CLOUD_STATE_DISCONNECTED,
	CLOUD_STATE_CONNECTED
} state;

/* Time state. */
enum time_state {
	TIME_STATE_NOT_OBTAINED,
	TIME_STATE_OBTAINED
} time_state;

static struct k_delayed_work data_send_work;

/* List used to keep track of responses from other managers with data that is
 * requested to be sampled/published.
 */
static enum app_mgr_data_type data_types_list[APP_DATA_NUMBER_OF_TYPES_MAX];

/* Total number of data types requested for a particular sample/publish
 * cycle.
 */
static int received_data_type_count;

/* Counter of data types received from other managers. When this number
 * matches the affirmed_data_type variable all requested data has been
 * received by the data manager.
 */
static int data_cnt;

/* Data that has been encoded and shipped on, but has not yet been ACKed as sent
 */
static void *pending_data[10];

/* Forward declarations */
static int config_settings_handler(const char *key, size_t len,
				   settings_read_cb read_cb, void *cb_arg);

/* Static handlers */
SETTINGS_STATIC_HANDLER_DEFINE(MODULE, DEVICE_SETTINGS_KEY, NULL,
			       config_settings_handler, NULL, NULL);

static char *state2str(enum cloud_connection_state state)
{
	switch (state) {
	case CLOUD_STATE_DISCONNECTED:
		return "CLOUD_STATE_DISCONNECTED";
	case CLOUD_STATE_CONNECTED:
		return "CLOUD_STATE_CONNECTED";
	default:
		return "Unknown";
	}
}

static char *time_state2str(enum time_state state)
{
	switch (state) {
	case TIME_STATE_NOT_OBTAINED:
		return "TIME_STATE_NOT_OBTAINED";
	case TIME_STATE_OBTAINED:
		return "TIME_STATE_OBTAINED";
	default:
		return "Unknown";
	}
}

static void state_set(enum cloud_connection_state new_state)
{
	if (new_state == state) {
		LOG_DBG("State: %s", log_strdup(state2str(state)));
		return;
	}

	LOG_DBG("State transition %s --> %s",
		log_strdup(state2str(state)),
		log_strdup(state2str(new_state)));

	state = new_state;
}

static void time_state_set(enum time_state new_state)
{
	if (new_state == time_state) {
		LOG_DBG("State: %s", log_strdup(time_state2str(time_state)));
		return;
	}

	LOG_DBG("Time state transition %s --> %s",
		log_strdup(time_state2str(time_state)),
		log_strdup(time_state2str(new_state)));

	time_state = new_state;
}

static bool pending_data_add(void *ptr)
{
	for (size_t i = 0; i < ARRAY_SIZE(pending_data); i++) {
		if (pending_data[i] == NULL) {
			pending_data[i] = ptr;

			LOG_DBG("Pending data added: %p", ptr);

			return true;
		}
	}

	LOG_WRN("Could not add pointer to pending list");

	return false;
}

static bool pending_data_ack(void *ptr)
{
	for (size_t i = 0; i < ARRAY_SIZE(pending_data); i++) {
		if (pending_data[i] == ptr) {
			k_free(ptr);

			LOG_DBG("Pending data ACKed: %p", pending_data[i]);

			pending_data[i] = NULL;

			return true;
		}
	}

	LOG_WRN("No matching pointer was found");

	return false;
}

static int config_settings_handler(const char *key, size_t len,
				   settings_read_cb read_cb, void *cb_arg)
{
	int err;

	if (strcmp(key, DEVICE_SETTINGS_CONFIG_KEY) == 0) {
		err = read_cb(cb_arg, &current_cfg, sizeof(current_cfg));
		if (err < 0) {
			LOG_ERR("Failed to load configuration, error: %d", err);
			return err;
		}
	}

	LOG_DBG("Device configuration loaded from flash");

	return 0;
}

static int data_manager_save_config(const void *buf, size_t buf_len)
{
	int err;

	err = settings_save_one(DEVICE_SETTINGS_KEY "/"
				DEVICE_SETTINGS_CONFIG_KEY,
				buf, buf_len);
	if (err) {
		LOG_WRN("settings_save_one, error: %d", err);
		return err;
	}

	LOG_DBG("Device configuration stored to flash");

	return 0;
}

static int data_manager_setup(void)
{
	int err;

	cloud_codec_init();

	err = settings_subsys_init();
	if (err) {
		LOG_ERR("settings_subsys_init, error: %d", err);
		return err;
	}

	err = settings_load_subtree(DEVICE_SETTINGS_KEY);
	if (err) {
		LOG_ERR("settings_load_subtree, error: %d", err);
		return err;
	}

	return 0;
}

static void signal_error(int err)
{
	struct data_mgr_event *data_mgr_event = new_data_mgr_event();

	data_mgr_event->type = DATA_MGR_EVT_ERROR;
	data_mgr_event->data.err = err;

	EVENT_SUBMIT(data_mgr_event);
}

/* This function allocates buffer on the heap, which needs to be freed afte use.
 */
static void data_send(void)
{
	int err;
	struct data_mgr_event *data_mgr_event_new;
	struct data_mgr_event *data_mgr_event_batch;
	struct cloud_codec_data codec;

	err = cloud_codec_encode_data(
		&codec,
		&gps_buf[head_gps_buf],
		&sensors_buf[head_sensor_buf],
		&modem_buf[head_modem_buf],
		&ui_buf[head_ui_buf],
		&accel_buf[head_accel_buf],
		&bat_buf[head_bat_buf]);
	if (err == -ENODATA) {
		/* This error might occurs when data has not been obtained prior
		 * to data encoding.
		 */
		LOG_WRN("Ringbuffers empty...");
		LOG_WRN("No data to encode, error: %d", err);
		return;
	} else if (err) {
		LOG_ERR("Error encoding message %d", err);
		signal_error(err);
		return;
	}

	LOG_DBG("Data encoded successfully");


	data_mgr_event_new = new_data_mgr_event();
	data_mgr_event_new->type = DATA_MGR_EVT_DATA_SEND;

	data_mgr_event_new->data.buffer.buf = codec.buf;
	data_mgr_event_new->data.buffer.len = codec.len;

	pending_data_add(codec.buf);
	EVENT_SUBMIT(data_mgr_event_new);

	codec.buf = NULL;
	codec.len = 0;

	// Encode batch data
	err = cloud_codec_encode_batch_data(&codec,
					gps_buf,
					sensors_buf,
					modem_buf,
					ui_buf,
					accel_buf,
					bat_buf,
					ARRAY_SIZE(gps_buf),
					ARRAY_SIZE(sensors_buf),
					ARRAY_SIZE(modem_buf),
					ARRAY_SIZE(ui_buf),
					ARRAY_SIZE(accel_buf),
					ARRAY_SIZE(bat_buf));
	if (err == -ENODATA) {
		LOG_WRN("No batch data to encode, ringbuffers empty");
		return;
	} else if (err) {
		LOG_ERR("Error batch-enconding data: %d", err);
		signal_error(err);
		return;
	}

	data_mgr_event_batch = new_data_mgr_event();
	data_mgr_event_batch->type = DATA_MGR_EVT_DATA_SEND_BATCH;
	data_mgr_event_batch->data.buffer.buf = codec.buf;
	data_mgr_event_batch->data.buffer.len = codec.len;

	pending_data_add(codec.buf);
	EVENT_SUBMIT(data_mgr_event_batch);
}

static void cloud_manager_config_get(void)
{
	struct data_mgr_event *evt = new_data_mgr_event();

	evt->type = DATA_MGR_EVT_CONFIG_GET;

	EVENT_SUBMIT(evt);
}

static void cloud_manager_config_send(void)
{
	int err;
	struct cloud_codec_data codec;
	struct data_mgr_event *evt;

	err = cloud_codec_encode_config(&codec, &current_cfg);
	if (err) {
		LOG_ERR("Error encoding configuration, error: %d", err);
		signal_error(err);
		return;
	}

	evt = new_data_mgr_event();
	evt->type = DATA_MGR_EVT_CONFIG_SEND;
	evt->data.buffer.buf = codec.buf;
	evt->data.buffer.len = codec.len;

	pending_data_add(codec.buf);
	EVENT_SUBMIT(evt);
}


static void data_ui_send(void)
{
	int err;
	struct data_mgr_event *evt;
	struct cloud_codec_data codec;

	err = cloud_codec_encode_ui_data(&codec, &ui_buf[head_ui_buf]);
	if (err) {
		LOG_ERR("Enconding button press, error: %d", err);
		signal_error(err);
		return;
	}

	evt = new_data_mgr_event();
	evt->type = DATA_MGR_EVT_UI_DATA_SEND;
	evt->data.buffer.buf = codec.buf;
	evt->data.buffer.len = codec.len;

	pending_data_add(codec.buf);

	/* Since a copy of data is sent we must unqueue the head of the
	 * UI buffer.
	 */

	ui_buf[head_ui_buf].queued = false;

	EVENT_SUBMIT(evt);
}

static void config_distribute(enum data_mgr_event_types type)
{
	struct data_mgr_event *data_mgr_event = new_data_mgr_event();

	data_mgr_event->type = type;
	data_mgr_event->data.cfg = current_cfg;

	EVENT_SUBMIT(data_mgr_event);
}

static bool event_handler(const struct event_header *eh)
{
	if (is_modem_mgr_event(eh)) {
		struct modem_mgr_event *event = cast_modem_mgr_event(eh);
		struct data_msg_data data_msg = {
			.manager.modem = *event
		};

		module_enqueue_msg(&self, &data_msg);
	}

	if (is_cloud_mgr_event(eh)) {
		struct cloud_mgr_event *event = cast_cloud_mgr_event(eh);
		struct data_msg_data data_msg = {
			.manager.cloud = *event
		};

		module_enqueue_msg(&self, &data_msg);
	}

	if (is_gps_mgr_event(eh)) {
		struct gps_mgr_event *event = cast_gps_mgr_event(eh);
		struct data_msg_data data_msg = {
			.manager.gps = *event
		};

		module_enqueue_msg(&self, &data_msg);
	}

	if (is_sensor_mgr_event(eh)) {
		struct sensor_mgr_event *event = cast_sensor_mgr_event(eh);
		struct data_msg_data data_msg = {
			.manager.sensor = *event
		};

		module_enqueue_msg(&self, &data_msg);
	}

	if (is_ui_mgr_event(eh)) {
		struct ui_mgr_event *event = cast_ui_mgr_event(eh);
		struct data_msg_data data_msg = {
			.manager.ui = *event
		};

		module_enqueue_msg(&self, &data_msg);
	}

	if (is_app_mgr_event(eh)) {
		struct app_mgr_event *event = cast_app_mgr_event(eh);
		struct data_msg_data data_msg = {
			.manager.app = *event
		};

		module_enqueue_msg(&self, &data_msg);
	}

	if (is_data_mgr_event(eh)) {
		struct data_mgr_event *event = cast_data_mgr_event(eh);
		struct data_msg_data data_msg = {
			.manager.data = *event
		};

		module_enqueue_msg(&self, &data_msg);
	}

	if (is_util_mgr_event(eh)) {
		struct util_mgr_event *event = cast_util_mgr_event(eh);
		struct data_msg_data data_msg = {
			.manager.util = *event
		};

		module_enqueue_msg(&self, &data_msg);
	}

	return false;
}

static void clear_local_data_list(void)
{
	received_data_type_count = 0;
	data_cnt = 0;
}

static void data_send_work_fn(struct k_work *work)
{
	struct app_mgr_event *app_mgr_event = new_app_mgr_event();

	app_mgr_event->type = APP_MGR_EVT_DATA_SEND;

	EVENT_SUBMIT(app_mgr_event);

	clear_local_data_list();
	k_delayed_work_cancel(&data_send_work);
}

static void data_status_set(enum app_mgr_data_type data_type)
{
	for (size_t i = 0; i < received_data_type_count; i++) {
		if (data_types_list[i] == data_type) {
			data_cnt++;
			break;
		}
	}

	if (data_cnt == received_data_type_count) {
		data_send_work_fn(NULL);
	}
}

static void data_list_set(enum app_mgr_data_type *data_list, size_t count)
{
	if ((count == 0) || (count > APP_DATA_NUMBER_OF_TYPES_MAX)) {
		LOG_ERR("Invalid data type list length");
		return;
	}

	clear_local_data_list();

	for (size_t i = 0; i < count; i++) {
		data_types_list[i] = data_list[i];
	}

	received_data_type_count = count;
}

static void on_cloud_state_disconnected(struct data_msg_data *msg)
{
	if (IS_EVENT(msg, cloud, CLOUD_MGR_EVT_CONNECTED)) {
		state_set(CLOUD_STATE_CONNECTED);
	}
}

static void on_cloud_state_connected(struct data_msg_data *msg)
{
	/* Send data only if time is obtained. Otherwise cache it. */
	switch (time_state) {
	case TIME_STATE_OBTAINED:
		if (IS_EVENT(msg, app, APP_MGR_EVT_DATA_SEND)) {
			data_send();
			return;
		}

		if (IS_EVENT(msg, ui, UI_MGR_EVT_BUTTON_DATA_READY)) {
			cloud_codec_populate_ui_buffer(
						ui_buf,
						&msg->manager.ui.data.ui,
						&head_ui_buf);
			data_ui_send();
			return;
		}
		break;
	case TIME_STATE_NOT_OBTAINED:
		if (IS_EVENT(msg, modem, MODEM_MGR_EVT_DATE_TIME_OBTAINED)) {
			time_state_set(TIME_STATE_OBTAINED);
			return;
		}
		break;
	default:
		LOG_WRN("Unknown sub-sub state.");
		break;
	}

	if (IS_EVENT(msg, app, APP_MGR_EVT_CONFIG_GET)) {
		cloud_manager_config_get();
		return;
	}

	if (IS_EVENT(msg, cloud, CLOUD_MGR_EVT_DISCONNECTED)) {
		state_set(CLOUD_STATE_DISCONNECTED);
		return;
	}

	/* Config is not timestamped and does not to be dependent on the
	 * MODEM_MGR_SUB_SUB_STATE_DATE_TIME_OBTAINED state.
	 */
	if (IS_EVENT(msg, app, APP_MGR_EVT_CONFIG_SEND)) {
		cloud_manager_config_send();
		return;
	}

	/* DIstribute new configuration received form cloud */
	if (IS_EVENT(msg, cloud, CLOUD_MGR_EVT_CONFIG_RECEIVED)) {

		int err;
		bool config_change = false;
		struct cloud_data_cfg new = {
			.act = msg->manager.cloud.data.config.act,
			.actw = msg->manager.cloud.data.config.actw,
			.pasw = msg->manager.cloud.data.config.pasw,
			.movt = msg->manager.cloud.data.config.movt,
			.gpst = msg->manager.cloud.data.config.gpst,
			.acct = msg->manager.cloud.data.config.acct,

		};

		/* Only change values that are not 0 and have changed.
		 * Since 0 is a valid value for the mode configuration we dont
		 * include the 0 check. In general I think we should have
		 * minimum allowed values so that extremely low configurations
		 * dont suffocate the application.
		 */
		if (current_cfg.act != new.act) {
			current_cfg.act = new.act;

			if (current_cfg.act) {
				LOG_WRN("New Device mode: Active");
			} else {
				LOG_WRN("New Device mode: Passive");
			}

			config_change = true;
		}

		if (current_cfg.actw != new.actw && new.actw != 0) {
			current_cfg.actw = new.actw;
			LOG_WRN("New Active timeout: %d", current_cfg.actw);

			config_change = true;
		}

		if (current_cfg.pasw != new.pasw && new.pasw != 0) {
			current_cfg.pasw = new.pasw;
			LOG_WRN("New Movement resolution: %d",
				current_cfg.pasw);

			config_change = true;
		}

		if (current_cfg.movt != new.movt && new.movt != 0) {
			current_cfg.movt = new.movt;
			LOG_WRN("New Movement timeout: %d", current_cfg.movt);

			config_change = true;
		}

		if (current_cfg.acct != new.acct && new.acct != 0) {
			current_cfg.acct = new.acct;
			LOG_WRN("New Movement threshold: %d", current_cfg.acct);

			config_change = true;
		}

		if (current_cfg.gpst != new.gpst && new.gpst != 0) {
			current_cfg.gpst = new.gpst;
			LOG_WRN("New GPS timeout: %d", current_cfg.gpst);

			config_change = true;
		}

		if (config_change) {
			err = data_manager_save_config(&current_cfg,
						       sizeof(current_cfg));
			if (err) {
				LOG_WRN("Configuration not stored, error: %d",
					err);
			}

			config_distribute(DATA_MGR_EVT_CONFIG_READY);
		} else {
			LOG_DBG("No change in device configuration");
		}
	}
}

static void on_all_states(struct data_msg_data *msg)
{
	if (IS_EVENT(msg, app, APP_MGR_EVT_START)) {
		config_distribute(DATA_MGR_EVT_CONFIG_INIT);
	}

	if (IS_EVENT(msg, util, UTIL_MGR_EVT_SHUTDOWN_REQUEST)) {
		struct data_mgr_event *data_mgr_event = new_data_mgr_event();

		data_mgr_event->type = DATA_MGR_EVT_SHUTDOWN_READY;

		/* The module doesn't have anything to shut down and can
		 * report back immediately.
		 */
		EVENT_SUBMIT(data_mgr_event);
	}

	if (IS_EVENT(msg, app, APP_MGR_EVT_DATA_GET)) {
		/* Store which data is requested by the app, later to be used
		 * to confirm data is reported to the data manger.
		 */
		data_list_set(msg->manager.app.data_list,
			      msg->manager.app.count);

		/* Start countdown until data must have been received by the
		 * data manager in order to be sent to cloud
		 */
		k_delayed_work_submit(&data_send_work,
				      K_SECONDS(msg->manager.app.timeout));

		return;
	}

	if (IS_EVENT(msg, modem, MODEM_MGR_EVT_MODEM_DATA_READY)) {
		cloud_codec_populate_modem_buffer(
			modem_buf,
			&msg->manager.modem.data.modem,
			&head_modem_buf);

		data_status_set(APP_DATA_MODEM);
	}


	if (IS_EVENT(msg, modem, MODEM_MGR_EVT_BATTERY_DATA_READY)) {
		cloud_codec_populate_bat_buffer(
			bat_buf,
			&msg->manager.modem.data.bat,
			&head_bat_buf);

		data_status_set(APP_DATA_BATTERY);
	}

	if (IS_EVENT(msg, sensor, SENSOR_MGR_EVT_ENVIRONMENTAL_DATA_READY)) {
		cloud_codec_populate_sensor_buffer(
			sensors_buf,
			&msg->manager.sensor.data.sensors,
			&head_sensor_buf);

		data_status_set(APP_DATA_ENVIRONMENTALS);
	}

	if (IS_EVENT(msg, sensor, SENSOR_MGR_EVT_MOVEMENT_DATA_READY)) {
		cloud_codec_populate_accel_buffer(
			accel_buf,
			&msg->manager.sensor.data.accel,
			&head_accel_buf);
}

	if (IS_EVENT(msg, gps, GPS_MGR_EVT_DATA_READY)) {
		cloud_codec_populate_gps_buffer(
			gps_buf,
			&msg->manager.gps.data.gps,
			&head_gps_buf);

		data_status_set(APP_DATA_GNSS);
	}

	if (IS_EVENT(msg, gps, GPS_MGR_EVT_TIMEOUT)) {
		data_status_set(APP_DATA_GNSS);
	}


	if (IS_EVENT(msg, cloud, CLOUD_MGR_EVT_DATA_ACK)) {
		pending_data_ack(msg->manager.cloud.data.ptr);
		return;
	}
}

static void data_manager(void)
{
	int err;
	struct data_msg_data msg;

	self.thread_id = k_current_get();

	atomic_inc(&manager_count);

	err = data_manager_setup();
	if (err) {
		LOG_ERR("data_manager_setup, error: %d", err);
		signal_error(err);
	}

	state_set(CLOUD_STATE_DISCONNECTED);

	while (true) {
		module_get_next_msg(&self, &msg);

		switch (state) {
		case CLOUD_STATE_DISCONNECTED:
			on_cloud_state_disconnected(&msg);
			break;
		case CLOUD_STATE_CONNECTED:
			on_cloud_state_connected(&msg);
			break;
		default:
			LOG_WRN("Unknown sub state.");
			break;
		}

		on_all_states(&msg);
	}
}

K_THREAD_DEFINE(data_manager_thread, CONFIG_DATA_MGR_THREAD_STACK_SIZE,
		data_manager, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);

EVENT_LISTENER(MODULE, event_handler);
EVENT_SUBSCRIBE(MODULE, app_mgr_event);
EVENT_SUBSCRIBE(MODULE, util_mgr_event);
EVENT_SUBSCRIBE(MODULE, data_mgr_event);
EVENT_SUBSCRIBE_EARLY(MODULE, modem_mgr_event);
EVENT_SUBSCRIBE_EARLY(MODULE, cloud_mgr_event);
EVENT_SUBSCRIBE_EARLY(MODULE, gps_mgr_event);
EVENT_SUBSCRIBE_EARLY(MODULE, ui_mgr_event);
EVENT_SUBSCRIBE_EARLY(MODULE, sensor_mgr_event);
