/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#ifndef _DATA_EVENT_H_
#define _DATA_EVENT_H_

/**
 * @brief Data Event
 * @defgroup data_module_event Data Event
 * @{
 */

#include "event_manager.h"
#include "cloud/cloud_codec/cloud_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Data event types submitted by Data module. */
enum data_module_event_types {
	DATA_EVT_DATA_READY,
	DATA_EVT_DATA_SEND,
	DATA_EVT_DATA_SEND_BATCH,
	DATA_EVT_UI_DATA_SEND,
	DATA_EVT_UI_DATA_READY,
	DATA_EVT_CONFIG_INIT,
	DATA_EVT_CONFIG_READY,
	DATA_EVT_CONFIG_SEND,
	DATA_EVT_CONFIG_GET,
	DATA_EVT_SHUTDOWN_READY,
	DATA_EVT_DATE_TIME_OBTAINED,
	DATA_EVT_ERROR
};

/** Struct containing pointer to array of data elements. */
struct data_module_data_buffers {
	char *buf;
	size_t len;
};

/** @brief Data event. */
struct data_module_event {
	struct event_header header;
	enum data_module_event_types type;

	union {
		struct data_module_data_buffers buffer;
		struct cloud_data_cfg cfg;
		struct cloud_data_ui ui;
		int err;
	} data;
};

EVENT_TYPE_DECLARE(data_module_event);

#ifdef __cplusplus
}
#endif

/**
 * @}
 */

#endif /* _DATA_EVENT_H_ */
