#pragma once

#include "core/module/MoonModule.h"
#include "core/util/ActiveInstance.h"   // the boot-registry seat election (the seat + its RAII vacate)
#include "core/module/Control.h"
#include "core/util/JsonSink.h"
#include "core/util/JsonUtil.h"         // recursive reader — restoreList parses the persisted array
#include "core/util/Sort.h"             // mm::insertionSort — generic bounded sort (core); we supply the comparator
#include "core/system/DeviceIdentify.h"   // DevType, devTypeStr (the device-kind enum + its labels)
#include "core/system/DevicePlugin.h"     // the interop plugin seam + the bundled plugins
#include "core/system/FilesystemModule.h" // FilesystemModule::noteDirty — persist on list change
#include "platform/platform.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace mm {

/// Discovers other devices on the LAN and presents them as a browsable list.
///
/// It covers every device on the network, this one included and marked as such.
/// Core and domain-neutral: it finds a device, and light modules consume the list.
/// A submodule of the network module, discovery depending on the network being up.
/// @card DevicesModule.png
///
/// @moreinfo
///
/// ## Discovery is passive
///
/// Each device broadcasts a small presence packet, and this listens for them.
/// There is no subnet sweep: a device appears when heard, and ages out when it stops.
/// We announce ourselves on a slow cadence, so peers discover us.
///
/// ## Plugins are the interop seam
///
/// A foreign ecosystem hooks in as a plugin rather than a branch.
/// Each declares its port and turns a datagram into a device kind, ours offered first.
/// A new system is one new file, and a device found out of band registers through a seam.
///
/// ## Ageing out, and persistence
///
/// Each sighting is stamped, and a live-confirmed device is kept as durable history.
/// A row restored from persistence gets a short probation, so a ghost cannot persist.
/// The list persists, so the last-known set shows on boot before any packet arrives.
/// This module does discovery only, never device-to-device commands.
class DevicesModule : public MoonModule, public ListSource {
public:
    /// Adopt this device's own name, so its own row matches what everything else shows.
    void setSelfName(const char* name) { selfName_ = name; }

    /// How many devices are currently listed.
    uint8_t listRowCount() const override { return deviceCount_; }

    /// Append one device's summary: its name, address and kind.
    void writeListRow(JsonSink& sink, uint8_t row) const override {
        if (row >= deviceCount_) { sink.append("{}"); return; }
        const Device& d = devices_[row];
        char ip[16];
        formatDottedQuad(ip, d.ip);
        sink.append("{\"name\":");
        sink.writeJsonString(d.name[0] ? d.name : ip);
        sink.appendf(",\"ip\":\"%s\",\"type\":\"%s\"", ip, devTypeStr(d.type));
        if (d.type == DevType::Hue) sink.appendf(",\"color\":%u", d.colorCount);
        if (d.self) sink.append(",\"self\":true");
        sink.append("}");
    }

    /// Append one device's detail, which adds its address and how long since it was heard.
    void writeListRowDetail(JsonSink& sink, uint8_t row) const override {
        if (row >= deviceCount_) { sink.append("{}"); return; }
        const Device& d = devices_[row];
        char ip[16];
        formatDottedQuad(ip, d.ip);
        sink.append("{\"name\":");
        sink.writeJsonString(d.name[0] ? d.name : ip);
        sink.appendf(",\"ip\":\"%s\",\"url\":\"http://%s/\",\"type\":\"%s\"",
                     ip, ip, devTypeStr(d.type));
        if (d.type == DevType::Hue) sink.appendf(",\"color\":%u", d.colorCount);
        if (d.self) {
            sink.append(",\"self\":true");   // always now, so no age is meaningful
        } else if (d.cached) {
            // Restored but not re-heard, so an age would be the boot stamp rather than real.
            sink.append(",\"cached\":true");
        } else {
            // Computed here so the UI gets one finished number, and wrap-safe.
            uint32_t ageSec = (platform::millis() - d.lastSeenMs) / 1000u;
            sink.appendf(",\"ageSec\":%u", static_cast<unsigned>(ageSec));
        }
        sink.append("}");
    }

