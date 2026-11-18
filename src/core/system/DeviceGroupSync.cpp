/// The device groups' definitions on the network: sent when Trickle says, merged on arrival, a group created or dropped as its members change, and deletions kept.
#include "core/system/DeviceGroupSync.h"

#include "core/module/Scheduler.h"
#include "core/module/StateDocument.h"   // applyStateDocument: a group created, renamed or dropped as the API does it
#include "core/system/Devices.h"           // the fleet's clock, which stamps a deletion
#include "core/services/OscPacket.h"
#include "core/system/DeviceGroup.h"
#include "core/util/JsonSink.h"
#include "core/util/format.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mm {

void DeviceGroupSync::tick(MoonModule& devices, char* saved, size_t savedCap) {
    if (!loaded_) { loadTombs(saved); loaded_ = true; }
    if (ensureOpen()) {
        for (int n = 0; n < 16; n++) {   // a bounded drain, as Discovery's
            const int len = sock_.recvFrom(buf_.data(), buf_.count());
            if (len <= 0) break;
            handle(devices, buf_.data(), static_cast<size_t>(len));
        }
    }
    noticeDeleted(devices);   // with the socket closed too, so a deletion is marked and kept until it can go out
    if (open_) {
        sendDue(devices);
        for (uint8_t i = 0; i < tombCount_; i++) if (tombs_[i].sendsLeft) { sendDeletion(tombs_[i]); tombs_[i].sendsLeft--; }
    }
    if (tombsChanged_) {
        saveTombs(saved, savedCap);
        tombsChanged_ = false;
        owner_.markDirty();   // the markers are Discovery's saved text, written here rather than through the API, so the save is asked for here
        if (Scheduler* s = Scheduler::instance()) s->noteDirty();
    }
}

bool DeviceGroupSync::ensureOpen() {
    if (open_) return true;
    if (!buf_.resize(kDatagram) || !sock_.open()) return false;
    if (!sock_.bind(kPort)) { sock_.close(); return false; }
    sock_.joinMulticast(kGroup);   // best-effort: a network that drops multicast still carries what each device sends itself
    open_ = true;
    return true;
}

void DeviceGroupSync::close() {
    if (open_) sock_.close();
    open_ = false;
    buf_.resize(0);
    seenCount_ = 0;      // release frees the held ids and the markers with the buffers,
    tombCount_ = 0;
    loaded_ = false;     // and the next tick reloads the markers from the saved text
}

void DeviceGroupSync::handle(MoonModule& devices, const uint8_t* pkt, size_t len) {
    // One message, one handler: an answer sent from the definition's handler reuses the receive buffer.
    if (!handleDefinition(devices, pkt, len)) handleDeletion(devices, pkt, len);
}

bool DeviceGroupSync::handleDefinition(MoonModule& devices, const uint8_t* pkt, size_t len) {
    osc::Reader r(pkt, len, DeviceGroup::kAddress);
    if (!r.ok()) return false;
    const char* received = r.str();
    if (!r.ok() || !DeviceGroup::isId(received)) return true;
    char id[DeviceGroup::kIdLen + 1];   // copied, since an answer reuses the receive buffer
    mm::formatTo(id, sizeof(id), "%s", received);
    if (answerIfDeleted(id)) return true;
    DeviceGroup* g = groupById(devices, id);
    if (!g) {
        g = adopt(devices, id, r);
        if (!g) return true;
        r = osc::Reader(pkt, len, DeviceGroup::kAddress);   // the same chunk again, merged into the group now held
        r.str();
    }
    const char* wantName = nullptr;
    g->merge(r, wantName);
    settle(devices, *g, wantName);
    return true;
}

void DeviceGroupSync::settle(MoonModule& devices, DeviceGroup& g, const char* wantName) {
    uint8_t mac[6];
    platform::getMacAddress(mac);
    if (!g.hasMember(mac) && !g.madeHere()) { removeLocal(devices, g); return; }   // removed from it: this device drops it
    if (wantName && g.name()) rename(devices, g, wantName);
}

bool DeviceGroupSync::answerIfDeleted(const char* id) {
    const int i = tombIndex(id);
    if (i >= 0) sendDeletion(tombs_[i]);   // a late copy of a deleted group hears it was deleted
    return i >= 0;
}

DeviceGroup* DeviceGroupSync::adopt(MoonModule& devices, const char* id, osc::Reader& r) {
    // A copy decides whether this device belongs, read before any module is made: a group it is no member of stays with its members.
    uint8_t mac[6];
    platform::getMacAddress(mac);
    const char* name = nullptr;
    if (!DeviceGroup::chunkHasMember(r, mac, name)) return nullptr;
    create(devices, id, name && name[0] ? name : "Group");
    return groupById(devices, id);
}

