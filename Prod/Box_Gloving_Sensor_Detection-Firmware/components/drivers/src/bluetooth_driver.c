/**
 * @file bluetooth_driver.c
 * @brief One service/characteristic NimBLE peripheral with explicit v2 handshake.
 */
#include "bluetooth_driver.h"

#include <string.h>

#include "esp_log.h"
#include "esp_nimble_hci.h"
#include "freertos/FreeRTOS.h"
#include "glove_protocol.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "BLE";
static uint8_t own_addr_type;
uint16_t glove_data_val_handle;
static volatile bool ble_connected;
static volatile bool notify_enabled;
static volatile bool protocol_active;
static volatile uint16_t current_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static volatile uint16_t active_session_id;
static volatile uint16_t next_session_id = 1u;
static volatile uint8_t active_capabilities;
static volatile uint16_t notification_payload_max = GLOVE_PROTOCOL_MIN_ATT_PAYLOAD;
static portMUX_TYPE ble_state_lock = portMUX_INITIALIZER_UNLOCKED;

/* 12345678-1234-5678-1234-56789abcdef0 */
static const ble_uuid128_t glove_service_uuid =
    BLE_UUID128_INIT(0xf0, 0xde, 0xbc, 0x9a, 0x78, 0x56, 0x34, 0x12,
                     0x78, 0x56, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12);
/* 12345678-1234-5678-1234-56789abcdef1 */
static const ble_uuid128_t glove_data_char_uuid =
    BLE_UUID128_INIT(0xf1, 0xde, 0xbc, 0x9a, 0x78, 0x56, 0x34, 0x12,
                     0x78, 0x56, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12);

static void start_advertising(void);

static void log_connection_parameters(uint16_t conn_handle, const char *stage)
{
    struct ble_gap_conn_desc desc = {0};
    const int rc = ble_gap_conn_find(conn_handle, &desc);
    if (rc != 0) {
        ESP_LOGW(TAG, "%s connection descriptor unavailable: %d", stage, rc);
        return;
    }
    ESP_LOGI(TAG, "%s: interval=%u units (%lu us), latency=%u, supervision=%u ms",
             stage, (unsigned)desc.conn_itvl, (unsigned long)desc.conn_itvl * 1250UL,
             (unsigned)desc.conn_latency, (unsigned)desc.supervision_timeout * 10u);
}

static uint16_t allocate_session_id_locked(void)
{
    uint16_t session = next_session_id++;
    if (session == 0u) {
        session = next_session_id++;
    }
    return session;
}

static void reset_protocol_session_locked(void)
{
    protocol_active = false;
    active_capabilities = 0u;
    active_session_id = allocate_session_id_locked();
}

static void update_notification_payload_limit(void)
{
    uint16_t conn_handle;
    portENTER_CRITICAL(&ble_state_lock);
    conn_handle = current_conn_handle;
    portEXIT_CRITICAL(&ble_state_lock);
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        portENTER_CRITICAL(&ble_state_lock);
        notification_payload_max = GLOVE_PROTOCOL_MIN_ATT_PAYLOAD;
        portEXIT_CRITICAL(&ble_state_lock);
        return;
    }

    const uint16_t mtu = ble_att_mtu(conn_handle);
    uint16_t payload = mtu > 3u ? (uint16_t)(mtu - 3u) : GLOVE_PROTOCOL_MIN_ATT_PAYLOAD;
    if (payload < GLOVE_PROTOCOL_MIN_ATT_PAYLOAD) {
        payload = GLOVE_PROTOCOL_MIN_ATT_PAYLOAD;
    }
    if (payload > GLOVE_BLE_MAX_NOTIFICATION_PAYLOAD) {
        payload = GLOVE_BLE_MAX_NOTIFICATION_PAYLOAD;
    }
    portENTER_CRITICAL(&ble_state_lock);
    if (current_conn_handle == conn_handle) {
        notification_payload_max = payload;
    }
    portEXIT_CRITICAL(&ble_state_lock);
}

