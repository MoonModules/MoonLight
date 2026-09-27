/// @defgroup platform_esp32_improv The serial provisioning listener
/// The serial and native-USB command dispatch.
///
/// Self-contained: it owns its state privately and reaches the rest of the layer only through declared accessors.
///
/// @moreinfo
///
/// ## It runs on every target, wireless or not
///
/// The serial transport and the vendor commands need no radio, so the installer can push a device model's config over serial to a wired device too.
/// Only the provisioning commands and their radio calls are guarded out.
/// A wired build does not offer those, having no station to provision, and the state query reports on the wired link instead.
///
/// ## Two transports, because the port may be either
///
/// Several chips have a built-in USB peripheral exposing a serial endpoint with no bridge chip, and many cheap boards wire the socket to it rather than to the hardware port.
/// So the bytes arrive on one or the other, and both are listened on, which makes one firmware work on either kind of board.
/// The install is soft-failing: when the secondary console grabbed the peripheral first, that path is skipped and the hardware port carries on alone.
///
/// One parser per transport, each keeping its own framing state and buffer.
/// A shared one would let a partial frame on one side be corrupted by bytes arriving on the other, its state machine not knowing they came from different sources.
/// Both are polled symmetrically, draining whichever has data and yielding only when both come up empty: an earlier shape blocked on one and starved the other.
///
/// ## Replies go back the way the request came
///
/// The task is single-threaded and handles one frame at a time, so one stored source is enough, set before dispatch and read by every send within it.
/// Broadcasting to both stays available for status messages during startup, which reply to no particular request.
/// A send on the USB path never blocks: a host that opened the endpoint without draining it would otherwise stall the task.
/// And a reply dropped there is retried by the installer.
///
/// ## The two vendor operations
///
/// One carries a transmit-power cap, the escape hatch for a board whose supply browns out at full power.
/// Its cap normally arrives over the network once the device is online, which such a board can never reach.
/// It fails to associate before any of that exists, proven on the bench.
/// This carries the cap over the same serial channel as the credentials, so it persists before the first association attempt.
///
/// The other carries one operation as a document, the same shape the network interface takes, so the installer can apply device-model defaults while it owns the port.
/// Most fit one frame and a long value is chunked, with reassembly and the duplicate guard living in a core helper that is tested without hardware.
/// Each frame is acknowledged so the installer can pace and retry, and the reassembled operation is applied on the main loop rather than this task.

#include "platform/platform.h"

#include "core/util/ImprovFrame.h"
#include "core/util/ImprovOpReassembler.h"

#include "driver/uart.h"
#include "esp_log.h"
#ifndef MM_NO_WIFI
#include "esp_wifi.h"   // only the WiFi-provisioning RPCs touch esp_wifi_*
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "improv.h"
#include "soc/soc_caps.h"

