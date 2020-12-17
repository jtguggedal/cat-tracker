/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#include <zephyr.h>
#include <stdio.h>
#include <event_manager.h>

#include "ui.h"

#define MODULE output_manager

#include "modules_common.h"
#include "events/app_mgr_event.h"
#include "events/data_mgr_event.h"
#include "events/output_mgr_event.h"
#include "events/sensor_mgr_event.h"
#include "events/util_mgr_event.h"
#include "events/gps_mgr_event.h"
#include "events/modem_mgr_event.h"

#include <logging/log.h>
LOG_MODULE_REGISTER(MODULE, CONFIG_CAT_TRACKER_LOG_LEVEL);

extern atomic_t manager_count;

struct output_msg_data {
	union {
		struct app_mgr_event app;
		struct modem_mgr_event modem;
		struct data_mgr_event data;
		struct gps_mgr_event gps;
		struct util_mgr_event util;
	} manager;
};

/* Output manager super states. */
enum output_manager_states {
	OUTPUT_MGR_STATE_INIT,
	OUTPUT_MGR_STATE_RUNNING,
	OUTPUT_MGR_STATE_ERROR
} output_state;

/* Output manager sub states. */
enum output_manager_sub_states {
	OUTPUT_MGR_SUB_STATE_ACTIVE,
	OUTPUT_MGR_SUB_STATE_PASSIVE
} output_sub_state;

/* Output manager sub-sub states. */
enum output_manager_sub_sub_states {
	OUTPUT_MGR_SUB_SUB_STATE_GPS_INACTIVE,
	OUTPUT_MGR_SUB_SUB_STATE_GPS_ACTIVE
} output_sub_sub_state;

/* Delayed works that is used to make sure the device always reverts back to the
 * device mode or GPS search LED pattern.
 */
static struct k_delayed_work led_pat_active_work;
static struct k_delayed_work led_pat_passive_work;
static struct k_delayed_work led_pat_gps_work;

K_MSGQ_DEFINE(msgq_output, sizeof(struct output_msg_data), 10, 4);

static void message_handler(struct output_msg_data *msg);

static void signal_error(int err)
{
	struct output_mgr_event *output_mgr_event = new_output_mgr_event();

	output_mgr_event->type = OUTPUT_MGR_EVT_ERROR;
	output_mgr_event->err = err;

	EVENT_SUBMIT(output_mgr_event);
}

static void signal_event(enum output_mgr_event_types type)
{
	struct output_mgr_event *output_mgr_event = new_output_mgr_event();

	output_mgr_event->type = type;

	EVENT_SUBMIT(output_mgr_event);
}

static int setup(void)
{
	int err;

	err = ui_init();
	if (err) {
		LOG_ERR("ui_init, error: %d", err);
		return err;
	}

	return 0;
}

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

static bool event_handler(const struct event_header *eh)
{
	if (is_app_mgr_event(eh)) {
		struct app_mgr_event *event = cast_app_mgr_event(eh);
		struct output_msg_data msg = {
			.manager.app = *event
		};

		if (IS_EVENT((&msg), app, APP_MGR_EVT_START)) {
			int err;

			atomic_inc(&manager_count);
			k_delayed_work_init(&led_pat_gps_work,
					    led_pat_gps_work_fn);
			k_delayed_work_init(&led_pat_active_work,
					    led_pat_active_work_fn);
			k_delayed_work_init(&led_pat_passive_work,
					    led_pat_passive_work_fn);

			err = setup();
			if (err) {
				LOG_ERR("setup, error: %d", err);
				signal_error(err);
			}
		}
	}

	if (is_data_mgr_event(eh)) {
		struct data_mgr_event *event = cast_data_mgr_event(eh);
		struct output_msg_data output_msg = {
			.manager.data = *event
		};

		message_handler(&output_msg);
	}

	if (is_modem_mgr_event(eh)) {
		struct modem_mgr_event *event = cast_modem_mgr_event(eh);
		struct output_msg_data output_msg = {
			.manager.modem = *event
		};

		message_handler(&output_msg);
	}

	if (is_gps_mgr_event(eh)) {
		struct gps_mgr_event *event = cast_gps_mgr_event(eh);
		struct output_msg_data output_msg = {
			.manager.gps = *event
		};

		message_handler(&output_msg);
	}

	if (is_util_mgr_event(eh)) {
		struct util_mgr_event *event = cast_util_mgr_event(eh);
		struct output_msg_data output_msg = {
			.manager.util = *event
		};

		message_handler(&output_msg);
	}

	return false;
}

