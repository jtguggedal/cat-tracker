/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#ifndef _SENSOR_EVENT_H_
#define _SENSOR_EVENT_H_

/**
 * @brief Sensor Event
 * @defgroup sensor_module_event Sensor Event
 * @{
 */

#include "event_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ACCELEROMETER_AXIS_COUNT 3

/** @brief Sensor event types su bmitted by Sensor module. */
enum sensor_module_event_types {
	SENSOR_EVT_MOVEMENT_DATA_READY,
	SENSOR_EVT_ENVIRONMENTAL_DATA_READY,
	SENSOR_EVT_ENVIRONMENTAL_NOT_SUPPORTED,
	SENSOR_EVT_SHUTDOWN_READY,
	SENSOR_EVT_ERROR
};

struct sensor_module_data {
	int64_t timestamp;
	double temperature;
	double humidity;
};

struct sensor_module_accel_data {
	int64_t timestamp;
	double values[ACCELEROMETER_AXIS_COUNT];
};

/** @brief Sensor event. */
struct sensor_module_event {
	struct event_header header;
	enum sensor_module_event_types type;
	union {
		struct sensor_module_data sensors;
		struct sensor_module_accel_data accel;
		int err;
	} data;
};

EVENT_TYPE_DECLARE(sensor_module_event);

#ifdef __cplusplus
}
#endif

/**
 * @}
 */

#endif /* _SENSOR_EVENT_H_ */