// The built-in USB peripheral, which many boards wire their socket to instead of the hardware port: @xref{two-transports-because-the-port-may-be-either|why both are listened on}.
#if SOC_USB_SERIAL_JTAG_SUPPORTED
#include "driver/usb_serial_jtag.h"
#endif

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace mm::platform {
namespace {

// Improv-serial framing, see src/core/ImprovFrame.h. That header carries the parser, builder, and checksum, all unit-tested at test/test_improv_frame.cpp. This task only does the IO + RPC dispatch.

// Shared with the Improv task; const after init.
struct ImprovTaskState {
    char name[33] = {};               // copied from ImprovDeviceInfo at init
    char chipFamily[16] = {};
    char firmwareVersion[16] = {};
    char* ssidOut = nullptr;          // module-owned buffers (NetworkModule's
    char* passwordOut = nullptr;      // ssid_ / password_ via the module)
    size_t ssidOutLen = 0;
    size_t passwordOutLen = 0;
    std::atomic<bool>* ready = nullptr;   // module polls and clears
    char* statusBuf = nullptr;        // module shows as `provision_status`
    size_t statusBufLen = 0;

    // Vendor SET_TX_POWER RPC (command 0xFD): pre-association TX-power cap in whole dBm for brown-out-prone boards. Same producer/consumer dance.
    uint8_t* txPowerOut = nullptr;
    std::atomic<bool>* txPowerReady = nullptr;

    // The operation buffer and its ready flag, the same producer and consumer dance as the credentials: @xref{the-two-vendor-operations|what it carries}. Module-owned and sized for the largest operation; only these two are shared state here.
    char* opOut = nullptr;
    size_t opOutLen = 0;
    std::atomic<bool>* opReady = nullptr;
};
static ImprovTaskState g_improv;  // single global — only one Improv task per device

static const char* IMPROV_TAG = "mm_improv";  // ESP_LOG* tag for the Improv task

static void improvSetStatus(const char* fmt, ...) {
    if (!g_improv.statusBuf || g_improv.statusBufLen == 0) return;
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(g_improv.statusBuf, g_improv.statusBufLen, fmt, args);
    va_end(args);
}

// Whether the USB read driver is up, which gates sending on that transport so a board without it never writes into a dead peripheral. Read and written on the same task, so a plain flag is enough.
#if SOC_USB_SERIAL_JTAG_SUPPORTED
static bool g_jtagReady = false;
#endif

// Which transport a reply goes back on: @xref{replies-go-back-the-way-the-request-came|why one stored value suffices}.
enum class ImprovSource : uint8_t { Both, Uart, Jtag };
static ImprovSource g_replySource = ImprovSource::Both;

// Send a framed message, routed to the transport that received the request being replied to, or broadcast when no particular source is set. The frame-type values match the upstream protocol numerically; the host test path keeps its own enumeration rather than including that header.
static void improvSend(ImprovFrameType type, const std::vector<uint8_t>& payload) {
    uint8_t frame[6 + 1 + 1 + 1 + kImprovMaxPayload + 1];
    size_t n = buildImprovFrame(type, payload.data(), payload.size(),
                                frame, sizeof(frame));
    if (n == 0) return;  // oversize payload — caller bug, silently drop
    const bool toUart =
#if SOC_USB_SERIAL_JTAG_SUPPORTED
        (g_replySource != ImprovSource::Jtag);
#else
        true;
#endif
    if (toUart) {
        uart_write_bytes(UART_NUM_0, reinterpret_cast<const char*>(frame), n);
    }
#if SOC_USB_SERIAL_JTAG_SUPPORTED
    if (g_jtagReady && g_replySource != ImprovSource::Uart) {
        // Non-blocking, so a host that opened the endpoint without draining cannot stall the task: what fits is queued and the rest dropped, and the installer retries. Replies are small and fit one transaction on any healthy host.
        usb_serial_jtag_write_bytes(frame, n, 0);
    }
#endif
}

static void improvSendCurrentState(improv::State state) {
    improvSend(ImprovFrameType::CurrentState, {static_cast<uint8_t>(state)});
}

static void improvSendError(improv::Error err) {
    improvSend(ImprovFrameType::ErrorState, {static_cast<uint8_t>(err)});
}

static void improvSendDeviceInfo() {
    // RPC response: [type=GET_DEVICE_INFO][len][n strings].
    std::vector<std::string> data = {
        "MoonLight",                            // firmware name
        g_improv.firmwareVersion,
        g_improv.chipFamily,
        g_improv.name,
    };
    auto rpc = improv::build_rpc_response(improv::GET_DEVICE_INFO, data, false);
    improvSend(ImprovFrameType::RpcResponse, rpc);
}

#ifndef MM_NO_WIFI
// --- WiFi-provisioning RPCs: only on WiFi builds. On Ethernet-only (MM_NO_WIFI)
// these aren't offered (no STA to provision) and the esp_wifi_* calls aren't linked. ---

static void improvSendWifiNetworks() {
    // One network per frame as the protocol specifies, then an empty payload to end the list, bounded to keep the response set small.
    // The scan needs the radio started, which on the chip whose radio lives on a companion happens only after its own prelude.
    // A scan before that returns an error cleanly rather than scanning a cold link.
    wifi_scan_config_t scan_cfg = {};
    if (esp_wifi_scan_start(&scan_cfg, true /*block*/) != ESP_OK) {
        improvSendError(improv::ERROR_UNKNOWN);
        return;
    }
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 10) n = 10;
    wifi_ap_record_t records[10] = {};
    esp_wifi_scan_get_ap_records(&n, records);
    for (uint16_t i = 0; i < n; i++) {
        char rssi[8];
        std::snprintf(rssi, sizeof(rssi), "%d", static_cast<int>(records[i].rssi));
        std::vector<std::string> data = {
            reinterpret_cast<const char*>(records[i].ssid),
            rssi,
            records[i].authmode == WIFI_AUTH_OPEN ? "NO" : "YES",
        };
        auto rpc = improv::build_rpc_response(improv::GET_WIFI_NETWORKS, data, false);
        improvSend(ImprovFrameType::RpcResponse, rpc);
    }
    // End-of-list sentinel: empty payload.
    improvSend(ImprovFrameType::RpcResponse,
               improv::build_rpc_response(improv::GET_WIFI_NETWORKS, {}, false));
}

// Stash the credentials for the module to consume on its own tick, rather than bringing the radio up from the parser task's stack.
static void improvHandleProvision(const improv::ImprovCommand& cmd) {
    if (wifiStaConnected()) {
        improvSetStatus("error: already connected");
        improvSendError(improv::ERROR_UNABLE_TO_CONNECT);
        return;
    }
    improvSetStatus("received credentials");
    std::strncpy(g_improv.ssidOut, cmd.ssid.c_str(), g_improv.ssidOutLen - 1);
    g_improv.ssidOut[g_improv.ssidOutLen - 1] = 0;
    std::strncpy(g_improv.passwordOut, cmd.password.c_str(), g_improv.passwordOutLen - 1);
    g_improv.passwordOut[g_improv.passwordOutLen - 1] = 0;
    // release-store: pairs with the module's acquire-load in tick1s() so the SSID/password buffer writes above are visible before the consumer sees ready=true (matters on the dual-core ESP32-S3; single-core ESP32 is a no-op but the explicit ordering documents intent).
    g_improv.ready->store(true, std::memory_order_release);
    improvSendCurrentState(improv::STATE_PROVISIONING);

    // Wait up to 30 s for a usable IP. Polls existing platform state, no extra wiring. Loop until the lease actually lands (a non-zero address), not merely until association: DHCP completes shortly after WIFI_EVENT_STA_CONNECTED, so breaking on wifiStaConnected() alone could read 0.0.0.0.
    uint8_t ip[4] = {};
    for (int i = 0; i < 300; i++) {  // 30 s @ 100 ms
        vTaskDelay(pdMS_TO_TICKS(100));
        if (wifiStaConnected()) {
            wifiStaGetIPv4(ip);
            if (ip[0] || ip[1] || ip[2] || ip[3]) break;   // have a real address
        }
    }
    // No usable lease (never associated, or associated but DHCP never completed) is a failure, same as the timeout, http://0.0.0.0/ would be worse than an honest error.
    if (!ip[0] && !ip[1] && !ip[2] && !ip[3]) {
        improvSetStatus("error: no IP after 30s");
        improvSendError(improv::ERROR_UNABLE_TO_CONNECT);
        return;
    }
    improvSetStatus("connected: %s", cmd.ssid.c_str());
    // Success frame: RPC response carrying the device URL. Format the dotted-quad inline (platform layer doesn't pull core/Control.h's formatDottedQuad).
    char url[64];
    std::snprintf(url, sizeof(url), "http://%u.%u.%u.%u/", ip[0], ip[1], ip[2], ip[3]);
    std::vector<std::string> urls = { url };
    auto rpc = improv::build_rpc_response(improv::WIFI_SETTINGS, urls, false);
    improvSend(ImprovFrameType::RpcResponse, rpc);
    improvSendCurrentState(improv::STATE_PROVISIONED);
}

#endif // MM_NO_WIFI — end WiFi-provisioning RPCs

// The transmit-power operation, the escape hatch for a board that browns out at full power: @xref{the-two-vendor-operations|why it must arrive before association}. The payload is one byte of whole decibels, zero lifting any cap; the module's tick persists and applies it.
static constexpr uint8_t IMPROV_CMD_SET_TX_POWER = 0xFD;
static constexpr uint8_t IMPROV_ERROR_INVALID_TX_POWER = 0x81;

static void improvHandleSetTxPower(const uint8_t* payload, uint8_t len) {
    if (!g_improv.txPowerOut || !g_improv.txPowerReady) {
        improvSendError(improv::ERROR_UNKNOWN_RPC);
        return;
    }
    // payload[0] = command (dispatched on), payload[1] = data_len, payload[2] = dBm.
    if (len != 3 || payload[1] != 1 || payload[2] > 21) {
        improvSendError(static_cast<improv::Error>(IMPROV_ERROR_INVALID_TX_POWER));
        return;
    }
    *g_improv.txPowerOut = payload[2];
    // release-store pairs with the module's acquire-load in tick1s().
    g_improv.txPowerReady->store(true, std::memory_order_release);
    auto rpc = improv::build_rpc_response(
        static_cast<improv::Command>(IMPROV_CMD_SET_TX_POWER),
        std::vector<std::string>{}, false);
    improvSend(ImprovFrameType::RpcResponse, rpc);
}

// The apply operation, carrying one request as a document in the same shape the network interface takes: @xref{the-two-vendor-operations|why over serial}.
// The payload is a chunk index, a last flag and a slice.
// The first index resets the buffer and the last one marks it ready for the main loop to apply.
static constexpr uint8_t IMPROV_CMD_APPLY_OP = 0xFC;
static constexpr uint8_t IMPROV_ERROR_INVALID_OP = 0x82;

static void improvHandleApplyOp(const uint8_t* payload, uint8_t len) {
    if (!g_improv.opOut || !g_improv.opReady) {
        improvSendError(improv::ERROR_UNKNOWN_RPC);
        return;
    }
    // Single-buffered: refuse a new op while the module hasn't consumed the previous one (opReady still set), so a fast installer can't overwrite an unapplied op. The installer treats this error as "retry shortly" and re-sends. Acquire-load pairs with the module's release-store when it clears the flag after applying.
    if (g_improv.opReady->load(std::memory_order_acquire)) {
        improvSendError(static_cast<improv::Error>(IMPROV_ERROR_INVALID_OP));
        return;
    }
    // [0xFC][seq][last] header = 3 bytes; chunk is the rest.
    if (len < 3) {
        improvSendError(static_cast<improv::Error>(IMPROV_ERROR_INVALID_OP));
        return;
    }
    uint8_t seq = payload[1];
    uint8_t last = payload[2];
    const uint8_t* chunk = payload + 3;
    size_t chunkLen = static_cast<size_t>(len) - 3;

    // `last` is a boolean flag on the wire; anything but 0/1 is a malformed frame (a desync the parser's checksum didn't catch, or a non-conforming sender). Reject before reassembly rather than coerce a stray value to "more chunks".
    if (last > 1) {
        improvSendError(static_cast<improv::Error>(IMPROV_ERROR_INVALID_OP));
        return;
    }

    // Reassembly and the duplicate guard live in a core helper tested without hardware, leaving only the serial handling here. Bound once at the first call, the buffer being set before the task starts and one task living for the device's lifetime, so it never sees a stale one.
    static mm::ImprovOpReassembler reasm(g_improv.opOut, g_improv.opOutLen);
    switch (reasm.feed(seq, last, chunk, chunkLen)) {
        case mm::ImprovOpReassembler::Result::Error:
            improvSendError(static_cast<improv::Error>(IMPROV_ERROR_INVALID_OP));
            return;
        case mm::ImprovOpReassembler::Result::Continue:
            break;
        case mm::ImprovOpReassembler::Result::Ready:
            // release-store pairs with the module's acquire-load before it applies.
            g_improv.opReady->store(true, std::memory_order_release);
            break;
    }
    // Ack every chunk (empty RpcResponse for 0xFC) so the installer awaits each.
    auto rpc = improv::build_rpc_response(
        static_cast<improv::Command>(IMPROV_CMD_APPLY_OP),
        std::vector<std::string>{}, false);
    improvSend(ImprovFrameType::RpcResponse, rpc);
}

// Dispatch a completed frame from the parser. Only RPC frames carry commands we care about; the spec lets the other types through silently.
static void improvDispatchFrame(const ImprovFrameParser& parser) {
    if (parser.lastType() != improv::TYPE_RPC) return;
    // Vendor RPCs short-circuit the standard improv::parse_improv_data path because that helper is WIFI_SETTINGS-shaped (n length-prefixed strings into ssid/password). Peek at the command byte first; vendor-RPC parsing handles its own payload.
    const uint8_t* raw = parser.lastPayload();
    uint8_t rawLen = parser.lastPayloadLen();
    if (rawLen >= 1 && raw[0] == IMPROV_CMD_SET_TX_POWER) {
        improvHandleSetTxPower(raw, rawLen);
        return;
    }
    if (rawLen >= 1 && raw[0] == IMPROV_CMD_APPLY_OP) {
        improvHandleApplyOp(raw, rawLen);
        return;
    }
    improv::ImprovCommand cmd = improv::parse_improv_data(
        parser.lastPayload(), parser.lastPayloadLen(), false);
    switch (cmd.command) {
        case improv::GET_CURRENT_STATE: {
            // "Connected" means: on WiFi, the STA has an IP; on Ethernet-only, the eth link is up with a DHCP lease. Either way report PROVISIONED + the device URL (the way ESPHome does, makes the protocol self-describing on every reconnect; observable via improv_probe.py). Not connected → AUTHORIZED.
            uint8_t ip[4] = {};
            bool connected = false;
#ifndef MM_NO_WIFI
            if (wifiStaConnected()) { wifiStaGetIPv4(ip); connected = true; }
            else
#endif
            if (ethConnected()) { ethGetIPv4(ip); connected = true; }
            if (connected) {
                improvSendCurrentState(improv::STATE_PROVISIONED);
                if (ip[0] || ip[1] || ip[2] || ip[3]) {
                    char url[64];
                    std::snprintf(url, sizeof(url), "http://%u.%u.%u.%u/", ip[0], ip[1], ip[2], ip[3]);
                    std::vector<std::string> urls = { url };
                    improvSend(ImprovFrameType::RpcResponse,
                               improv::build_rpc_response(improv::WIFI_SETTINGS, urls, false));
                }
            } else {
                improvSendCurrentState(improv::STATE_AUTHORIZED);
            }
            break;
        }
        case improv::GET_DEVICE_INFO: improvSendDeviceInfo(); break;
#ifndef MM_NO_WIFI
        case improv::GET_WIFI_NETWORKS:
            // Refuse scans while WiFi STA is connected, esp_wifi_scan_start puts the radio into scan mode for 2-5 s, dropping inbound ArtNet (a visible glitch on a 16K-LED rig). GET_CURRENT_STATE already reports online.
            if (wifiStaConnected()) improvSendError(improv::ERROR_UNABLE_TO_CONNECT);
            else                    improvSendWifiNetworks();
            break;
        case improv::WIFI_SETTINGS: improvHandleProvision(cmd); break;
#endif
        default:                    improvSendError(improv::ERROR_UNKNOWN_RPC); break;
    }
}

// Feed one byte into the parser and dispatch as needed, the source naming which transport it arrived on so the reply routes back there alone. Reset to both afterwards, so any later unsolicited send broadcasts as before.
static void improvFeedByte(ImprovFrameParser& parser, uint8_t b, ImprovSource source) {
    switch (parser.feed(b)) {
        case ImprovFeedResult::NeedMore:
            break;
        case ImprovFeedResult::FrameReady:
            g_replySource = source;
            improvDispatchFrame(parser);
            g_replySource = ImprovSource::Both;
            break;
        case ImprovFeedResult::BadChecksum:
            g_replySource = source;
            improvSendError(improv::ERROR_INVALID_RPC);
            g_replySource = ImprovSource::Both;
            break;
        case ImprovFeedResult::OversizePayload:
            // Length byte > 128, almost certainly noise / bit-flip; resync silently.
            break;
    }
}

static void improvTask(void* /*arg*/) {
    // UART0 driver install. UART0 is already configured at 115200-8N1 by the bootloader; we just claim the interrupt + RX FIFO. RX buf 256 is plenty (Improv RPC payloads max out around 96 bytes).
    bool uartReady = false;
    esp_err_t uart_err = uart_driver_install(UART_NUM_0, 256, 0, 0, nullptr, 0);
    if (uart_err == ESP_OK) {
        uartReady = true;
    } else {
        // Do not park the task, since the other transport may still work on this board; the warning reaches the log and the status below surfaces it in the interface.
        ESP_LOGW(IMPROV_TAG, "uart_driver_install failed: %s",
                 esp_err_to_name(uart_err));
    }

#if SOC_USB_SERIAL_JTAG_SUPPORTED
    // The USB driver install, which on a native-USB board is the interface the host actually talks to; best-effort, since the secondary console may have taken the peripheral first.
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t jtag_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        esp_err_t jtag_err = usb_serial_jtag_driver_install(&jtag_cfg);
        if (jtag_err == ESP_OK) {
            g_jtagReady = true;
        } else {
            ESP_LOGW(IMPROV_TAG, "usb_serial_jtag_install failed: %s",
                     esp_err_to_name(jtag_err));
        }
    } else {
        // Someone else already installed it (rare). We can still read + write through it.
        g_jtagReady = true;
    }
#endif

    // If both installs failed, the task has nothing to do. Park it.
#if SOC_USB_SERIAL_JTAG_SUPPORTED
    const bool anyReady = uartReady || g_jtagReady;
#else
    const bool anyReady = uartReady;
#endif
    if (!anyReady) {
        improvSetStatus("error: no transport (uart + jtag install both failed)");
        vTaskDelete(nullptr);
        return;
    }

    // Compound status so a user inspecting `provision_status` can see partial failures (one transport up, the other failed). Without this the listening-state status overwrites any prior warn line and the failure is invisible in the UI.
#if SOC_USB_SERIAL_JTAG_SUPPORTED
    if (uartReady && g_jtagReady)         improvSetStatus("listening");
    else if (uartReady)                   improvSetStatus("listening (jtag unavailable)");
    else                                   improvSetStatus("listening (uart unavailable)");
#else
    improvSetStatus("listening");
#endif

    // One parser per transport, each with its own framing state and buffer: @xref{two-transports-because-the-port-may-be-either|why sharing one corrupts a partial frame}.
    ImprovFrameParser parser_uart;
#if SOC_USB_SERIAL_JTAG_SUPPORTED
    ImprovFrameParser parser_jtag;
#endif
    uint8_t b;
    for (;;) {
        // A symmetric non-blocking poll of both, draining whichever has data and yielding only when both come up empty. Blocking on one made the other lumpy on a board that has both.
        bool anyRead = false;
        if (uartReady) {
            for (int drained = 0; drained < 64; ++drained) {
                int n = uart_read_bytes(UART_NUM_0, &b, 1, 0);
                if (n <= 0) break;
                improvFeedByte(parser_uart, b, ImprovSource::Uart);
                anyRead = true;
            }
        }
#if SOC_USB_SERIAL_JTAG_SUPPORTED
        if (g_jtagReady) {
            for (int drained = 0; drained < 64; ++drained) {
                int n = usb_serial_jtag_read_bytes(&b, 1, 0);
                if (n <= 0) break;
                improvFeedByte(parser_jtag, b, ImprovSource::Jtag);
                anyRead = true;
            }
        }
#endif
        if (!anyRead) {
            // Nothing on either side, so yield and let lower-priority work run; the wait matches what the blocking poll achieved and is identical from this task's perspective.
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

} // anonymous namespace

bool improvProvisioningInit(const ImprovDeviceInfo& info,
                            char* ssidOut, size_t ssidOutLen,
                            char* passwordOut, size_t passwordOutLen,
                            std::atomic<bool>* ready,
                            char* statusBuf, size_t statusBufLen,
                            uint8_t* txPowerOut,
                            std::atomic<bool>* txPowerReady,
                            char* opOut, size_t opOutLen,
                            std::atomic<bool>* opReady) {
    if (!info.name || !info.chipFamily || !info.firmwareVersion ||
        !ssidOut || ssidOutLen == 0 ||
        !passwordOut || passwordOutLen == 0 ||
        !ready || !statusBuf || statusBufLen == 0) {
        return false;
    }
    std::strncpy(g_improv.name, info.name, sizeof(g_improv.name) - 1);
    std::strncpy(g_improv.chipFamily, info.chipFamily, sizeof(g_improv.chipFamily) - 1);
    std::strncpy(g_improv.firmwareVersion, info.firmwareVersion, sizeof(g_improv.firmwareVersion) - 1);
    g_improv.ssidOut = ssidOut;
    g_improv.ssidOutLen = ssidOutLen;
    g_improv.passwordOut = passwordOut;
    g_improv.passwordOutLen = passwordOutLen;
    g_improv.ready = ready;
    g_improv.statusBuf = statusBuf;
    g_improv.statusBufLen = statusBufLen;
    // SET_TX_POWER opt-in, same shape.
    g_improv.txPowerOut = txPowerOut;
    g_improv.txPowerReady = txPowerReady;
    // APPLY_OP opt-in, same shape (the op reassembly buffer + ready flag).
    g_improv.opOut = opOut;
    g_improv.opOutLen = opOutLen;
    g_improv.opReady = opReady;

    // 6 KB stack: parser is small, scan response uses std::vector + std::string (some short-string-optimized, some heap). Priority 4, below OTA (5), above idle. Single task per device; not pinned to a core.
    BaseType_t ok = xTaskCreate(&improvTask, "improv", 6144, nullptr, 4, nullptr);
    if (ok != pdPASS) {
        improvSetStatus("error: task create failed");
        return false;
    }
    return true;
}

} // namespace mm::platform
