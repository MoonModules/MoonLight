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
#include "platform/platform.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

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
/// This device sits in the middle, ringed, each kind in its own color. A line is traffic, labeled with its protocol and rate. Devices without traffic sit in rows beneath. A click opens a device's details, and for a MoonLight device an update to a newer release.
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
/// **Discovery is passive.** Each device announces a presence packet on the multicast group and the broadcast address WLED listens on, and this listens for both. A device appears when heard and ages out when it stops.
///
/// **Ports and addresses.** A device whose page is not on port 80 adds its port to the packet. A link-local address (169.254.0.0/16) gets no row: no peer reaches it, and a board's second interface would list the board twice.
///
/// **Plugins are the interop seam.** A foreign ecosystem hooks in as a plugin, not a branch. Each declares its port and turns a datagram into a device kind, ours offered first; a new system is one new file.
///
/// **Aging out.** Each sighting is stamped, and a device unheard for a day leaves the list. The list is runtime state, never saved: every device announces itself again within seconds. Discovery only, never device commands.
///
/// **The map's edges come from the modules.** Once a second every enabled module reports its services and traffic (NetworkUse). A peer gets a row, a group address one of kind group. This device's row carries its outbound flows and services, a peer's row its inbound flow. A flow from a peer is a sighting; one to a silent receiver is not, so a dead receiver ages out.
///
/// **Growth, and what memory refuses.** The rows are one block that doubles when a device needs a row, checked against the heap's reserve, so N devices cost about log₂ N allocations. A refused growth replaces the least recently heard row of the lowest worth: other devices, then MoonLight boards, never this device.
class DevicesModule : public MoonModule, public ListSource {
public:
    /// Adopt this device's own name, so its own row matches what everything else shows.
    void setSelfName(const char* name) { selfName_ = name; }

