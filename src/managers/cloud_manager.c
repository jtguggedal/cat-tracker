/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#include <zephyr.h>
#include <net/socket.h>
#include <net/cloud.h>
#include <stdio.h>
#include <dfu/mcuboot.h>
#include <modem/at_cmd.h>
#include <math.h>
#include <event_manager.h>

#include "cloud_codec.h"

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

/* Application specific AWS topics. */
#if !defined(CONFIG_USE_CUSTOM_MQTT_CLIENT_ID)
#define AWS_CLOUD_CLIENT_ID_LEN 15
#else
#define AWS_CLOUD_CLIENT_ID_LEN (sizeof(CONFIG_MQTT_CLIENT_ID) - 1)
#endif
#define AWS "$aws/things/"
#define AWS_LEN (sizeof(AWS) - 1)
#define CFG_TOPIC AWS "%s/shadow/get/accepted/desired/cfg"
#define CFG_TOPIC_LEN (AWS_LEN + AWS_CLOUD_CLIENT_ID_LEN + 32)
#define BATCH_TOPIC "%s/batch"
#define BATCH_TOPIC_LEN (AWS_CLOUD_CLIENT_ID_LEN + 6)
#define MESSAGES_TOPIC "%s/messages"
#define MESSAGES_TOPIC_LEN (AWS_CLOUD_CLIENT_ID_LEN + 9)

enum app_endpoint_type { CLOUD_EP_MESSAGES = CLOUD_EP_PRIV_START };

static struct cloud_endpoint sub_ep_topics_sub[1];
static struct cloud_endpoint pub_ep_topics_sub[2];

static char client_id_buf[AWS_CLOUD_CLIENT_ID_LEN + 1];
static char batch_topic[BATCH_TOPIC_LEN + 1];
static char cfg_topic[CFG_TOPIC_LEN + 1];
static char messages_topic[MESSAGES_TOPIC_LEN + 1];

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

static struct cloud_backend *cloud_backend;

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
	struct cloud_msg msg = {
		.buf = evt->data.buffer.buf,
		.len = evt->data.buffer.len,
		.qos = CLOUD_QOS_AT_MOST_ONCE,
		.endpoint.type = CLOUD_EP_STATE,
	};

	err = cloud_send(cloud_backend, &msg);
	if (err) {
		LOG_ERR("Cloud send failed, err: %d", err);
	} else {
		LOG_DBG("Data sent");
	}

	if (msg.len > 0) {
		signal_data_ack(msg.buf);
	}
}

static void cloud_manager_batch_data_send(struct data_mgr_event *evt)
{
	int err;

	/* Publish batched data in one chunk to the batch endpoint. */
	struct cloud_msg msg = {
		.buf = evt->data.buffer.buf,
		.len = evt->data.buffer.len,
		.qos = CLOUD_QOS_AT_MOST_ONCE,
		.endpoint = pub_ep_topics_sub[0],
		/* Custom endpoint, type not needed. */
	};

	err = cloud_send(cloud_backend, &msg);
	if (err) {
		LOG_ERR("Cloud send failed, err: %d", err);
	} else {
		LOG_DBG("Batch data sent");
	}

	signal_data_ack(msg.buf);
}

static void cloud_manager_ui_data_send(struct data_mgr_event *evt)
{
	int err;
	struct cloud_msg msg = {
		.buf = evt->data.buffer.buf,
		.len = evt->data.buffer.len,
		.qos = CLOUD_QOS_AT_MOST_ONCE,
		.endpoint = pub_ep_topics_sub[1],
		/* Custom endpoint, type not needed. */
	};

	err = cloud_send(cloud_backend, &msg);
	if (err) {
		LOG_ERR("Cloud send failed, err: %d", err);
	}

	signal_data_ack(msg.buf);
}

