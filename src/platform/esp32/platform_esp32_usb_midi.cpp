/// @defgroup platform_esp32_usb_midi USB MIDI host
/// The USB MIDI seam: one class-compliant desk on the chip's own USB port, the board being the host.
/// Two blocking tasks own the host stack, as in Espressif's examples, and the render thread reaches them only through two queues of USB-MIDI packets.

#include "platform/platform.h"

#if defined(CONFIG_MM_USB_MIDI_HOST)

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "usb/usb_host.h"

#include <atomic>
#include <cstring>

namespace mm::platform {

namespace {

constexpr const char* kTag = "usbmidi";
constexpr uint8_t kSubclassMidiStreaming = 0x03;
constexpr size_t kRxPackets = 64;    ///< queued packets from the desk: a burst of knob turns between two ticks
constexpr size_t kTxPackets = 160;   ///< queued packets to the desk: every light of an APC40 at once, plus its greeting
constexpr size_t kTransferBytes = 64;   ///< one full-speed bulk packet, 16 USB-MIDI packets

/// Everything the task owns; the render thread touches only the queues and the flags.
struct UsbMidi {
    QueueHandle_t rx = nullptr;
    QueueHandle_t tx = nullptr;
    TaskHandle_t libTask = nullptr;
    TaskHandle_t clientTask = nullptr;
    std::atomic<bool> connected{false};
    std::atomic<bool> stop{false};
    usb_host_client_handle_t client = nullptr;
    usb_device_handle_t dev = nullptr;
    uint8_t pendingAddr = 0;     ///< a device enumerated and waiting to be opened, 0 for none
    bool gone = false;           ///< the open device was unplugged
    uint8_t intf = 0;
    uint8_t epIn = 0, epOut = 0;
    usb_transfer_t* in = nullptr;
    usb_transfer_t* out = nullptr;
    bool outBusy = false;
    bool inBusy = false;         ///< whether the bulk IN transfer is in flight
};
UsbMidi s;

/// A bulk IN completed: queue its packets and ask for the next; an error or a cancel ends the reading, and an error closes the desk.
void onIn(usb_transfer_t* t) {
    if (t->status == USB_TRANSFER_STATUS_COMPLETED) {
        for (int at = 0; at + 4 <= t->actual_num_bytes; at += 4) {
            const uint8_t* p = t->data_buffer + at;
            if (p[0] == 0 && p[1] == 0) continue;   // padding some desks send in a short packet
            xQueueSend(s.rx, p, 0);                 // full: the oldest are what a hand already moved past
        }
        if (s.connected && usb_host_transfer_submit(t) == ESP_OK) return;
    } else if (t->status != USB_TRANSFER_STATUS_CANCELED) {
        s.gone = true;   // a stall, an error or an unplug: close it, and a replug opens it again
    }
    s.inBusy = false;
}

/// A bulk OUT completed: the next batch may go.
void onOut(usb_transfer_t*) { s.outBusy = false; }   // runs on the client task, which sends the next batch after it

/// The client's events, run inside usb_host_client_handle_events on the task.
void onClientEvent(const usb_host_client_event_msg_t* msg, void*) {
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV && !s.dev) s.pendingAddr = msg->new_dev.address;
    else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE && msg->dev_gone.dev_hdl == s.dev) s.gone = true;
}