    /// The devices drawn as a map above the list, each row a node keyed by its address.
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
        }
        sink.append("}");
    }

    /// The rows are rebuilt from what is heard, so the list is never saved.
    bool persistsList() const override { return false; }

    /// The modes `addressing` picks from; its options start one entry into the shared Addressing names, since presence is never unicast.
    enum Mode : uint8_t { kDevMulticast, kDevMulticastBroadcast, kDevModeCount };
    /// How presence travels: multicast, or multicast and broadcast.
    /// @guide Broadcast is the default, since WLED devices hear it too.
    uint8_t addressing = kDevMulticastBroadcast;

    /// Declare the addressing and the list of discovered devices.
    void defineControls() override {
        MoonModule::defineControls();
        controls_.addSelect("addressing", addressing, kAddressingNames + 1, kDevModeCount);
        /// The discovered devices; a row expands to its details.
        /// @field name The device's name, else its address.
        /// @field ip Its address on the network.
        /// @field url Its own page, opened in a new tab.
        /// @field type What it is: a MoonLight, a WLED, a Hue bridge or a generic device.
        /// @field color How many color lights a Hue bridge has.
        /// @field ageSec When it was last heard.
        controls_.addList("devices", *this);   // this module is the ListSource
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
        uint16_t httpPort = WledPacket::kDefaultHttpPort;   ///< the port its web page answers on, from its presence
    };

    /// The seat behind `active()`, claimed on build and vacated on release.
    ActiveInstance<DevicesModule> seat_{*this};

    /// The most rows the list index can address, a bound memory reaches first.
    static constexpr ListRow  kMaxRows = static_cast<ListRow>(~ListRow{0});
    /// What a row is worth keeping when memory is full: this device most, then MoonLight boards, then everything else.
    static constexpr uint8_t  kWorthOther = 0, kWorthMoonLight = 1, kWorthSelf = 2;
    /// How often we announce ourselves, in ticks, which is how fast a peer discovers us.
    static constexpr uint32_t kBroadcastEverySec = 10;
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
    WledPlugin wledPlugin_;
    static constexpr uint8_t kPluginCount = 2;
    const DevicePlugin* plugins_[kPluginCount] = { &mmPlugin_, &wledPlugin_ };

    platform::UdpSocket listener_;       // bound to the presence port; drained each tick
    bool     listenerBound_ = false;
    uint32_t boundMs_ = 0;               // when the listener opened, which starts the both-ways window
    uint32_t broadcastTick_ = 0;         // counts tick1s ticks toward the next presence broadcast

    ScratchBuffer<Device> devices_{*this};   // its count() is the capacity, doubled as devices are heard
    ListRow deviceCount_ = 0;
    bool    memoryFull_ = false;   // a growth was refused since the last one that succeeded
    const char* selfName_ = nullptr;   // this device's name (wired via setSelfName)
    char    statusBuf_[72] = "idle";   // "65535 devices, multicast blocked: broadcasting too, memory full"

    /// Open a row's object with what the summary and the detail share, its name, address and kind, leaving the address text in `ip`.
    void openRow(JsonSink& sink, const Device& d, char (&ip)[16]) const {
        formatDottedQuad(ip, d.ip);
        sink.append("{\"name\":");
        sink.writeJsonString(d.name[0] ? d.name : ip);
        sink.appendf(",\"ip\":\"%s\",\"type\":\"%s\"", ip, devTypeStr(d.type));
        if (d.type == DevType::Hue) sink.appendf(",\"color\":%u", d.colorCount);
    }

    /// What this row is worth keeping when memory is full.
    static uint8_t worth(const Device& d) {
        return d.self ? kWorthSelf : (d.type == DevType::MoonLight ? kWorthMoonLight : kWorthOther);
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
    struct Flow { const char* protocol; const char* unit; uint8_t peer[4]; uint32_t perSecond; bool outbound; };
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
        void flow(const char* protocol, bool outbound, const uint8_t peer[4], uint32_t perSecond, const char* unit) override {
            if (d_.flowCount_ == kMaxRows || !d_.flows_.ensure(d_.flowCount_ + 1u)) { d_.memoryFull_ = true; return; }
            Flow& f = d_.flows_[d_.flowCount_++];
            f = Flow{protocol, unit, {peer[0], peer[1], peer[2], peer[3]}, perSecond, outbound};
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
            if (f.perSecond) sink.appendf(" %u %s", static_cast<unsigned>(f.perSecond), f.unit);
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
                if (WledPacket::hasMmMarker(data, len)) noteCopy(srcIp, WledPacket::viaBroadcast(data));
                return;
            }
        }
        // No plugin claimed it: an unrecognized packet on a port we listen on; ignore.
    }

    /// The discovery group, in the organization-local scope that is never routed off the network.
    static constexpr uint8_t kDiscoveryGroup[4] = {239, 255, 77, 77};

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

    /// Announce ourselves: a compatible packet, stamped so a peer types us correctly.
    void broadcastPresence(const uint8_t ip[4]) {
        uint8_t pkt[WledPacket::kSizeWithPort];
        const char* n = (selfName_ && selfName_[0]) ? selfName_ : "MoonLight";
        WledPacket::build(pkt, ip, n, boardTypeByte(), /*lightsOn=*/true);
        WledPacket::stampMmMarker(pkt);
        const size_t len = WledPacket::stampHttpPort(pkt, selfHttpPort());
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
        Device* d = findByIp(ip);
        bool changed = false;
        if (!d) {
            const uint8_t newcomerWorth = isSelf ? kWorthSelf
                                        : (found.type == DevType::MoonLight ? kWorthMoonLight : kWorthOther);
            d = addRow(ip, newcomerWorth);
            if (!d) { refreshStatus(); return; }   // memory full and every row worth more
            d->type = found.type;          // first sighting: take the plugin's type
            changed = true;
        }
        // The marker is definitive, so an established identity is raised toward, never downgraded.
        const bool isMm = isSelf || found.type == DevType::MoonLight;
        const DevType newType = isMm ? DevType::MoonLight
                              : (d->type != DevType::MoonLight ? found.type : d->type);
        if (d->type != newType) { d->type = newType; changed = true; }
        if (d->self != isSelf) { d->self = isSelf; changed = true; }
        if (d->httpPort != found.httpPort) { d->httpPort = found.httpPort; changed = true; }
        d->lastSeenMs = platform::millis();
        // Only an authoritative packet renames a row, though an empty name is always filled.
        const bool authoritative =
            (found.type == DevType::MoonLight && d->type == DevType::MoonLight) ||
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
        mm::formatTo(statusBuf_, sizeof(statusBuf_), "%u device%s%s%s",
                      deviceCount_, deviceCount_ == 1 ? "" : "s",
                      NetworkPath::multicastDropped ? ", multicast blocked: broadcasting too" : "",
                      memoryFull_ ? ", memory full" : "");
        setStatus(statusBuf_);
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
