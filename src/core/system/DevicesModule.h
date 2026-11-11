#pragma once

#include "core/util/format.h"   // formatTo: nonblocking formatting into a fixed buffer
#include "core/util/parse.h"    // asciiCaseEqual: a host name compares without case
#include "core/module/MoonModule.h"
#include "core/util/ActiveInstance.h"   // the boot-registry seat election (the seat + its RAII vacate)
#include "core/module/Control.h"
#include "core/module/Scheduler.h"      // the tree whose modules report their network use
#include "core/util/JsonSink.h"
#include "core/util/Sort.h"             // mm::insertionSort: generic bounded sort (core); we supply the comparator
#include "core/util/ScratchBuffer.h"    // the rows, grown by doubling as devices are heard
#include "core/system/DeviceIdentify.h"   // DevType, devTypeStr (the device-kind enum + its labels)
#include "core/system/DevicePlugin.h"     // the interop plugin seam + the bundled plugins
#include "core/system/WledPacket.h"       // the presence packet and which copy it is
#include "core/util/Addressing.h"         // sendAddressed, and the network's multicast evidence
#include "core/util/HostResolver.h"       // setNameSource: a host setting naming a board resolves from this list, and a status names it
#include "core/util/build_info.h"         // kVersion, kFirmwareName: this device's own row carries what it runs
#include "core/util/LightOutput.h"        // writeOutputs: this device's own row carries what drives its lights
#include "core/util/Mdns.h"               // the browse for devices that announce only over mDNS
#include "core/util/HostList.h"           // parseHostList: the added addresses, as every host list is read
#include "core/util/HttpHead.h"           // findHeader: the probe reads a page's Server header as the server reads a request's
#include "platform/platform.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>   // the two services the browse asks for

namespace mm {

/// Discovers other devices on the LAN, MoonLight and WLED among them, and lists them, this one included and marked as such.
/// @card DevicesModule.png
///
/// The Discovery card under Devices, listening once the network is up; the list is rebuilt within seconds of boot.
///
/// @guide
///
/// ## The map
///
/// This device is the ringed center; lines are traffic, quiet devices sit beneath. Nodes show lights, address, version and services. Scroll zooms, drag pans, ⌖ resets; the bar shows IP and version or hides traffic. Click or Enter opens details and Identify, a blink of its lights. Everyone on the network sees it: [security](https://moonmodules.org/MoonLight/explanation/security.html).
///
/// ## The emoji
///
/// Lights: 💡 LEDs, 🔆 PWM lights, 🔲 a HUB75 panel, 📺 a screen, 🧱 a ColorLight card, 🏮 Hue, 📡 sent over the network, 🎥 a video stream, 🎯 moving heads; 🛟 MoonBase holds the board. Services and traffic: 🌐 the web page, 🎛️ OSC, 🎹 RTP-MIDI, 🔊 audio sync, 🎭 Art-Net, 🎚️ E1.31, 📦 DDP, 🐙 the WLED API, which Home Assistant polls.
///
/// ## As many devices as memory holds
///
/// When memory runs short the card says "memory full", and a new device replaces the one heard longest ago, MoonLight boards kept longest.
///
/// ## Finding WLED devices
///
/// WLED lists a device only from the broadcast copy, and the copy costs the rest of the LAN next to nothing.
///
/// A network that drops multicast is detected, and the status says so ([IGMP snooping](https://moonmodules.org/MoonLight/explanation/architecture/moonlight.html#multicast-and-igmp-snooping)).
///
/// ## Boards by name
///
/// A host setting naming a board, such as `MM-S31`, resolves from this list before mDNS, so the name holds where a router drops multicast. A card naming a peer shows its name beside its address.
///
/// @technical
///
/// **Discovery is passive, but for one question.** Each device announces a presence packet on the multicast group and the broadcast address WLED listens on, and this listens for both. A device appears when heard and ages out when it stops.
///
/// ESPHome announces only over mDNS, so once a minute a browse for `_esphomelib._tcp` and one for `_wled._tcp` go out from an ordinary port and the answers come back to it (`Mdns.h`): ESPHome's naming the device and its version, WLED's the MAC its presence packet lacks.
///
/// **One device, one row: the MAC is its identity**, named by MoonLight's and MoonBase's packet or an ESPHome or WLED mDNS record, the address standing in until then; a board moving to Ethernet keeps its row, and a rename never splits one.
///
/// **Ports and addresses.** MoonLight's and MoonBase's packet appends the page's port and the board's base MAC. A link-local address (169.254.0.0/16) gets no row: no peer reaches it, and a board's second interface would list the board twice.
///
/// **Plugins are the interop seam.** A foreign ecosystem hooks in as a plugin, not a branch. Each declares its port and turns a datagram into a device kind, ours offered first; a new system is one new file.
///
/// **Aging out.** Each sighting is stamped, and a device unheard for a day leaves the list. The list is runtime state, never saved: every device announces itself again within seconds. Discovery only, never device commands.
///
/// **The summary is `/api/system`.** A node's details read the device's own health endpoint, which leads with who it is and what it runs, the light outputs and effects through `LightOutput`. This device's row carries its outputs too, so a peer's map labels its node without a request per node. Identify presses the device's own Drivers `identify` button through `/api/control`.
///
/// **The map's edges come from the modules.** Once a second every enabled module reports its services and traffic (NetworkUse). A peer gets a row, a group address one of kind group. This device's row carries its outbound flows and services, a peer's row its inbound flow. A flow from a peer is a sighting; one to a silent receiver is not, so a dead receiver ages out.
///
/// **Growth, and what memory refuses.** The rows are one block that doubles when a device needs a row, checked against the heap's reserve, so N devices cost about log₂ N allocations. A refused growth replaces the least recently heard row of the lowest worth: other devices, then MoonLight boards, never this device.
///
class DevicesModule : public MoonModule, public ListSource {
public:
    /// Adopt this device's own name, so its own row matches what everything else shows.
    void setSelfName(const char* name) { selfName_ = name; }

    /// The devices drawn as a map above the list, each row a node keyed by its address, which anyone on the network reads.
    bool listAsGraph() const override { return true; }

    /// How many devices are currently listed.
    ListRow listRowCount() const override { return deviceCount_; }

    /// Append one device's summary: its name, address and kind.
    void writeListRow(JsonSink& sink, ListRow row) const override {
        if (row >= deviceCount_) { sink.append("{}"); return; }
        const Device& d = devices_[row];
        char ip[16];
        openRow(sink, d, ip);
        if (d.httpPort != WledPacket::kDefaultHttpPort) sink.appendf(",\"port\":%u", static_cast<unsigned>(d.httpPort));
        if (d.self) sink.append(",\"self\":true");
        sink.append("}");
    }

