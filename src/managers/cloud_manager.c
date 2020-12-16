/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#include <zephyr.h>
#include <net/socket.h>
#include <stdio.h>
#include <dfu/mcuboot.h>
#include <math.h>
#include <event_manager.h>

#include "cloud_wrapper.h"
#include "cloud/cloud_codec/cloud_codec.h"

#define MODULE cloud_manager

#include "modules_common.h"
#include "events/cloud_mgr_event.h"
#include "events/app_mgr_event.h"
#include "events/data_mgr_event.h"
#include "events/util_mgr_event.h"
#include "events/modem_mgr_event.h"
#include "events/gps_mgr_event.h"

#include <logging/log.h>
LOG_MODULE_REGISTER(MODULE, CONFIG_CAT_TRACKER_LOG_LEVEL);

extern atomic_t manager_count;

BUILD_ASSERT(CONFIG_CLOUD_CONNECT_RETRIES < 14,
	    "Cloud connect retries too large");

struct cloud_msg_data {
	union {
		struct app_mgr_event app;
		struct data_mgr_event data;
		struct modem_mgr_event modem;
		struct cloud_mgr_event cloud;
		struct util_mgr_event util;
		struct gps_mgr_event gps;
	} manager;
};

static struct k_delayed_work connect_check_work;

struct cloud_backoff_delay_lookup {
	int delay;
};

/* Cloud manager super-states. */
static enum cloud_manager_state_type {
	CLOUD_MGR_STATE_LTE_DISCONNECTED,
	CLOUD_MGR_STATE_LTE_CONNECTED
} cloud_state;

static enum cloud_manager_sub_state_type {
	CLOUD_MGR_SUB_STATE_CLOUD_DISCONNECTED,
	CLOUD_MGR_SUB_STATE_CLOUD_CONNECTED
} cloud_sub_state;

/* Lookup table for backoff reconnection to cloud. Binary scaling. */
static struct cloud_backoff_delay_lookup backoff_delay[] = {
	{ 32 }, { 64 }, { 128 }, { 256 }, { 512 },
	{ 2048 }, { 4096 }, { 8192 }, { 16384 }, { 32768 },
	{ 65536 }, { 131072 }, { 262144 }, { 524288 }, { 1048576 }
};

/* Variable that keeps track of how many times a reconnection to cloud
 * has been tried without success.
 */
static int connect_retries;

/* Local copy of the device configuration. */
static struct cloud_data_cfg copy_cfg;
const k_tid_t cloud_manager_thread;

K_MSGQ_DEFINE(msgq_cloud, sizeof(struct cloud_msg_data), 10, 4);

static struct module_data self = {
	.msg_q = &msgq_cloud,
};

static void connect_check_work_fn(struct k_work *work);

static void state_set(enum cloud_manager_state_type new_state)
{
	cloud_state = new_state;
}

static void sub_state_set(enum cloud_manager_sub_state_type new_state)
{
	cloud_sub_state = new_state;
}

static void signal_error(int err)
{
	struct cloud_mgr_event *cloud_mgr_event = new_cloud_mgr_event();

	cloud_mgr_event->type = CLOUD_MGR_EVT_ERROR;
	cloud_mgr_event->data.err = err;

	EVENT_SUBMIT(cloud_mgr_event);
}

static void signal_data_ack(void *ptr)
{
	struct cloud_mgr_event *cloud_mgr_event = new_cloud_mgr_event();

	cloud_mgr_event->type = CLOUD_MGR_EVT_DATA_ACK;
	cloud_mgr_event->data.ptr = ptr;

	EVENT_SUBMIT(cloud_mgr_event);
}

static void cloud_manager_data_send(struct data_mgr_event *evt)
{
	int err;

	err = cloud_wrap_data_send(evt->data.buffer.buf, evt->data.buffer.len);
	if (err) {
		LOG_ERR("cloud_wrap_data_send, err: %d", err);
	} else {
		LOG_DBG("Data sent");
	}

	if (evt->data.buffer.len > 0) {
		signal_data_ack(evt->data.buffer.buf);
	}
}