    /// Rebuild the list from the saved array, so the last-known set shows before any packet.
    bool restoreList(const char* json, const char* key) override {
        deviceCount_ = 0;
        const bool ok = mm::json::forEachListElement(json, key,
            [&](const mm::json::JsonDoc& doc, const mm::json::JsonNode* el) {
                if (deviceCount_ >= kMaxDevices) return;
                if (mm::json::readBool(mm::json::member(doc, el, "self"))) return;  // skip persisted self
                char ipStr[16] = {}, name[24] = {}, typeStr[12] = {};
                mm::json::readString(mm::json::member(doc, el, "ip"), ipStr, sizeof(ipStr));
                mm::json::readString(mm::json::member(doc, el, "name"), name, sizeof(name));
                mm::json::readString(mm::json::member(doc, el, "type"), typeStr, sizeof(typeStr));
                uint8_t octets[4];
                if (!parseDottedQuad(ipStr, octets)) return;
                Device& d = devices_[deviceCount_++];
                std::memcpy(d.ip, octets, 4);
                std::snprintf(d.name, sizeof(d.name), "%s", name);
                d.type = (std::strcmp(typeStr, "MoonLight") == 0)  ? DevType::MoonLight
                       : (std::strcmp(typeStr, "WLED") == 0)       ? DevType::Wled
                       : (std::strcmp(typeStr, "Hue bridge") == 0) ? DevType::Hue
                                                                   : DevType::Generic;
                // Clamped, so a corrupt or hand-edited entry cannot wrap when narrowed.
                const long color = mm::json::readInt(mm::json::member(doc, el, "color"));
                d.colorCount = static_cast<uint8_t>(color < 0 ? 0 : (color > 127 ? 127 : color));
                d.self = false;
                d.cached = true;  // restored rather than heard, so the UI says so
                // Stamped now, which starts its probation rather than the full window.
                d.lastSeenMs = platform::millis();
            });
        sortByName();   // cached list shows alphabetically too, before the first sighting
        return ok;      // false on a malformed/missing file (list left empty)
    }

    /// Whether to announce on the broadcast address as well as the multicast group.
    bool wledCompatible = false;

    /// Declare the compatibility toggle and the list of discovered devices.
    void defineControls() override {
        MoonModule::defineControls();
        controls_.addControl("wledCompatible", wledCompatible);
        controls_.addList("devices", *this);   // this module is the ListSource
    }

    /// The one live instance, through which a light-domain driver registers what it found.
    static DevicesModule* active() { return ActiveInstance<DevicesModule>::active(); }

    /// Register a bridge a driver found out of band, updating its row or inserting one.
    void upsertHueBridge(const uint8_t ip[4], const char* name, uint8_t color) {
        Device* d = findByIp(ip);
        bool persistChanged = false;
        if (!d) {
            if (deviceCount_ >= kMaxDevices) return;   // bounded; silently cap
            d = &devices_[deviceCount_++];
            std::memcpy(d->ip, ip, 4);
            persistChanged = true;
        }
        if (d->type != DevType::Hue) { d->type = DevType::Hue; persistChanged = true; }
        if (name && name[0] && std::strcmp(d->name, name) != 0) {
            std::snprintf(d->name, sizeof(d->name), "%s", name);
            persistChanged = true;
        }
        if (!d->name[0]) { formatDottedQuad(d->name, ip); persistChanged = true; }
        if (d->colorCount != color) { d->colorCount = color; persistChanged = true; }
        d->lastSeenMs = platform::millis();   // transient — keeps the bridge from ageing out
        // A cached row coming back online is a status change even with no field edit.
        const bool wasCached = d->cached;
        d->cached = false;
        if (persistChanged) sortByName();              // re-sort only on a real persisted change
        if (persistChanged || wasCached) refreshStatus();
    }

