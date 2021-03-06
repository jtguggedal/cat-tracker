#
# Copyright (c) 2019-2020 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
#

cmake_minimum_required(VERSION 3.8.2)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(cat_tracker)

# NORDIC SDK APP START
target_sources(app PRIVATE src/main.c)
# NORDIC SDK APP END

# Include source directory
zephyr_include_directories(src)

# Application directories
add_subdirectory(src/modules)
add_subdirectory(src/events)
add_subdirectory(src/cloud/cloud_codec)

add_subdirectory_ifdef(CONFIG_CLOUD_MODULE src/cloud)
add_subdirectory_ifdef(CONFIG_UI_MODULE src/led)
add_subdirectory_ifdef(CONFIG_SENSOR_MODULE src/ext_sensors)
add_subdirectory_ifdef(CONFIG_WATCHDOG_APPLICATION src/watchdog)
