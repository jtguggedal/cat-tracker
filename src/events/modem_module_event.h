/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#ifndef _MODEM_EVENT_H_
#define _MODEM_EVENT_H_

/**
 * @brief Modem Event
 * @defgroup modem_module_event Modem Event
 * @{
 */

#include "event_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Modem event types submitted by Modem module. */
enum modem_module_event_type {
	MODEM_EVT_LTE_CONNECTED,
	MODEM_EVT_LTE_DISCONNECTED,
	MODEM_EVT_LTE_CONNECTING,
	MODEM_EVT_LTE_CELL_UPDATE,
	MODEM_EVT_LTE_PSM_UPDATE,
	MODEM_EVT_LTE_EDRX_UPDATE,
	MODEM_EVT_MODEM_DATA_READY,
	MODEM_EVT_BATTERY_DATA_READY,
	MODEM_EVT_SHUTDOWN_READY,
	MODEM_EVT_ERROR
};

/** @brief LTE cell information. */
struct modem_module_cell {
	/** E-UTRAN cell ID. */
	uint32_t cell_id;
	/** Tracking area code. */
	uint32_t tac;
};

/** @brief PSM information. */
struct modem_module_psm {
	/** Tracking Area Update interval [s]. -1 if the timer is disabled. */
	int tau;
	/** Active time [s]. -1 if the timer is disabled. */
	int active_time;
};

/** @brief eDRX information. */
struct modem_module_edrx {
	/** eDRX interval value [s] */
	float edrx;
	/** Paging time window [s] */
	float ptw;
};

struct modem_module_modem_data {
	/** Dynamic modem data timestamp. UNIX milliseconds. */
	int64_t mod_ts;
	/** Static modem data timestamp. UNIX milliseconds. */
	int64_t mod_ts_static;
	/** Area code. */
	uint16_t area;
	/** Cell id. */
	uint16_t cell;
	/** Band number. */
	uint16_t bnd;
	/** Network mode GPS. */
	uint16_t nw_gps;
	/** Network mode LTE-M. */
	uint16_t nw_lte_m;
	/** Network mode NB-IoT. */
	uint16_t nw_nb_iot;
	/** Reference Signal Received Power. */
	uint16_t rsrp;
	/** Internet Protocol Address. */
	char *ip;
	/* Mobile Country Code*/
	char *mccmnc;
	/** Application version and Mobile Network Code. */
	char *appv;
	/** Device board version. */
	const char *brdv;
	/** Modem firmware. */
	char *fw;
	/** Integrated Circuit Card Identifier. */
	char *iccid;
};

struct modem_module_battery_data {
	/** Battery voltage level. */
	uint16_t bat;
	/** Battery data timestamp. UNIX milliseconds. */
	int64_t bat_ts;
};

/** @brief Modem event. */
struct modem_module_event {
	struct event_header header;
	enum modem_module_event_type type;
	union {
		struct modem_module_modem_data modem;
		struct modem_module_battery_data bat;
		struct modem_module_cell cell;
		struct modem_module_psm psm;
		struct modem_module_edrx edrx;
		int err;
	} data;
};

EVENT_TYPE_DECLARE(modem_module_event);

#ifdef __cplusplus
}
#endif

/**
 * @}
 */

#endif /* _MODEM_EVENT_H_ */
