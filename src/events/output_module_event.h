/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#ifndef _OUTPUT_EVENT_H_
#define _OUTPUT_EVENT_H_

/**
 * @brief Output Event
 * @defgroup output_module_event Output Event
 * @{
 */

#include "event_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief output event types submitted by output module. */
enum output_module_event_types {
	OUTPUT_EVT_SHUTDOWN_READY,
	OUTPUT_EVT_ERROR
};

/** @brief output event. */
struct output_module_event {
	struct event_header header;
	enum output_module_event_types type;
	union {
		int err;
	} data;
};

EVENT_TYPE_DECLARE(output_module_event);

#ifdef __cplusplus
}
#endif

/**
 * @}
 */

#endif /* _OUTPUT_EVENT_H_ */
