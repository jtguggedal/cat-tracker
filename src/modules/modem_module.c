/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#include <zephyr.h>
#include <stdio.h>
#include <stdio.h>
#include <event_manager.h>

#include <modem/lte_lc.h>
#include <modem/modem_info.h>

#define MODULE modem_module

#include "modules_common.h"
#include "events/app_module_event.h"
#include "events/data_module_event.h"
#include "events/modem_module_event.h"
#include "events/util_module_event.h"
#include "events/cloud_module_event.h"

#include <logging/log.h>
LOG_MODULE_REGISTER(MODULE, CONFIG_CAT_TRACKER_LOG_LEVEL);

BUILD_ASSERT(!IS_ENABLED(CONFIG_LTE_AUTO_INIT_AND_CONNECT),
		"The Modem module does not support this configuration");

static struct module_data self = {
	.name = "modem",
};

struct modem_msg_data {
	union {
		struct app_module_event app;
		struct cloud_module_event cloud;
		struct util_module_event util;
		struct modem_module_event modem;
	} module;
};

/* Modem module connection states. */
static enum connection_state {
	LTE_STATE_DISCONNECTED,
	LTE_STATE_CONNECTING,
	LTE_STATE_CONNECTED,
	LTE_STATE_SHUTTING_DOWN,
} connection_state;

/* Forward declarations. */
static void message_handler(struct modem_msg_data *msg);

/* Convenience function that translates enumerator states to strings. */
static char *state2str(enum connection_state state)
{
	switch (state) {
	case LTE_STATE_DISCONNECTED:
		return "LTE_STATE_DISCONNECTED";
	case LTE_STATE_CONNECTING:
		return "LTE_STATE_CONNECTING";
	case LTE_STATE_CONNECTED:
		return "LTE_STATE_CONNECTED";
	case LTE_STATE_SHUTTING_DOWN:
		return "LTE_STATE_SHUTTING_DOWN";
	default:
		return "Unknown state";
	}
}

/* Function to set the internal connection state of the Modem module. */
static void connection_state_set(enum connection_state new_state)
{
	if (new_state == connection_state) {
		LOG_DBG("State: %s", log_strdup(state2str(connection_state)));
		return;
	}

	LOG_DBG("State transition %s --> %s",
		log_strdup(state2str(connection_state)),
		log_strdup(state2str(new_state)));

	connection_state = new_state;
}

/* Struct that holds data from the modem information module. */
static struct modem_param_info modem_param;

/* Value that always holds the latest RSRP value. */
static uint16_t rsrp_value_latest;


/* Event manager handler */

static bool event_handler(const struct event_header *eh)
{
	if (is_modem_module_event(eh)) {
		struct modem_module_event *event = cast_modem_module_event(eh);
		struct modem_msg_data modem_msg = {
			.module.modem = *event
		};

		message_handler(&modem_msg);
	}

	if (is_app_module_event(eh)) {
		struct app_module_event *event = cast_app_module_event(eh);
		struct modem_msg_data modem_msg = {
			.module.app = *event
		};

		message_handler(&modem_msg);
	}

	if (is_cloud_module_event(eh)) {
		struct cloud_module_event *event = cast_cloud_module_event(eh);
		struct modem_msg_data modem_msg = {
			.module.cloud = *event
		};

		message_handler(&modem_msg);
	}

	if (is_util_module_event(eh)) {
		struct util_module_event *event = cast_util_module_event(eh);
		struct modem_msg_data modem_msg = {
			.module.util = *event
		};

		message_handler(&modem_msg);
	}

	return false;
}

static void send_cell_update(uint32_t cell_id, uint32_t tac)
{
	struct modem_module_event *evt = new_modem_module_event();

	evt->type = MODEM_EVT_LTE_CELL_UPDATE;
	evt->data.cell.cell_id = cell_id;
	evt->data.cell.tac = tac;

	EVENT_SUBMIT(evt);
}

static void send_psm_update(int tau, int active_time)
{
	struct modem_module_event *evt = new_modem_module_event();

	evt->type = MODEM_EVT_LTE_PSM_UPDATE;
	evt->data.psm.tau = tau;
	evt->data.psm.active_time = active_time;

	EVENT_SUBMIT(evt);
}

