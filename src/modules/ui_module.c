/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#include <zephyr.h>
#include <stdio.h>
#include <dk_buttons_and_leds.h>
#include <event_manager.h>

#define MODULE ui_module

#include "modules_common.h"
#include "events/ui_module_event.h"
#include "events/app_module_event.h"
#include "events/util_module_event.h"

#include <logging/log.h>
LOG_MODULE_REGISTER(MODULE, CONFIG_UI_MODULE_LOG_LEVEL);

struct ui_msg_data {
	union {
		struct util_module_event util;
		struct app_module_event app;
	} module;
};

static struct module_data self = {
	.name = "ui",
	.msg_q = NULL,
};

/* Forward declarations. */
static void message_handler(struct ui_msg_data *msg);

/* Handlers */
static bool event_handler(const struct event_header *eh)
{
	if (is_app_module_event(eh)) {
		struct app_module_event *event = cast_app_module_event(eh);
		struct ui_msg_data msg = {
			.module.app = *event
		};

		message_handler(&msg);
	}

	if (is_util_module_event(eh)) {
		struct util_module_event *event = cast_util_module_event(eh);
		struct ui_msg_data msg = {
			.module.util = *event
		};

		message_handler(&msg);
	}

	return false;
}

static void button_handler(uint32_t button_states, uint32_t has_changed)
{
	if (has_changed & button_states & DK_BTN1_MSK) {

		struct ui_module_event *ui_module_event =
				new_ui_module_event();

		ui_module_event->type = UI_EVT_BUTTON_DATA_READY;
		ui_module_event->data.ui.button_number = 1;
		ui_module_event->data.ui.timestamp = k_uptime_get();

		EVENT_SUBMIT(ui_module_event);
	}

#if defined(CONFIG_BOARD_NRF9160DK_NRF9160NS)
	if (has_changed & button_states & DK_BTN2_MSK) {

		struct ui_module_event *ui_module_event =
				new_ui_module_event();

		ui_module_event->type = UI_EVT_BUTTON_DATA_READY;
		ui_module_event->data.ui.button_number = 2;
		ui_module_event->data.ui.timestamp = k_uptime_get();

		EVENT_SUBMIT(ui_module_event);

	}
#endif
}

/* Static module functions. */
static int setup(void)
{
	int err;

	err = dk_buttons_init(button_handler);
	if (err) {
		LOG_ERR("dk_buttons_init, error: %d", err);
		return err;
	}

	return 0;
}

static void message_handler(struct ui_msg_data *msg)
{
	if (IS_EVENT(msg, app, APP_EVT_START)) {
		int err;

		module_start(&self);

		err = setup();
		if (err) {
			LOG_ERR("setup, error: %d", err);
			SEND_ERROR(ui, UI_EVT_ERROR, err);
		}
	}

	if (IS_EVENT(msg, util, UTIL_EVT_SHUTDOWN_REQUEST)) {
		SEND_EVENT(ui, UI_EVT_SHUTDOWN_READY);
	}
}

EVENT_LISTENER(MODULE, event_handler);
EVENT_SUBSCRIBE(MODULE, util_module_event);
EVENT_SUBSCRIBE(MODULE, app_module_event);
