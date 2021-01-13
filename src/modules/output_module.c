/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#include <zephyr.h>
#include <stdio.h>
#include <event_manager.h>

#include "ui.h"

#define MODULE output_module

#include "modules_common.h"
#include "events/app_module_event.h"
#include "events/data_module_event.h"
#include "events/output_module_event.h"
#include "events/sensor_module_event.h"
#include "events/util_module_event.h"
#include "events/gps_module_event.h"
#include "events/modem_module_event.h"

#include <logging/log.h>
LOG_MODULE_REGISTER(MODULE, CONFIG_OUTPUT_MODULE_LOG_LEVEL);

struct output_msg_data {
	union {
		struct app_module_event app;
		struct modem_module_event modem;
		struct data_module_event data;
		struct gps_module_event gps;
		struct util_module_event util;
	} module;
};

/* Output module states. */
static enum state_type {
	STATE_ACTIVE,
	STATE_PASSIVE,
	STATE_ERROR
} state;

/* Output module sub states. */
static enum sub_state_type {
	SUB_STATE_GPS_INACTIVE,
	SUB_STATE_GPS_ACTIVE
} sub_state;

/* Forward declarations */
static void led_pat_active_work_fn(struct k_work *work);
static void led_pat_passive_work_fn(struct k_work *work);
static void led_pat_gps_work_fn(struct k_work *work);

/* Delayed works that is used to make sure the device always reverts back to the
 * device mode or GPS search LED pattern.
 */
static struct k_delayed_work led_pat_active_work = {
	.work = Z_WORK_INITIALIZER(led_pat_active_work_fn)
};

static struct k_delayed_work led_pat_passive_work = {
	.work = Z_WORK_INITIALIZER(led_pat_passive_work_fn)
};

static struct k_delayed_work led_pat_gps_work = {
	.work = Z_WORK_INITIALIZER(led_pat_gps_work_fn)
};

/* Output module message queue. */
#define OUTPUT_QUEUE_ENTRY_COUNT	10
#define OUTPUT_QUEUE_BYTE_ALIGNMENT	4

K_MSGQ_DEFINE(msgq_output, sizeof(struct output_msg_data),
	      OUTPUT_QUEUE_ENTRY_COUNT, OUTPUT_QUEUE_BYTE_ALIGNMENT);

static struct module_data self = {
	.name = "output",
	.msg_q = NULL,
};

/* Forward declarations. */
static void message_handler(struct output_msg_data *msg);

/* Convenience functions used in internal state handling. */
static char *state2str(enum state_type new_state)
{
	switch (new_state) {
	case STATE_ACTIVE:
		return "STATE_ACTIVE";
	case STATE_PASSIVE:
		return "STATE_PASSIVE";
	case STATE_ERROR:
		return "STATE_ERROR";
	default:
		return "Unknown";
	}
}