static void cloud_manager_config_send(struct data_mgr_event *evt)
{
	int err;

	err = cloud_wrap_state_send(evt->data.buffer.buf, evt->data.buffer.len);
	if (err) {
		LOG_ERR("cloud_wrap_state_send, err: %d", err);
	} else {
		LOG_DBG("Data sent");
	}

	if (evt->data.buffer.len > 0) {
		signal_data_ack(evt->data.buffer.buf);
	}
}

static void cloud_manager_config_get(void)
{
	int err;

	err = cloud_wrap_state_get();
	if (err) {
		LOG_ERR("cloud_wrap_state_get, err: %d", err);
	} else {
		LOG_DBG("Device configuration requested");
	}
}

static void cloud_manager_batch_data_send(struct data_mgr_event *evt)
{
	int err;

	err = cloud_wrap_batch_send(evt->data.buffer.buf, evt->data.buffer.len);
	if (err) {
		LOG_ERR("cloud_wrap_batch_send, err: %d", err);
	} else {
		LOG_DBG("Batch sent");
	}

	if (evt->data.buffer.len > 0) {
		signal_data_ack(evt->data.buffer.buf);
	}
}

static void cloud_manager_ui_data_send(struct data_mgr_event *evt)
{
	int err;

	err = cloud_wrap_ui_send(evt->data.buffer.buf, evt->data.buffer.len);
	if (err) {
		LOG_ERR("cloud_wrap_ui_send, err: %d", err);
	} else {
		LOG_DBG("UI sent");
	}

	if (evt->data.buffer.len > 0) {
		signal_data_ack(evt->data.buffer.buf);
	}
}

static void connect_cloud(void)
{
	int err;
	int backoff_sec = backoff_delay[connect_retries].delay;

	LOG_DBG("Connecting to cloud");

	if (connect_retries > CONFIG_CLOUD_CONNECT_RETRIES) {
		LOG_WRN("Too many failed cloud connection attempts");
		signal_error(-ENETUNREACH);
		return;
	}

	/* The cloud will return error if cloud_wrap_connect() is called while
	 * the socket is polled on in the internal cloud thread or the
	 * cloud backend is the wrong state. We cannot treat this as an error as
	 * it is rather common that cloud_connect can be called under these
	 * conditions.
	 */
	err = cloud_wrap_connect();
	if (err) {
		LOG_ERR("cloud_connect failed, error: %d", err);
	}

	connect_retries++;

	LOG_WRN("New connection attempt in %d seconds", backoff_sec);

	/* Start timer to check connection status after backoff */
	k_delayed_work_submit(&connect_check_work, K_SECONDS(backoff_sec));
}

/* If this work is executed, it means that the connection attempt was not
 * successful before the backoff timer expired. A timeout message is then
 * added to the message queue to signal the timeout.
 */
static void connect_check_work_fn(struct k_work *work)
{
	struct cloud_mgr_event *cloud_mgr_event = new_cloud_mgr_event();

	cloud_mgr_event->type = CLOUD_MGR_EVT_CONNECTION_TIMEOUT;

	LOG_DBG("Cloud connection timeout occurred");

	EVENT_SUBMIT(cloud_mgr_event);
}