static void on_state_init(struct output_msg_data *output_msg)
{
	if (is_data_mgr_event(&output_msg->manager.data.header) &&
	    output_msg->manager.data.type == DATA_MGR_EVT_CONFIG_INIT) {
		output_state = OUTPUT_MGR_STATE_RUNNING;
		output_sub_state = output_msg->manager.data.data.cfg.act ?
						OUTPUT_MGR_SUB_STATE_ACTIVE :
						OUTPUT_MGR_SUB_STATE_PASSIVE;
	}
}

static void on_active_gps_active(struct output_msg_data *output_msg)
{
	if (is_gps_mgr_event(&output_msg->manager.gps.header)) {
		switch (output_msg->manager.gps.type) {
		case GPS_MGR_EVT_INACTIVE:
			ui_led_set_pattern(UI_LED_ACTIVE_MODE);
			output_sub_sub_state =
					OUTPUT_MGR_SUB_SUB_STATE_GPS_INACTIVE;
			break;
		default:
			break;
		}
	}

	if (is_data_mgr_event(&output_msg->manager.data.header)) {
		switch (output_msg->manager.data.type) {
		case DATA_MGR_EVT_DATA_SEND:
			/* Fall through. */
		case DATA_MGR_EVT_UI_DATA_SEND:
			ui_led_set_pattern(UI_CLOUD_PUBLISHING);
			k_delayed_work_submit(&led_pat_gps_work, K_SECONDS(5));
			break;
		default:
			break;
		}
	}
}

static void on_active_gps_inactive(struct output_msg_data *output_msg)
{
	if (is_gps_mgr_event(&output_msg->manager.gps.header)) {
		switch (output_msg->manager.gps.type) {
		case GPS_MGR_EVT_ACTIVE:
			ui_led_set_pattern(UI_LED_GPS_SEARCHING);
			output_sub_sub_state =
					OUTPUT_MGR_SUB_SUB_STATE_GPS_ACTIVE;
			break;
		default:
			break;
		}
	}

	if (is_data_mgr_event(&output_msg->manager.data.header)) {
		switch (output_msg->manager.data.type) {
		case DATA_MGR_EVT_DATA_SEND:
			/* Fall through. */
		case DATA_MGR_EVT_UI_DATA_SEND:
			ui_led_set_pattern(UI_CLOUD_PUBLISHING);
			k_delayed_work_submit(&led_pat_active_work,
					      K_SECONDS(5));
			break;
		default:
			break;
		}
	}
}

static void on_passive_gps_active(struct output_msg_data *output_msg)
{
	if (is_gps_mgr_event(&output_msg->manager.gps.header)) {
		switch (output_msg->manager.gps.type) {
		case GPS_MGR_EVT_INACTIVE:
			ui_led_set_pattern(UI_LED_PASSIVE_MODE);
			output_sub_sub_state =
					OUTPUT_MGR_SUB_SUB_STATE_GPS_INACTIVE;
			break;
		default:
			break;
		}
	}

	if (is_data_mgr_event(&output_msg->manager.data.header)) {
		switch (output_msg->manager.data.type) {
		case DATA_MGR_EVT_DATA_SEND:
			/* Fall through. */
		case DATA_MGR_EVT_UI_DATA_SEND:
			ui_led_set_pattern(UI_CLOUD_PUBLISHING);
			k_delayed_work_submit(&led_pat_gps_work, K_SECONDS(5));
			break;
		default:
			break;
		}
	}
}

static void on_passive_gps_inactive(struct output_msg_data *output_msg)
{
	if (is_gps_mgr_event(&output_msg->manager.gps.header)) {
		switch (output_msg->manager.gps.type) {
		case GPS_MGR_EVT_ACTIVE:
			ui_led_set_pattern(UI_LED_GPS_SEARCHING);
			output_sub_sub_state =
					OUTPUT_MGR_SUB_SUB_STATE_GPS_ACTIVE;
			break;
		default:
			break;
		}
	}

	if (is_data_mgr_event(&output_msg->manager.data.header)) {
		switch (output_msg->manager.data.type) {
		case DATA_MGR_EVT_DATA_SEND:
			/* Fall through. */
		case DATA_MGR_EVT_UI_DATA_SEND:
			ui_led_set_pattern(UI_CLOUD_PUBLISHING);
			k_delayed_work_submit(&led_pat_passive_work,
					      K_SECONDS(5));
			break;
		default:
			break;
		}
	}
}