void DeviceGroupSync::rename(MoonModule& devices, DeviceGroup& g, const char* to) {
    JsonSink doc;
    doc.append("{");
    doc.writeJsonString(devices.name());
    doc.append(":{");
    doc.writeJsonString(g.name());
    doc.append(":{\"$name\":");
    doc.writeJsonString(to);
    doc.append("}}}");
    apply(doc.data());
}

void DeviceGroupSync::handleDeletion(MoonModule& devices, const uint8_t* pkt, size_t len) {
    osc::Reader r(pkt, len, kDeletedAddress);
    if (!r.ok()) return;
    const char* id = r.str();
    const Stamp s{r.i64(), r.i32()};
    if (!r.ok() || !DeviceGroup::isId(id) || !s.valid()) return;
    DeviceGroup* g = groupById(devices, id);
    if (g && !s.newerThan(g->newest())) return;   // edited after the deletion: the group stands, and its next copy says so
    markDeleted(id, s, 0);
    if (g) removeLocal(devices, *g);
}

void DeviceGroupSync::noticeDeleted(MoonModule& devices) {
    // Every id held last second and gone now, unless this sync removed it: a group the user deleted here.
    for (uint16_t i = 0; i < seenCount_; i++) {
        if (groupById(devices, seen_[i].id) || deleted(seen_[i].id)) continue;
        markDeleted(seen_[i].id, stampNow(devices), 3);
    }
    // A group held under a marker goes: a removal memory refused is tried again each second until the group is gone.
    for (uint8_t c = devices.childCount(); c > 0; c--) {
        MoonModule* m = devices.child(c - 1);
        if (m->role() == ModuleRole::DeviceGroup && deleted(static_cast<DeviceGroup*>(m)->groupId())) removeLocal(devices, *static_cast<DeviceGroup*>(m));
    }
    uint16_t n = 0;
    for (uint8_t c = 0; c < devices.childCount(); c++) {
        const MoonModule* m = devices.child(c);
        if (m->role() != ModuleRole::DeviceGroup) continue;
        if (!seen_.ensure(n + 1u)) break;
        mm::formatTo(seen_[n].id, sizeof(seen_[n].id), "%s", static_cast<const DeviceGroup*>(m)->groupId());
        n++;
    }
    seenCount_ = n;
}

void DeviceGroupSync::sendDue(MoonModule& devices) {
    for (uint8_t c = 0; c < devices.childCount(); c++) {
        MoonModule* m = devices.child(c);
        if (m->role() != ModuleRole::DeviceGroup) continue;
        auto* g = static_cast<DeviceGroup*>(m);
        if (!g->sendDue() || !open_) continue;
        for (uint16_t k = 0; k < g->chunkCount(kPerChunk); k++)
            send(buf_.data(), g->writeChunk(buf_.data(), buf_.count(), k, kPerChunk));
    }
}

Stamp DeviceGroupSync::stampNow(MoonModule& devices) {
    // The fleet's clock, which observed every copy, so a deletion is newer than each edit this device heard.
    if (devices.acceptsRole(ModuleRole::DeviceGroup)) return static_cast<Devices&>(devices).clock().now(DeviceGroup::selfWriter());
    return Stamp{static_cast<uint64_t>(platform::millis()) << 16, DeviceGroup::selfWriter()};
}

bool DeviceGroupSync::deleted(const char* id) const {
    return tombIndex(id) >= 0;
}

int DeviceGroupSync::tombIndex(const char* id) const {
    for (uint8_t i = 0; i < tombCount_; i++) if (std::strcmp(tombs_[i].id, id) == 0) return i;
    return -1;
}

DeviceGroupSync::Tomb* DeviceGroupSync::tombSlot() {
    if (tombCount_ < kMaxTombs && tombs_.ensure(tombCount_ + 1u)) return &tombs_[tombCount_++];
    if (!tombCount_) return nullptr;   // memory refused the first marker
    Tomb* oldest = &tombs_[0];   // full, or memory refused one more: the oldest marker gives way
    for (uint8_t i = 1; i < tombCount_; i++) if (oldest->stamp.newerThan(tombs_[i].stamp)) oldest = &tombs_[i];
    return oldest;
}

void DeviceGroupSync::markDeleted(const char* id, const Stamp& stamp, uint8_t sends) {
    if (const int i = tombIndex(id); i >= 0) {
        // One marker per group, which only grows newer: an equal or older deletion, its own copy looping back among them, leaves it and its sends as they are.
        Tomb& held = tombs_[i];
        if (!stamp.newerThan(held.stamp)) return;
        held.stamp = stamp;
        if (sends > held.sendsLeft) held.sendsLeft = sends;
        tombsChanged_ = true;
        return;
    }
    Tomb* t = tombSlot();
    if (!t) return;   // memory refused the first marker: the deletion stays on this device
    mm::formatTo(t->id, sizeof(t->id), "%s", id);
    t->stamp = stamp;
    t->sendsLeft = sends;
    tombsChanged_ = true;
}

