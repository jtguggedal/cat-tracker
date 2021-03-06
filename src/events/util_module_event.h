/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#ifndef _UTIL_MODULE_EVENT_H_
#define _UTIL_MODULE_EVENT_H_

/**
 * @brief Util module event
 * @defgroup util_module_event Util module event
 * @{
 */

#include "event_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

enum util_module_event_type {
	UTIL_EVT_SHUTDOWN_REQUEST
};

/** @brief Util event. */
struct util_module_event {
	struct event_header header;
	enum util_module_event_type type;
};

EVENT_TYPE_DECLARE(util_module_event);

#ifdef __cplusplus
}
#endif

/**
 * @}
 */

#endif /* _UTIL_MODULE_EVENT_H_ */