    /// Append one device's detail, which adds its address and how long since it was heard.
    void writeListRowDetail(JsonSink& sink, ListRow row) const override {
        if (row >= deviceCount_) { sink.append("{}"); return; }
        const Device& d = devices_[row];
        char ip[16];
        openRow(sink, d, ip);
        if (d.httpPort != WledPacket::kDefaultHttpPort) sink.appendf(",\"url\":\"http://%s:%u/\"", ip, static_cast<unsigned>(d.httpPort));
        else sink.appendf(",\"url\":\"http://%s/\"", ip);
        if (d.self) {
            sink.append(",\"self\":true");   // always now, so no age is meaningful
        } else {
            // Computed here so the UI gets one finished number, and wrap-safe.
            uint32_t ageSec = (platform::millis() - d.lastSeenMs) / 1000u;
            sink.appendf(",\"ageSec\":%u", static_cast<unsigned>(ageSec));
        }
        writeEdges(sink, d);
        if (d.self) {
            // What this device runs, so a peer merging this graph shows it on this device's node.
            sink.append(",\"version\":");
            sink.writeJsonString(kVersion);
            sink.append(",\"firmware\":");
            sink.writeJsonString(kFirmwareName);
            writeServices(sink);
            // What drives its lights, which a peer's map shows under this device's name.
            if (const LightOutput* out = LightOutput::active()) {
                sink.append(",\"outputs\":[");
                out->writeOutputs(sink);
                sink.append("]");
            }
        } else if (d.version[0]) {
            sink.append(",\"version\":");   // what another ecosystem's device announced it runs
            sink.writeJsonString(d.version);
        }
        if (!d.self && d.chip[0]) sink.appendf(",\"chip\":\"%s\"", d.chip);
        sink.append("}");
    }

    /// The rows are rebuilt from what is heard, so the list is never saved.
    bool persistsList() const override { return false; }

    /// The modes `addressing` picks from; its options start one entry into the shared Addressing names, since presence is never unicast.
    enum Mode : uint8_t { kDevMulticast, kDevMulticastBroadcast, kDevModeCount };
    /// How presence travels: multicast, or multicast and broadcast.
    /// @guide Broadcast is the default, since WLED devices hear it too.
    uint8_t addressing = kDevMulticastBroadcast;
    /// Devices that announce nothing, a Tasmota among them, by IP address, comma-separated.
    char addedIps[96] = "";

    /// Declare the addressing and the list of discovered devices.
    void defineControls() override {
        MoonModule::defineControls();
        controls_.addSelect("addressing", addressing, kAddressingNames + 1, kDevModeCount);
        controls_.addText("addedIps", addedIps, sizeof(addedIps));
        /// The discovered devices; a row expands to its details.
        /// @field name The device's name, else its address.
        /// @field ip Its address on the network.
        /// @field url Its own page, opened in a new tab.
        /// @field type What it is: a MoonLight, a WLED, a Hue bridge or a generic device.
        /// @field color How many color lights a Hue bridge has.
        /// @field ageSec When it was last heard.
        controls_.addList("devices", *this);   // this module is the ListSource
    }

    /// Take a changed list of added addresses, which a probe asks in turn.
    void onControlChanged(const char* name) override {
        if (std::strcmp(name, "addedIps") == 0) parseAdded();
    }

    /// Parse `addedIps` into the addresses the probe asks, starting with the first now; an entry that is not an address is refused on the status.
    void parseAdded() {
        uint8_t n = 0;
        const char* err = parseHostList(addedIps, addedHosts_, kMaxAdded, n);
        for (uint8_t i = 0; !err && i < n; i++)
            if (addedHosts_[i].isName()) err = "addedIps: IP addresses only";
        addedCount_ = err ? 0 : n;
        addedError_ = err;
        probe_.conn.close();
        probe_ = Probe{};
        probeTick_ = kProbeEverySec;   // ask the new list now
        refreshStatus();
    }

    /// The one live instance, through which a light-domain driver registers what it found.
    static DevicesModule* active() MM_NONBLOCKING { return ActiveInstance<DevicesModule>::active(); }

    /// The name of the board heard at `ip`, or null when none was or it gave none.
    const char* nameOf(const uint8_t ip[4]) const MM_NONBLOCKING {
        for (ListRow i = 0; i < deviceCount_; i++) {
            const Device& d = devices_[i];
            if (ipEq(d.ip, ip) && d.name[0] && !isIpPlaceholder(d.name, ip)) return d.name;
        }
        return nullptr;
    }

    /// The address of the board named `name` and heard this session, letter case and a trailing `.local` aside, as mDNS compares them; false when none was. A board that moved leaves its old row until it ages out, so the one heard last wins.
    bool addressOf(const char* name, uint8_t ip[4]) const MM_NONBLOCKING {
        if (!name || !name[0]) return false;
        size_t len = std::strlen(name);
        if (len > 6 && asciiCaseEqual(name + len - 6, ".local", 6)) len -= 6;
        const Device* best = nullptr;
        for (ListRow i = 0; i < deviceCount_; i++) {
            const Device& d = devices_[i];
            // Heard later, wrap-safe: the difference of two stamps, read signed.
            if (heardAs(d, name, len) && (!best || static_cast<int32_t>(d.lastSeenMs - best->lastSeenMs) > 0)) best = &d;
        }
        if (!best) return false;
        std::memcpy(ip, best->ip, 4);
        return true;
    }

    /// Register a bridge a driver found out of band, updating its row or inserting one.
    void upsertHueBridge(const uint8_t ip[4], const char* name, uint8_t color) {
        Device* d = findByIp(ip);
        bool changed = false;
        if (!d) {
            d = addRow(ip, kWorthOther);
            if (!d) { refreshStatus(); return; }   // memory full and every row worth more
            changed = true;
        }
        if (d->type != DevType::Hue) { d->type = DevType::Hue; changed = true; }
        if (name && name[0] && std::strcmp(d->name, name) != 0) {
            std::snprintf(d->name, sizeof(d->name), "%s", name);
            changed = true;
        }
        if (!d->name[0]) { formatDottedQuad(d->name, ip); changed = true; }
        if (d->colorCount != color) { d->colorCount = color; changed = true; }
        d->lastSeenMs = platform::millis();   // keeps the bridge from aging out
        if (changed) { sortByName(); refreshStatus(); }
    }

