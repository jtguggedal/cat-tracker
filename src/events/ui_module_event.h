/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#ifndef _UI_EVENT_H_
#define _UI_EVENT_H_

/**
 * @brief UI Event
 * @defgroup ui_module_event UI Event
 * @{
 */

#include "event_manager.h"
#include "cloud/cloud_codec/cloud_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief UI event types submitted by UI module. */
enum ui_module_event_types {
	UI_EVT_BUTTON_DATA_READY,
	UI_EVT_SHUTDOWN_READY,
	UI_EVT_ERROR
};

/** @brief UI event. */
struct ui_module_event {
	struct event_header header;
	enum ui_module_event_types type;

	union {
		struct cloud_data_ui ui;
		int err;
	} data;
};

EVENT_TYPE_DECLARE(ui_module_event);

#ifdef __cplusplus
}
#endif

/**
 * @}
 */

#endif /* _UI_EVENT_H_ */
