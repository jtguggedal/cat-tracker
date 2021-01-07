/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#include <zephyr.h>
#include <event_manager.h>
#include <settings/settings.h>
#include <date_time.h>

#include "cloud/cloud_codec/cloud_codec.h"

#define MODULE data_module

#include "modules_common.h"
#include "events/app_module_event.h"
#include "events/cloud_module_event.h"
#include "events/data_module_event.h"
#include "events/gps_module_event.h"
#include "events/modem_module_event.h"
#include "events/sensor_module_event.h"
#include "events/ui_module_event.h"
#include "events/util_module_event.h"

#include <logging/log.h>
LOG_MODULE_REGISTER(MODULE, CONFIG_DATA_MODULE_LOG_LEVEL);

#define DEVICE_SETTINGS_KEY			"data_module"
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
		struct modem_module_event modem;
		struct cloud_module_event cloud;
		struct gps_module_event gps;
		struct ui_module_event ui;
		struct sensor_module_event sensor;
		struct data_module_event data;
		struct app_module_event app;
		struct util_module_event util;
	} module;
};

K_MSGQ_DEFINE(msgq_data, sizeof(struct data_msg_data), 10, 4);

static struct module_data self = {
	.name = "data",
	.msg_q = &msgq_data,
};
/* Ringbuffers. All data received by the Data module are stored in ringbuffers.
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
static enum state_type {
	STATE_CLOUD_DISCONNECTED,
	STATE_CLOUD_CONNECTED
} state;

static struct k_delayed_work data_send_work;

/* List used to keep track of responses from other modules with data that is
 * requested to be sampled/published.
 */
static enum app_module_data_type data_types_list[APP_DATA_COUNT];

/* Total number of data types requested for a particular sample/publish
 * cycle.
 */
static int received_data_type_count;

/* Counter of data types received from other modules. When this number
 * matches the affirmed_data_type variable all requested data has been
 * received by the Data module.
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

static char *state2str(enum state_type new_state)
{
	switch (new_state) {
	case STATE_CLOUD_DISCONNECTED:
		return "STATE_CLOUD_DISCONNECTED";
	case STATE_CLOUD_CONNECTED:
		return "STATE_CLOUD_CONNECTED";
	default:
		return "Unknown";
	}
}

static void state_set(enum state_type new_state)
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

static int save_config(const void *buf, size_t buf_len)
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

static int setup(void)
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

static void config_distribute(enum data_module_event_types type)
{
	struct data_module_event *data_module_event = new_data_module_event();

	data_module_event->type = type;
	data_module_event->data.cfg = current_cfg;

	EVENT_SUBMIT(data_module_event);
}

/* Date and time control */

static void date_time_event_handler(const struct date_time_evt *evt)
{
	switch (evt->type) {
	case DATE_TIME_OBTAINED_MODEM:
		/* Fall through. */
	case DATE_TIME_OBTAINED_NTP:
		/* Fall through. */
	case DATE_TIME_OBTAINED_EXT: {
		SEND_EVENT(data, DATA_EVT_DATE_TIME_OBTAINED);

		/* De-register handler. At this point the application will have
		 * date time to depend on indefinitely until a reboot occurs.
		 */
		date_time_register_handler(NULL);
		break;
	}
	case DATE_TIME_NOT_OBTAINED:
		break;
	default:
		break;
	}
}

/* This function allocates buffer on the heap, which needs to be freed afte use.
 */
static void data_send(void)
{
	int err;
	struct data_module_event *data_module_event_new;
	struct data_module_event *data_module_event_batch;
	struct cloud_codec_data codec;

	if (!date_time_is_valid()) {
		/* Date time library does not have valid time to
		 * timestamp cloud data. Abort cloud publicaton. Data will
		 * be cached in it respective ringbuffer.
		 */
		return;
	}

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
		SEND_ERROR(data, DATA_EVT_ERROR, err);
		return;
	}

	LOG_DBG("Data encoded successfully");


	data_module_event_new = new_data_module_event();
	data_module_event_new->type = DATA_EVT_DATA_SEND;

	data_module_event_new->data.buffer.buf = codec.buf;
	data_module_event_new->data.buffer.len = codec.len;

	pending_data_add(codec.buf);
	EVENT_SUBMIT(data_module_event_new);

	codec.buf = NULL;
	codec.len = 0;

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
		SEND_ERROR(data, DATA_EVT_ERROR, err);
		return;
	}

	data_module_event_batch = new_data_module_event();
	data_module_event_batch->type = DATA_EVT_DATA_SEND_BATCH;
	data_module_event_batch->data.buffer.buf = codec.buf;
	data_module_event_batch->data.buffer.len = codec.len;

	pending_data_add(codec.buf);
	EVENT_SUBMIT(data_module_event_batch);
}