    /// Offer this list as the source host settings resolve board names from.
    void setup() override {
        MoonModule::setup();
        HostResolver::setNameSource(&resolveBoard, &boardNameAt);
        setStatus(statusBuf_);
        parseAdded();   // a saved list is restored without a change hook
    }

    /// Claim the seat that makes this the instance a driver registers through.
    void prepare() override {
        seat_.claim();
    }

    /// Vacate the seat, close the socket, which releases its port, and free the rows.
    void release() override {
        seat_.vacate();
        listener_.close();
        listenerBound_ = false;
        mdns_.close();
        mdnsOpen_ = false;
        probe_.conn.close();
        probe_ = Probe{};
        deviceCount_ = flowCount_ = serviceCount_ = 0;   // the base frees their blocks
        MoonModule::release();
    }

    /// Drain inbound packets, announce ourselves on a slow cadence, and age out the silent.
    void tick1s() MM_NONBLOCKING override {
        MoonModule::tick1s();
        uint8_t local[4] = {};
        platform::localIPv4(local);
        const bool online = local[0] || local[1] || local[2] || local[3];
        if (!online) return;   // no network yet: nothing to discover

        // Against the current address every tick, since ours changes on a renew or a switch.
        upsertSelf(local);
        ensureListener();

        // Bounded, since a busy network sends only a handful per interval.
        uint8_t buf[64];
        uint8_t srcIp[4];
        for (int i = 0; i < kMaxDrainPerTick; i++) {
            int n = listener_.recvFrom(buf, sizeof(buf), srcIp);
            if (n <= 0) break;   // -1 = nothing pending; done for this tick
            mergePacket(buf, static_cast<size_t>(n), srcIp);
        }

        browseMdns();
        probeAdded();

        // Broadcast our own presence every kBroadcastEverySec ticks so peers discover us.
        if (++broadcastTick_ >= kBroadcastEverySec) {
            broadcastTick_ = 0;
            broadcastPresence(local);
        }

        refreshRows(local);
    }

    /// Feed one packet through the real pipeline, so a test needs no network.
    void injectPacketForTest(const uint8_t* data, size_t len, const uint8_t srcIp[4]) {
        mergePacket(data, len, srcIp);
    }
    /// Test-only: one tick of the probe, and where it stands: which address, and whether it holds a connection.
    void stepProbeForTest() { probeAdded(); }
    uint8_t probeHostForTest() const { return probe_.host; }
    bool probeOpenForTest() const { return probe_.conn.valid(); }
    /// Test-only: the first added address's page arriving in pieces of `piece` bytes, as the probe's reads take it.
    void injectPageForTest(const char* page, size_t piece) {
        probe_ = Probe{};
        const size_t len = std::strlen(page);
        for (size_t at = 0; at < len; at += piece)
            if (scanPage(reinterpret_cast<const uint8_t*>(page) + at, len - at < piece ? len - at : piece)) break;
        finishProbe();
    }
    /// Test-only: an mDNS reply arriving from `srcIp`, as the browse's read takes it.
    void injectMdnsReplyForTest(const uint8_t* data, size_t len, const uint8_t srcIp[4]) { noteMdnsReply(data, len, srcIp); }

private:
    /// One discovered device, as the list serializes it.
    struct Device {
        uint8_t  ip[4] = {};
        char     name[24] = {};
        DevType  type = DevType::Generic;   ///< which ecosystem it belongs to
        bool     self = false;              ///< this device's own row
        uint32_t lastSeenMs = 0; ///< platform::millis() at the most recent presence sighting.
                                 ///< Age-out drops a non-self device unheard for kStaleMs.
        uint8_t  colorCount = 0; ///< Hue bridge only: how many of its lights are color-capable
                                    ///< (the figure for sizing a layout). 0 for non-bridge rows.
        uint8_t  broadcastRun = 0;   ///< broadcast copies since this peer's last multicast one, saturating
        uint32_t markedMs = 0;       ///< platform::millis() at its last MoonLight- or MoonBase-marked packet
        uint16_t httpPort = WledPacket::kDefaultHttpPort;   ///< the port its web page answers on, from its presence
        char     version[16] = {};   ///< the firmware version a device of another ecosystem announces, ESPHome's in its mDNS TXT
        uint8_t  mac[6] = {};        ///< its identity once a source names it, all zero until then, while its address stands in
        char     chip[10] = {};      ///< the chip a device of another ecosystem names, a Tasmota's in its Server header, as MoonLight's files spell it ("esp32s3")
    };

    /// The seat behind `active()`, claimed on build and vacated on release.
    ActiveInstance<DevicesModule> seat_{*this};

    /// The most rows the list index can address, a bound memory reaches first.
    static constexpr ListRow  kMaxRows = static_cast<ListRow>(~ListRow{0});
    /// What a row is worth keeping when memory is full: this device most, then MoonLight boards, then everything else.
    static constexpr uint8_t  kWorthOther = 0, kWorthMoonLight = 1, kWorthSelf = 2;
    /// How often we announce ourselves, in ticks, which is how fast a peer discovers us.
    static constexpr uint32_t kBroadcastEverySec = 10;
    /// How long a row stays one of ours with only unmarked packets arriving: three of MoonLight's presence intervals.
    static constexpr uint32_t kMarkedHoldMs = 3 * kBroadcastEverySec * 1000;
    /// How long a live-confirmed device stays listed, the list being durable history.
    static constexpr uint32_t kStaleMs = 24u * 60u * 60u * 1000u;
    /// How many packets one tick will process, which bounds the work.
    static constexpr int      kMaxDrainPerTick = 16;
    /// How long after the listener opens presence goes out both ways, the bootstrap for the evidence.
    static constexpr uint32_t kBothWaysMs = 2u * 60u * 1000u;
    /// How many broadcast copies in a row, with no multicast one between them, prove the group does not arrive.
    static constexpr uint8_t  kEvidence = 3;

    /// The device plugins in claim order: ours is first, so a peer's marked packet is typed before the fallback.
    MmPlugin   mmPlugin_;
    MbPlugin   mbPlugin_;
    WledPlugin wledPlugin_;
    static constexpr uint8_t kPluginCount = 3;
    const DevicePlugin* plugins_[kPluginCount] = { &mmPlugin_, &mbPlugin_, &wledPlugin_ };

