/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#ifndef _MODEM_MGR_EVENT_H_
#define _MODEM_MGR_EVENT_H_

/**
 * @brief Modem Event
 * @defgroup modem_mgr_event Modem Event
 * @{
 */

#include "event_manager.h"
#include "cloud/cloud_codec/cloud_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Modem event types submitted by Modem manager. */
enum modem_mgr_event_type {
	MODEM_MGR_EVT_LTE_CONNECTED,
	MODEM_MGR_EVT_LTE_DISCONNECTED,
	MODEM_MGR_EVT_LTE_CONNECTING,
	MODEM_MGR_EVT_LTE_CELL_UPDATE,
	MODEM_MGR_EVT_LTE_PSM_UPDATE,
	MODEM_MGR_EVT_LTE_EDRX_UPDATE,
	MODEM_MGR_EVT_MODEM_DATA_READY,
	MODEM_MGR_EVT_BATTERY_DATA_READY,
	MODEM_MGR_EVT_DATE_TIME_OBTAINED,
	MODEM_MGR_EVT_SHUTDOWN_READY,
	MODEM_MGR_EVT_ERROR
};

/** @brief LTE cell information. */
struct modem_mgr_cell {
	/** E-UTRAN cell ID. */
	uint32_t cell_id;
	/** Tracking area code. */
	uint32_t tac;
};

/** @brief PSM information. */
struct modem_mgr_psm {
	/** Tracking Area Update interval [s]. -1 if the timer is disabled. */
	int tau;
	/** Active time [s]. -1 if the timer is disabled. */
	int active_time;
};

/** @brief eDRX information. */
struct modem_mgr_edrx {
	/** eDRX interval value [s] */
	float edrx;
	/** Paging time window [s] */
	float ptw;
};

/** @brief Modem event. */
struct modem_mgr_event {
	struct event_header header;
	enum modem_mgr_event_type type;
	union {
		struct cloud_data_modem modem;
		struct cloud_data_battery bat;
		struct modem_mgr_cell cell;
		struct modem_mgr_psm psm;
		struct modem_mgr_edrx edrx;
		int err;
	} data;
};

EVENT_TYPE_DECLARE(modem_mgr_event);

#ifdef __cplusplus
}
#endif

/**
 * @}
 */

#endif /* _MODEM_MGR_EVENT_H_ */
