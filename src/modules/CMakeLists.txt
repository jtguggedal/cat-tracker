#
# Copyright (c) 2020 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
#

zephyr_include_directories(.)
target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/modules_common.c)
target_sources_ifdef(CONFIG_CLOUD_MODULE app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/cloud_module.c)
target_sources_ifdef(CONFIG_GPS_MODULE app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/gps_module.c)
target_sources_ifdef(CONFIG_MODEM_MODULE app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/modem_module.c)
target_sources_ifdef(CONFIG_UI_MODULE app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/ui_module.c)
target_sources_ifdef(CONFIG_SENSOR_MODULE app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/sensor_module.c)
target_sources_ifdef(CONFIG_DATA_MODULE app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/data_module.c)
target_sources_ifdef(CONFIG_UTIL_MODULE app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/util_module.c)
