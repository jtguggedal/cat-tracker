
/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#include <zephyr.h>
#include <power/reboot.h>
#include <logging/log.h>

#define MODULE util_manager

#include "modules_common.h"
#include "events/app_mgr_event.h"
#include "events/cloud_mgr_event.h"
#include "events/data_mgr_event.h"
#include "events/sensor_mgr_event.h"
#include "events/ui_mgr_event.h"
#include "events/util_mgr_event.h"
#include "events/gps_mgr_event.h"
#include "events/modem_mgr_event.h"
#include "events/output_mgr_event.h"

#include <logging/log.h>
LOG_MODULE_REGISTER(MODULE, CONFIG_CAT_TRACKER_LOG_LEVEL);

/* Atomic variable that is incremented by other managers in the application.
 * Used to keep track of shutdown request acknowledgments from managers.
 */
atomic_t manager_count;

/* Util manager states. */
static enum util_state {
	STATE_INIT,
	STATE_REBOOT_PENDING
} state;


static void state_set(enum util_state new_state)
{
	state = new_state;
}

struct util_msg_data {
	union {
		struct cloud_mgr_event cloud;
		struct ui_mgr_event ui;
		struct sensor_mgr_event sensor;
		struct data_mgr_event data;
		struct app_mgr_event app;
		struct gps_mgr_event gps;
		struct modem_mgr_event modem;
		struct output_mgr_event output;
	} manager;
};

static void message_handler(struct util_msg_data *msg);

static struct k_delayed_work reboot_work;

static void reboot(void)
{
	LOG_ERR("Rebooting!");
#if !defined(CONFIG_DEBUG) && defined(CONFIG_REBOOT)
	LOG_PANIC();
	sys_reboot(0);
#else
	while (true) {
		k_cpu_idle();
	}
#endif
}

static void reboot_work_fn(struct k_work *work)
{
	reboot();
}

static void signal_reboot_request(void)
{
	/* Flag ensuring that multiple reboot requests are not emitted
	 * upon an error from multiple managers.
	 */
	static bool error_signaled;

	if (!error_signaled) {
		struct util_mgr_event *util_mgr_event = new_util_mgr_event();

		util_mgr_event->type = UTIL_MGR_EVT_SHUTDOWN_REQUEST;

		k_delayed_work_submit(&reboot_work,
				      K_SECONDS(CONFIG_REBOOT_TIMEOUT));

		EVENT_SUBMIT(util_mgr_event);

		error_signaled = true;
	}
}

void bsd_recoverable_error_handler(uint32_t err)
{
	signal_reboot_request();
}

void k_sys_fatal_error_handler(unsigned int reason, const z_arch_esf_t *esf)
{
	ARG_UNUSED(esf);

	LOG_PANIC();
	signal_reboot_request();
}

static bool event_handler(const struct event_header *eh)
{
	if (is_modem_mgr_event(eh)) {
		struct modem_mgr_event *event = cast_modem_mgr_event(eh);
		struct util_msg_data util_msg = {
			.manager.modem = *event
		};

		message_handler(&util_msg);
	}

	if (is_cloud_mgr_event(eh)) {
		struct cloud_mgr_event *event = cast_cloud_mgr_event(eh);
		struct util_msg_data util_msg = {
			.manager.cloud = *event
		};

		message_handler(&util_msg);
	}

	if (is_gps_mgr_event(eh)) {
		struct gps_mgr_event *event = cast_gps_mgr_event(eh);
		struct util_msg_data util_msg = {
			.manager.gps = *event
		};

		message_handler(&util_msg);
	}

	if (is_sensor_mgr_event(eh)) {
		struct sensor_mgr_event *event = cast_sensor_mgr_event(eh);
		struct util_msg_data util_msg = {
			.manager.sensor = *event
		};

		message_handler(&util_msg);
	}

	if (is_ui_mgr_event(eh)) {
		struct ui_mgr_event *event = cast_ui_mgr_event(eh);
		struct util_msg_data util_msg = {
			.manager.ui = *event
		};

		message_handler(&util_msg);
	}

	if (is_app_mgr_event(eh)) {
		struct app_mgr_event *event = cast_app_mgr_event(eh);
		struct util_msg_data util_msg = {
			.manager.app = *event
		};

		message_handler(&util_msg);
	}

	if (is_data_mgr_event(eh)) {
		struct data_mgr_event *event = cast_data_mgr_event(eh);
		struct util_msg_data util_msg = {
			.manager.data = *event
		};

		message_handler(&util_msg);
	}

	if (is_output_mgr_event(eh)) {
		struct output_mgr_event *event = cast_output_mgr_event(eh);
		struct util_msg_data util_msg = {
			.manager.output = *event
		};

		message_handler(&util_msg);
	}

	return false;
}