static char *sub_state2str(enum sub_state_type new_state)
{
	switch (new_state) {
	case SUB_STATE_GPS_INACTIVE:
		return "SUB_STATE_GPS_INACTIVE";
	case SUB_STATE_GPS_ACTIVE:
		return "SUB_STATE_GPS_ACTIVE";
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

static void sub_state_set(enum sub_state_type new_state)
{
	if (new_state == sub_state) {
		LOG_DBG("Sub state: %s", log_strdup(sub_state2str(sub_state)));
		return;
	}

	LOG_DBG("Sub state transition %s --> %s",
		log_strdup(sub_state2str(sub_state)),
		log_strdup(sub_state2str(new_state)));

	sub_state = new_state;
}

/* Handlers */
static bool event_handler(const struct event_header *eh)
{
	if (is_app_module_event(eh)) {
		struct app_module_event *event = cast_app_module_event(eh);
		struct output_msg_data msg = {
			.module.app = *event
		};

		message_handler(&msg);
	}

	if (is_data_module_event(eh)) {
		struct data_module_event *event = cast_data_module_event(eh);
		struct output_msg_data output_msg = {
			.module.data = *event
		};

		message_handler(&output_msg);
	}

	if (is_modem_module_event(eh)) {
		struct modem_module_event *event = cast_modem_module_event(eh);
		struct output_msg_data output_msg = {
			.module.modem = *event
		};

		message_handler(&output_msg);
	}

	if (is_gps_module_event(eh)) {
		struct gps_module_event *event = cast_gps_module_event(eh);
		struct output_msg_data output_msg = {
			.module.gps = *event
		};

		message_handler(&output_msg);
	}

	if (is_util_module_event(eh)) {
		struct util_module_event *event = cast_util_module_event(eh);
		struct output_msg_data output_msg = {
			.module.util = *event
		};

		message_handler(&output_msg);
	}

	return false;
}

/* Static module functions. */
static void led_pat_active_work_fn(struct k_work *work)
{
	ui_led_set_pattern(UI_LED_ACTIVE_MODE);
}

static void led_pat_passive_work_fn(struct k_work *work)
{
	ui_led_set_pattern(UI_LED_PASSIVE_MODE);
}

static void led_pat_gps_work_fn(struct k_work *work)
{
	ui_led_set_pattern(UI_LED_GPS_SEARCHING);
}

/* Message handler for SUB_STATE_GPS_ACTIVE in STATE_ACTIVE. */
static void on_state_active_sub_state_gps_active(struct output_msg_data *msg)
{
	if (IS_EVENT(msg, gps, GPS_EVT_INACTIVE)) {
		ui_led_set_pattern(UI_LED_ACTIVE_MODE);
		sub_state_set(SUB_STATE_GPS_INACTIVE);
	}

	if ((IS_EVENT(msg, data, DATA_EVT_DATA_SEND)) ||
	    (IS_EVENT(msg, data, DATA_EVT_UI_DATA_SEND))) {
		ui_led_set_pattern(UI_CLOUD_PUBLISHING);
		k_delayed_work_submit(&led_pat_gps_work, K_SECONDS(5));
	}

	if (IS_EVENT(msg, data, DATA_EVT_CONFIG_READY)) {
		if (!msg->module.data.data.cfg.act) {
			state_set(STATE_PASSIVE);
			k_delayed_work_submit(&led_pat_gps_work,
					      K_SECONDS(5));
		}
	}
}

/* Message handler for SUB_STATE_GPS_INACTIVE in STATE_ACTIVE. */
static void on_state_active_sub_state_gps_inactive(struct output_msg_data *msg)
{
	if (IS_EVENT(msg, gps, GPS_EVT_ACTIVE)) {
		ui_led_set_pattern(UI_LED_GPS_SEARCHING);
		sub_state_set(SUB_STATE_GPS_ACTIVE);
	}

	if ((IS_EVENT(msg, data, DATA_EVT_DATA_SEND)) ||
	    (IS_EVENT(msg, data, DATA_EVT_UI_DATA_SEND))) {
		ui_led_set_pattern(UI_CLOUD_PUBLISHING);
		k_delayed_work_submit(&led_pat_active_work, K_SECONDS(5));
	}

	if (IS_EVENT(msg, data, DATA_EVT_CONFIG_READY)) {
		if (!msg->module.data.data.cfg.act) {
			state_set(STATE_PASSIVE);
			k_delayed_work_submit(&led_pat_passive_work,
					      K_SECONDS(5));
		}
	}
}

/* Message handler for SUB_STATE_GPS_ACTIVE in STATE_PASSIVE. */
static void on_state_passive_sub_state_gps_active(struct output_msg_data *msg)
{
	if (IS_EVENT(msg, gps, GPS_EVT_INACTIVE)) {
		ui_led_set_pattern(UI_LED_PASSIVE_MODE);
		sub_state_set(SUB_STATE_GPS_INACTIVE);
	}

	if ((IS_EVENT(msg, data, DATA_EVT_DATA_SEND)) ||
	    (IS_EVENT(msg, data, DATA_EVT_UI_DATA_SEND))) {
		ui_led_set_pattern(UI_CLOUD_PUBLISHING);
		k_delayed_work_submit(&led_pat_gps_work, K_SECONDS(5));
	}

	if (IS_EVENT(msg, data, DATA_EVT_CONFIG_READY)) {
		if (msg->module.data.data.cfg.act) {
			state_set(STATE_ACTIVE);
			k_delayed_work_submit(&led_pat_gps_work,
					      K_SECONDS(5));
		}
	}
}

/* Message handler for SUB_STATE_GPS_INACTIVE in STATE_PASSIVE. */
static void on_state_passive_sub_state_gps_inactive(struct output_msg_data *msg)
{
	if (IS_EVENT(msg, gps, GPS_EVT_ACTIVE)) {
		ui_led_set_pattern(UI_LED_GPS_SEARCHING);
		sub_state_set(SUB_STATE_GPS_ACTIVE);
	}

	if ((IS_EVENT(msg, data, DATA_EVT_DATA_SEND)) ||
	    (IS_EVENT(msg, data, DATA_EVT_UI_DATA_SEND))) {
		ui_led_set_pattern(UI_CLOUD_PUBLISHING);
		k_delayed_work_submit(&led_pat_passive_work, K_SECONDS(5));
	}

	if (IS_EVENT(msg, data, DATA_EVT_CONFIG_READY)) {
		if (msg->module.data.data.cfg.act) {
			state_set(STATE_ACTIVE);
			k_delayed_work_submit(&led_pat_active_work,
					      K_SECONDS(5));
		}
	}
}

/* Message handler for all states. */
static void on_all_states(struct output_msg_data *msg)
{
	if (IS_EVENT(msg, app, APP_EVT_START)) {

		module_start(&self);

		state_set(STATE_ACTIVE);
		sub_state_set(SUB_STATE_GPS_INACTIVE);
	}

	if (IS_EVENT(msg, util, UTIL_EVT_SHUTDOWN_REQUEST)) {
		ui_led_set_pattern(UI_LED_ERROR_SYSTEM_FAULT);

		state_set(STATE_ERROR);

		SEND_EVENT(output, OUTPUT_EVT_SHUTDOWN_READY);
	}

	if (IS_EVENT(msg, modem, MODEM_EVT_LTE_CONNECTING)) {
		ui_led_set_pattern(UI_LTE_CONNECTING);
	}

	if (IS_EVENT(msg, data, DATA_EVT_CONFIG_INIT)) {
		state_set(msg->module.data.data.cfg.act ?
			 STATE_ACTIVE :
			 STATE_PASSIVE);
	}
}

static void message_handler(struct output_msg_data *msg)
{
	switch (state) {
	case STATE_ACTIVE:
		switch (sub_state) {
		case SUB_STATE_GPS_ACTIVE:
			on_state_active_sub_state_gps_active(msg);
			break;
		case SUB_STATE_GPS_INACTIVE:
			on_state_active_sub_state_gps_inactive(msg);
			break;
		default:
			LOG_WRN("Unknown output module sub state.");
			break;
		}
		break;
	case STATE_PASSIVE:
		switch (sub_state) {
		case SUB_STATE_GPS_ACTIVE:
			on_state_passive_sub_state_gps_active(msg);
			break;
		case SUB_STATE_GPS_INACTIVE:
			on_state_passive_sub_state_gps_inactive(msg);
			break;
		default:
			LOG_WRN("Unknown output module sub state.");
			break;
		}
		break;
	case STATE_ERROR:
		/* The error state has no transition. */
		break;
	default:
		LOG_WRN("Unknown output module state.");
		break;
	}

	on_all_states(msg);
}

EVENT_LISTENER(MODULE, event_handler);
EVENT_SUBSCRIBE_EARLY(MODULE, app_module_event);
EVENT_SUBSCRIBE_EARLY(MODULE, data_module_event);
EVENT_SUBSCRIBE_EARLY(MODULE, gps_module_event);
EVENT_SUBSCRIBE_EARLY(MODULE, modem_module_event);
EVENT_SUBSCRIBE_EARLY(MODULE, util_module_event);