/// Find the first MIDI Streaming interface (USB Audio, subclass 3) and its two bulk endpoints, false when the device has none.
bool findMidi(const usb_config_desc_t* cfg) {
    int offset = 0;
    const auto* d = reinterpret_cast<const usb_standard_desc_t*>(cfg);
    while ((d = usb_parse_next_descriptor_of_type(d, cfg->wTotalLength, USB_B_DESCRIPTOR_TYPE_INTERFACE, &offset))) {
        const auto* i = reinterpret_cast<const usb_intf_desc_t*>(d);
        if (i->bInterfaceClass != USB_CLASS_AUDIO || i->bInterfaceSubClass != kSubclassMidiStreaming) continue;
        s.epIn = s.epOut = 0;
        for (int e = 0; e < i->bNumEndpoints; e++) {
            int epOffset = offset;
            const usb_ep_desc_t* ep = usb_parse_endpoint_descriptor_by_index(i, e, cfg->wTotalLength, &epOffset);
            if (!ep || USB_EP_DESC_GET_XFERTYPE(ep) != USB_TRANSFER_TYPE_BULK) continue;
            if (USB_EP_DESC_GET_EP_DIR(ep)) s.epIn = ep->bEndpointAddress;
            else s.epOut = ep->bEndpointAddress;
        }
        if (s.epIn && s.epOut) {
            s.intf = i->bInterfaceNumber;
            return true;
        }
    }
    return false;
}

/// Open the device that enumerated, claim its MIDI interface and start reading; a device that is not a desk is closed again.
void openDesk(uint8_t addr) {
    if (usb_host_device_open(s.client, addr, &s.dev) != ESP_OK) { s.dev = nullptr; return; }
    const usb_config_desc_t* cfg = nullptr;
    if (usb_host_get_active_config_descriptor(s.dev, &cfg) != ESP_OK || !findMidi(cfg)
        || usb_host_interface_claim(s.client, s.dev, s.intf, 0) != ESP_OK) {
        usb_host_device_close(s.client, s.dev);
        s.dev = nullptr;
        return;
    }
    s.in->device_handle = s.dev;
    s.in->bEndpointAddress = s.epIn;
    s.in->num_bytes = kTransferBytes;
    s.out->device_handle = s.dev;
    s.out->bEndpointAddress = s.epOut;
    s.outBusy = false;
    s.connected = true;
    s.inBusy = usb_host_transfer_submit(s.in) == ESP_OK;
    ESP_LOGI(kTag, "desk on address %u, interface %u", addr, s.intf);
}

/// Let go of the desk, unplugged or given back: an interface with a transfer in flight cannot be released, so both endpoints are flushed and their callbacks run first.
void closeDesk() {
    if (!s.dev) return;
    s.connected = false;
    const uint8_t endpoints[] = {s.epIn, s.epOut};
    for (const uint8_t ep : endpoints) {
        usb_host_endpoint_halt(s.dev, ep);
        usb_host_endpoint_flush(s.dev, ep);
    }
    for (int i = 0; i < 50 && (s.inBusy || s.outBusy); i++) usb_host_client_handle_events(s.client, 1);
    for (const uint8_t ep : endpoints) usb_host_endpoint_clear(s.dev, ep);
    if (usb_host_interface_release(s.client, s.dev, s.intf) != ESP_OK) ESP_LOGW(kTag, "interface release failed");
    if (usb_host_device_close(s.client, s.dev) != ESP_OK) ESP_LOGW(kTag, "device close failed");
    s.dev = nullptr;
    xQueueReset(s.tx);
}

/// Hand the next batch of queued packets to a bulk OUT transfer.
void sendQueued() {
    if (!s.connected || s.outBusy) return;
    int n = 0;
    while (n + 4 <= static_cast<int>(kTransferBytes) && xQueueReceive(s.tx, s.out->data_buffer + n, 0) == pdTRUE) n += 4;
    if (n == 0) return;
    s.out->num_bytes = n;
    s.outBusy = true;
    if (usb_host_transfer_submit(s.out) != ESP_OK) s.outBusy = false;
}

