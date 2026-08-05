#ifndef BLUETOOTH_DRIVER
#define BLUETOOTH_DRIVER

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_nimble_hci.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/util/util.h"
#include "host/ble_gatt.h"
#include "host/ble_hs_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define CHANNEL_ID 1
#define DEVICE_NAME "TEST_BLE_SMART_GLOVE"
#define BLE_APPEARANCE_GENERIC_TAG 0x0200
#define PROTO_HEADER_SIZE 5
#define BLE_RX_MAX_LEN 255





// Function to initialize Nimble and advertise
void blueetooth_init();

esp_err_t set_working_state(int state);
int get_working_state();

#endif