void DeviceGroupSync::sendDeletion(const Tomb& t) {
    if (!open_) return;
    osc::Writer w(buf_.data(), buf_.count(), kDeletedAddress, ",shi");
    w.str(t.id);
    w.i64(t.stamp.hlc);
    w.i32(t.stamp.writer);
    send(buf_.data(), w.size());
}

void DeviceGroupSync::send(const uint8_t* data, size_t len) {
    if (open_ && len) sock_.sendToAddr(kGroup, kPort, data, len);
}

void DeviceGroupSync::loadTombs(const char* saved) {
    // "id:hlc:writer;" per marker, in hex.
    tombCount_ = 0;
    for (const char* p = saved; p && *p && tombCount_ < kMaxTombs;) {
        const char* end = std::strchr(p, ';');
        char entry[64];
        const size_t n = end ? static_cast<size_t>(end - p) : std::strlen(p);
        mm::formatTo(entry, sizeof(entry), "%.*s", static_cast<int>(n < sizeof(entry) ? n : sizeof(entry) - 1), p);
        Tomb t{};
        if (parseTomb(entry, t) && tombs_.ensure(tombCount_ + 1u)) tombs_[tombCount_++] = t;
        p = end ? end + 1 : nullptr;
    }
}

bool DeviceGroupSync::parseTomb(char* entry, Tomb& t) {
    char* c1 = std::strchr(entry, ':');
    char* c2 = c1 ? std::strchr(c1 + 1, ':') : nullptr;
    if (!c1 || !c2) return false;
    *c1 = 0;
    if (!DeviceGroup::isId(entry)) return false;
    mm::formatTo(t.id, sizeof(t.id), "%s", entry);
    t.stamp = Stamp{std::strtoull(c1 + 1, nullptr, 16), static_cast<uint32_t>(std::strtoul(c2 + 1, nullptr, 16))};
    return t.stamp.valid();
}

void DeviceGroupSync::saveTombs(char* saved, size_t cap) const {
    size_t at = 0;
    saved[0] = 0;
    for (uint8_t i = 0; i < tombCount_; i++) {
        const int n = std::snprintf(saved + at, cap - at, "%s:%" PRIx64 ":%" PRIx32 ";", tombs_[i].id, tombs_[i].stamp.hlc, tombs_[i].stamp.writer);
        if (n < 0 || at + static_cast<size_t>(n) >= cap) { saved[at] = 0; return; }
        at += static_cast<size_t>(n);
    }
}

DeviceGroup* DeviceGroupSync::groupById(MoonModule& devices, const char* id) {
    for (uint8_t c = 0; c < devices.childCount(); c++) {
        MoonModule* m = devices.child(c);
        if (m->role() == ModuleRole::DeviceGroup && std::strcmp(static_cast<DeviceGroup*>(m)->groupId(), id) == 0) return static_cast<DeviceGroup*>(m);
    }
    return nullptr;
}

void DeviceGroupSync::apply(const char* doc) {
    if (Scheduler* s = Scheduler::instance()) applyStateDocument(*s, doc);
}

void DeviceGroupSync::create(MoonModule& devices, const char* id, const char* name) {
    // A name no module in the tree holds, so the document makes a module rather than editing one; the shared name arrives with the merge.
    Scheduler* s = Scheduler::instance();
    char unique[MoonModule::kNameLen];
    if (!s || !s->freeName(name, unique, sizeof(unique))) return;
    JsonSink doc;
    doc.append("{");
    doc.writeJsonString(devices.name());
    doc.append(":{");
    doc.writeJsonString(unique);
    doc.append(":{\"type\":\"DeviceGroup\",\"id\":");
    doc.writeJsonString(id);
    doc.append("}}}");
    apply(doc.data());
}

void DeviceGroupSync::removeLocal(MoonModule& devices, DeviceGroup& g) {
    // Off the held list first, so the next second does not read it as a deletion by the user.
    for (uint16_t i = 0; i < seenCount_; i++)
        if (std::strcmp(seen_[i].id, g.groupId()) == 0) { seen_[i] = seen_[--seenCount_]; break; }
    JsonSink doc;
    doc.append("{");
    doc.writeJsonString(devices.name());
    doc.append(":{");
    doc.writeJsonString(g.name() ? g.name() : "");
    doc.append(":null}}");
    apply(doc.data());
}

}  // namespace mm