static bool event_handler(const struct event_header *eh)
{
	if (is_app_mgr_event(eh)) {
		struct app_mgr_event *event = cast_app_mgr_event(eh);
		struct cloud_msg_data msg = {
			.manager.app = *event
		};

		if (IS_EVENT((&msg), app, APP_MGR_EVT_START)) {
			k_thread_start(cloud_manager_thread);
		}

		module_enqueue_msg(&self, &msg);
	}

	if (is_data_mgr_event(eh)) {
		struct data_mgr_event *event = cast_data_mgr_event(eh);
		struct cloud_msg_data msg = {
			.manager.data = *event
		};

		module_enqueue_msg(&self, &msg);
	}

	if (is_modem_mgr_event(eh)) {
		struct modem_mgr_event *event = cast_modem_mgr_event(eh);
		struct cloud_msg_data msg = {
			.manager.modem = *event
		};

		module_enqueue_msg(&self, &msg);
	}

	if (is_cloud_mgr_event(eh)) {
		struct cloud_mgr_event *event = cast_cloud_mgr_event(eh);
		struct cloud_msg_data msg = {
			.manager.cloud = *event
		};

		module_enqueue_msg(&self, &msg);
	}

	if (is_util_mgr_event(eh)) {
		struct util_mgr_event *event = cast_util_mgr_event(eh);
		struct cloud_msg_data msg = {
			.manager.util = *event
		};

		module_enqueue_msg(&self, &msg);
	}

	if (is_gps_mgr_event(eh)) {
		struct gps_mgr_event *event = cast_gps_mgr_event(eh);
		struct cloud_msg_data msg = {
			.manager.gps = *event
		};

		module_enqueue_msg(&self, &msg);
	}

	return false;
}

static void on_state_lte_connected(struct cloud_msg_data *cloud_msg)
{
	if (IS_EVENT(cloud_msg, modem, MODEM_MGR_EVT_LTE_DISCONNECTED)) {
		state_set(CLOUD_MGR_STATE_LTE_DISCONNECTED);
		sub_state_set(CLOUD_MGR_SUB_STATE_CLOUD_DISCONNECTED);

		connect_retries = 0;

		k_delayed_work_cancel(&connect_check_work);

		return;
	}

#if defined(CONFIG_AGPS) && defined(CONFIG_AGPS_SRC_SUPL)
	if (IS_EVENT(cloud_msg, gps, GPS_MGR_EVT_AGPS_NEEDED)) {
		int err;

		err = gps_agps_request(cloud_msg->manager.gps.data.agps_request,
				       GPS_SOCKET_NOT_PROVIDED);
		if (err) {
			LOG_WRN("Failed to request A-GPS data, error: %d", err);
		}
	}
#endif /* CONFIG_AGPS && CONFIG_AGPS_SRC_SUPL*/
}

static void on_state_lte_disconnected(struct cloud_msg_data *msg)
{
	if (IS_EVENT(msg, modem, MODEM_MGR_EVT_LTE_CONNECTED)) {
		state_set(CLOUD_MGR_STATE_LTE_CONNECTED);

		/* LTE is now connected, cloud connection can be attempted */
		connect_cloud();
	}
}

static void on_sub_state_cloud_connected(struct cloud_msg_data *msg)
{
	if (IS_EVENT(msg, cloud, CLOUD_MGR_EVT_DISCONNECTED)) {
		sub_state_set(CLOUD_MGR_SUB_STATE_CLOUD_DISCONNECTED);

		k_delayed_work_submit(&connect_check_work, K_NO_WAIT);

		return;
	}

#if defined(CONFIG_AGPS) && defined(CONFIG_AGPS_SRC_NRF_CLOUD)
	if (IS_EVENT(msg, gps, GPS_MGR_EVT_AGPS_NEEDED)) {
		int err;

		err = gps_agps_request(msg->manager.gps.data.agps_request,
				       GPS_SOCKET_NOT_PROVIDED);
		if (err) {
			LOG_WRN("Failed to request A-GPS data, error: %d", err);
		}

		return;
	}
#endif /* CONFIG_AGPS && CONFIG_AGPS_SRC_NRF_CLOUD */

	if (IS_EVENT(msg, data, DATA_MGR_EVT_DATA_SEND)) {
		cloud_manager_data_send(&msg->manager.data);
	}

	if (IS_EVENT(msg, data, DATA_MGR_EVT_CONFIG_SEND)) {
		cloud_manager_config_send(&msg->manager.data);
	}

	if (IS_EVENT(msg, data, DATA_MGR_EVT_CONFIG_GET)) {
		cloud_manager_config_get();
	}

	if (IS_EVENT(msg, data, DATA_MGR_EVT_DATA_SEND_BATCH)) {
		cloud_manager_batch_data_send(&msg->manager.data);
	}

	if (IS_EVENT(msg, data, DATA_MGR_EVT_UI_DATA_SEND)) {
		cloud_manager_ui_data_send(&msg->manager.data);
	}
}