    platform::UdpSocket listener_;       // bound to the presence port; drained each tick
    platform::UdpSocket mdns_;           // an ordinary port the mDNS browse goes out from and its replies come back to (Mdns.h)
    bool     mdnsOpen_ = false;
    uint32_t mdnsTick_ = kMdnsEverySec - 5;   // the first browse a few seconds after the socket opens
    ScratchBuffer<uint8_t> mdnsBuf_{*this};   // one reply, taken when the socket opens
    /// How often the mDNS browse goes out, in ticks: one small packet a minute.
    static constexpr uint32_t kMdnsEverySec = 60;
    static constexpr size_t   kMdnsReplyMax = 768;   ///< a responder's whole reply, an ESPHome board's runs to about 400 bytes
    static constexpr const char* kEsphomeService = "_esphomelib._tcp.local";
    static constexpr const char* kWledService = "_wled._tcp.local";   ///< WLED's, which MoonLight answers too, its TXT carrying the MAC WLED's presence packet lacks

    /// The added addresses, parsed from `addedIps`, and the one probe asking them in turn.
    static constexpr uint8_t kMaxAdded = 8;
    Host    addedHosts_[kMaxAdded] = {};
    uint8_t addedCount_ = 0;
    const char* addedError_ = nullptr;   // why `addedIps` was refused, kept in the status until it is fixed
    /// One HTTP request for an added address's page: its Server header names the firmware, its first <h2> the name a Tasmota was given.
    struct Probe {
        platform::TcpConnection conn;   ///< the request's connection
        uint8_t host = 0;      ///< which added address
        uint8_t state = 0;     ///< 0 idle, 1 connecting, 2 sending, 3 reading
        uint8_t sent = 0;      ///< how much of the request the socket took
        uint8_t ticks = 0;     ///< ticks in this state, which bounds a silent address
        uint8_t h2At = 0;      ///< how much of "<h2>" the page has matched across reads
        bool    inH2 = false;  ///< inside the first <h2>, whose text is the name
        char    server[40] = {};   ///< the Server header's value
        char    h2[24] = {};       ///< the first <h2>'s text
        uint8_t h2Len = 0;     ///< its length so far
        char    line[48] = {}; ///< the header line being read, for the Server one
        uint8_t lineLen = 0;   ///< its length so far
        bool    body = false;  ///< past the headers
    } probe_;
    uint32_t probeTick_ = 0;
    /// How often each added address is asked, in ticks, and how many ticks a probe waits for it.
    static constexpr uint32_t kProbeEverySec = 60;
    static constexpr uint8_t  kProbeWaitSec = 4;
    bool     listenerBound_ = false;
    uint32_t boundMs_ = 0;               // when the listener opened, which starts the both-ways window
    uint32_t broadcastTick_ = 0;         // counts tick1s ticks toward the next presence broadcast

    ScratchBuffer<Device> devices_{*this};   // its count() is the capacity, doubled as devices are heard
    ListRow deviceCount_ = 0;
    bool    memoryFull_ = false;   // a growth was refused since the last one that succeeded
    const char* selfName_ = nullptr;   // this device's name (wired via setSelfName)
    char    statusBuf_[104] = "idle";   // "65535 devices, multicast blocked: broadcasting too, memory full, addedIps: IP addresses only"

    /// Open a row's object with what the summary and the detail share, its name, address and kind, leaving the address text in `ip`.
    void openRow(JsonSink& sink, const Device& d, char (&ip)[16]) const {
        formatDottedQuad(ip, d.ip);
        sink.append("{\"name\":");
        sink.writeJsonString(d.name[0] ? d.name : ip);
        sink.appendf(",\"ip\":\"%s\",\"type\":\"%s\"", ip, devTypeStr(d.type));
        if (hasMac(d.mac)) sink.appendf(",\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\"", d.mac[0], d.mac[1], d.mac[2], d.mac[3], d.mac[4], d.mac[5]);
        if (d.type == DevType::Hue) sink.appendf(",\"color\":%u", d.colorCount);
    }

    /// Whether a kind is one of ours, a board MoonLight or MoonBase holds, each marked by its own sentinel.
    static bool ours(DevType t) { return t == DevType::MoonLight || t == DevType::MoonBase; }

    /// What this row is worth keeping when memory is full.
    static uint8_t worth(const Device& d) {
        return d.self ? kWorthSelf : (ours(d.type) ? kWorthMoonLight : kWorthOther);
    }

    /// A fresh row at `ip` for a newcomer worth `newcomerWorth`: a grown one, or, when memory refuses, the least recently heard row of the lowest worth not above the newcomer's; null when every row is worth more.
    Device* addRow(const uint8_t ip[4], uint8_t newcomerWorth) {
        if (isLinkLocal(ip)) return nullptr;   // reachable from its own link only, so no row: a second interface's fallback lists the device twice
        Device* d = nullptr;
        if (deviceCount_ < kMaxRows && devices_.ensure(deviceCount_ + 1u)) {
            d = &devices_[deviceCount_++];
            memoryFull_ = false;
        } else {
            memoryFull_ = true;
            d = leastWorthKeeping(newcomerWorth);
            if (!d) return nullptr;
        }
        *d = Device{};
        std::memcpy(d->ip, ip, 4);
        return d;
    }

    /// One flow a module reported this second; protocol and unit point at the reporter's literals.
    struct Flow { const char* protocol; const char* unit; const char* peerName; uint8_t peer[4]; uint32_t rate; bool outbound; };
    /// One service this device offers, as a module reported it.
    struct Service { const char* protocol; uint16_t port; };
    ScratchBuffer<Flow>    flows_{*this};      // this second's flows, refilled each second
    ScratchBuffer<Service> services_{*this};   // this second's services, refilled each second
    ListRow flowCount_ = 0, serviceCount_ = 0;
    uint8_t selfIp_[4] = {};                   // this device's address, the far end of an inbound flow's edge

    /// Takes every module's report into the flows and services, and drops what memory refuses, which the status then says.
    class Collector : public NetworkUse {
    public:
        /// Collects into `devices`' own tables.
        explicit Collector(DevicesModule& devices) : d_(devices) {}
        /// Add one service, or mark memory full when it refuses.
        void service(const char* protocol, uint16_t port) override {
            if (!d_.services_.ensure(d_.serviceCount_ + 1u)) { d_.memoryFull_ = true; return; }
            d_.services_[d_.serviceCount_++] = Service{protocol, port};
        }
        /// Add one flow, or mark memory full when it refuses.
        using NetworkUse::flow;   // the nameless form, beside the override
        void flow(const char* protocol, bool outbound, const uint8_t peer[4], uint32_t rate, const char* unit, const char* peerName) override {
            if (d_.flowCount_ == kMaxRows || !d_.flows_.ensure(d_.flowCount_ + 1u)) { d_.memoryFull_ = true; return; }
            Flow& f = d_.flows_[d_.flowCount_++];
            f = Flow{protocol, unit, peerName, {peer[0], peer[1], peer[2], peer[3]}, rate, outbound};
        }
    private:
        DevicesModule& d_;
    };