static void config_get(void)
{
	SEND_EVENT(data, DATA_EVT_CONFIG_GET);
}

static void config_send(void)
{
	int err;
	struct cloud_codec_data codec;
	struct data_module_event *evt;

	err = cloud_codec_encode_config(&codec, &current_cfg);
	if (err) {
		LOG_ERR("Error encoding configuration, error: %d", err);
		SEND_ERROR(data, DATA_EVT_ERROR, err);
		return;
	}

	evt = new_data_module_event();
	evt->type = DATA_EVT_CONFIG_SEND;
	evt->data.buffer.buf = codec.buf;
	evt->data.buffer.len = codec.len;

	pending_data_add(codec.buf);
	EVENT_SUBMIT(evt);
}


static void data_ui_send(void)
{
	int err;
	struct data_module_event *evt;
	struct cloud_codec_data codec;

	if (!date_time_is_valid()) {
		/* Date time library does not have valid time to
		 * timestamp cloud data. Abort cloud publicaton. Data will
		 * be cached in it respective ringbuffer.
		 */
		return;
	}

	err = cloud_codec_encode_ui_data(&codec, &ui_buf[head_ui_buf]);
	if (err) {
		LOG_ERR("Encoding button press, error: %d", err);
		SEND_ERROR(data, DATA_EVT_ERROR, err);
		return;
	}

	evt = new_data_module_event();
	evt->type = DATA_EVT_UI_DATA_SEND;
	evt->data.buffer.buf = codec.buf;
	evt->data.buffer.len = codec.len;

	pending_data_add(codec.buf);

	EVENT_SUBMIT(evt);
}