/// The client task: the desk's events, its transfers and the send queue, until the port is given back.
void runClient(void*) {
    usb_host_client_config_t client = {};
    client.max_num_event_msg = 5;
    client.async.client_event_callback = onClientEvent;
    if (usb_host_client_register(&client, &s.client) != ESP_OK
        || usb_host_transfer_alloc(kTransferBytes, 0, &s.in) != ESP_OK || usb_host_transfer_alloc(kTransferBytes, 0, &s.out) != ESP_OK) {
        ESP_LOGE(kTag, "USB client did not start");
        s.clientTask = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    s.in->callback = onIn;
    s.out->callback = onOut;
    while (!s.stop) {
        usb_host_client_handle_events(s.client, portMAX_DELAY);   // woken by a desk, a transfer, or usbMidiWrite
        if (s.gone) { s.gone = false; closeDesk(); }
        if (s.pendingAddr) { const uint8_t a = s.pendingAddr; s.pendingAddr = 0; openDesk(a); }
        sendQueued();
    }
    closeDesk();
    usb_host_transfer_free(s.in);
    usb_host_transfer_free(s.out);
    s.in = s.out = nullptr;
    // The library task sees no clients left and finishes; a failed deregister leaves the stack installed, which the log says.
    if (usb_host_client_deregister(s.client) != ESP_OK) ESP_LOGE(kTag, "client deregister failed: the port stays a host until a reboot");
    s.client = nullptr;
    s.clientTask = nullptr;
    vTaskDelete(nullptr);
}

/// The library task: installs the host stack, starts the client, and uninstalls once the client is gone.
void runLibrary(void*) {
    usb_host_config_t host = {};
    host.intr_flags = ESP_INTR_FLAG_LEVEL1;
    if (usb_host_install(&host) != ESP_OK) {
        ESP_LOGE(kTag, "USB host did not start");
        s.libTask = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    // Same priority as this task, so the client registers before the first event is handled.
    xTaskCreate(runClient, "mmUsbMidiCl", 4096, nullptr, uxTaskPriorityGet(nullptr), &s.clientTask);
    for (;;) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            if (usb_host_device_free_all() == ESP_OK) break;   // nothing left to free
        }
        if (flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) break;
    }
    usb_host_uninstall();
    s.libTask = nullptr;
    vTaskDelete(nullptr);
}

}  // namespace

bool usbMidiBegin() {
    if (s.libTask) return !s.stop;   // still giving the port back: the next tick asks again
    if (!s.rx) s.rx = xQueueCreate(kRxPackets, 4);
    if (!s.tx) s.tx = xQueueCreate(kTxPackets, 4);
    if (!s.rx || !s.tx) return false;
    s.stop = false;
    // Below the render loop: both tasks block until the desk or a send needs them.
    return xTaskCreate(runLibrary, "mmUsbMidi", 4096, nullptr, 4, &s.libTask) == pdPASS;
}

void usbMidiEnd() {
    s.stop = true;
    if (s.client) usb_host_client_unblock(s.client);
}

bool usbMidiConnected() { return s.connected; }

size_t usbMidiRead(uint8_t (*packets)[4], size_t max) {
    if (!s.rx) return 0;
    size_t n = 0;
    while (n < max && xQueueReceive(s.rx, packets[n], 0) == pdTRUE) n++;
    return n;
}

bool usbMidiWrite(const uint8_t (*packets)[4], size_t count) {
    if (!s.connected || !s.tx || uxQueueSpacesAvailable(s.tx) < count) return false;
    for (size_t i = 0; i < count; i++) xQueueSend(s.tx, packets[i], 0);
    usb_host_client_unblock(s.client);   // the client task sends them now, not on its next event
    return true;
}

}  // namespace mm::platform

#else

namespace mm::platform {

// This image has no USB host: the S3-Zero's 4 MB leaves no room, and other chips have no OTG port here.
bool usbMidiBegin() { return false; }
void usbMidiEnd() {}
bool usbMidiConnected() { return false; }
size_t usbMidiRead(uint8_t (*)[4], size_t) { return 0; }
bool usbMidiWrite(const uint8_t (*)[4], size_t) { return false; }

}  // namespace mm::platform

#endif

namespace mm::platform {

// The test desk is the desktop's; a board's USB host has a real one.
void setTestUsbMidiDesk(bool) {}
void injectTestUsbMidi(const uint8_t (*)[4], size_t) {}
size_t takeTestUsbMidiSent(uint8_t (*)[4], size_t) { return 0; }

}  // namespace mm::platform