static void on_sub_state_active(struct output_msg_data *output_msg)
{
	if (is_data_mgr_event(&output_msg->manager.data.header)) {
		switch (output_msg->manager.data.type) {
		case DATA_MGR_EVT_CONFIG_READY:
			if (!output_msg->manager.data.data.cfg.act) {
				output_sub_state = OUTPUT_MGR_SUB_STATE_PASSIVE;
			}
			break;
		default:
			break;
		}
	}
}

static void on_sub_state_passive(struct output_msg_data *output_msg)
{
	if (is_data_mgr_event(&output_msg->manager.data.header)) {
		switch (output_msg->manager.data.type) {
		case DATA_MGR_EVT_CONFIG_READY:
			if (output_msg->manager.data.data.cfg.act) {
				output_sub_state = OUTPUT_MGR_SUB_STATE_ACTIVE;
			}
			break;
		default:
			break;
		}
	}
}

static void on_state_running(struct output_msg_data *output_msg)
{
	if (is_modem_mgr_event(&output_msg->manager.modem.header) &&
		output_msg->manager.modem.type ==
				MODEM_MGR_EVT_LTE_CONNECTING) {
		ui_led_set_pattern(UI_LTE_CONNECTING);
	}
}

static void on_all_states(struct output_msg_data *output_msg)
{
	if (is_util_mgr_event(&output_msg->manager.util.header) &&
	    output_msg->manager.util.type == UTIL_MGR_EVT_SHUTDOWN_REQUEST) {
		ui_led_set_pattern(UI_LED_ERROR_SYSTEM_FAULT);

		output_state = OUTPUT_MGR_STATE_ERROR;

		signal_event(OUTPUT_MGR_EVT_SHUTDOWN_READY);
	}
}

static void message_handler(struct output_msg_data *msg)
{
	switch (output_state) {
	case OUTPUT_MGR_STATE_INIT:
		on_state_init(msg);
		break;
	case OUTPUT_MGR_STATE_RUNNING:
		switch (output_sub_state) {
		case OUTPUT_MGR_SUB_STATE_ACTIVE:
			switch (output_sub_sub_state) {
			case OUTPUT_MGR_SUB_SUB_STATE_GPS_ACTIVE:
				on_active_gps_active(msg);
				break;
			case OUTPUT_MGR_SUB_SUB_STATE_GPS_INACTIVE:
				on_active_gps_inactive(msg);
				break;
			default:
				break;
			}

			on_sub_state_active(msg);
			break;
		case OUTPUT_MGR_SUB_STATE_PASSIVE:
			switch (output_sub_sub_state) {
			case OUTPUT_MGR_SUB_SUB_STATE_GPS_ACTIVE:
				on_passive_gps_active(msg);
				break;
			case OUTPUT_MGR_SUB_SUB_STATE_GPS_INACTIVE:
				on_passive_gps_inactive(msg);
				break;
			default:
				break;
			}

			on_sub_state_passive(msg);
			break;
		default:
			LOG_WRN("Unknown output manager sub state.");
			break;
		}
		on_state_running(msg);
		break;
	case OUTPUT_MGR_STATE_ERROR:
		/* The error state has no transition. */
		break;
	default:
		LOG_WRN("Unknown output manager state.");
		break;
	}

	on_all_states(msg);
}

EVENT_LISTENER(MODULE, event_handler);
EVENT_SUBSCRIBE_EARLY(MODULE, app_mgr_event);
EVENT_SUBSCRIBE_EARLY(MODULE, data_mgr_event);
EVENT_SUBSCRIBE_EARLY(MODULE, gps_mgr_event);
EVENT_SUBSCRIBE_EARLY(MODULE, modem_mgr_event);
EVENT_SUBSCRIBE_EARLY(MODULE, util_mgr_event);