    /// Report the cached list on boot, which persistence restored before this ran.
    void setup() override {
        MoonModule::setup();
        if (deviceCount_) {
            std::snprintf(statusBuf_, sizeof(statusBuf_), "%u device%s (cached)",
                          deviceCount_, deviceCount_ == 1 ? "" : "s");
        }
        setStatus(statusBuf_);
    }

    /// Claim the seat that makes this the instance a driver registers through.
    void prepare() override {
        seat_.claim();
    }

    /// Vacate the seat and close the socket, which releases its port.
    void release() override {
        seat_.vacate();
        listener_.close();
        listenerBound_ = false;
        MoonModule::release();
    }

    /// Drain inbound packets, announce ourselves on a slow cadence, and age out the silent.
    void tick1s() MM_NONBLOCKING override {
        MoonModule::tick1s();
        uint8_t local[4] = {};
        localIp(local);
        const bool online = local[0] || local[1] || local[2] || local[3];
        if (!online) return;   // no network yet — nothing to discover

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

        ageOut(local);
    }

    ModuleRole role() const MM_NONBLOCKING override { return ModuleRole::Generic; }

    /// Feed one packet through the real pipeline, so a test needs no network.
    void injectPacketForTest(const uint8_t* data, size_t len, const uint8_t srcIp[4]) {
        mergePacket(data, len, srcIp);
    }

private:
    /// One discovered device, as the list serializes it.
    struct Device {
        uint8_t  ip[4] = {};
        char     name[24] = {};
        DevType  type = DevType::Generic;
        bool     self = false;
        bool     cached = false; ///< restored from persistence, not yet re-heard live this
                                 ///< session. Cleared on the first live sighting.
        uint32_t lastSeenMs = 0; ///< platform::millis() at the most recent presence sighting.
                                 ///< Age-out drops a non-self device unheard for kStaleMs.
        uint8_t  colorCount = 0; ///< Hue bridge only: how many of its lights are color-capable
                                    ///< (the figure for sizing a layout). 0 for non-bridge rows.
    };

    /// The seat behind `active()`, claimed on build and vacated on release.
    ActiveInstance<DevicesModule> seat_{*this};

    static constexpr uint8_t  kMaxDevices = 32;   ///< a LAN's worth; bounded, no heap
    /// How often we announce ourselves, in ticks, which is how fast a peer discovers us.
    static constexpr uint32_t kBroadcastEverySec = 10;
    /// How long a live-confirmed device stays listed, the list being durable history.
    static constexpr uint32_t kStaleMs = 24u * 60u * 60u * 1000u;
    /// How long a restored device has to be re-confirmed before it is dropped as a ghost.
    static constexpr uint32_t kCachedGraceMs = 60u * 1000u;
    /// How many packets one tick will process, which bounds the work.
    static constexpr int      kMaxDrainPerTick = 16;

    // Order matters: ours is first, so a peer's marked packet is typed before the fallback.
    MmPlugin   mmPlugin_;
    WledPlugin wledPlugin_;
    static constexpr uint8_t kPluginCount = 2;
    const DevicePlugin* plugins_[kPluginCount] = { &mmPlugin_, &wledPlugin_ };

    platform::UdpSocket listener_;       // bound to the presence port; drained each tick
    bool     listenerBound_ = false;
    uint32_t broadcastTick_ = 0;         // counts tick1s ticks toward the next presence broadcast

    Device  devices_[kMaxDevices];
    uint8_t deviceCount_ = 0;
    const char* selfName_ = nullptr;   // this device's name (wired via setSelfName)
    char    statusBuf_[40] = "idle";

    /// Our own address, from whichever interface is up.
    void localIp(uint8_t out[4]) const {
        platform::ethGetIPv4(out);
        if (!out[0] && !out[1] && !out[2] && !out[3]) platform::wifiStaGetIPv4(out);
    }

