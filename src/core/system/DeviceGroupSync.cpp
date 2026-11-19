/// The device groups on the network: definitions and shared controls sent when Trickle says, merged on arrival, a group created or dropped as its members change, and deletions kept.
#include "core/system/DeviceGroupSync.h"

#include "core/util/ClockSync.h"   // FleetTime: when a timed change applies

#include "core/module/Scheduler.h"
#include "core/module/StateDocument.h"   // applyStateDocument: a group created, renamed or dropped as the API does it
#include "core/system/Devices.h"           // the fleet's clock, which stamps a deletion and a shared change
#include "core/services/OscPacket.h"
#include "core/system/ControlModule.h"     // the slots whose changes are shared, and what each drives
#include "core/system/DeviceGroup.h"
#include "core/system/HttpServerModule.h"  // servedPort: where a member fetches a look applied here
#include "core/util/JsonSink.h"
#include "core/util/fnv.h"
#include "core/util/format.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

namespace mm {

void DeviceGroupSync::tick(MoonModule& devices, char* saved, size_t savedCap) {
    devices_ = &devices;
    if (!loaded_) loaded_ = loadTombs(saved);
    if (!loaded_) return;   // memory refused the saved markers: a merge waits until they hold, tried again next second
    ensureOpen();
    readAll(devices);
    attach();
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

void DeviceGroupSync::readAll(MoonModule& devices) {
    noticeDeleted(devices);   // before what arrived, so a copy of a group deleted here since the last second meets its marker
    if (pendingLen_) handle(devices, pending_.data(), pendingLen_);   // the definition or deletion the 20 ms read set aside
    pendingLen_ = 0;
    for (int n = 0; open_ && n < 16; n++) {   // a bounded drain, as Discovery's
        const int len = sock_.recvFrom(buf_.data(), buf_.count());
        if (len <= 0) break;
        handle(devices, buf_.data(), static_cast<size_t>(len));
    }
    holdGroups(devices);   // after what arrived, so a group created by it is held, and noticed when deleted here
}

void DeviceGroupSync::receive(MoonModule& devices) {
    if (!loaded_) return;   // the markers first, so a copy of a deleted group meets its marker
    devices_ = &devices;
    // Shared values and recalls apply now; a definition or deletion, which may create or remove a module, waits for the once-a-second tick, and the read stops until it has.
    applyDue(devices);
    for (int n = 0; open_ && !pendingLen_ && n < 16; n++) {
        const int len = sock_.recvFrom(buf_.data(), buf_.count());
        if (len <= 0) break;
        if (receiveShared(devices, buf_.data(), static_cast<size_t>(len))) continue;
        std::memcpy(pending_.data(), buf_.data(), static_cast<size_t>(len));
        pendingLen_ = static_cast<size_t>(len);
    }
    shared_.tickFetch();   // starts a recall's fetch within 20 ms; JsonFetch's once-a-second poll takes the answer in, so a look follows within about a second
}

bool DeviceGroupSync::ensureOpen() {
    if (open_) return true;
    if (!buf_.resize(kDatagram) || !pending_.resize(kDatagram) || !held_.resize(kHeld) || !sock_.open()) return false;
    if (!sock_.bind(kPort)) { sock_.close(); return false; }
    sock_.joinMulticast(kGroup);   // best-effort: a network that drops multicast still carries what each device sends itself
    open_ = true;
    return true;
}

void DeviceGroupSync::close() {
    if (ControlModule* c = attached_ ? ControlModule::active() : nullptr) c->removeSurface(this);
    attached_ = false;
    shared_.clear();
    if (open_) sock_.close();
    open_ = false;
    buf_.resize(0);
    pending_.resize(0);
    pendingLen_ = 0;
    held_.resize(0);
    heldCount_ = 0;
    seenCount_ = 0;      // release frees the held ids and the markers with the buffers,
    tombCount_ = 0;
    loaded_ = false;     // and the next tick reloads the markers from the saved text
}

void DeviceGroupSync::handle(MoonModule& devices, const uint8_t* pkt, size_t len) {
    // One message, one handler: an answer sent from the definition's handler reuses the receive buffer.
    if (!handleDefinition(devices, pkt, len) && !receiveShared(devices, pkt, len)) handleDeletion(devices, pkt, len);
}

bool DeviceGroupSync::receiveShared(MoonModule& devices, const uint8_t* pkt, size_t len) {
    uint64_t tag = 0;
    const uint8_t* msg = nullptr;
    size_t msgLen = 0;
    if (!osc::readBundle(pkt, len, tag, msg, msgLen)) return handleShared(devices, pkt, len);
    const int64_t at = osc::usOfTimetag(tag);
    // Held only on the fleet clock and only while there is room: otherwise now, as before the clock existed.
    const int64_t now = FleetTime::nowUs();
    if (FleetTime::synced() && at > now && at - now <= kMaxHoldUs && heldCount_ < held_.count() && msgLen <= sizeof(Held::msg)) {
        Held& h = held_[heldCount_++];
        h.atUs = at;
        h.len = static_cast<uint16_t>(msgLen);
        std::memcpy(h.msg, msg, msgLen);
        return true;
    }
    handleShared(devices, msg, msgLen);
    return true;   // a bundle is a timed shared change, handled or not
}

void DeviceGroupSync::applyDue(MoonModule& devices) {
    if (!heldCount_) return;
    const int64_t now = FleetTime::nowUs();
    for (uint8_t i = 0; i < heldCount_;) {
        if (held_[i].atUs > now) { i++; continue; }
        Held due = held_[i];
        held_[i] = held_[--heldCount_];   // the order among changes of one moment is their stamps', which the merge reads
        handleShared(devices, due.msg, due.len);
    }
}

void DeviceGroupSync::sendTimed(const uint8_t* data, size_t len) {
    if (!len) return;
    if (!FleetTime::synced()) { send(data, len); return; }
    uint8_t bundle[192];
    const size_t n = osc::writeBundle(bundle, sizeof(bundle), osc::timetagOfUs(FleetTime::nowUs() + kApplyLeadUs), data, len);
    if (n) send(bundle, n);
    else send(data, len);   // too long for a timed change: plain, applied on arrival
}

bool DeviceGroupSync::handleShared(MoonModule& devices, const uint8_t* pkt, size_t len) {
    osc::Reader values(pkt, len, SharedControls::kValuesAddress);
    osc::Reader look(pkt, len, SharedControls::kLookAddress);
    osc::Reader& r = values.ok() ? values : look;
    if (!r.ok()) return false;
    const char* id = r.str();
    DeviceGroup* g = r.ok() && DeviceGroup::isId(id) ? groupById(devices, id) : nullptr;
    if (!g) return true;   // a group this device does not hold
    Hlc* clock = devices.acceptsRole(ModuleRole::DeviceGroup) ? &static_cast<Devices&>(devices).clock() : nullptr;
    const uint32_t key = SharedControls::keyOf(id);
    const bool agrees = values.ok() ? shared_.mergeValues(r, key, clock) : shared_.mergeRecall(r, key, clock);
    if (!agrees) g->hearInconsistent();   // this device holds newer: Trickle sends it soon
    return true;
}

namespace {
/// The surface control at Control slot `slot`, ordered switches, faders, encoders as ControlModule counts them.
SurfaceControl kindOfSlot(uint8_t slot) {
    return slot < ControlModule::kSwitchCount ? SurfaceControl::Switch
         : slot < ControlModule::kSwitchCount + ControlModule::kFaderCount ? SurfaceControl::Fader : SurfaceControl::Encoder;
}
/// Its index within its bank.
uint8_t indexOfSlot(uint8_t slot) {
    return slot < ControlModule::kSwitchCount ? slot
         : slot < ControlModule::kSwitchCount + ControlModule::kFaderCount ? static_cast<uint8_t>(slot - ControlModule::kSwitchCount)
                                                                            : static_cast<uint8_t>(slot - ControlModule::kSwitchCount - ControlModule::kFaderCount);
}
/// Call `fn(id)` for the id of each group held under `devices`.
template <class Fn>
void forEachGroupId(const MoonModule& devices, Fn&& fn) {
    for (uint8_t i = 0; i < devices.childCount(); i++) {
        const MoonModule* m = devices.child(i);
        if (m->role() == ModuleRole::DeviceGroup) fn(static_cast<const DeviceGroup*>(m)->groupId());
    }
}
/// The slot of a bank's control.
uint8_t slotOf(SurfaceControl kind, uint8_t index) {
    return kind == SurfaceControl::Switch ? index
         : kind == SurfaceControl::Fader ? static_cast<uint8_t>(ControlModule::kSwitchCount + index)
                                         : static_cast<uint8_t>(ControlModule::kSwitchCount + ControlModule::kFaderCount + index);
}
}  // namespace

void DeviceGroupSync::attach() {
    static_assert(kSlots == ControlModule::kSlotCount, "a seen record per Control slot");
    ControlModule* c = ControlModule::active();
    if (attached_ || !c) return;
    // Each slot's parameter is seen before any change counts, so what this device holds at boot stays local until a real change: a member back from a restart takes the group's values.
    for (uint8_t slot = 0; slot < kSlots; slot++) {
        int32_t value = 0;
        changedHere(slot, c->targetOf(kindOfSlot(slot), indexOfSlot(slot)), value);
    }
    c->addSurface(this, ControlModule::Seed::None);
    attached_ = true;
}

bool DeviceGroupSync::changedHere(uint8_t slot, const char* target, int32_t& value) {
    SlotSeen& seen = slotSeen_[slot];
    const uint32_t key = target ? fnv1a(target, std::strlen(target)) : 0;
    char module[24];
    const char* control = nullptr;
    const ControlDescriptor* c = SharedControls::shareable(target, module, control);   // only what a member would apply is sent
    Scheduler* sched = Scheduler::instance();
    if (!c || !sched || !sched->getControlWide(module, control, value)) { seen = SlotSeen{key, 0}; return false; }
    const bool moved = seen.target == key && seen.value != value;   // a slot pointed at another parameter re-reads its baseline
    seen = SlotSeen{key, value};
    return moved && !shared_.holds(target, value);   // a value a group set is not sent on
}

void DeviceGroupSync::sendValue(SurfaceControl kind, uint8_t index, uint8_t) {
    ControlModule* c = ControlModule::active();
    if (kind == SurfaceControl::Pad || !c || index >= ControlModule::kSwitchCount) return;
    const char* target = c->targetOf(kind, index);
    int32_t value = 0;
    if (changedHere(slotOf(kind, index), target, value)) shareValue(target, value);
}

void DeviceGroupSync::shareValue(const char* param, int32_t value) {
    if (!devices_) return;
    forEachGroupId(*devices_, [&](const char* id) {
        if (!shared_.setLocal(SharedControls::keyOf(id), param, value, stampNow(*devices_))) return;
        uint8_t msg[160];   // its own buffer: a merged value's apply moves a slot and reaches here while buf_ still holds the datagram being read
        uint16_t from = 0;
        sendTimed(msg, shared_.writeValues(msg, sizeof(msg), id, from, param));
    });
}

void DeviceGroupSync::lookApplied(const char* name) {
    if (!devices_) return;
    uint8_t ip[4] = {};
    platform::localIPv4(ip);
    const uint16_t served = HttpServerModule::servedPort();
    forEachGroupId(*devices_, [&](const char* id) {
        shared_.recallLocal(SharedControls::keyOf(id), name, ip, served ? served : 80, stampNow(*devices_));
        uint8_t msg[160];
        sendTimed(msg, shared_.writeRecall(msg, sizeof(msg), id));
    });
}

void DeviceGroupSync::sendShared(const DeviceGroup& g) {
    // One message at least, so a member whose values differ hears the digest even from a device holding none.
    uint16_t from = 0;
    do send(buf_.data(), shared_.writeValues(buf_.data(), buf_.count(), g.groupId(), from));
    while (from < shared_.valueCount());
    send(buf_.data(), shared_.writeRecall(buf_.data(), buf_.count(), g.groupId()));
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
    markDeleted(id, s, 0);
    if (DeviceGroup* g = groupById(devices, id)) removeLocal(devices, *g);   // a deletion wins over every edit, as a marker answers every copy
}

void DeviceGroupSync::noticeDeleted(MoonModule& devices) {
    for (uint16_t i = 0; i < seenCount_; i++) {
        if (groupById(devices, seen_[i].id) || deleted(seen_[i].id)) continue;
        markDeleted(seen_[i].id, stampNow(devices), 3);
    }
}

void DeviceGroupSync::holdGroups(MoonModule& devices) {
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
    // A group that left this device, deleted here or anywhere, or this device removed from it, takes its shared values and recall with it.
    shared_.retain([&](uint32_t key) {
        bool held = false;
        forEachGroupId(devices, [&](const char* id) { held = held || SharedControls::keyOf(id) == key; });
        return held;
    });
}

void DeviceGroupSync::sendDue(MoonModule& devices) {
    for (uint8_t c = 0; c < devices.childCount(); c++) {
        MoonModule* m = devices.child(c);
        if (m->role() != ModuleRole::DeviceGroup) continue;
        auto* g = static_cast<DeviceGroup*>(m);
        if (!g->sendDue() || !open_) continue;
        for (uint16_t k = 0; k < g->chunkCount(kPerChunk); k++)
            send(buf_.data(), g->writeChunk(buf_.data(), buf_.count(), k, kPerChunk));
        sendShared(*g);
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

DeviceGroupSync::Tomb* DeviceGroupSync::tombSlot(const Stamp& stamp) {
    if (tombCount_ < kMaxTombs && tombs_.ensure(tombCount_ + 1u)) return &tombs_[tombCount_++];
    if (!tombCount_) return nullptr;   // memory refused the first marker
    Tomb* oldest = &tombs_[0];   // full, or memory refused one more: the oldest marker gives way to a newer one
    for (uint8_t i = 1; i < tombCount_; i++) if (oldest->stamp.newerThan(tombs_[i].stamp)) oldest = &tombs_[i];
    return stamp.newerThan(oldest->stamp) ? oldest : nullptr;
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
    Tomb* t = tombSlot(stamp);
    if (!t) return;   // older than every marker kept, or memory refused the first: the deletion stays on this device
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
    if (len) transmit(data, len);
}

void DeviceGroupSync::transmit(const uint8_t* data, size_t len) {
    if (open_) sock_.sendToAddr(kGroup, kPort, data, len);
}

bool DeviceGroupSync::loadTombs(const char* saved) {
    // "id:hlc:writer;" per marker, in hex.
    tombCount_ = 0;
    for (const char* p = saved; p && *p && tombCount_ < kMaxTombs;) {
        const char* end = std::strchr(p, ';');
        char entry[64];
        const size_t n = end ? static_cast<size_t>(end - p) : std::strlen(p);
        mm::formatTo(entry, sizeof(entry), "%.*s", static_cast<int>(n < sizeof(entry) ? n : sizeof(entry) - 1), p);
        Tomb t{};
        if (parseTomb(entry, t)) {
            if (!tombs_.ensure(tombCount_ + 1u)) { tombCount_ = 0; return false; }
            tombs_[tombCount_++] = t;
        }
        p = end ? end + 1 : nullptr;
    }
    return true;
}

bool DeviceGroupSync::parseTomb(char* entry, Tomb& t) {
    char* c1 = std::strchr(entry, ':');
    if (!c1) return false;
    *c1 = 0;
    if (!DeviceGroup::isId(entry)) return false;
    mm::formatTo(t.id, sizeof(t.id), "%s", entry);
    return Stamp::parse(c1 + 1, t.stamp);
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