static void cloud_event_handler(const struct cloud_backend *const backend,
				const struct cloud_event *const evt, void *user_data)
{
	ARG_UNUSED(user_data);

	struct cloud_mgr_event *cloud_mgr_event = new_cloud_mgr_event();

	switch (evt->type) {
	case CLOUD_EVT_CONNECTING:
		LOG_DBG("CLOUD_EVT_CONNECTING");

		cloud_mgr_event->type = CLOUD_MGR_EVT_CONNECTING;
		EVENT_SUBMIT(cloud_mgr_event);
		break;
	case CLOUD_EVT_CONNECTED:
		LOG_DBG("CLOUD_EVT_CONNECTED");

		/* Mark image as valid upon a successful connection. */
		boot_write_img_confirmed();
		break;
	case CLOUD_EVT_READY:
		LOG_DBG("CLOUD_EVT_READY");

		/* Make sure that CLOUD_MGR_EVT_CONNECTED is not sent for
		 * every received CLOUD_EVT_READY from cloud backend.
		 * The current implementation of AWS IoT propogates a
		 * CLOUD_EVT_READY event for every MQTT SUBACK. The current
		 * implementation is bad practice and should be fixed
		 */
		cloud_mgr_event->type = CLOUD_MGR_EVT_CONNECTED;
		EVENT_SUBMIT(cloud_mgr_event);
		break;
	case CLOUD_EVT_DISCONNECTED:
		LOG_WRN("CLOUD_EVT_DISCONNECTED");

		cloud_mgr_event->type = CLOUD_MGR_EVT_DISCONNECTED;
		EVENT_SUBMIT(cloud_mgr_event);
		break;
	case CLOUD_EVT_ERROR:
		LOG_ERR("CLOUD_EVT_ERROR");
		break;
	case CLOUD_EVT_FOTA_START:
		LOG_DBG("CLOUD_EVT_FOTA_START");
		break;
	case CLOUD_EVT_FOTA_ERASE_PENDING:
		LOG_DBG("CLOUD_EVT_FOTA_ERASE_PENDING");
		break;
	case CLOUD_EVT_FOTA_ERASE_DONE:
		LOG_DBG("CLOUD_EVT_FOTA_ERASE_DONE");
		break;
	case CLOUD_EVT_FOTA_DONE:
		LOG_DBG("CLOUD_EVT_FOTA_DONE");
		cloud_mgr_event->type = CLOUD_MGR_EVT_FOTA_DONE;
		EVENT_SUBMIT(cloud_mgr_event);
		break;
	case CLOUD_EVT_DATA_SENT:
		LOG_DBG("CLOUD_EVT_DATA_SENT");
		break;
	case CLOUD_EVT_DATA_RECEIVED:
		LOG_DBG("CLOUD_EVT_DATA_RECEIVED");

		int err;

		/* Use the config copy when populating the config variable
		 * before it is sent to the data manager. This way we avoid
		 * sending uninitialized variables to the data manager.
		 */
		// A bit unclear to me here. Where is generic data, meaning
		// non-config data received?

		err = cloud_codec_decode_response(evt->data.msg.buf, &copy_cfg);
		if (err == 0) {
			LOG_DBG("Device configuration encoded");

			cloud_mgr_event->type = CLOUD_MGR_EVT_CONFIG_RECEIVED;
			cloud_mgr_event->data.config = copy_cfg;

			EVENT_SUBMIT(cloud_mgr_event);
			break;
		}

		// Perhaps DBG level logging here

#if defined(CONFIG_AGPS)
		err = gps_process_agps_data(evt->data.msg.buf,
						evt->data.msg.len);
		if (err) {
			// It might be that it wasn't A-GPS data
			LOG_WRN("Unable to process agps data, error: %d", err);
		}
#endif

		break;
	case CLOUD_EVT_PAIR_REQUEST:
		LOG_DBG("CLOUD_EVT_PAIR_REQUEST");
		break;
	case CLOUD_EVT_PAIR_DONE:
		LOG_DBG("CLOUD_EVT_PAIR_DONE");
		break;
	case CLOUD_EVT_FOTA_DL_PROGRESS:
		/* Do not print to avoid spamming. */
		break;
	default:
		LOG_ERR("Unknown cloud event type: %d", evt->type);
		break;
	}
}

static int populate_app_endpoint_topics(void)
{
	int err;
	// Perhaps we should keep these topic buffers on the stack if they're
	// only used once
	err = snprintf(batch_topic, sizeof(batch_topic), BATCH_TOPIC,
		       client_id_buf);
	if (err != BATCH_TOPIC_LEN) {
		return -ENOMEM;
	}

	pub_ep_topics_sub[0].str = batch_topic;
	pub_ep_topics_sub[0].len = BATCH_TOPIC_LEN;
	pub_ep_topics_sub[0].type = 1;

	err = snprintf(messages_topic, sizeof(messages_topic), MESSAGES_TOPIC,
		       client_id_buf);
	if (err != MESSAGES_TOPIC_LEN) {
		return -ENOMEM;
	}

	pub_ep_topics_sub[1].str = messages_topic;
	pub_ep_topics_sub[1].len = MESSAGES_TOPIC_LEN;
	pub_ep_topics_sub[1].type = CLOUD_EP_MESSAGES;

	err = snprintf(cfg_topic, sizeof(cfg_topic), CFG_TOPIC, client_id_buf);
	if (err != CFG_TOPIC_LEN) {
		return -ENOMEM;
	}

	sub_ep_topics_sub[0].str = cfg_topic;
	sub_ep_topics_sub[0].len = CFG_TOPIC_LEN;
	sub_ep_topics_sub[0].type = 1;

	err = cloud_ep_subscriptions_add(cloud_backend, sub_ep_topics_sub,
					 ARRAY_SIZE(sub_ep_topics_sub));
	if (err) {
		LOG_ERR("cloud_ep_subscriptions_add, error: %d", err);
		return err;
	}

	return 0;
}