static void send_edrx_update(float edrx, float ptw)
{
	struct modem_module_event *evt = new_modem_module_event();

	evt->type = MODEM_EVT_LTE_EDRX_UPDATE;
	evt->data.edrx.edrx = edrx;
	evt->data.edrx.ptw = ptw;

	EVENT_SUBMIT(evt);
}

/* Modem info handling */

static void modem_rsrp_handler(char rsrp_value)
{
	/* RSRP raw values that represent actual signal strength are
	 * 0 through 97 (per "nRF91 AT Commands" v1.1).
	 */

	if (rsrp_value > 97) {
		return;
	}

	/* Set temporary variable to hold RSRP value. RSRP callbacks and other
	 * data from the modem info module are retrieved separately.
	 * This temporarily saves the latest value which are sent to
	 * the Data module upon a modem data request.
	 */
	rsrp_value_latest = rsrp_value;

	LOG_DBG("Incoming RSRP status message, RSRP value is %d",
		rsrp_value_latest);
}

static int modem_data_init(void)
{
	int err;

	err = modem_info_init();
	if (err) {
		LOG_INF("modem_info_init, error: %d", err);
		return err;
	}

	err = modem_info_params_init(&modem_param);
	if (err) {
		LOG_INF("modem_info_params_init, error: %d", err);
		return err;
	}

	err = modem_info_rsrp_register(modem_rsrp_handler);
	if (err) {
		LOG_INF("modem_info_rsrp_register, error: %d", err);
		return err;
	}

	return 0;
}

/* Produce a warning if modem firmware version is unexpected. */
static void check_modem_fw_version(void)
{
	static bool modem_fw_version_checked;

	if (modem_fw_version_checked) {
		return;
	}
	if (strcmp(modem_param.device.modem_fw.value_string,
		   CONFIG_EXPECTED_MODEM_FIRMWARE_VERSION) != 0) {
		LOG_WRN("Unsupported modem firmware version: %s",
			log_strdup(modem_param.device.modem_fw.value_string));
		LOG_WRN("Expected firmware version: %s",
			CONFIG_EXPECTED_MODEM_FIRMWARE_VERSION);
		LOG_WRN("You can change the expected version through the");
		LOG_WRN("EXPECTED_MODEM_FIRMWARE_VERSION setting.");
		LOG_WRN("Please upgrade: http://bit.ly/nrf9160-mfw-update");
	} else {
		LOG_DBG("Board is running expected modem firmware version: %s",
			log_strdup(modem_param.device.modem_fw.value_string));
	}
	modem_fw_version_checked = true;
}

static int modem_data_get(void)
{
	int err;

	/* Request data from modem information module. */
	err = modem_info_params_get(&modem_param);
	if (err) {
		LOG_ERR("modem_info_params_get, error: %d", err);
		return err;
	}

	check_modem_fw_version();

	struct modem_module_event *modem_module_event = new_modem_module_event();

	modem_module_event->data.modem.rsrp = rsrp_value_latest;
	modem_module_event->data.modem.ip =
		modem_param.network.ip_address.value_string;
	modem_module_event->data.modem.cell = modem_param.network.cellid_dec;
	modem_module_event->data.modem.mccmnc =
		modem_param.network.current_operator.value_string;
	modem_module_event->data.modem.area = modem_param.network.area_code.value;
	modem_module_event->data.modem.appv = CONFIG_CAT_TRACKER_APP_VERSION;
	modem_module_event->data.modem.brdv = modem_param.device.board;
	modem_module_event->data.modem.fw =
		modem_param.device.modem_fw.value_string;
	modem_module_event->data.modem.iccid = modem_param.sim.iccid.value_string;
	modem_module_event->data.modem.nw_lte_m =
		modem_param.network.lte_mode.value;
	modem_module_event->data.modem.nw_nb_iot =
		modem_param.network.nbiot_mode.value;
	modem_module_event->data.modem.nw_gps = modem_param.network.gps_mode.value;
	modem_module_event->data.modem.bnd =
		modem_param.network.current_band.value;
	modem_module_event->data.modem.mod_ts = k_uptime_get();
	modem_module_event->data.modem.mod_ts_static = k_uptime_get();
	modem_module_event->data.modem.queued = true;
	modem_module_event->type = MODEM_EVT_MODEM_DATA_READY;

	EVENT_SUBMIT(modem_module_event);

	return 0;
}