    /// Offer one datagram to each plugin, the first to claim it winning.
    void mergePacket(const uint8_t* data, size_t len, const uint8_t srcIp[4]) {
        if (!srcIp[0] && !srcIp[1] && !srcIp[2] && !srcIp[3]) return;  // no source
        for (const DevicePlugin* p : plugins_) {
            DiscoveredDevice found;
            if (p->classifyPacket(data, len, srcIp, found)) { upsertDevice(srcIp, found); return; }
        }
        // No plugin claimed it — an unrecognized packet on a port we listen on; ignore.
    }

    /// The discovery group, in the organization-local scope that is never routed off the network.
    static constexpr uint8_t kDiscoveryGroup[4] = {239, 255, 77, 77};

    /// Bind the listener once the network is up, which the plugins' shared port names.
    void ensureListener() {
        if (listenerBound_) return;
        const uint16_t port = plugins_[0]->discoveryPort();
        for (const DevicePlugin* p : plugins_)
            if (p->discoveryPort() != port) return;   // divergent ports unsupported yet — see note
        if (!listener_.open()) return;
        if (listener_.bind(port)) {
            listenerBound_ = true;
            // Best-effort: a stack without multicast still hears the broadcast half.
            char grp[16];
            std::snprintf(grp, sizeof(grp), "%u.%u.%u.%u", kDiscoveryGroup[0], kDiscoveryGroup[1],
                          kDiscoveryGroup[2], kDiscoveryGroup[3]);
            listener_.joinMulticast(grp);
        } else {
            // Close it, or every retry would leak a descriptor until the process runs out.
            listener_.close();
        }
    }

