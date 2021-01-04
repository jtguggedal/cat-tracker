#include "cloud/cloud_wrapper.h"
#include <zephyr.h>
#include <net/cloud.h>
#include <net/mqtt.h>

#define MODULE nrf_cloud_integration

#include <logging/log.h>
LOG_MODULE_REGISTER(MODULE, CONFIG_CLOUD_INTEGRATION_LOG_LEVEL);

#define NRF_CLOUD_SERVICE_INFO "{\"state\":{\"reported\":{\"device\": \
		{\"serviceInfo\":{\"ui\":[\"GPS\",\"HUMID\",\"TEMP\"]}}}}}"

#define REQUEST_DEVICE_STATE_STRING ""

static struct cloud_backend *cloud_backend;

static cloud_wrap_evt_handler_t wrapper_evt_handler;

/* TODO in this library: Use the direct API of the nRF Cloud library when the
 * required features are supported.
 */

static void cloud_wrapper_notify_event(const struct cloud_wrap_event *evt)
{
	if ((wrapper_evt_handler != NULL) && (evt != NULL)) {
		wrapper_evt_handler(evt);
	} else {
		LOG_ERR("Library event handler not registered, or empty event");
	}
}

static void send_service_info(void)
{
	int err;
	struct cloud_msg msg = {
		.buf = NRF_CLOUD_SERVICE_INFO,
		.len = sizeof(NRF_CLOUD_SERVICE_INFO) - 1,
		.qos = CLOUD_QOS_AT_MOST_ONCE,
		.endpoint.type = CLOUD_EP_STATE,
	};

	err = cloud_send(cloud_backend, &msg);
	if (err) {
		LOG_ERR("cloud_send, error: %d", err);
	}

	LOG_DBG("nRF Cloud service info sent: %s",
		log_strdup(NRF_CLOUD_SERVICE_INFO));
}

static void cloud_event_handler(
			const struct cloud_backend *const backend,
			const struct cloud_event *const evt, void *user_data)
{
	ARG_UNUSED(user_data);

	struct cloud_wrap_event cloud_wrap_evt = { 0 };
	bool notify = false;

	switch (evt->type) {
	case CLOUD_EVT_CONNECTING:
		LOG_DBG("NRF_CLOUD_EVT_CONNECTING");
		cloud_wrap_evt.type = CLOUD_WRAP_EVT_CONNECTING;
		notify = true;
		break;
	case CLOUD_EVT_CONNECTED:
		LOG_DBG("NRF_CLOUD_EVT_CONNECTED");
		send_service_info();
		break;
	case CLOUD_EVT_READY:
		LOG_DBG("NRF_CLOUD_EVT_READY");
		cloud_wrap_evt.type = CLOUD_WRAP_EVT_CONNECTED;
		notify = true;
		break;
	case CLOUD_EVT_DISCONNECTED:
		LOG_WRN("NRF_CLOUD_EVT_DISCONNECTED");
		cloud_wrap_evt.type = CLOUD_WRAP_EVT_DISCONNECTED;
		notify = true;
		break;
	case CLOUD_EVT_ERROR:
		LOG_ERR("NRF_CLOUD_EVT_ERROR");
		cloud_wrap_evt.type = CLOUD_WRAP_EVT_ERROR;
		notify = true;
		break;
	case CLOUD_EVT_FOTA_START:
		LOG_DBG("NRF_CLOUD_EVT_FOTA_START");
		cloud_wrap_evt.type = CLOUD_WRAP_EVT_FOTA_START;
		notify = true;
		break;
	case CLOUD_EVT_FOTA_ERASE_PENDING:
		LOG_DBG("NRF_CLOUD_EVT_FOTA_ERASE_PENDING");
		cloud_wrap_evt.type = CLOUD_WRAP_EVT_FOTA_ERASE_PENDING;
		notify = true;
		break;
	case CLOUD_EVT_FOTA_ERASE_DONE:
		LOG_DBG("NRF_CLOUD_EVT_FOTA_ERASE_DONE");
		cloud_wrap_evt.type = CLOUD_WRAP_EVT_FOTA_ERASE_DONE;
		notify = true;
		break;
	case CLOUD_EVT_FOTA_DONE:
		LOG_DBG("NRF_CLOUD_EVT_FOTA_DONE");
		cloud_wrap_evt.type = CLOUD_WRAP_EVT_FOTA_DONE;
		notify = true;
		break;
	case CLOUD_EVT_DATA_SENT:
		LOG_DBG("NRF_CLOUD_EVT_DATA_SENT");
		break;
	case CLOUD_EVT_DATA_RECEIVED:
		LOG_DBG("NRF_CLOUD_EVT_DATA_RECEIVED");
		cloud_wrap_evt.type = CLOUD_WRAP_EVT_DATA_RECEIVED;
		cloud_wrap_evt.data.buf = evt->data.msg.buf;
		cloud_wrap_evt.data.len = evt->data.msg.len;
		notify = true;
		break;
	case CLOUD_EVT_PAIR_REQUEST:
		LOG_DBG("NRF_CLOUD_EVT_PAIR_REQUEST");
		break;
	case CLOUD_EVT_PAIR_DONE:
		LOG_DBG("NRF_CLOUD_EVT_PAIR_DONE");
		break;
	case CLOUD_EVT_FOTA_DL_PROGRESS:
		/* Do not print to avoid spamming. */
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

	cloud_backend = cloud_get_binding("NRF_CLOUD");
	__ASSERT(cloud_backend != NULL, "%s cloud backend not found",
		 "NRF_CLOUD");

	err = cloud_init(cloud_backend, cloud_event_handler);
	if (err) {
		LOG_ERR("cloud_init, error: %d", err);
		return err;
	}

	LOG_DBG("********************************************");
	LOG_DBG(" The cat tracker has started");
	LOG_DBG(" Version:     %s", log_strdup(CONFIG_CAT_TRACKER_APP_VERSION));
	LOG_DBG(" Cloud:       %s", log_strdup("nRF Cloud"));
	LOG_DBG(" Endpoint:    %s",
		log_strdup(CONFIG_NRF_CLOUD_HOST_NAME));
	LOG_DBG("********************************************");

	wrapper_evt_handler = event_handler;

	return 0;
}

int cloud_wrap_connect(void)
{
	int err;

	err = cloud_connect(cloud_backend);
	if (err) {
		LOG_ERR("cloud_connect, error: %d", err);
		return err;
	}

	return 0;
}

int cloud_wrap_disconnect(void)
{
	int err;

	err = cloud_disconnect(cloud_backend);
	if (err) {
		LOG_ERR("cloud_disconnect, error: %d", err);
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

	struct cloud_msg msg = {
		.buf = buf,
		.len = len,
		.qos = MQTT_QOS_0_AT_MOST_ONCE,
		.endpoint.type = CLOUD_EP_STATE,
	};

	err = cloud_send(cloud_backend, &msg);
	if (err) {
		LOG_ERR("cloud_send, error: %d", err);
		return err;
	}

	return 0;
}

int cloud_wrap_data_send(char *buf, size_t len)
{
	int err;

	struct cloud_msg msg = {
		.buf = buf,
		.len = len,
		.qos = MQTT_QOS_0_AT_MOST_ONCE,
		.endpoint.type = CLOUD_EP_STATE,
	};

	err = cloud_send(cloud_backend, &msg);
	if (err) {
		LOG_ERR("cloud_send, error: %d", err);
		return err;
	}

	return 0;
}

int cloud_wrap_batch_send(char *buf, size_t len)
{
	int err;

	struct cloud_msg msg = {
		.buf = buf,
		.len = len,
		.qos = MQTT_QOS_0_AT_MOST_ONCE,
		.endpoint.type = CLOUD_EP_MSG
	};

	err = cloud_send(cloud_backend, &msg);
	if (err) {
		LOG_ERR("cloud_send, error: %d", err);
		return err;
	}

	return 0;
}

int cloud_wrap_ui_send(char *buf, size_t len)
{
	int err;

	struct cloud_msg msg = {
		.buf = buf,
		.len = len,
		.qos = MQTT_QOS_0_AT_MOST_ONCE,
		.endpoint.type = CLOUD_EP_MSG
	};

	err = cloud_send(cloud_backend, &msg);
	if (err) {
		LOG_ERR("cloud_send, error: %d", err);
		return err;
	}

	return 0;
}