static void on_sub_state_cloud_disconnected(struct cloud_msg_data *msg)
{
	if (IS_EVENT(msg, cloud, CLOUD_MGR_EVT_CONNECTED)) {
		sub_state_set(CLOUD_MGR_SUB_STATE_CLOUD_CONNECTED);

		connect_retries = 0;
		k_delayed_work_cancel(&connect_check_work);
	}

	if (IS_EVENT(msg, cloud, CLOUD_MGR_EVT_CONNECTION_TIMEOUT)) {
		connect_cloud();
	}
}

static void on_all_states(struct cloud_msg_data *msg)
{
	if (IS_EVENT(msg, util, UTIL_MGR_EVT_SHUTDOWN_REQUEST)) {

		struct cloud_mgr_event *cloud_mgr_event = new_cloud_mgr_event();

		cloud_mgr_event->type = CLOUD_MGR_EVT_SHUTDOWN_READY;
		EVENT_SUBMIT(cloud_mgr_event);
	}

	if (is_data_mgr_event(&msg->manager.data.header)) {
		switch (msg->manager.data.type) {
		case DATA_MGR_EVT_CONFIG_INIT: /* Fall through. */
		case DATA_MGR_EVT_CONFIG_READY:
			copy_cfg = msg->manager.data.data.cfg;
			break;
		default:
			break;
		}
	}
}

static void cloud_wrap_event_handler(const struct cloud_wrap_event *const evt)
{
	struct cloud_mgr_event *cloud_mgr_event = new_cloud_mgr_event();

	switch (evt->type) {
	case CLOUD_WRAP_EVT_CONNECTING:
		LOG_DBG("CLOUD_WRAP_EVT_CONNECTING");
		cloud_mgr_event->type = CLOUD_MGR_EVT_CONNECTING;
		EVENT_SUBMIT(cloud_mgr_event);
		break;
	case CLOUD_WRAP_EVT_CONNECTED:
		LOG_DBG("CLOUD_WRAP_EVT_CONNECTED");
		cloud_mgr_event->type = CLOUD_MGR_EVT_CONNECTED;
		EVENT_SUBMIT(cloud_mgr_event);
		break;
	case CLOUD_WRAP_EVT_DISCONNECTED:
		LOG_DBG("CLOUD_WRAP_EVT_DISCONNECTED");
		cloud_mgr_event->type = CLOUD_MGR_EVT_DISCONNECTED;
		EVENT_SUBMIT(cloud_mgr_event);
		break;
	case CLOUD_WRAP_EVT_DATA_RECEIVED:
		LOG_DBG("CLOUD_WRAP_EVT_DATA_RECEIVED");

		int err;

		/* Use the config copy when populating the config variable
		 * before it is sent to the data manager. This way we avoid
		 * sending uninitialized variables to the data manager.
		 */

		// A bit unclear to me here. Where is generic data, meaning
		// non-config data received?

		err = cloud_codec_decode_config(evt->data.buf, &copy_cfg);
		if (err == 0) {
			LOG_DBG("Device configuration encoded");

			cloud_mgr_event->type = CLOUD_MGR_EVT_CONFIG_RECEIVED;
			cloud_mgr_event->data.config = copy_cfg;

			EVENT_SUBMIT(cloud_mgr_event);
			break;
		} else if (err == -ENODATA) {
			LOG_WRN("Device configuration empty!");
		} else {
			LOG_ERR("Decoding of device configuration, error: %d",
				err);
			signal_error(err);
			break;
		}

		// Perhaps DBG level logging here

#if defined(CONFIG_AGPS)
		err = gps_process_agps_data(evt->data.buf, evt->data.len);
		if (err) {
			// It might be that it wasn't A-GPS data
			LOG_WRN("Unable to process agps data, error: %d", err);
		}
#endif
		break;
	case CLOUD_WRAP_EVT_FOTA_DONE:
		LOG_DBG("CLOUD_WRAP_EVT_FOTA_DONE");
		cloud_mgr_event->type = CLOUD_MGR_EVT_FOTA_DONE;
		EVENT_SUBMIT(cloud_mgr_event);
		break;
	case CLOUD_WRAP_EVT_FOTA_START:
		LOG_DBG("CLOUD_WRAP_EVT_FOTA_START");
		break;
	case CLOUD_WRAP_EVT_FOTA_ERASE_PENDING:
		LOG_DBG("CLOUD_WRAP_EVT_FOTA_ERASE_PENDING");
		break;
	case CLOUD_WRAP_EVT_FOTA_ERASE_DONE:
		LOG_DBG("CLOUD_WRAP_EVT_FOTA_ERASE_DONE");
		break;
	case CLOUD_WRAP_EVT_ERROR:
		LOG_DBG("CLOUD_WRAP_EVT_ERROR");
		cloud_mgr_event->type = CLOUD_MGR_EVT_ERROR;
		EVENT_SUBMIT(cloud_mgr_event);
		break;
	default:
		break;

	}
}