static bool modem_data_requested(enum app_module_data_type *data_list,
				size_t count)
{
	for (size_t i = 0; i < count; i++) {
		if (data_list[i] == APP_DATA_MODEM) {
			return true;
		}
	}

	return false;
}

static bool battery_data_requested(enum app_module_data_type *data_list,
				   size_t count)
{
	for (size_t i = 0; i < count; i++) {
		if (data_list[i] == APP_DATA_BATTERY) {
			return true;
		}
	}

	return false;
}

static int battery_data_get(void)
{
	int err;

	/* Replace this function with a function that specifically
	 * requests battery.
	 */
	err = modem_info_params_get(&modem_param);
	if (err) {
		LOG_ERR("modem_info_params_get, error: %d", err);
		return err;
	}

	struct modem_module_event *modem_module_event = new_modem_module_event();

	modem_module_event->data.bat.bat = modem_param.device.battery.value;
	modem_module_event->data.bat.bat_ts = k_uptime_get();
	modem_module_event->data.bat.queued = true;
	modem_module_event->type = MODEM_EVT_BATTERY_DATA_READY;

	EVENT_SUBMIT(modem_module_event);

	return 0;
}


/* LTE configuration and handling */

static void lte_evt_handler(const struct lte_lc_evt *const evt)
{
	switch (evt->type) {
	case LTE_LC_EVT_NW_REG_STATUS:
		if (evt->nw_reg_status == LTE_LC_NW_REG_UICC_FAIL) {
			LOG_ERR("No SIM card detected!");
			SEND_ERROR(modem, MODEM_EVT_ERROR, -ENOTSUP);
			break;
		}

		if ((evt->nw_reg_status != LTE_LC_NW_REG_REGISTERED_HOME) &&
		    (evt->nw_reg_status != LTE_LC_NW_REG_REGISTERED_ROAMING)) {
			SEND_EVENT(modem, MODEM_EVT_LTE_DISCONNECTED);
			break;
		}

		LOG_DBG("Network registration status: %s",
			evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME ?
			"Connected - home network" : "Connected - roaming");

		SEND_EVENT(modem, MODEM_EVT_LTE_CONNECTED);
		break;
	case LTE_LC_EVT_PSM_UPDATE:
		LOG_DBG("PSM parameter update: TAU: %d, Active time: %d",
			evt->psm_cfg.tau, evt->psm_cfg.active_time);
		send_psm_update(evt->psm_cfg.tau, evt->psm_cfg.active_time);
		break;
	case LTE_LC_EVT_EDRX_UPDATE: {
		char log_buf[60];
		ssize_t len;

		len = snprintf(log_buf, sizeof(log_buf),
			       "eDRX parameter update: eDRX: %f, PTW: %f",
			       evt->edrx_cfg.edrx, evt->edrx_cfg.ptw);
		if (len > 0) {
			LOG_DBG("%s", log_strdup(log_buf));
		}

		send_edrx_update(evt->edrx_cfg.edrx, evt->edrx_cfg.ptw);
		break;
	}
	case LTE_LC_EVT_RRC_UPDATE:
		LOG_DBG("RRC mode: %s",
			evt->rrc_mode == LTE_LC_RRC_MODE_CONNECTED ?
			"Connected" : "Idle");
		break;
	case LTE_LC_EVT_CELL_UPDATE:
		LOG_DBG("LTE cell changed: Cell ID: %d, Tracking area: %d",
			evt->cell.id, evt->cell.tac);
		send_cell_update(evt->cell.id, evt->cell.tac);
		break;
	default:
		break;
	}
}

static int modem_configure_low_power(void)
{
	int err;

	err = lte_lc_psm_req(true);
	if (err) {
		LOG_ERR("lte_lc_psm_req, error: %d", err);
		return err;
	}

	LOG_DBG("PSM requested");

	return 0;
}

static int lte_connect(void)
{
	int err;

	err = lte_lc_connect_async(lte_evt_handler);
	if (err) {
		LOG_ERR("lte_lc_connect_async, error: %d", err);

		return err;
	}

	SEND_EVENT(modem, MODEM_EVT_LTE_CONNECTING);

	return 0;
}

