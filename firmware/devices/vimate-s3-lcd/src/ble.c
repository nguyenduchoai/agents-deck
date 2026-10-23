// NimBLE peripheral with one service: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E,
// RX 6E400002 (write, write without response) and TX 6E400003 (notify).
#include "ble.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#define NUS_UUID(n) BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, n, 0x00, 0x40, 0x6e)
#define RX_BUF 4096

static const char *TAG = "ble";
static const ble_uuid128_t SVC = NUS_UUID(0x01), RX = NUS_UUID(0x02), TX = NUS_UUID(0x03);
static StreamBufferHandle_t rx;
static uint16_t tx_handle, mtu = 23;
static volatile uint16_t conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool listening; // the central subscribed to TX
static uint8_t addr_type;

static int on_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
	(void)conn_handle, (void)attr_handle, (void)arg;
	if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR)
		return BLE_ATT_ERR_UNLIKELY;
	uint8_t buf[128];
	uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
	for (uint16_t off = 0; off < len;) {
		uint16_t n = len - off < sizeof buf ? len - off : sizeof buf;
		os_mbuf_copydata(ctxt->om, off, n, buf);
		xStreamBufferSend(rx, buf, n, 0); // a full buffer drops bytes; Deck Link resyncs
		off += n;
	}
	return 0;
}

static const struct ble_gatt_svc_def services[] = {
	{
		.type = BLE_GATT_SVC_TYPE_PRIMARY,
		.uuid = &SVC.u,
		.characteristics =
			(struct ble_gatt_chr_def[]){
				{.uuid = &RX.u, .access_cb = on_access, .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP},
				{.uuid = &TX.u, .access_cb = on_access, .flags = BLE_GATT_CHR_F_NOTIFY, .val_handle = &tx_handle},
				{0},
			},
	},
	{0},
};

static int on_gap(struct ble_gap_event *e, void *arg);

static void advertise(void)
{
	struct ble_hs_adv_fields f = {0};
	const char *name = ble_svc_gap_device_name();
	f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
	f.name = (const uint8_t *)name;
	f.name_len = strlen(name);
	f.name_is_complete = 1;
	ble_gap_adv_set_fields(&f);
	struct ble_hs_adv_fields rsp = {0}; // the service in the scan response: it doesn't fit beside the name
	rsp.uuids128 = &SVC;
	rsp.num_uuids128 = 1;
	rsp.uuids128_is_complete = 1;
	ble_gap_adv_rsp_set_fields(&rsp);
	struct ble_gap_adv_params p = {.conn_mode = BLE_GAP_CONN_MODE_UND, .disc_mode = BLE_GAP_DISC_MODE_GEN};
	int rc = ble_gap_adv_start(addr_type, NULL, BLE_HS_FOREVER, &p, on_gap, NULL);
	if (rc && rc != BLE_HS_EALREADY)
		ESP_LOGW(TAG, "advertising failed: %d", rc);
}

static int on_gap(struct ble_gap_event *e, void *arg)
{
	(void)arg;
	switch (e->type) {
	case BLE_GAP_EVENT_CONNECT:
		if (e->connect.status == 0)
			conn = e->connect.conn_handle;
		else
			advertise();
		break;
	case BLE_GAP_EVENT_DISCONNECT:
		conn = BLE_HS_CONN_HANDLE_NONE;
		listening = false;
		mtu = 23;
		advertise();
		break;
	case BLE_GAP_EVENT_SUBSCRIBE:
		if (e->subscribe.attr_handle == tx_handle)
			listening = e->subscribe.cur_notify;
		break;
	case BLE_GAP_EVENT_MTU:
		mtu = e->mtu.value;
		break;
	case BLE_GAP_EVENT_ADV_COMPLETE:
		advertise();
		break;
	}
	return 0;
}

static void on_sync(void)
{
	ble_hs_id_infer_auto(0, &addr_type);
	advertise();
}

static void host_task(void *arg)
{
	(void)arg;
	nimble_port_run();
	nimble_port_freertos_deinit();
}

void ble_start(const char *name)
{
	esp_err_t err = nvs_flash_init(); // the controller keeps its calibration there
	if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		nvs_flash_erase();
		err = nvs_flash_init();
	}
	ESP_ERROR_CHECK(err);
	rx = xStreamBufferCreate(RX_BUF, 1);
	ESP_ERROR_CHECK(nimble_port_init());
	ble_hs_cfg.sync_cb = on_sync;
	ble_svc_gap_init();
	ble_svc_gatt_init();
	ESP_ERROR_CHECK(ble_gatts_count_cfg(services));
	ESP_ERROR_CHECK(ble_gatts_add_svcs(services));
	ble_svc_gap_device_name_set(name);
	nimble_port_freertos_init(host_task);
	ESP_LOGI(TAG, "advertising as %s", name);
}

bool ble_connected(void) { return conn != BLE_HS_CONN_HANDLE_NONE && listening; }

size_t ble_read(uint8_t *buf, size_t len, TickType_t wait) { return xStreamBufferReceive(rx, buf, len, wait); }

size_t ble_write(const uint8_t *data, size_t len)
{
	size_t sent = 0;
	while (sent < len && ble_connected()) {
		size_t n = len - sent < (size_t)(mtu - 3) ? len - sent : (size_t)(mtu - 3);
		struct os_mbuf *om = ble_hs_mbuf_from_flat(data + sent, n);
		if (om && ble_gatts_notify_custom(conn, tx_handle, om) == 0) {
			sent += n;
			continue;
		}
		vTaskDelay(pdMS_TO_TICKS(5)); // the stack's buffers are full: let it send
	}
	return sent;
}