static bool event_handler(const struct event_header *eh)
{
	if (is_modem_module_event(eh)) {
		struct modem_module_event *event = cast_modem_module_event(eh);
		struct data_msg_data data_msg = {
			.module.modem = *event
		};

		module_enqueue_msg(&self, &data_msg);
	}

	if (is_cloud_module_event(eh)) {
		struct cloud_module_event *event = cast_cloud_module_event(eh);
		struct data_msg_data data_msg = {
			.module.cloud = *event
		};

		module_enqueue_msg(&self, &data_msg);
	}

	if (is_gps_module_event(eh)) {
		struct gps_module_event *event = cast_gps_module_event(eh);
		struct data_msg_data data_msg = {
			.module.gps = *event
		};

		module_enqueue_msg(&self, &data_msg);
	}

	if (is_sensor_module_event(eh)) {
		struct sensor_module_event *event =
				cast_sensor_module_event(eh);
		struct data_msg_data data_msg = {
			.module.sensor = *event
		};

		module_enqueue_msg(&self, &data_msg);
	}

	if (is_ui_module_event(eh)) {
		struct ui_module_event *event = cast_ui_module_event(eh);
		struct data_msg_data data_msg = {
			.module.ui = *event
		};

		module_enqueue_msg(&self, &data_msg);
	}

	if (is_app_module_event(eh)) {
		struct app_module_event *event = cast_app_module_event(eh);
		struct data_msg_data data_msg = {
			.module.app = *event
		};

		module_enqueue_msg(&self, &data_msg);
	}

	if (is_data_module_event(eh)) {
		struct data_module_event *event = cast_data_module_event(eh);
		struct data_msg_data data_msg = {
			.module.data = *event
		};

		module_enqueue_msg(&self, &data_msg);
	}

	if (is_util_module_event(eh)) {
		struct util_module_event *event = cast_util_module_event(eh);
		struct data_msg_data data_msg = {
			.module.util = *event
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
	SEND_EVENT(data, DATA_EVT_DATA_READY);

	clear_local_data_list();
	k_delayed_work_cancel(&data_send_work);
}

static void data_status_set(enum app_module_data_type data_type)
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

static void data_list_set(enum app_module_data_type *data_list, size_t count)
{
	if ((count == 0) || (count > APP_DATA_COUNT)) {
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
	if (IS_EVENT(msg, cloud, CLOUD_EVT_CONNECTED)) {
		date_time_update_async(date_time_event_handler);
		state_set(STATE_CLOUD_CONNECTED);
	}
}

static void on_cloud_state_connected(struct data_msg_data *msg)
{
	if (IS_EVENT(msg, data, DATA_EVT_DATA_READY)) {
		data_send();
		return;
	}

	if (IS_EVENT(msg, app, APP_EVT_CONFIG_GET)) {
		config_get();
		return;
	}

	if (IS_EVENT(msg, app, APP_EVT_CONFIG_SEND)) {
		config_send();
		return;
	}

	if (IS_EVENT(msg, data, DATA_EVT_UI_DATA_READY)) {
		data_ui_send();
		return;
	}

	if (IS_EVENT(msg, cloud, CLOUD_EVT_DISCONNECTED)) {
		state_set(STATE_CLOUD_DISCONNECTED);
		return;
	}

	/* DIstribute new configuration received form cloud */
	if (IS_EVENT(msg, cloud, CLOUD_EVT_CONFIG_RECEIVED)) {

		int err;
		bool config_change = false;
		struct cloud_data_cfg new = {
			.act = msg->module.cloud.data.config.act,
			.actw = msg->module.cloud.data.config.actw,
			.pasw = msg->module.cloud.data.config.pasw,
			.movt = msg->module.cloud.data.config.movt,
			.gpst = msg->module.cloud.data.config.gpst,
			.acct = msg->module.cloud.data.config.acct,

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
			err = save_config(&current_cfg,
					  sizeof(current_cfg));
			if (err) {
				LOG_WRN("Configuration not stored, error: %d",
					err);
			}

			config_distribute(DATA_EVT_CONFIG_READY);
		} else {
			LOG_DBG("No change in device configuration");
		}
	}
}

static void on_all_states(struct data_msg_data *msg)
{
	if (IS_EVENT(msg, app, APP_EVT_START)) {
		config_distribute(DATA_EVT_CONFIG_INIT);
	}

	if (IS_EVENT(msg, util, UTIL_EVT_SHUTDOWN_REQUEST)) {
		/* The module doesn't have anything to shut down and can
		 * report back immediately.
		 */
		SEND_EVENT(data, DATA_EVT_SHUTDOWN_READY);
	}

	if (IS_EVENT(msg, app, APP_EVT_DATA_GET)) {
		/* Store which data is requested by the app, later to be used
		 * to confirm data is reported to the data manger.
		 */
		data_list_set(msg->module.app.data_list,
			      msg->module.app.count);

		/* Start countdown until data must have been received by the
		 * Data module in order to be sent to cloud
		 */
		k_delayed_work_submit(&data_send_work,
				      K_SECONDS(msg->module.app.timeout));

		return;
	}

	if (IS_EVENT(msg, ui, UI_EVT_BUTTON_DATA_READY)) {
		struct cloud_data_ui new_ui_data = {
			.btn = msg->module.ui.data.ui.btn,
			.btn_ts = msg->module.ui.data.ui.btn_ts,
			.queued = true
		};

		cloud_codec_populate_ui_buffer(ui_buf, &new_ui_data,
					       &head_ui_buf);

		SEND_EVENT(data, DATA_EVT_UI_DATA_READY);
		return;
	}

	if (IS_EVENT(msg, modem, MODEM_EVT_MODEM_DATA_READY)) {
		struct cloud_data_modem new_modem_data = {
			.appv = msg->module.modem.data.modem.appv,
			.area = msg->module.modem.data.modem.area,
			.bnd = msg->module.modem.data.modem.bnd,
			.brdv = msg->module.modem.data.modem.brdv,
			.cell = msg->module.modem.data.modem.cell,
			.fw = msg->module.modem.data.modem.fw,
			.iccid = msg->module.modem.data.modem.iccid,
			.ip = msg->module.modem.data.modem.ip,
			.mccmnc = msg->module.modem.data.modem.mccmnc,
			.mod_ts = msg->module.modem.data.modem.mod_ts,
			.mod_ts_static =
				msg->module.modem.data.modem.mod_ts_static,
			.nw_gps = msg->module.modem.data.modem.nw_gps,
			.nw_lte_m = msg->module.modem.data.modem.nw_lte_m,
			.nw_nb_iot = msg->module.modem.data.modem.nw_nb_iot,
			.rsrp = msg->module.modem.data.modem.rsrp,
			.queued = true
		};

		cloud_codec_populate_modem_buffer(modem_buf, &new_modem_data,
						  &head_modem_buf);

		data_status_set(APP_DATA_MODEM);
	}

	if (IS_EVENT(msg, modem, MODEM_EVT_BATTERY_DATA_READY)) {
		struct cloud_data_battery new_battery_data = {
			.bat = msg->module.modem.data.bat.bat,
			.bat_ts = msg->module.modem.data.bat.bat_ts,
			.queued = true
		};

		cloud_codec_populate_bat_buffer(bat_buf, &new_battery_data,
						&head_bat_buf);

		data_status_set(APP_DATA_BATTERY);
	}

	if (IS_EVENT(msg, sensor, SENSOR_EVT_ENVIRONMENTAL_DATA_READY)) {
		struct cloud_data_sensors new_sensor_data = {
			.temp = msg->module.sensor.data.sensors.temp,
			.hum = msg->module.sensor.data.sensors.hum,
			.env_ts = msg->module.sensor.data.sensors.env_ts,
			.queued = true
		};

		cloud_codec_populate_sensor_buffer(sensors_buf,
						   &new_sensor_data,
						   &head_sensor_buf);

		data_status_set(APP_DATA_ENVIRONMENTAL);
	}

	if (IS_EVENT(msg, sensor, SENSOR_EVT_ENVIRONMENTAL_NOT_SUPPORTED)) {
		data_status_set(APP_DATA_ENVIRONMENTAL);
	}

	if (IS_EVENT(msg, sensor, SENSOR_EVT_MOVEMENT_DATA_READY)) {
		struct cloud_data_accelerometer new_movement_data = {
			.values = {(bool)msg->module.sensor.data.accel.values},
			.ts = msg->module.sensor.data.accel.ts,
			.queued = true
		};

		cloud_codec_populate_accel_buffer(accel_buf, &new_movement_data,
						  &head_accel_buf);
	}

	if (IS_EVENT(msg, gps, GPS_EVT_DATA_READY)) {
		struct cloud_data_gps new_gps_data = {
			.acc = msg->module.gps.data.gps.acc,
			.alt = msg->module.gps.data.gps.alt,
			.hdg = msg->module.gps.data.gps.hdg,
			.lat = msg->module.gps.data.gps.lat,
			.longi = msg->module.gps.data.gps.longi,
			.spd = msg->module.gps.data.gps.spd,
			.gps_ts = msg->module.gps.data.gps.gps_ts,
			.queued = true
		};

		cloud_codec_populate_gps_buffer(gps_buf, &new_gps_data,
						&head_gps_buf);

		data_status_set(APP_DATA_GNSS);
	}

	if (IS_EVENT(msg, gps, GPS_EVT_TIMEOUT)) {
		data_status_set(APP_DATA_GNSS);
	}

	if (IS_EVENT(msg, cloud, CLOUD_EVT_DATA_ACK)) {
		pending_data_ack(msg->module.cloud.data.ptr);
		return;
	}
}

static void data_module(void)
{
	int err;
	struct data_msg_data msg;

	self.thread_id = k_current_get();

	module_start(&self);

	state_set(STATE_CLOUD_DISCONNECTED);

	err = setup();
	if (err) {
		LOG_ERR("setup, error: %d", err);
		SEND_ERROR(data, DATA_EVT_ERROR, err);
	}

	while (true) {
		module_get_next_msg(&self, &msg);

		switch (state) {
		case STATE_CLOUD_DISCONNECTED:
			on_cloud_state_disconnected(&msg);
			break;
		case STATE_CLOUD_CONNECTED:
			on_cloud_state_connected(&msg);
			break;
		default:
			LOG_WRN("Unknown sub state.");
			break;
		}

		on_all_states(&msg);
	}
}

K_THREAD_DEFINE(data_module_thread, CONFIG_DATA_THREAD_STACK_SIZE,
		data_module, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);

EVENT_LISTENER(MODULE, event_handler);
EVENT_SUBSCRIBE(MODULE, app_module_event);
EVENT_SUBSCRIBE(MODULE, util_module_event);
EVENT_SUBSCRIBE(MODULE, data_module_event);
EVENT_SUBSCRIBE_EARLY(MODULE, modem_module_event);
EVENT_SUBSCRIBE_EARLY(MODULE, cloud_module_event);
EVENT_SUBSCRIBE_EARLY(MODULE, gps_module_event);
EVENT_SUBSCRIBE_EARLY(MODULE, ui_module_event);
EVENT_SUBSCRIBE_EARLY(MODULE, sensor_module_event);
