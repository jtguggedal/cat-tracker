#include "cloud/cloud_wrapper.h"
#include <zephyr.h>
#include <net/nrf_cloud.h>
#include <net/mqtt.h>

#define MODULE nrf_cloud_integration

#include <logging/log.h>
LOG_MODULE_REGISTER(MODULE, CONFIG_CLOUD_INTEGRATION_LOG_LEVEL);

#define NRF_CLOUD_SERVICE_INFO "{\"state\":{\"reported\":{\"device\": \
		{\"serviceInfo\":{\"ui\":[\"GPS\",\"HUMID\",\"TEMP\"]}}}}}"

#define REQUEST_DEVICE_STATE_STRING ""

static cloud_wrap_evt_handler_t wrapper_evt_handler;

static void cloud_wrapper_notify_event(const struct cloud_wrap_event *evt)
{
	if ((wrapper_evt_handler != NULL) && (evt != NULL)) {
		wrapper_evt_handler(evt);
	} else {
		LOG_ERR("Library event handler not registered, or empty event");
	}
}

static int send_service_info(void)
{
	int err;

	struct nrf_cloud_tx_data msg = {
		.data.ptr = NRF_CLOUD_SERVICE_INFO,
		.data.len = sizeof(NRF_CLOUD_SERVICE_INFO) - 1,
		.qos = MQTT_QOS_0_AT_MOST_ONCE,
		.topic_type = NRF_CLOUD_TOPIC_STATE,
	};

	err = nrf_cloud_send(&msg);
	if (err) {
		LOG_ERR("nrf_cloud_send, error: %d", err);
		return err;
	}

	LOG_DBG("nRF Cloud service info sent: %s",
		log_strdup(NRF_CLOUD_SERVICE_INFO));

	return 0;
}

static void nrf_cloud_event_handler(const struct nrf_cloud_evt *evt)
{
	struct cloud_wrap_event cloud_wrap_evt = { 0 };
	bool notify = false;

	switch (evt->type) {
	case NRF_CLOUD_EVT_TRANSPORT_CONNECTING:
		LOG_DBG("NRF_CLOUD_EVT_TRANSPORT_CONNECTING");
		break;
	case NRF_CLOUD_EVT_TRANSPORT_CONNECTED:
		LOG_DBG("NRF_CLOUD_EVT_TRANSPORT_CONNECTED");
		break;
	case NRF_CLOUD_EVT_READY:
		LOG_DBG("NRF_CLOUD_EVT_READY");
		cloud_wrap_evt.type = CLOUD_WRAP_EVT_CONNECTED;
		notify = true;

		int err = send_service_info();

		if (err) {
			LOG_ERR("Failed to send nRF Cloud service information");
			cloud_wrap_evt.type = CLOUD_WRAP_EVT_ERROR;
		}
		break;
	case NRF_CLOUD_EVT_TRANSPORT_DISCONNECTED:
		LOG_WRN("NRF_CLOUD_EVT_TRANSPORT_DISCONNECTED");
		cloud_wrap_evt.type = CLOUD_WRAP_EVT_DISCONNECTED;
		notify = true;
		break;
	case NRF_CLOUD_EVT_ERROR:
		LOG_ERR("NRF_CLOUD_EVT_ERROR");
		cloud_wrap_evt.type = CLOUD_WRAP_EVT_ERROR;
		notify = true;
		break;
	case NRF_CLOUD_EVT_SENSOR_ATTACHED:
		LOG_DBG("NRF_CLOUD_EVT_SENSOR_ATTACHED");
		break;
	case NRF_CLOUD_EVT_SENSOR_DATA_ACK:
		LOG_DBG("NRF_CLOUD_EVT_SENSOR_DATA_ACK");
		break;
	case NRF_CLOUD_EVT_FOTA_DONE:
		LOG_DBG("NRF_CLOUD_EVT_FOTA_DONE");
		cloud_wrap_evt.type = CLOUD_WRAP_EVT_FOTA_DONE;
		notify = true;
		break;
	case NRF_CLOUD_EVT_RX_DATA:
		LOG_DBG("NRF_CLOUD_EVT_RX_DATA");
		cloud_wrap_evt.type = CLOUD_WRAP_EVT_DATA_RECEIVED;
		cloud_wrap_evt.data.buf = (char *)evt->data.ptr;
		cloud_wrap_evt.data.len = evt->data.len;
		notify = true;
		break;
	case NRF_CLOUD_EVT_USER_ASSOCIATION_REQUEST:
		LOG_DBG("NRF_CLOUD_EVT_USER_ASSOCIATION_REQUEST");
		break;
	case NRF_CLOUD_EVT_USER_ASSOCIATED:
		LOG_DBG("NRF_CLOUD_EVT_USER_ASSOCIATED");
		break;
	default:
		LOG_ERR("Unknown nRF Cloud event type: %d", evt->type);
		break;
	}

	if (notify) {
		cloud_wrapper_notify_event(&cloud_wrap_evt);
	}
}

int cloud_wrap_init(cloud_wrap_evt_handler_t event_handler)
{
	int err;

	struct nrf_cloud_init_param config = {
		.event_handler = nrf_cloud_event_handler,
	};

	err = nrf_cloud_init(&config);
	if (err) {
		LOG_ERR("nrf_cloud_init, error: %d", err);
		return err;
	}

	LOG_DBG("********************************************");
	LOG_DBG(" The cat tracker has started");
	LOG_DBG(" Version:     %s", log_strdup(CONFIG_CAT_TRACKER_APP_VERSION));
	LOG_DBG(" Cloud:       %s", log_strdup("nRF Cloud"));
	LOG_DBG(" Endpoint:    %s", log_strdup(CONFIG_NRF_CLOUD_HOST_NAME));
	LOG_DBG("********************************************");

	wrapper_evt_handler = event_handler;

	return 0;
}

int cloud_wrap_connect(void)
{
	int err;

	err = nrf_cloud_connect(NULL);
	if (err) {
		LOG_ERR("nrf_cloud_connect, error: %d", err);
		return err;
	}

	return 0;
}

int cloud_wrap_disconnect(void)
{
	int err;

	err = nrf_cloud_disconnect();
	if (err) {
		LOG_ERR("nrf_cloud_disconnect, error: %d", err);
		return err;
	}

	return 0;
}

int cloud_wrap_state_get(void)
{
	/* Not supported by nRF Cloud */
	return 0;
}

int cloud_wrap_state_send(char *buf, size_t len)
{
	int err;

	struct nrf_cloud_tx_data msg = {
		.data.ptr = buf,
		.data.len = len,
		.qos = MQTT_QOS_0_AT_MOST_ONCE,
		.topic_type = NRF_CLOUD_TOPIC_STATE,
	};

	err = nrf_cloud_send(&msg);
	if (err) {
		LOG_ERR("nrf_cloud_send, error: %d", err);
		return err;
	}

	return 0;
}

int cloud_wrap_data_send(char *buf, size_t len)
{
	int err;

	struct nrf_cloud_tx_data msg = {
		.data.ptr = buf,
		.data.len = len,
		.qos = MQTT_QOS_0_AT_MOST_ONCE,
		.topic_type = NRF_CLOUD_TOPIC_STATE,
	};

	err = nrf_cloud_send(&msg);
	if (err) {
		LOG_ERR("nrf_cloud_send, error: %d", err);
		return err;
	}

	return 0;
}

int cloud_wrap_batch_send(char *buf, size_t len)
{
	int err;

	struct nrf_cloud_tx_data msg = {
		.data.ptr = buf,
		.data.len = len,
		.qos = MQTT_QOS_0_AT_MOST_ONCE,
		.topic_type = NRF_CLOUD_TOPIC_MESSAGE,
	};

	err = nrf_cloud_send(&msg);
	if (err) {
		LOG_ERR("nrf_cloud_send, error: %d", err);
		return err;
	}

	return 0;
}

int cloud_wrap_ui_send(char *buf, size_t len)
{
	int err;

	struct nrf_cloud_tx_data msg = {
		.data.ptr = buf,
		.data.len = len,
		.qos = MQTT_QOS_0_AT_MOST_ONCE,
		.topic_type = NRF_CLOUD_TOPIC_STATE,
	};

	err = nrf_cloud_send(&msg);
	if (err) {
		LOG_ERR("nrf_cloud_send, error: %d", err);
		return err;
	}

	return 0;
}