    /// Once a second: what the modules report, then the age-out, which a reported flow's sighting feeds.
    void refreshRows(const uint8_t local[4]) {
        collectNetworkUse();
        ageOut(local);
    }

    /// Ask every enabled module in the tree what it offers and exchanges, and give each peer a row: one nobody announced shows up, a multicast group or the broadcast address as a group.
    void collectNetworkUse() {
        flowCount_ = serviceCount_ = 0;
        Scheduler* scheduler = Scheduler::instance();
        if (!scheduler) return;
        Collector collector(*this);
        for (uint8_t i = 0; i < scheduler->moduleCount(); i++) report(scheduler->module(i), collector);
        bool added = false;
        for (ListRow i = 0; i < flowCount_; i++) added |= notePeer(flows_[i]);
        if (added) { sortByName(); refreshStatus(); }
    }

    /// One module's report and its children's, a disabled module and everything under it reporting nothing.
    static void report(const MoonModule* m, NetworkUse& use) {
        if (!m || !m->enabled()) return;
        m->reportNetworkUse(use);
        for (uint8_t i = 0; i < m->childCount(); i++) report(m->child(i), use);
    }

    /// The row of a flow's peer, added when missing; true when added. A stream from a peer, or any stream through a group, is a sighting; one sent to a silent receiver is not, so a dead receiver still ages out.
    bool notePeer(const Flow& f) {
        if (ipEq(f.peer, selfIp_)) return false;
        const bool group = isGroupAddress(f.peer);
        Device* d = findByIp(f.peer);
        bool added = false;
        if (!d) {
            d = addRow(f.peer, kWorthOther);
            if (!d) return false;
            d->type = group ? DevType::Group : DevType::Generic;
            if (f.peer[0] == 255) mm::formatTo(d->name, sizeof(d->name), "broadcast");
            else formatDottedQuad(d->name, f.peer);
            d->lastSeenMs = platform::millis();
            added = true;
        }
        // A name the reporting module learned, a client's User-Agent or a multicast group's format, names a row nothing else named: never a MoonLight board's or a WLED's own.
        if (f.peerName && (d->type == DevType::Generic || d->type == DevType::Group) && std::strcmp(d->name, f.peerName) != 0) {
            mm::formatTo(d->name, sizeof(d->name), "%s", f.peerName);
            added = true;   // re-sorted by its new name
        }
        if (!f.outbound || d->type == DevType::Group) {
            d->lastSeenMs = platform::millis();
        }
        return added;
    }

    /// A row's edges, the adjacency list the map draws: this device's outbound flows from its own row, a peer's inbound flow from the peer's row to this device.
    void writeEdges(JsonSink& sink, const Device& d) const {
        bool first = true;
        for (ListRow i = 0; i < flowCount_; i++) {
            const Flow& f = flows_[i];
            if (d.self ? !f.outbound : (f.outbound || !ipEq(f.peer, d.ip))) continue;
            char to[16];
            formatDottedQuad(to, d.self ? f.peer : selfIp_);
            sink.append(first ? ",\"edges\":[" : ",");
            first = false;
            sink.appendf("{\"to\":\"%s\",\"label\":\"%s", to, f.protocol);
            if (f.rate) sink.appendf(" %u%s", static_cast<unsigned>(f.rate), f.unit);   // "46/s"
            sink.append("\"}");
        }
        if (!first) sink.append("]");
    }

    /// This device's services, each a protocol and its port.
    void writeServices(JsonSink& sink) const {
        if (!serviceCount_) return;
        sink.append(",\"services\":[");
        for (ListRow i = 0; i < serviceCount_; i++) {
            const Service& s = services_[i];
            if (s.port) sink.appendf("%s\"%s %u\"", i ? "," : "", s.protocol, static_cast<unsigned>(s.port));
            else sink.appendf("%s\"%s\"", i ? "," : "", s.protocol);   // a service with no port, raw Ethernet's
        }
        sink.append("]");
    }

    /// The row a newcomer replaces: lowest worth first, then heard longest ago; never this device, never one worth more than the newcomer.
    Device* leastWorthKeeping(uint8_t newcomerWorth) {
        Device* victim = nullptr;
        for (ListRow i = 0; i < deviceCount_; i++) {
            Device& c = devices_[i];
            const uint8_t w = worth(c);
            if (c.self || w > newcomerWorth) continue;
            const bool lower = victim && w < worth(*victim);
            // Heard earlier, wrap-safe: the difference of two stamps, read signed.
            const bool older = victim && w == worth(*victim) && static_cast<int32_t>(c.lastSeenMs - victim->lastSeenMs) < 0;
            if (!victim || lower || older) victim = &c;
        }
        return victim;
    }

    /// Offer one datagram to each plugin, the first to claim it winning.
    void mergePacket(const uint8_t* data, size_t len, const uint8_t srcIp[4]) {
        if (!srcIp[0] && !srcIp[1] && !srcIp[2] && !srcIp[3]) return;  // no source
        for (const DevicePlugin* p : plugins_) {
            DiscoveredDevice found;
            if (p->classifyPacket(data, len, srcIp, found)) {
                upsertDevice(srcIp, found);
                if (WledPacket::hasMarker(data, len)) noteCopy(srcIp, WledPacket::viaBroadcast(data));
                return;
            }
        }
        // No plugin claimed it: an unrecognized packet on a port we listen on; ignore.
    }

    /// The discovery group, shared with MoonBase's announcement.
    static constexpr const uint8_t* kDiscoveryGroup = WledPacket::kDiscoveryGroup;

    /// Bind the listener once the network is up, which the plugins' shared port names.
    void ensureListener() {
        if (listenerBound_) return;
        const uint16_t port = plugins_[0]->discoveryPort();
        for (const DevicePlugin* p : plugins_)
            if (p->discoveryPort() != port) return;   // divergent ports unsupported yet: see note
        if (!listener_.open()) return;
        if (listener_.bind(port)) {
            listenerBound_ = true;
            boundMs_ = platform::millis();
            listener_.joinMulticast(kDiscoveryGroup);   // best-effort: a stack without multicast still hears the broadcast half
        } else {
            // Close it, or every retry would leak a descriptor until the process runs out.
            listener_.close();
        }
    }