static int glove_data_access_cb(
    uint16_t conn_handle,
    uint16_t attr_handle,
    struct ble_gatt_access_ctxt *ctxt,
    void *arg
)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        static const char ready[] = "Smart Boxing Glove protocol v2; write GB\\x02<capabilities>";
        const int rc = os_mbuf_append(ctxt->om, ready, sizeof(ready) - 1u);
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    const uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    bool subscribed;
    portENTER_CRITICAL(&ble_state_lock);
    subscribed = notify_enabled && ble_connected && current_conn_handle == conn_handle;
    portEXIT_CRITICAL(&ble_state_lock);
    if (len != GLOVE_PROTOCOL_HELLO_SIZE || !subscribed) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    uint8_t hello[GLOVE_PROTOCOL_HELLO_SIZE] = {0};
    if (os_mbuf_copydata(ctxt->om, 0u, len, hello) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint8_t capabilities = 0u;
    if (!glove_protocol_parse_hello(hello, sizeof(hello), &capabilities)) {
        ESP_LOGW(TAG, "Rejected malformed v2 HELLO");
        return BLE_ATT_ERR_UNLIKELY;
    }

    update_notification_payload_limit();
    uint16_t session_id;
    uint16_t payload_limit;
    portENTER_CRITICAL(&ble_state_lock);
    if (!notify_enabled || !ble_connected || current_conn_handle != conn_handle) {
        portEXIT_CRITICAL(&ble_state_lock);
        return BLE_ATT_ERR_UNLIKELY;
    }
    /* Invalidate first so readers cannot combine an old session with new caps. */
    protocol_active = false;
    active_capabilities = 0u;
    active_session_id = allocate_session_id_locked();
    active_capabilities = capabilities;
    protocol_active = true;
    session_id = active_session_id;
    payload_limit = notification_payload_max;
    portEXIT_CRITICAL(&ble_state_lock);
    ESP_LOGI(TAG, "v2 session %u enabled, capabilities=0x%02X, payload=%u",
             session_id, capabilities, payload_limit);
    return 0;
}

static const struct ble_gatt_svc_def gatt_svr_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &glove_service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &glove_data_char_uuid.u,
                .access_cb = glove_data_access_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &glove_data_val_handle,
            },
            {0},
        },
    },
    {0},
};

static void ble_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "Connection attempt failed: %d", event->connect.status);
            start_advertising();
            return 0;
        }
        portENTER_CRITICAL(&ble_state_lock);
        current_conn_handle = event->connect.conn_handle;
        ble_connected = true;
        notify_enabled = false;
        notification_payload_max = GLOVE_PROTOCOL_MIN_ATT_PAYLOAD;
        reset_protocol_session_locked();
        portEXIT_CRITICAL(&ble_state_lock);
        update_notification_payload_limit();
        log_connection_parameters(event->connect.conn_handle, "Connected");
        ESP_LOGI(TAG, "Client connected, awaiting CCCD and v2 HELLO");
        {
            const struct ble_gap_upd_params params = {
                .itvl_min = 6,
                .itvl_max = 12,
                .latency = 0,
                .supervision_timeout = 400,
                .min_ce_len = 0,
                .max_ce_len = 0,
            };
            const int rc = ble_gap_update_params(current_conn_handle, &params);
            if (rc != 0) {
                ESP_LOGW(TAG, "Fast connection interval request failed: %d", rc);
            }
        }
        {
            const int rc = ble_gap_set_data_len(event->connect.conn_handle, 251u, 2120u);
            if (rc != 0) {
                ESP_LOGW(TAG, "DLE 251-octet request failed: %d", rc);
            }
        }
        {
            const int rc = ble_gap_set_prefered_le_phy(event->connect.conn_handle,
                                                        BLE_GAP_LE_PHY_2M_MASK,
                                                        BLE_GAP_LE_PHY_2M_MASK, 0u);
            if (rc != 0) {
                ESP_LOGW(TAG, "2M PHY request failed (1M remains usable): %d", rc);
            }
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "Client disconnected: reason=%d", event->disconnect.reason);
        portENTER_CRITICAL(&ble_state_lock);
        current_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        ble_connected = false;
        notify_enabled = false;
        notification_payload_max = GLOVE_PROTOCOL_MIN_ATT_PAYLOAD;
        reset_protocol_session_locked();
        portEXIT_CRITICAL(&ble_state_lock);
        start_advertising();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == glove_data_val_handle) {
            bool enabled;
            portENTER_CRITICAL(&ble_state_lock);
            notify_enabled = event->subscribe.cur_notify != 0;
            enabled = notify_enabled;
            if (!notify_enabled) {
                reset_protocol_session_locked();
            }
            portEXIT_CRITICAL(&ble_state_lock);
            ESP_LOGI(TAG, "Notifications %s", enabled ? "enabled" : "disabled");
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        update_notification_payload_limit();
        ESP_LOGI(TAG, "MTU=%u notification payload=%u",
                 event->mtu.value, glove_ble_notification_payload_max());
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE:
        if (event->conn_update.status == 0) {
            log_connection_parameters(event->conn_update.conn_handle, "Connection updated");
        } else {
            ESP_LOGW(TAG, "Connection update failed: %d", event->conn_update.status);
        }
        return 0;

    case BLE_GAP_EVENT_DATA_LEN_CHG:
        ESP_LOGI(TAG, "DLE actual tx=%u/%uus rx=%u/%uus",
                 event->data_len_chg.max_tx_octets, event->data_len_chg.max_tx_time,
                 event->data_len_chg.max_rx_octets, event->data_len_chg.max_rx_time);
        return 0;

    case BLE_GAP_EVENT_PHY_UPDATE_COMPLETE:
        ESP_LOGI(TAG, "PHY update status=%d tx=%u rx=%u",
                 event->phy_updated.status, event->phy_updated.tx_phy, event->phy_updated.rx_phy);
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        start_advertising();
        return 0;

    default:
        return 0;
    }
}

