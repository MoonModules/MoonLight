/// NimBLE control transport: the host task owns pairing, advertisements and notifications; the render task copies bounded messages (Espressif bleprph pattern).
#include "platform/platform.h"
#include "core/util/BleMessage.h"
#include "sdkconfig.h"

#if CONFIG_BT_NIMBLE_ENABLED
#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "host/ble_uuid.h"
#include "esp_err.h"
#include "esp_log.h"
#include "host/util/util.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
extern "C" void ble_store_config_init(void);

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>

namespace mm::platform {
namespace {
constexpr const char* kBleTag = "mm_ble";
// The historical BLE2JSON UUID family, with a separate 0x0700 service for this versioned contract.
const ble_uuid128_t serviceUuid = BLE_UUID128_INIT(0x19,0x18,0x8c,0x2c,0x19,0x13,0x9b,0x88,0x7a,0x4f,0xeb,0xa2,0x00,0x07,0xe3,0xbe);
const ble_uuid128_t stateUuid = BLE_UUID128_INIT(0x19,0x18,0x8c,0x2c,0x19,0x13,0x9b,0x88,0x7a,0x4f,0xeb,0xa2,0x01,0x07,0xe3,0xbe);
const ble_uuid128_t commandUuid = BLE_UUID128_INIT(0x19,0x18,0x8c,0x2c,0x19,0x13,0x9b,0x88,0x7a,0x4f,0xeb,0xa2,0x02,0x07,0xe3,0xbe);
const ble_uuid128_t identityUuid = BLE_UUID128_INIT(0x19,0x18,0x8c,0x2c,0x19,0x13,0x9b,0x88,0x7a,0x4f,0xeb,0xa2,0x03,0x07,0xe3,0xbe);

struct Peripheral {
    std::mutex mutex;
    BleMessage incoming;
    char response[513] = "{\"v\":1,\"ok\":true}";
    char identity[256] = {};
    uint32_t revision = 0;
    uint8_t addressType = 0;
    uint16_t valueHandle = 0;
    std::atomic<uint16_t> connection{BLE_HS_CONN_HANDLE_NONE};
    std::atomic<bool> subscribed{false};
    ble_gatt_chr_def characteristics[4] = {};
    ble_gatt_svc_def services[2] = {};
    ble_npl_event changed;
};
Peripheral* active = nullptr;  // one radio, claimed during prepare and released after the host stops

bool validIdentity(const char* name, const char* identity) {
    return name && std::strlen(name) <= 29 && identity && std::strlen(identity) < 256;
}

int gapEvent(ble_gap_event* event, void* arg);

void advertise() {
    if (!active) return;
    ble_hs_adv_fields fields = {};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = const_cast<ble_uuid128_t*>(&serviceUuid);
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    int status = ble_gap_adv_set_fields(&fields);
    if (status != 0) { ESP_LOGW(kBleTag, "advertising fields: %d", status); return; }
    ble_hs_adv_fields scan = {};
    const char* name = ble_svc_gap_device_name();
    scan.name = reinterpret_cast<const uint8_t*>(name);
    scan.name_len = static_cast<uint8_t>(std::strlen(name));
    scan.name_is_complete = 1;
    status = ble_gap_adv_rsp_set_fields(&scan);
    if (status != 0) { ESP_LOGW(kBleTag, "scan response: %d", status); return; }
    ble_gap_adv_params params = {};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    status = ble_gap_adv_start(active->addressType, nullptr, BLE_HS_FOREVER, &params, gapEvent, active);
    if (status != 0) ESP_LOGW(kBleTag, "advertising start: %d", status);
    else ESP_LOGI(kBleTag, "advertising, address type %u", static_cast<unsigned>(active->addressType));
}

int gapEvent(ble_gap_event* event, void* arg) {
    auto* peripheral = static_cast<Peripheral*>(arg);
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            ESP_LOGI(kBleTag, "connect status=%d handle=%u", event->connect.status,
                     static_cast<unsigned>(event->connect.conn_handle));
            if (event->connect.status == 0) peripheral->connection.store(event->connect.conn_handle);
            else advertise();
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(kBleTag, "disconnect reason=%d", event->disconnect.reason);
            peripheral->connection.store(BLE_HS_CONN_HANDLE_NONE);
            peripheral->subscribed.store(false);
            { std::lock_guard<std::mutex> lock(peripheral->mutex); peripheral->incoming.reset(); }
            advertise();
            break;
        case BLE_GAP_EVENT_ADV_COMPLETE: advertise(); break;
        case BLE_GAP_EVENT_SUBSCRIBE:
            if (event->subscribe.attr_handle == peripheral->valueHandle)
                peripheral->subscribed.store(event->subscribe.cur_notify != 0);
            break;
        case BLE_GAP_EVENT_ENC_CHANGE:
            ESP_LOGI(kBleTag, "encryption status=%d", event->enc_change.status);
            break;
        case BLE_GAP_EVENT_REPEAT_PAIRING:
            ESP_LOGW(kBleTag, "peer requests replacement of an existing bond");
            break;
        default: break;
    }
    return 0;
}

int accessValue(uint16_t, uint16_t, ble_gatt_access_ctxt* context, void* arg) {
    auto* peripheral = static_cast<Peripheral*>(arg);
    if (context->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint8_t bytes[513];
        uint16_t count = 0;
        if (ble_hs_mbuf_to_flat(context->om, bytes, sizeof(bytes), &count) != 0)
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        std::lock_guard<std::mutex> lock(peripheral->mutex);
        const auto result = peripheral->incoming.feed(bytes, count);
        if (result == BleMessage::Result::Busy) return BLE_ATT_ERR_INSUFFICIENT_RES;
        return result == BleMessage::Result::Invalid ? BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN : 0;
    }
    if (context->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
    char bytes[513];
    {
        std::lock_guard<std::mutex> lock(peripheral->mutex);
        const bool identity = ble_uuid_cmp(context->chr->uuid, &identityUuid.u) == 0;
        std::snprintf(bytes, sizeof(bytes), "%s", identity ? peripheral->identity : peripheral->response);
    }
    return os_mbuf_append(context->om, bytes, static_cast<uint16_t>(std::strlen(bytes))) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

void notifyChanged(ble_npl_event* event) {
    auto* peripheral = static_cast<Peripheral*>(ble_npl_event_get_arg(event));
    const uint16_t connection = peripheral->connection.load();
    if (connection == BLE_HS_CONN_HANDLE_NONE || !peripheral->subscribed.load()) return;
    uint32_t revision = 0;
    { std::lock_guard<std::mutex> lock(peripheral->mutex); revision = peripheral->revision; }
    const uint32_t now = millis();
    uint8_t bytes[8];
    for (uint8_t i = 0; i < 4; ++i) { bytes[i] = static_cast<uint8_t>(revision >> (i * 8)); bytes[i + 4] = static_cast<uint8_t>(now >> (i * 8)); }
    if (os_mbuf* buffer = ble_hs_mbuf_from_flat(bytes, sizeof(bytes)))
        ble_gatts_notify_custom(connection, peripheral->valueHandle, buffer);
}

void synchronize() {
    if (!active) return;
    int status = ble_hs_util_ensure_addr(0);
    if (status == 0) status = ble_hs_id_infer_auto(0, &active->addressType);
    if (status != 0) { ESP_LOGE(kBleTag, "address synchronization: %d", status); return; }
    advertise();
}

void hostTask(void*) { nimble_port_run(); nimble_port_freertos_deinit(); }
}  // namespace

bool bleControlAvailable() MM_NONBLOCKING { return true; }

bool bleControlStart(BleControlHandle& handle, const char* name, const char* identity) {
    bleControlStop(handle);
    if (active || !validIdentity(name, identity)) return false;
    auto* peripheral = new (std::nothrow) Peripheral();
    if (!peripheral) return false;
    std::snprintf(peripheral->identity, sizeof(peripheral->identity), "%s", identity);
    const esp_err_t initialized = nimble_port_init();
    if (initialized != ESP_OK) { ESP_LOGE(kBleTag, "host initialization: %s", esp_err_to_name(initialized)); delete peripheral; return false; }
    active = peripheral;
    ble_hs_cfg.sync_cb = synchronize;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    auto& state = peripheral->characteristics[0];
    state.uuid = &stateUuid.u; state.access_cb = accessValue; state.arg = peripheral;
    state.flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_NOTIFY;
    state.val_handle = &peripheral->valueHandle;
    auto& command = peripheral->characteristics[1];
    command.uuid = &commandUuid.u; command.access_cb = accessValue; command.arg = peripheral;
    command.flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC;
    auto& info = peripheral->characteristics[2];
    info.uuid = &identityUuid.u; info.access_cb = accessValue; info.arg = peripheral;
    info.flags = BLE_GATT_CHR_F_READ;
    peripheral->services[0].type = BLE_GATT_SVC_TYPE_PRIMARY;
    peripheral->services[0].uuid = &serviceUuid.u;
    peripheral->services[0].characteristics = peripheral->characteristics;
    if (ble_gatts_count_cfg(peripheral->services) != 0 || ble_gatts_add_svcs(peripheral->services) != 0 ||
        ble_svc_gap_device_name_set(name) != 0) {
        active = nullptr; nimble_port_deinit(); delete peripheral; return false;
    }
    ble_store_config_init();
    ble_npl_event_init(&peripheral->changed, notifyChanged, peripheral);
    handle.impl = peripheral;
    nimble_port_freertos_init(hostTask);
    return true;
}

void bleControlStop(BleControlHandle& handle) {
    auto* peripheral = static_cast<Peripheral*>(handle.impl);
    if (!peripheral) return;
    // nimble_port_stop synchronizes with the host loop before its callback storage is freed.
    if (nimble_port_stop() != 0) return;
    nimble_port_deinit();
    active = nullptr;
    delete peripheral;
    handle.impl = nullptr;
}

size_t bleControlReceive(BleControlHandle& handle, char* out, size_t capacity) MM_NONBLOCKING {
    auto* peripheral = static_cast<Peripheral*>(handle.impl);
    if (!peripheral) return 0;
    std::unique_lock<std::mutex> lock(peripheral->mutex, std::try_to_lock);
    return lock.owns_lock() ? peripheral->incoming.take(out, capacity) : 0;
}

bool bleControlPublish(BleControlHandle& handle, const char* response, uint32_t revision) MM_NONBLOCKING {
    auto* peripheral = static_cast<Peripheral*>(handle.impl);
    if (!peripheral || !response || std::strlen(response) > 512) return false;
    std::unique_lock<std::mutex> lock(peripheral->mutex, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    std::snprintf(peripheral->response, sizeof(peripheral->response), "%s", response);
    peripheral->revision = revision;
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &peripheral->changed);
    return true;
}

bool bleControlConnected(const BleControlHandle& handle) MM_NONBLOCKING {
    const auto* peripheral = static_cast<const Peripheral*>(handle.impl);
    return peripheral && peripheral->connection.load() != BLE_HS_CONN_HANDLE_NONE;
}
}  // namespace mm::platform
#else
#include "platform/ble_control_stub.inc"
#endif