    /// Ask once a minute who offers the ESPHome and WLED services, and read a few replies a tick: devices silent on the presence port, and WLED's MAC.
    void browseMdns() {
        if (!mdnsOpen_) {
            if (!mdns_.open()) return;
            if (!mdns_.bind(0) || !mdnsBuf_.resize(kMdnsReplyMax)) { mdns_.close(); return; }   // an ordinary port: the platform's own responder keeps 5353
            mdnsOpen_ = true;
        }
        if (++mdnsTick_ >= kMdnsEverySec) {
            mdnsTick_ = 0;
            for (const char* service : {kEsphomeService, kWledService}) {
                uint8_t q[48];
                const size_t n = mdns::buildQuery(service, q, sizeof(q));
                if (n) mdns_.sendToAddr(mdns::kGroup, mdns::kPort, q, n);
            }
        }
        uint8_t src[4];
        for (int i = 0; i < kMaxDrainPerTick; i++) {
            const int n = mdns_.recvFrom(mdnsBuf_.data(), mdnsBuf_.count(), src);
            if (n <= 0) break;
            noteMdnsReply(mdnsBuf_.data(), static_cast<size_t>(n), src);
        }
    }

    /// A reply to either browse: an ESPHome device named and versioned as it announces itself, or the MAC of a row a presence packet made.
    void noteMdnsReply(const uint8_t* data, size_t len, const uint8_t ip[4]) {
        mdns::Service s;
        if (mdns::parseReply(data, len, kEsphomeService, s)) { noteMdnsDevice(ip, s); return; }
        if (!mdns::parseReply(data, len, kWledService, s)) return;
        // Only a MAC the row lacks: MoonLight boards announce this service too, for WLED's apps, from either interface, so the presence packet keeps the row's identity, type and name.
        Device* d = findByIp(ip);
        if (!d || hasMac(d->mac) || !hasMac(s.mac) || findByMac(s.mac)) return;
        std::memcpy(d->mac, s.mac, 6);
        refreshStatus();
    }

    /// A device that answered the ESPHome browse: its row by its MAC, else the reply's source address, named and versioned as it announces itself.
    void noteMdnsDevice(const uint8_t ip[4], const mdns::Service& s) {
        DiscoveredDevice found;
        found.type = DevType::Esphome;
        setDeviceName(found, s.name);
        std::memcpy(found.mac, s.mac, 6);
        upsertDevice(ip, found);
        Device* d = findByIp(ip);
        if (d && std::strcmp(d->version, s.version) != 0) mm::formatTo(d->version, sizeof(d->version), "%s", s.version);
        if (d && s.chip[0]) mm::formatTo(d->chip, sizeof(d->chip), "%s", s.chip);   // what the map's move installs for
    }

    /// Ask each added address for its page, one at a time and once a minute, never waiting: a connect and a read move one step a tick.
    void probeAdded() {
        if (!addedCount_) return;
        Probe& p = probe_;
        if (p.state == 0) {
            if (p.host == 0 && ++probeTick_ < kProbeEverySec) return;
            probeTick_ = 0;
            char ip[16];
            formatDottedQuad(ip, addedHosts_[p.host].ip);
            p.ticks = 0;
            p.state = p.conn.connectStart(ip, WledPacket::kDefaultHttpPort) ? 1 : 0;
            if (!p.state) nextAdded();
            return;
        }
        if (++p.ticks > kProbeWaitSec) { finishProbe(); return; }   // silent: the next one
        if (p.state == 1) {
            const auto r = p.conn.connectPoll();
            if (r == platform::TcpConnection::ConnectResult::Failed) { finishProbe(); return; }
            if (r == platform::TcpConnection::ConnectResult::Pending) return;
            p.state = 2;   // connected: send the request
        }
        if (p.state == 2) {
            // What the socket takes now, the rest on the next tick: a peer that never reads must not hold the tick.
            static constexpr char kGet[] = "GET / HTTP/1.0\r\nConnection: close\r\n\r\n";
            const int n = p.conn.writeSome(reinterpret_cast<const uint8_t*>(kGet) + p.sent, sizeof(kGet) - 1 - p.sent);
            if (n < 0) { finishProbe(); return; }
            p.sent = static_cast<uint8_t>(p.sent + n);
            if (p.sent < sizeof(kGet) - 1) return;
            p.state = 3;
            p.ticks = 0;
        }
        uint8_t buf[256];
        for (int i = 0; i < 24; i++) {   // a few KB a tick at most
            const int n = p.conn.read(buf, sizeof(buf));
            if (n < 0) return;                       // nothing yet: next tick
            if (n == 0) { finishProbe(); return; }  // the page ended
            if (scanPage(buf, static_cast<size_t>(n))) { finishProbe(); return; }
        }
    }

    /// Read a piece of the page: the Server header among the headers, then the first <h2>'s text; true once that text is complete.
    bool scanPage(const uint8_t* b, size_t n) {
        Probe& p = probe_;
        static constexpr char kH2[] = "<h2>";
        for (size_t i = 0; i < n; i++) {
            const char c = static_cast<char>(b[i]);
            if (!p.body) {
                if (c == '\n') {
                    p.line[p.lineLen] = 0;
                    if (p.lineLen == 0) p.body = true;   // the blank line ends the headers
                    else if (const char* h = http::findHeader(p.line, "Server:")) mm::formatTo(p.server, sizeof(p.server), "%s", http::skipSpace(h + 7));
                    p.lineLen = 0;
                } else if (c != '\r' && p.lineLen + 1u < sizeof(p.line)) {
                    p.line[p.lineLen++] = c;
                }
            } else if (p.inH2) {
                if (c == '<') return true;
                if (p.h2Len + 1u < sizeof(p.h2)) { p.h2[p.h2Len++] = c; p.h2[p.h2Len] = 0; }
            } else {
                p.h2At = (c == kH2[p.h2At]) ? p.h2At + 1 : (c == kH2[0] ? 1 : 0);
                if (p.h2At == sizeof(kH2) - 1) p.inH2 = true;
            }
        }
        return false;
    }

    /// What the page said about the device, its row named and versioned by it, and the next address.
    void finishProbe() {
        Probe& p = probe_;
        if (p.server[0]) {
            DiscoveredDevice found;
            const char* slash = std::strchr(p.server, '/');
            const bool tasmota = std::strncmp(p.server, "Tasmota/", 8) == 0;
            found.type = tasmota ? DevType::Tasmota : DevType::Generic;
            setDeviceName(found, p.h2[0] ? p.h2 : "");
            const uint8_t* ip = addedHosts_[p.host].ip;
            platform::arpLookup(ip, found.mac);   // the probe's connection put its MAC in the address table: the identity a page need not give
            upsertDevice(ip, found);
            if (Device* d = findByIp(ip); d && tasmota && slash) {
                char v[16] = {};
                size_t k = 0;
                for (const char* q = slash + 1; *q && *q != ' ' && k + 1 < sizeof(v); q++) v[k++] = *q;   // "16.0.0" from "Tasmota/16.0.0 (ESP32-D0WD-V3)"
                mm::formatTo(d->version, sizeof(d->version), "%s", v);
                tasmotaChip(p.server, d->chip, sizeof(d->chip));
            }
        }
        nextAdded();
    }