static void start_advertising(void)
{
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = &glove_service_uuid;
    fields.num_uuids128 = 1u;
    fields.uuids128_is_complete = 1u;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields failed: %d", rc);
        return;
    }

    struct ble_hs_adv_fields response = {0};
    response.name = (const uint8_t *)GLOVE_BLE_DEVICE_NAME;
    response.name_len = strlen(GLOVE_BLE_DEVICE_NAME);
    response.name_is_complete = 1u;
    rc = ble_gap_adv_rsp_set_fields(&response);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_rsp_set_fields failed: %d", rc);
        return;
    }

    const struct ble_gap_adv_params parameters = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min = 0x20,
        .itvl_max = 0x40,
    };
    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER,
                           &parameters, gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start failed: %d", rc);
    }
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) {
        rc = ble_hs_id_infer_auto(0, &own_addr_type);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE address setup failed: %d", rc);
        return;
    }
    start_advertising();
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE host reset: %d", reason);
    portENTER_CRITICAL(&ble_state_lock);
    current_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    ble_connected = false;
    notify_enabled = false;
    notification_payload_max = GLOVE_PROTOCOL_MIN_ATT_PAYLOAD;
    reset_protocol_session_locked();
    portEXIT_CRITICAL(&ble_state_lock);
}

esp_err_t bluetooth_init(void)
{
    int rc = nimble_port_init();
    if (rc != 0) {
        ESP_LOGE(TAG, "nimble_port_init failed: %d", rc);
        return ESP_FAIL;
    }

    ble_svc_gap_init();
    ble_svc_gatt_init();
    rc = ble_gatts_count_cfg(gatt_svr_svcs);
    if (rc == 0) {
        rc = ble_gatts_add_svcs(gatt_svr_svcs);
    }
    if (rc == 0) {
        rc = ble_svc_gap_device_name_set(GLOVE_BLE_DEVICE_NAME);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "NimBLE GATT setup failed: %d", rc);
        return ESP_FAIL;
    }

    portENTER_CRITICAL(&ble_state_lock);
    reset_protocol_session_locked();
    portEXIT_CRITICAL(&ble_state_lock);
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    nimble_port_freertos_init(ble_task);
    ESP_LOGI(TAG, "NimBLE peripheral initialized");
    return ESP_OK;
}

void blueetooth_init(void)
{
    (void)bluetooth_init();
}

bool glove_ble_can_notify(void)
{
    bool result;
    portENTER_CRITICAL(&ble_state_lock);
    result = ble_connected && notify_enabled && current_conn_handle != BLE_HS_CONN_HANDLE_NONE;
    portEXIT_CRITICAL(&ble_state_lock);
    return result;
}

bool glove_ble_protocol_is_active(void)
{
    glove_ble_session_t session = {0};
    return glove_ble_get_session(&session);
}

uint16_t glove_ble_session_id(void)
{
    uint16_t result;
    portENTER_CRITICAL(&ble_state_lock);
    result = active_session_id;
    portEXIT_CRITICAL(&ble_state_lock);
    return result;
}

uint8_t glove_ble_capabilities(void)
{
    uint8_t result;
    portENTER_CRITICAL(&ble_state_lock);
    result = active_capabilities;
    portEXIT_CRITICAL(&ble_state_lock);
    return result;
}

uint16_t glove_ble_notification_payload_max(void)
{
    uint16_t result;
    portENTER_CRITICAL(&ble_state_lock);
    result = notification_payload_max;
    portEXIT_CRITICAL(&ble_state_lock);
    return result;
}

uint16_t glove_ble_connection_handle(void)
{
    uint16_t result;
    portENTER_CRITICAL(&ble_state_lock);
    result = current_conn_handle;
    portEXIT_CRITICAL(&ble_state_lock);
    return result;
}

bool glove_ble_get_session(glove_ble_session_t *session)
{
    if (session == NULL) {
        return false;
    }
    portENTER_CRITICAL(&ble_state_lock);
    session->session_id = active_session_id;
    session->connection_handle = current_conn_handle;
    session->notification_payload_max = notification_payload_max;
    session->capabilities = active_capabilities;
    session->active = protocol_active && ble_connected && notify_enabled &&
                      current_conn_handle != BLE_HS_CONN_HANDLE_NONE;
    portEXIT_CRITICAL(&ble_state_lock);
    return session->active;
}

bool glove_ble_session_matches(uint16_t expected_session_id)
{
    glove_ble_session_t session = {0};
    return glove_ble_get_session(&session) && session.session_id == expected_session_id;
}
