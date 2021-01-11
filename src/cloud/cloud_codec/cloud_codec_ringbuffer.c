#include <cloud_codec.h>
#include <zephyr.h>
#include <cJSON.h>
#include <cJSON_os.h>
#include <math.h>

#include <logging/log.h>
LOG_MODULE_REGISTER(cloud_codec_ringbuffer, CONFIG_CLOUD_CODEC_LOG_LEVEL);

#define ACCELEROMETER_TOTAL_AXIS 3

void cloud_codec_populate_sensor_buffer(
				struct cloud_data_sensors *sensor_buffer,
				struct cloud_data_sensors *new_sensor_data,
				int *head_sensor_buf)
{
	if (!new_sensor_data->queued) {
		return;
	}

	/* Go to start of buffer if end is reached. */
	*head_sensor_buf += 1;
	if (*head_sensor_buf == CONFIG_SENSOR_BUFFER_MAX) {
		*head_sensor_buf = 0;
	}

	sensor_buffer[*head_sensor_buf] = *new_sensor_data;

	LOG_DBG("Entry: %d of %d in sensor buffer filled", *head_sensor_buf,
		CONFIG_SENSOR_BUFFER_MAX - 1);
}

void cloud_codec_populate_ui_buffer(struct cloud_data_ui *ui_buffer,
				   struct cloud_data_ui *new_ui_data,
				   int *head_ui_buf)
{

	if (!new_ui_data->queued) {
		return;
	}

	/* Go to start of buffer if end is reached. */
	*head_ui_buf += 1;
	if (*head_ui_buf == CONFIG_UI_BUFFER_MAX) {
		*head_ui_buf = 0;
	}

	ui_buffer[*head_ui_buf] = *new_ui_data;

	LOG_DBG("Entry: %d of %d in UI buffer filled", *head_ui_buf,
		CONFIG_UI_BUFFER_MAX - 1);
}

void cloud_codec_populate_accel_buffer(
				struct cloud_data_accelerometer *mov_buf,
				struct cloud_data_accelerometer *new_accel_data,
				int *head_mov_buf)
{
	if (!new_accel_data->queued) {
		return;
	}

	/* Go to start of buffer if end is reached. */
	*head_mov_buf += 1;
	if (*head_mov_buf == CONFIG_ACCEL_BUFFER_MAX) {
		*head_mov_buf = 0;
	}

	mov_buf[*head_mov_buf] = *new_accel_data;

	LOG_DBG("Entry: %d of %d in movement buffer filled", *head_mov_buf,
		CONFIG_ACCEL_BUFFER_MAX - 1);
}

void cloud_codec_populate_bat_buffer(struct cloud_data_battery *bat_buffer,
				     struct cloud_data_battery *new_bat_data,
				     int *head_bat_buf)
{
	if (!new_bat_data->queued) {
		return;
	}

	/* Go to start of buffer if end is reached. */
	*head_bat_buf += 1;
	if (*head_bat_buf == CONFIG_BAT_BUFFER_MAX) {
		*head_bat_buf = 0;
	}

	bat_buffer[*head_bat_buf] = *new_bat_data;

	LOG_DBG("Entry: %d of %d in battery buffer filled", *head_bat_buf,
		CONFIG_BAT_BUFFER_MAX - 1);
}

void cloud_codec_populate_gps_buffer(struct cloud_data_gps *gps_buffer,
				    struct cloud_data_gps *new_gps_data,
				    int *head_gps_buf)
{
	if (!new_gps_data->queued) {
		return;
	}

	/* Go to start of buffer if end is reached. */
	*head_gps_buf += 1;
	if (*head_gps_buf == CONFIG_GPS_BUFFER_MAX) {
		*head_gps_buf = 0;
	}

	gps_buffer[*head_gps_buf] = *new_gps_data;

	LOG_DBG("Entry: %d of %d in GPS buffer filled", *head_gps_buf,
		CONFIG_GPS_BUFFER_MAX - 1);
}

void cloud_codec_populate_modem_dynamic_buffer(
				struct cloud_data_modem_dynamic *modem_buffer,
				struct cloud_data_modem_dynamic *new_modem_data,
				int *head_modem_buf)
{
	if (!new_modem_data->queued) {
		return;
	}

	/* Go to start of buffer if end is reached. */
	*head_modem_buf += 1;
	if (*head_modem_buf == CONFIG_MODEM_BUFFER_DYNAMIC_MAX) {
		*head_modem_buf = 0;
	}

	modem_buffer[*head_modem_buf] = *new_modem_data;

	LOG_DBG("Entry: %d of %d in dynamic modem buffer filled",
		*head_modem_buf, CONFIG_MODEM_BUFFER_DYNAMIC_MAX - 1);
}
