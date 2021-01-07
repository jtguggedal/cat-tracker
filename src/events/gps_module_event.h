/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#ifndef _GPS_EVENT_H_
#define _GPS_EVENT_H_

/**
 * @brief GPS Event
 * @defgroup gps_module_event GPS Event
 * @{
 */

#include <drivers/gps.h>

#include "event_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief GPS event types submitted by GPS module. */
enum gps_module_event_type {
	GPS_EVT_DATA_READY,
	GPS_EVT_TIMEOUT,
	GPS_EVT_ACTIVE,
	GPS_EVT_INACTIVE,
	GPS_EVT_SHUTDOWN_READY,
	GPS_EVT_AGPS_NEEDED,
	GPS_EVT_ERROR_CODE,
};

struct gps_module_data {
	/** GPS data timestamp. UNIX milliseconds. */
	int64_t gps_ts;
	/** Longitude */
	double longi;
	/** Latitude */
	double lat;
	/** Altitude above WGS-84 ellipsoid in meters. */
	float alt;
	/** Accuracy in (2D 1-sigma) in meters. */
	float acc;
	/** Horizontal speed in meters. */
	float spd;
	/** Heading of movement in degrees. */
	float hdg;
};

/** @brief GPS event. */
struct gps_module_event {
	struct event_header header;
	enum gps_module_event_type type;

	union {
		struct gps_module_data gps;
		struct gps_agps_request agps_request;
		int err;
	} data;
};

EVENT_TYPE_DECLARE(gps_module_event);

#ifdef __cplusplus
}
#endif

/**
 * @}
 */

#endif /* _GPS_EVENT_H_ */