static int cloud_manager_setup(void)
{
	int err;

	err = cloud_wrap_init(cloud_wrap_event_handler);
	if (err) {
		LOG_ERR("cloud_wrap_init, error: %d", err);
		return err;
	}

	/* After a successful initializaton, tell the bootloader that the
	 * current image is confirmed to be working.
	 */
	boot_write_img_confirmed();

	return 0;
}

static void cloud_manager(void)
{
	int err;
	struct cloud_msg_data cloud_msg;

	self.thread_id = k_current_get();

	atomic_inc(&manager_count);
	state_set(CLOUD_MGR_STATE_LTE_DISCONNECTED);
	sub_state_set(CLOUD_MGR_SUB_STATE_CLOUD_DISCONNECTED);

	k_delayed_work_init(&connect_check_work, connect_check_work_fn);

	err = cloud_manager_setup();
	if (err) {
		LOG_ERR("cloud_manager_setup, error %d", err);
		signal_error(err);
	}

	while (true) {
		module_get_next_msg(&self, &cloud_msg);

		switch (cloud_state) {
		case CLOUD_MGR_STATE_LTE_CONNECTED:
			switch (cloud_sub_state) {
			case CLOUD_MGR_SUB_STATE_CLOUD_CONNECTED:
				on_sub_state_cloud_connected(&cloud_msg);
				break;
			case CLOUD_MGR_SUB_STATE_CLOUD_DISCONNECTED:
				on_sub_state_cloud_disconnected(&cloud_msg);
				break;
			default:
				LOG_ERR("Unknown cloud manager sub state");
				break;
			}

			on_state_lte_connected(&cloud_msg);
			break;
		case CLOUD_MGR_STATE_LTE_DISCONNECTED:
			on_state_lte_disconnected(&cloud_msg);
			break;
		default:
			LOG_ERR("Unknown cloud manager state.");
			break;
		}

		on_all_states(&cloud_msg);
	}
}

K_THREAD_DEFINE(cloud_manager_thread, CONFIG_CLOUD_MGR_THREAD_STACK_SIZE,
		cloud_manager, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);

EVENT_LISTENER(MODULE, event_handler);
EVENT_SUBSCRIBE(MODULE, data_mgr_event);
EVENT_SUBSCRIBE(MODULE, app_mgr_event);
EVENT_SUBSCRIBE(MODULE, modem_mgr_event);
EVENT_SUBSCRIBE(MODULE, cloud_mgr_event);
EVENT_SUBSCRIBE(MODULE, gps_mgr_event);
EVENT_SUBSCRIBE_EARLY(MODULE, util_mgr_event);