    /// The chip in a Tasmota's Server header, "(ESP32-C3FH4)" or "(ESP32-D0WD-V3)", as MoonLight's files spell it: the family inside the brackets, the classic's silicon names being the classic.
    static void tasmotaChip(const char* server, char* out, size_t cap) {
        const char* open = std::strstr(server, "(ESP32");
        if (!open) return;
        const char* close = std::strchr(open, ')');
        mdns::chipFamily(open + 6, close ? static_cast<size_t>(close - open - 6) : std::strlen(open + 6), out, cap);
    }

    /// Close this probe and move to the next added address, back to the first after the last.
    void nextAdded() {
        const uint8_t next = static_cast<uint8_t>((probe_.host + 1) % (addedCount_ ? addedCount_ : 1));
        probe_.conn.close();
        probe_ = Probe{};
        probe_.host = next;
    }

    /// Announce ourselves: a compatible packet, stamped so a peer types us correctly.
    void broadcastPresence(const uint8_t ip[4]) {
        uint8_t pkt[WledPacket::kSizeWithMac];
        const char* n = (selfName_ && selfName_[0]) ? selfName_ : "MoonLight";
        WledPacket::build(pkt, ip, n, boardTypeByte(), /*lightsOn=*/true);
        WledPacket::stampMarker(pkt);
        uint8_t mac[6];
        platform::getMacAddress(mac);
        const size_t len = WledPacket::stampTail(pkt, selfHttpPort(), mac);
        // Both ways during the bootstrap window, so a peer can tell which way reaches it.
        const bool bothWays = addressing == kDevMulticastBroadcast || platform::millis() - boundMs_ < kBothWaysMs;
        sendAddressed(bothWays ? Addressing::MulticastBroadcast : Addressing::Multicast, Traffic::Occasional, nullptr, 0,
                      kDiscoveryGroup, [&](const uint8_t to[4]) {
                          WledPacket::stampVia(pkt, std::memcmp(to, kBroadcastAddress, 4) == 0);
                          listener_.sendToAddr(to, WledPacket::kPort, pkt, len);
                      });
    }

    /// The port this device's page answers on, as the web server reports its service; 80 before the first report.
    uint16_t selfHttpPort() const {
        for (ListRow i = 0; i < serviceCount_; i++)
            if (std::strcmp(services_[i].protocol, kHttpService) == 0) return services_[i].port;
        return WledPacket::kDefaultHttpPort;
    }

    /// Track which way a MoonLight peer's presence arrives, and prove the network drops multicast when only broadcast keeps arriving.
    void noteCopy(const uint8_t ip[4], bool broadcast) {
        Device* d = findByIp(ip);
        if (!d || d->self) return;   // our own copies loop back and prove nothing
        bool dropped = NetworkPath::multicastDropped;
        if (!broadcast) {
            // A multicast copy from any peer proves the group arrives, a slow join being the likelier story.
            d->broadcastRun = 0;
            dropped = false;
        } else if (d->broadcastRun < 255 && ++d->broadcastRun == kEvidence) {
            // Only the run reaching the threshold sets it, so a peer whose group never arrives cannot undo another's multicast every ten seconds.
            dropped = true;
        }
        if (dropped != NetworkPath::multicastDropped) {
            NetworkPath::multicastDropped = dropped;
            refreshStatus();
        }
    }

    /// The board-type byte, read from the chip model, which is informational only.
    static uint8_t boardTypeByte() {
        const char* m = platform::chipModel();
        if (std::strstr(m, "S3")) return 34;
        if (std::strstr(m, "S2")) return 33;
        if (std::strstr(m, "C3")) return 35;
        if (std::strstr(m, "P4")) return 36;
        return 32;
    }

    /// Find or insert a classified device, re-sorting only when a shown field changes.
    void upsertDevice(const uint8_t ip[4], const DiscoveredDevice& found) {
        uint8_t local[4] = {};
        platform::localIPv4(local);
        const bool isSelf = ipEq(ip, local);
        bool changed = false;
        Device* d = rowFor(ip, found.mac, changed);
        if (!d) {
            const uint8_t newcomerWorth = isSelf ? kWorthSelf
                                        : (ours(found.type) ? kWorthMoonLight : kWorthOther);
            d = addRow(ip, newcomerWorth);
            if (!d) { refreshStatus(); return; }   // memory full and every row worth more
            d->type = found.type;          // first sighting: take the plugin's type
            std::memcpy(d->mac, found.mac, 6);
            changed = true;
        }
        // The latest marked packet says MoonLight or MoonBase; an unmarked one takes over once marked ones stop for three intervals, a board flashed to WLED.
        const uint32_t now = platform::millis();
        if (ours(found.type)) d->markedMs = now;
        const bool markedLately = ours(d->type) && now - d->markedMs < kMarkedHoldMs;
        // A generic sighting, such as the address probe's page with no kind of its own, carries no type evidence, so it keeps the row's.
        const bool keepsType = markedLately || found.type == DevType::Generic;
        const DevType newType = isSelf ? DevType::MoonLight
                              : (ours(found.type) ? found.type : (keepsType ? d->type : found.type));
        if (d->type != newType) {
            d->type = newType;
            d->version[0] = 0;   // what the old kind announced, a Tasmota's version on what is now MoonBase: the new kind's own source fills it again
            d->chip[0] = 0;
            changed = true;
        }
        if (d->self != isSelf) { d->self = isSelf; changed = true; }
        if (d->httpPort != found.httpPort) { d->httpPort = found.httpPort; changed = true; }
        d->lastSeenMs = platform::millis();
        // Only an authoritative packet renames a row, though an empty name is always filled.
        const bool authoritative =
            (ours(found.type) && d->type == found.type) || found.type == DevType::Esphome || found.type == DevType::Tasmota ||
            (found.type == DevType::Wled);
        if (found.name[0] && (!d->name[0] || isIpPlaceholder(d->name, ip) || authoritative)
            && std::strcmp(d->name, found.name) != 0) {
            std::snprintf(d->name, sizeof(d->name), "%s", found.name);
            changed = true;
        }
        if (!d->name[0]) { formatDottedQuad(d->name, ip); changed = true; }
        if (changed) {
            sortByName();
            refreshStatus();
        }
    }