    /// Announce ourselves: a compatible packet, stamped so a peer types us correctly.
    void broadcastPresence(const uint8_t ip[4]) {
        uint8_t pkt[WledPacket::kSize];
        const char* n = (selfName_ && selfName_[0]) ? selfName_ : "MoonLight";
        WledPacket::build(pkt, ip, n, boardTypeByte(), /*lightsOn=*/true);
        WledPacket::stampMmMarker(pkt);
        // The group always, since peers listen there and it costs the rest of the LAN nothing.
        listener_.sendToAddr(kDiscoveryGroup, WledPacket::kPort, pkt, sizeof(pkt));
        if (wledCompatible) {
            const uint8_t bcast[4] = {255, 255, 255, 255};
            listener_.sendToAddr(bcast, WledPacket::kPort, pkt, sizeof(pkt));
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

    /// Find or insert a classified device, arming persistence only when a saved field changes.
    void upsertDevice(const uint8_t ip[4], const DiscoveredDevice& found) {
        uint8_t local[4] = {};
        localIp(local);
        const bool isSelf = ipEq(ip, local);
        Device* d = findByIp(ip);
        bool persistChanged = false;
        if (!d) {
            if (deviceCount_ >= kMaxDevices) return;   // bounded; silently cap
            d = &devices_[deviceCount_++];
            std::memcpy(d->ip, ip, 4);
            d->type = found.type;          // first sighting — take the plugin's type
            persistChanged = true;         // a new row changes the saved list
        }
        // The marker is definitive, so an established identity is raised toward, never downgraded.
        const bool isMm = isSelf || found.type == DevType::MoonLight;
        const DevType newType = isMm ? DevType::MoonLight
                              : (d->type != DevType::MoonLight ? found.type : d->type);
        if (d->type != newType) { d->type = newType; persistChanged = true; }
        if (d->self != isSelf) { d->self = isSelf; persistChanged = true; }
        d->lastSeenMs = platform::millis();    // transient — not persisted
        d->cached = false;                     // transient — not persisted
        // Only an authoritative packet renames a row, though an empty name is always filled.
        const bool authoritative =
            (found.type == DevType::MoonLight && d->type == DevType::MoonLight) ||
            (found.type == DevType::Wled);
        if (found.name[0] && (!d->name[0] || isIpPlaceholder(d->name, ip) || authoritative)
            && std::strcmp(d->name, found.name) != 0) {
            std::snprintf(d->name, sizeof(d->name), "%s", found.name);
            persistChanged = true;
        }
        if (!d->name[0]) { formatDottedQuad(d->name, ip); persistChanged = true; }
        if (persistChanged) {                  // only a saved-field change touches disk + sort
            sortByName();
            refreshStatus();
        }
    }

    /// Keep our own row at the current address, arming persistence only on a real change.
    void upsertSelf(const uint8_t ip[4]) {
        bool changed = false;
        // Demote any prior row at another address, so it expires rather than staying immortal.
        for (uint8_t i = 0; i < deviceCount_; i++)
            if (devices_[i].self && !ipEq(devices_[i].ip, ip)) { devices_[i].self = false; changed = true; }

        Device* d = findByIp(ip);
        if (!d) {
            if (deviceCount_ >= kMaxDevices) return;
            d = &devices_[deviceCount_++];
            std::memcpy(d->ip, ip, 4);
            changed = true;            // a new row changes the saved list
        }
        if (d->type != DevType::MoonLight) { d->type = DevType::MoonLight; changed = true; }
        if (!d->self) { d->self = true; changed = true; }
        d->cached = false;
        d->lastSeenMs = platform::millis();   // transient — not persisted
        if (!d->name[0]) {
            const char* n = (selfName_ && selfName_[0]) ? selfName_ : "this device";
            std::snprintf(d->name, sizeof(d->name), "%s", n);
            changed = true;
        }
        if (changed) {                 // only a real self-row change re-sorts + arms persistence
            sortByName();
            refreshStatus();
        }
    }

    /// Whether this name is only the address, a placeholder a real name overwrites.
    static bool isIpPlaceholder(const char* name, const uint8_t ip[4]) {
        char ipStr[16];
        formatDottedQuad(ipStr, ip);
        return std::strcmp(name, ipStr) == 0;
    }

    /// The device at this address, or null.
    Device* findByIp(const uint8_t ip[4]) {
        for (uint8_t i = 0; i < deviceCount_; i++)
            if (ipEq(devices_[i].ip, ip)) return &devices_[i];
        return nullptr;
    }

    /// Whether two addresses match.
    static bool ipEq(const uint8_t a[4], const uint8_t b[4]) {
        return std::memcmp(a, b, 4) == 0;
    }

    /// Drop whatever has gone unheard past its window, compacting in place.
    void ageOut(const uint8_t local[4]) {
        const uint32_t now = platform::millis();
        uint8_t w = 0;
        for (uint8_t r = 0; r < deviceCount_; r++) {
            Device& d = devices_[r];
            const bool isUs = ipEq(d.ip, local);
            // Guarded on the address rather than the flag, so a stale row still ages out.
            if (isUs) d.lastSeenMs = now;
            // A restored device is on probation; a live-confirmed one gets the full window.
            const uint32_t window = d.cached ? kCachedGraceMs : kStaleMs;
            if (!isUs && (now - d.lastSeenMs) > window) continue;   // drop, stale
            if (w != r) devices_[w] = d;
            w++;
        }
        if (w == deviceCount_) return;   // nothing dropped — common case, no churn
        deviceCount_ = w;
        refreshStatus();
    }

    /// Report how many devices are listed, and arm the save.
    void refreshStatus() {
        std::snprintf(statusBuf_, sizeof(statusBuf_), "%u device%s",
                      deviceCount_, deviceCount_ == 1 ? "" : "s");
        setStatus(statusBuf_);
        // Persisted, so the next boot shows the set instantly.
        markDirty();
        FilesystemModule::noteDirty();
    }

    /// Order the list by name, which core's sort does from the comparator below.
    void sortByName() {
        mm::insertionSort(devices_, deviceCount_, [](const Device& a, const Device& b) {
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