static int modem_setup(void)
{
	int err;

	err = lte_lc_init();
	if (err) {
		LOG_ERR("lte_lc_init, error: %d", err);
		return err;
	}

	if (IS_ENABLED(CONFIG_MODEMAUTO_REQUEST_POWER_SAVING_FEATURES)) {
		err = modem_configure_low_power();
		if (err) {
			LOG_ERR("modem_configure_low_power, error: %d", err);
			return err;
		}
	}

	err = modem_data_init();
	if (err) {
		LOG_ERR("modem_data_init, error: %d", err);
		return err;
	}

	return 0;
}


/* Message handlers for each state */

static void on_lte_state_disconnected(struct modem_msg_data *msg)
{
	if (IS_EVENT(msg, modem, MODEM_EVT_LTE_CONNECTED)) {
		connection_state_set(LTE_STATE_CONNECTED);
	}

	if (IS_EVENT(msg, modem, MODEM_EVT_LTE_CONNECTING)) {
		connection_state_set(LTE_STATE_CONNECTING);
	}
}

static void on_lte_state_connecting(struct modem_msg_data *msg)
{
	if (IS_EVENT(msg, app, APP_EVT_LTE_DISCONNECT)) {
		int err;

		err = lte_lc_offline();
		if (err) {
			LOG_ERR("LTE disconnect failed, error: %d", err);
			SEND_ERROR(modem, MODEM_EVT_ERROR, err);
			return;
		}

		connection_state_set(LTE_STATE_DISCONNECTED);
	}

	if (IS_EVENT(msg, modem, MODEM_EVT_LTE_CONNECTED)) {
		connection_state_set(LTE_STATE_CONNECTED);
	}
}

static void on_lte_state_connected(struct modem_msg_data *msg)
{
	if (IS_EVENT(msg, modem, MODEM_EVT_LTE_DISCONNECTED)) {
		connection_state_set(LTE_STATE_DISCONNECTED);
	}
}

static void on_all_states(struct modem_msg_data *msg)
{
	if (IS_EVENT(msg, app, APP_EVT_START)) {
		int err;

		connection_state_set(LTE_STATE_DISCONNECTED);
		module_start(&self);

		err = modem_setup();
		if (err) {
			LOG_ERR("Failed setting up the modem, error: %d", err);
			SEND_ERROR(modem, MODEM_EVT_ERROR, err);
			return;
		}

		err = lte_connect();
		if (err) {
			LOG_ERR("Failed connecting to LTE, error: %d", err);
			SEND_ERROR(modem, MODEM_EVT_ERROR, err);
			return;
		}
	}

	if (IS_EVENT(msg, app, APP_EVT_DATA_GET)) {
		if (modem_data_requested(msg->module.app.data_list,
					 msg->module.app.count)) {

			int err;

			err = modem_data_get();
			if (err) {
				SEND_ERROR(modem, MODEM_EVT_ERROR, err);
			}
		}

		if (battery_data_requested(msg->module.app.data_list,
					   msg->module.app.count)) {

			int err;

			err = battery_data_get();
			if (err) {
				SEND_ERROR(modem, MODEM_EVT_ERROR, err);
			}
		}
	}

	if (IS_EVENT(msg, util, UTIL_EVT_SHUTDOWN_REQUEST)) {
		lte_lc_power_off();
		connection_state_set(LTE_STATE_SHUTTING_DOWN);
		SEND_EVENT(modem, MODEM_EVT_SHUTDOWN_READY);
	}
}

static void message_handler(struct modem_msg_data *msg)
{
	switch (connection_state) {
	case LTE_STATE_DISCONNECTED:
		on_lte_state_disconnected(msg);
		break;
	case LTE_STATE_CONNECTING:
		on_lte_state_connecting(msg);
		break;
	case LTE_STATE_CONNECTED:
		on_lte_state_connected(msg);
		break;
	case LTE_STATE_SHUTTING_DOWN:
		LOG_WRN("No action allowed in LTE_STATE_SHUTTING_DOWN");
		break;
	default:
		LOG_WRN("Invalid state: %d", connection_state);
		break;
	}

	on_all_states(msg);
}

EVENT_LISTENER(MODULE, event_handler);
EVENT_SUBSCRIBE_EARLY(MODULE, modem_module_event);
EVENT_SUBSCRIBE(MODULE, app_module_event);
EVENT_SUBSCRIBE(MODULE, cloud_module_event);
EVENT_SUBSCRIBE_FINAL(MODULE, util_module_event);