    /// Keep our own row at the current address, re-sorting only on a real change.
    void upsertSelf(const uint8_t ip[4]) {
        bool changed = false;
        // Demote any prior row at another address, so it expires rather than staying immortal.
        for (ListRow i = 0; i < deviceCount_; i++)
            if (devices_[i].self && !ipEq(devices_[i].ip, ip)) { devices_[i].self = false; changed = true; }

        Device* d = findByIp(ip);
        if (!d) {
            d = addRow(ip, kWorthSelf);
            if (!d) { refreshStatus(); return; }   // no row at all to take: an empty block memory refuses to grow
            changed = true;
        }
        if (d->type != DevType::MoonLight) { d->type = DevType::MoonLight; changed = true; }
        if (!d->self) { d->self = true; changed = true; }
        std::memcpy(selfIp_, ip, 4);
        d->lastSeenMs = platform::millis();
        if (!d->name[0]) {
            const char* n = (selfName_ && selfName_[0]) ? selfName_ : "this device";
            mm::formatTo(d->name, sizeof(d->name), "%s", n);
            changed = true;
        }
        if (changed) {
            sortByName();
            refreshStatus();
        }
    }

    /// Whether a row is a board heard this session under the first `len` characters of `name`.
    static bool heardAs(const Device& d, const char* name, size_t len) MM_NONBLOCKING {
        return std::strlen(d.name) == len && asciiCaseEqual(d.name, name, len) && !isIpPlaceholder(d.name, d.ip);
    }

    /// Whether this name is only the address, a placeholder a real name overwrites.
    static bool isIpPlaceholder(const char* name, const uint8_t ip[4]) MM_NONBLOCKING {
        char ipStr[16];
        formatDottedQuad(ipStr, ip);
        return std::strcmp(name, ipStr) == 0;
    }

    /// The row a sighting from `ip` naming `mac` belongs to, null for a newcomer: the row holding the MAC, else the one at the address.
    Device* rowFor(const uint8_t ip[4], const uint8_t mac[6], bool& changed) {
        Device* byIp = findByIp(ip);
        if (!hasMac(mac)) return byIp;
        if (Device* byMac = findByMac(mac)) {
            if (byMac == byIp) return byMac;
            if (byIp) { dropRow(byIp); byMac = findByMac(mac); }   // the address's old row: this device's own before it named its MAC, or a device gone from it
            std::memcpy(byMac->ip, ip, 4);
            changed = true;
            return byMac;
        }
        if (byIp && hasMac(byIp->mac)) { dropRow(byIp); changed = true; return nullptr; }
        if (byIp) { std::memcpy(byIp->mac, mac, 6); changed = true; }
        return byIp;
    }

    /// Whether a source named a MAC: all zero means it did not.
    static bool hasMac(const uint8_t mac[6]) {
        for (int i = 0; i < 6; i++) if (mac[i]) return true;
        return false;
    }

    /// The device with this MAC, or null.
    Device* findByMac(const uint8_t mac[6]) {
        for (ListRow i = 0; i < deviceCount_; i++)
            if (std::memcmp(devices_[i].mac, mac, 6) == 0) return &devices_[i];
        return nullptr;
    }

    /// Remove one row, keeping the rest in order.
    void dropRow(Device* d) {
        const ListRow at = static_cast<ListRow>(d - devices_.data());
        for (ListRow r = at; r + 1u < deviceCount_; r++) devices_[r] = devices_[r + 1u];
        deviceCount_--;
    }

    /// The device at this address, or null.
    Device* findByIp(const uint8_t ip[4]) {
        for (ListRow i = 0; i < deviceCount_; i++)
            if (ipEq(devices_[i].ip, ip)) return &devices_[i];
        return nullptr;
    }

    /// Whether two addresses match.
    static bool ipEq(const uint8_t a[4], const uint8_t b[4]) MM_NONBLOCKING {
        return std::memcmp(a, b, 4) == 0;
    }

    /// A host setting's name as the board announcing it, through whichever instance is active.
    static const char* boardNameAt(const uint8_t ip[4]) MM_NONBLOCKING {
        const DevicesModule* devices = active();
        return devices ? devices->nameOf(ip) : nullptr;
    }

    static bool resolveBoard(const char* name, uint8_t ip[4]) {
        const DevicesModule* devices = active();
        return devices && devices->addressOf(name, ip);
    }

    /// Drop whatever has gone unheard past its window, compacting in place.
    void ageOut(const uint8_t local[4]) {
        const uint32_t now = platform::millis();
        ListRow w = 0;
        for (ListRow r = 0; r < deviceCount_; r++) {
            Device& d = devices_[r];
            const bool isUs = ipEq(d.ip, local);
            // Guarded on the address rather than the flag, so a stale row still ages out.
            if (isUs) d.lastSeenMs = now;
            if (!isUs && (now - d.lastSeenMs) > kStaleMs) continue;   // drop, stale
            if (w != r) devices_[w] = d;
            w++;
        }
        if (w == deviceCount_) return;   // nothing dropped: common case, no churn
        deviceCount_ = w;
        refreshStatus();
    }

    /// Report how many devices are listed, whether multicast arrives and whether memory is full.
    void refreshStatus() {
        mm::formatTo(statusBuf_, sizeof(statusBuf_), "%u device%s%s%s%s%s",
                      deviceCount_, deviceCount_ == 1 ? "" : "s",
                      NetworkPath::multicastDropped ? ", multicast blocked: broadcasting too" : "",
                      memoryFull_ ? ", memory full" : "", addedError_ ? ", " : "", addedError_ ? addedError_ : "");
        setStatus(statusBuf_, addedError_ ? Severity::Warning : Severity::Status);
    }

    /// Order the list by name, which core's sort does from the comparator below.
    void sortByName() {
        mm::insertionSort(devices_.data(), deviceCount_, [](const Device& a, const Device& b) {
            return ciLess(a.name, b.name);
        });
    }

    /// Whether one name sorts before another, ignoring case.
    static bool ciLess(const char* a, const char* b) {
        for (; *a && *b; a++, b++) {
            int ca = lower(*a), cb = lower(*b);
            if (ca != cb) return ca < cb;
        }
        return lower(*a) < lower(*b);   // shorter string sorts first
    }
    /// One character lowered.
    static int lower(char c) { return (c >= 'A' && c <= 'Z') ? c + 32 : static_cast<unsigned char>(c); }
};

}  // namespace mm
