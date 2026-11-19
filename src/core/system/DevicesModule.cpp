#include "core/system/DevicesModule.h"

/// The rows as the list and the map read them: a row's detail, its edges, its groups and this device's own facts, and a group's members as a receiver list reads them, kept out of the header by concern.

namespace mm {

void DevicesModule::writeListRowDetail(JsonSink& sink, ListRow row) const {
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
    writeGroups(sink, d);
    if (d.self) {
        writeSelfDetail(sink);
    } else if (d.version[0]) {
        sink.append(",\"version\":");   // what another ecosystem's device announced it runs
        sink.writeJsonString(d.version);
    }
    if (!d.self && d.chip[0]) sink.appendf(",\"chip\":\"%s\"", d.chip);
    sink.append("}");
}

void DevicesModule::writeMemberOptions(JsonSink& sink, const char* key) const {
    sink.appendf("\"%s\":[", key);
    bool first = true;
    for (ListRow i = 0; i < deviceCount_; i++) {
        const Device& d = devices_[i];
        if (!hasMac(d.mac)) continue;
        char ip[16], label[48];
        formatDottedQuad(ip, d.ip);
        mm::formatTo(label, sizeof(label), "%s (%s)", d.name[0] ? d.name : ip, ip);
        sink.append(first ? "" : ",");
        first = false;
        sink.writeJsonString(label);
    }
    sink.append("]");
}

bool DevicesModule::memberOption(uint32_t index, uint8_t mac[6], const char*& name) const {
    for (ListRow i = 0; i < deviceCount_; i++) {
        const Device& d = devices_[i];
        if (!hasMac(d.mac) || index--) continue;
        std::memcpy(mac, d.mac, 6);
        name = d.name;
        return true;
    }
    return false;
}

void DevicesModule::writeSelfDetail(JsonSink& sink) const {
    sink.append(",\"version\":");
    sink.writeJsonString(kVersion);
    sink.append(",\"firmware\":");
    sink.writeJsonString(kFirmwareName);
    writeDeviceFacts(sink);   // the mode and the images behind the app, as the device's summary carries them
    writeServices(sink);
    // What drives its lights, which a peer's map shows under this device's name.
    if (const LightOutput* out = LightOutput::active()) {
        sink.append(",\"outputs\":[");
        out->writeOutputs(sink);
        sink.append("]");
    }
}

void DevicesModule::writeEdges(JsonSink& sink, const Device& d) const {
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

void DevicesModule::writeGroups(JsonSink& sink, const Device& d) const {
    if (!hasMac(d.mac) || !parent()) return;
    bool first = true;
    for (uint8_t i = 0; i < parent()->childCount(); i++) {
        const MoonModule* m = parent()->child(i);
        if (m->role() != ModuleRole::DeviceGroup || !static_cast<const DeviceGroup*>(m)->hasMember(d.mac)) continue;
        sink.append(first ? ",\"groups\":[" : ",");
        first = false;
        sink.appendf("{\"id\":\"%s\",\"name\":", static_cast<const DeviceGroup*>(m)->groupId());
        sink.writeJsonString(static_cast<const DeviceGroup*>(m)->groupName());   // the shared name, the same on every member's map
        sink.append("}");
    }
    if (first) return;
    sink.append("]");
    if (!present(d.mac)) sink.append(",\"missing\":true");
}

const DeviceGroup* DevicesModule::groupNamed(const char* name) const MM_NONBLOCKING {
    if (!parent() || !name) return nullptr;
    const size_t len = std::strlen(name);
    for (uint8_t i = 0; i < parent()->childCount(); i++) {
        const MoonModule* m = parent()->child(i);
        if (m->role() != ModuleRole::DeviceGroup) continue;
        const char* shared = static_cast<const DeviceGroup*>(m)->groupName();
        if (std::strlen(shared) == len && asciiCaseEqual(shared, name, len)) return static_cast<const DeviceGroup*>(m);
    }
    return nullptr;
}

bool DevicesModule::appendGroupMembers(const char* name, Receiver* out, uint8_t& n, uint8_t cap, uint8_t& left) const MM_NONBLOCKING {
    const DeviceGroup* group = groupNamed(name);
    if (!group) return false;
    group->forEachMember([&](const uint8_t mac[6]) {
        const Device* d = rowOfMac(mac);
        if (d && d->self) return;   // this device is the one sending
        if (n >= cap) { left++; return; }
        out[n] = Receiver{};
        out[n].member = true;
        if (d && present(mac)) std::memcpy(out[n].ip, d->ip, 4);   // a member not present keeps its place with no address, so a split over the members holds
        n++;
    });
    return true;
}

void DevicesModule::writeServices(JsonSink& sink) const {
    if (!serviceCount_) return;
    sink.append(",\"services\":[");
    for (ListRow i = 0; i < serviceCount_; i++) {
        const Service& s = services_[i];
        if (s.port) sink.appendf("%s\"%s %u\"", i ? "," : "", s.protocol, static_cast<unsigned>(s.port));
        else sink.appendf("%s\"%s\"", i ? "," : "", s.protocol);   // a service with no port, raw Ethernet's
    }
    sink.append("]");
}

}  // namespace mm