static void on_all_states(struct util_msg_data *msg)
{
	static int reboot_ack_cnt;

	if (is_cloud_mgr_event(&msg->manager.cloud.header)) {
		switch (msg->manager.cloud.type) {
		case CLOUD_MGR_EVT_ERROR: /* Fall-through */
		case CLOUD_MGR_EVT_FOTA_DONE:
			signal_reboot_request();
			break;
		case CLOUD_MGR_EVT_SHUTDOWN_READY:
			reboot_ack_cnt++;
			break;
		default:
			break;
		}
	}

	if (is_modem_mgr_event(&msg->manager.modem.header)) {
		switch (msg->manager.modem.type) {
		case MODEM_MGR_EVT_ERROR:
			signal_reboot_request();
			break;
		case MODEM_MGR_EVT_SHUTDOWN_READY:
			reboot_ack_cnt++;
			break;
		default:
			break;
		}
	}

	if (is_sensor_mgr_event(&msg->manager.sensor.header)) {
		switch (msg->manager.sensor.type) {
		case SENSOR_MGR_EVT_ERROR:
			signal_reboot_request();
			break;
		case SENSOR_MGR_EVT_SHUTDOWN_READY:
			reboot_ack_cnt++;
			break;
		default:
			break;
		}
	}

	if (is_gps_mgr_event(&msg->manager.gps.header)) {
		switch (msg->manager.gps.type) {
		case GPS_MGR_EVT_ERROR:
			signal_reboot_request();
			break;
		case GPS_MGR_EVT_SHUTDOWN_READY:
			reboot_ack_cnt++;
			break;
		default:
			break;
		}
	}

	if (is_data_mgr_event(&msg->manager.data.header)) {
		switch (msg->manager.data.type) {
		case DATA_MGR_EVT_ERROR:
			signal_reboot_request();
			break;
		case DATA_MGR_EVT_SHUTDOWN_READY:
			reboot_ack_cnt++;
			break;
		default:
			break;
		}
	}

	if (is_app_mgr_event(&msg->manager.app.header)) {
		switch (msg->manager.app.type) {
		case APP_MGR_EVT_ERROR:
			signal_reboot_request();
			break;
		case APP_MGR_EVT_SHUTDOWN_READY:
			reboot_ack_cnt++;
			break;
		default:
			break;
		}
	}

	if (is_ui_mgr_event(&msg->manager.ui.header)) {
		switch (msg->manager.ui.type) {
		case UI_MGR_EVT_ERROR:
			signal_reboot_request();
			break;
		case UI_MGR_EVT_SHUTDOWN_READY:
			reboot_ack_cnt++;
			break;
		default:
			break;
		}
	}

	if (is_output_mgr_event(&msg->manager.output.header)) {
		switch (msg->manager.output.type) {
		case OUTPUT_MGR_EVT_ERROR:
			signal_reboot_request();
			break;
		case OUTPUT_MGR_EVT_SHUTDOWN_READY:
			reboot_ack_cnt++;
			break;
		default:
			break;
		}
	}

	/* Reboot if after a shorter timeout if all managers has acknowledged
	 * that the application is ready to shutdown. This ensures a graceful
	 * shutdown.
	 */
	if (reboot_ack_cnt >= manager_count) {
		k_delayed_work_submit(&reboot_work,
				      K_SECONDS(50));
	}
}

static void message_handler(struct util_msg_data *msg)
{
	if (IS_EVENT(msg, app, APP_MGR_EVT_START)) {
		state_set(STATE_INIT);
		k_delayed_work_init(&reboot_work, reboot_work_fn);
	}


	on_all_states(msg);
}

EVENT_LISTENER(MODULE, event_handler);
EVENT_SUBSCRIBE_EARLY(MODULE, app_mgr_event);
EVENT_SUBSCRIBE_EARLY(MODULE, modem_mgr_event);
EVENT_SUBSCRIBE_EARLY(MODULE, cloud_mgr_event);
EVENT_SUBSCRIBE_EARLY(MODULE, gps_mgr_event);
EVENT_SUBSCRIBE_EARLY(MODULE, ui_mgr_event);
EVENT_SUBSCRIBE_EARLY(MODULE, sensor_mgr_event);
EVENT_SUBSCRIBE_EARLY(MODULE, data_mgr_event);
EVENT_SUBSCRIBE_EARLY(MODULE, output_mgr_event);