static int cloud_manager_setup(void)
{
	int err;

	cloud_backend = cloud_get_binding(CONFIG_CLOUD_BACKEND);
	__ASSERT(cloud_backend != NULL, "%s cloud backend not found",
		 CONFIG_CLOUD_BACKEND);


	k_delayed_work_init(&connect_check_work, connect_check_work_fn);

	// Perhaps switch it around and hav "CONFIG_CLIENT_ID_USE_IMEI"
#if !defined(CONFIG_USE_CUSTOM_MQTT_CLIENT_ID)
	char imei_buf[50];

	/* Retrieve device IMEI from modem. */
	err = at_cmd_write("AT+CGSN", imei_buf, sizeof(imei_buf), NULL);
	if (err) {
		LOG_ERR("Not able to retrieve device IMEI from modem");
		return err;
	}

	/* Set null character at the end of the device IMEI. */
	// Should not be needed
	imei_buf[AWS_CLOUD_CLIENT_ID_LEN] = 0;

	strncpy(client_id_buf, imei_buf, sizeof(client_id_buf));

#else
	snprintf(client_id_buf, sizeof(client_id_buf), "%s",
		 CONFIG_MQTT_CLIENT_ID);
#endif

	/* Fetch IMEI from modem data and set IMEI as cloud connection ID **/
	cloud_backend->config->id = client_id_buf;
	cloud_backend->config->id_len = strlen(client_id_buf);

	err = cloud_init(cloud_backend, cloud_event_handler);
	if (err) {
		LOG_ERR("cloud_init, error: %d", err);
		return err;
	}

	/* Populate cloud specific endpoint topics */
	err = populate_app_endpoint_topics();
	if (err) {
		LOG_ERR("populate_app_endpoint_topics, error: %d", err);
		return err;
	}

	return 0;
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

	err = cloud_connect(cloud_backend);
	if (err) {
		LOG_ERR("cloud_connect failed, error: %d", err);
		signal_error(err);
	} else {
		connect_retries++;
	}

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

	LOG_DBG("Cloud connection timeout occured");

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

#if defined(CONFIG_AGPS)
	if (IS_EVENT(msg, gps, GPS_MGR_EVT_AGPS_NEEDED)) {
		int err;

		err = gps_agps_request(msg->manager.gps.data.agps_request,
				       GPS_SOCKET_NOT_PROVIDED);
		if (err) {
			LOG_WRN("Failed to request A-GPS data, error: %d", err);
		}

		return;
	}
#endif /* CONFIG_AGPS */

	if (IS_EVENT(msg, data, DATA_MGR_EVT_DATA_SEND)) {
		cloud_manager_data_send(&msg->manager.data);
	}

	if (IS_EVENT(msg, data, DATA_MGR_EVT_STATE_GET)) {
		cloud_manager_data_send(&msg->manager.data);
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

static void cloud_manager(void)
{
	int err;
	struct cloud_msg_data cloud_msg;

	self.thread_id = k_current_get();

	atomic_inc(&manager_count);
	state_set(CLOUD_MGR_STATE_LTE_DISCONNECTED);
	sub_state_set(CLOUD_MGR_SUB_STATE_CLOUD_DISCONNECTED);

	err = cloud_manager_setup();
	if (err) {
		LOG_ERR("cloud_manager_setup, error %d", err);
		signal_error(err);
	}

	LOG_INF("********************************************");
	LOG_INF(" The cat tracker has started");
	LOG_INF(" Version:     %s", log_strdup(CONFIG_CAT_TRACKER_APP_VERSION));
	LOG_INF(" Client ID:   %s", log_strdup(client_id_buf));
	LOG_INF(" Endpoint:    %s",
		log_strdup(CONFIG_AWS_IOT_BROKER_HOST_NAME));
	LOG_INF("********************************************");

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
