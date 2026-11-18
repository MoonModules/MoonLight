/// A device group's members against Discovery, and its definition merged and repeated, in a source of its own so Discovery's header can include the group's.
#include "core/system/DeviceGroup.h"

#include "core/module/Scheduler.h"       // noteDirty: a merged definition is saved
#include "core/services/OscPacket.h"     // Writer, Reader: the definition's message
#include "core/system/Devices.h"         // the fleet's clock
#include "core/system/DevicesModule.h"   // Discovery: the devices a member is picked from, and whether one was heard lately
#include "core/util/JsonUtil.h"
#include "core/util/Mdns.h"              // parseMac: a MAC as an API caller writes it
#include "core/util/fnv.h"
#include "core/util/format.h"
#include "platform/platform.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mm {

namespace {

bool isZeroMac(const uint8_t mac[6]) {
    for (int i = 0; i < 6; i++) if (mac[i]) return false;
    return true;
}

void writeMac(JsonSink& sink, const uint8_t mac[6]) {
    sink.appendf("\"%02x:%02x:%02x:%02x:%02x:%02x\"", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/// A stamp as "hlc:writer" in hex, the form the hidden name stamp is saved in.
void formatStamp(char* out, size_t cap, const Stamp& s) {
    mm::formatTo(out, cap, "%" PRIx64 ":%" PRIx32, s.hlc, s.writer);
}

/// A stamp saved as "hlc:writer", the oldest one when it is not valid, so any copy's stamp beats it.
Stamp parseStamp(const char* text) {
    Stamp s;
    char* end = nullptr;
    s.hlc = std::strtoull(text, &end, 16);
    if (end && *end == ':') s.writer = static_cast<uint32_t>(std::strtoul(end + 1, nullptr, 16));
    return s.valid() ? s : Stamp{};
}

/// Mark `m` changed and ask for the debounced save, for a change no API request saves: a merged definition.
void saveSoon(MoonModule& m) {
    m.markDirty();
    if (Scheduler* s = Scheduler::instance()) s->noteDirty();
}

/// The device an edit names, by its index in the offered devices (the UI) or by its MAC (an API caller); false when it names none.
bool pickedDevice(const char* field, const char* valueJson, uint8_t mac[6], const char*& name) {
    const DevicesModule* discovery = DevicesModule::active();
    if (std::strcmp(field, "device") == 0) {
        const int index = json::parseInt(valueJson, "value");   // resolved against the offered list as it stands now
        return index >= 0 && discovery && discovery->memberOption(static_cast<uint32_t>(index), mac, name);
    }
    if (std::strcmp(field, "mac") != 0) return false;
    char text[24] = "";
    json::parseString(valueJson, "value", text, sizeof(text));
    if (!mdns::parseMac(text, mac) || isZeroMac(mac)) return false;
    name = discovery ? discovery->nameOfMac(mac) : nullptr;
    return true;
}

}  // namespace

uint32_t DeviceGroup::selfWriter() {
    uint8_t mac[6];
    platform::getMacAddress(mac);
    return (static_cast<uint32_t>(mac[2]) << 24) | (static_cast<uint32_t>(mac[3]) << 16) | (static_cast<uint32_t>(mac[4]) << 8) | mac[5];
}

void DeviceGroup::defineControls() {
    MoonModule::defineControls();
    controls_.addText("id", id_, sizeof(id_));
    controls_.setHidden(controls_.count() - 1, true);   // saved, so the group stays the same group across a reboot, and shown nowhere
    controls_.addText("groupName", groupName_, sizeof(groupName_));
    controls_.setHidden(controls_.count() - 1, true);   // saved: the shared name, which a module name taken elsewhere in the tree leaves standing
    controls_.addText("nameStamp", nameHlc_, sizeof(nameHlc_));
    controls_.setHidden(controls_.count() - 1, true);   // saved, so a rename here keeps its place against a late copy
    /// The devices in this group.
    /// @field device The device, by name and address; it stays the member through a rename or a new address.
    /// @field mac Its identity.
    /// @field status Present when it was heard within three of its announcements, missing otherwise.
    controls_.addList("members", *this);
}

void DeviceGroup::setup() {
    MoonModule::setup();
    nameStamp_ = parseStamp(nameHlc_);
    if (Hlc* c = clock()) c->observe(nameStamp_.hlc);
    if (!groupName_[0] && name()) mm::formatTo(groupName_, sizeof(groupName_), "%s", name());
    startInterval(1);   // a group heard from at boot is shared at once
    if (id_[0]) return;
    makeId();
    mm::formatTo(groupName_, sizeof(groupName_), "%s", name() ? name() : "");
    nameStamp_ = stampNow();
    formatStamp(nameHlc_, sizeof(nameHlc_), nameStamp_);
    markDirty();
}

void DeviceGroup::makeId() {
    uint8_t mac[6];
    platform::getMacAddress(mac);
    uint32_t moment = platform::millis();
    // A sibling made at the same millisecond of another boot would share the id: the next moment is taken instead.
    for (bool clash = true; clash; moment++) {
        mm::formatTo(id_, sizeof(id_), "%02x%02x%02x%02x%02x%02x%08lx", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                     static_cast<unsigned long>(moment));
        clash = false;
        for (uint8_t i = 0; parent() && i < parent()->childCount(); i++) {
            const MoonModule* m = parent()->child(i);
            if (m != this && m->role() == ModuleRole::DeviceGroup && std::strcmp(static_cast<const DeviceGroup*>(m)->groupId(), id_) == 0) clash = true;
        }
    }
}

void DeviceGroup::tick1s() MM_NONBLOCKING {
    // Trickle: send at the interval's send moment unless a consistent copy was heard, then double the interval.
    if (++elapsed_ == sendAt_ && heard_ == 0) sendDue_ = true;
    if (elapsed_ >= interval_) startInterval(interval_ < kIntervalMax ? static_cast<uint8_t>(interval_ * 2) : kIntervalMax);
    if (++statusTick_ >= kStatusEverySec) { statusTick_ = 0; refreshStatus(); }
    MoonModule::tick1s();
}

void DeviceGroup::refreshStatus() MM_NONBLOCKING {
    const DevicesModule* discovery = DevicesModule::active();
    ListRow members = 0, missing = 0;
    for (ListRow i = 0; i < count_; i++) {
        if (members_[i].removed || isZeroMac(members_[i].mac)) continue;
        members++;
        if (!(discovery && discovery->heardLately(members_[i].mac))) missing++;
    }
    if (missing) mm::formatTo(status_, sizeof(status_), "%u members, %u missing", static_cast<unsigned>(members), static_cast<unsigned>(missing));
    else mm::formatTo(status_, sizeof(status_), "%u members", static_cast<unsigned>(members));
    setStatus(status_, missing ? Severity::Warning : Severity::Status);
}

void DeviceGroup::onModuleRenamed(const char* from, const char* to) {
    if (!name() || !to || std::strcmp(name(), to) != 0 || (from && std::strcmp(from, to) == 0)) return;
    if (std::strcmp(to, groupName_) == 0) return;   // the rename a merge asked for: the shared name holds it with its writer's stamp
    mm::formatTo(groupName_, sizeof(groupName_), "%s", to);
    nameStamp_ = stampNow();
    formatStamp(nameHlc_, sizeof(nameHlc_), nameStamp_);
    edited();
}

void DeviceGroup::onControlChanged(const char* control) {
    if (std::strcmp(control, "id") != 0) return;
    nameStamp_ = Stamp{};   // older than any copy's, so the next one brings the shared name and its stamp
    nameHlc_[0] = 0;
    groupName_[0] = 0;
}

ListRow DeviceGroup::listRowCount() const {
    ListRow n = 0;
    for (ListRow i = 0; i < count_; i++) if (!members_[i].removed) n++;
    return n;
}

ListRow DeviceGroup::visible(ListRow row) const {
    for (ListRow i = 0; i < count_; i++) if (!members_[i].removed && row-- == 0) return i;
    return count_;
}

void DeviceGroup::writeListRow(JsonSink& sink, ListRow row) const {
    const ListRow at = visible(row);
    if (at >= count_) { sink.append("{}"); return; }
    const Member& m = members_[at];
    const DevicesModule* discovery = DevicesModule::active();
    sink.appendf("{\"id\":%lu,\"device\":", static_cast<unsigned long>(m.id));
    const char* name = discovery ? discovery->nameOfMac(m.mac) : nullptr;
    sink.writeJsonString(name ? name : (m.name[0] ? m.name : "(pick a device)"));
    if (isZeroMac(m.mac)) { sink.append("}"); return; }
    sink.append(",\"mac\":");
    writeMac(sink, m.mac);
    sink.appendf(",\"status\":\"%s\"}", discovery && discovery->heardLately(m.mac) ? "present" : "missing");
}

void DeviceGroup::writeListRowSaved(JsonSink& sink, ListRow row) const {
    if (row >= count_) { sink.append("{}"); return; }
    const Member& m = members_[row];
    char stamp[40];
    formatStamp(stamp, sizeof(stamp), m.stamp);
    sink.append("{\"mac\":");
    writeMac(sink, m.mac);
    sink.append(",\"name\":");
    sink.writeJsonString(m.name);
    sink.appendf(",\"stamp\":\"%s\"%s}", stamp, m.removed ? ",\"removed\":true" : "");
}

void DeviceGroup::writeListRowDetail(JsonSink& sink, ListRow row) const {
    if (visible(row) >= count_) { sink.append("{}"); return; }
    // The pick starts unselected: an index into a list that moves as devices come and go goes stale within seconds.
    sink.append("{\"fields\":[{\"name\":\"device\",\"type\":\"select\",\"optionsRef\":\"devices\",\"value\":-1}]}");
}

void DeviceGroup::writeListOptionSets(JsonSink& sink) const {
    if (const DevicesModule* discovery = DevicesModule::active()) discovery->writeMemberOptions(sink, "devices");
    else sink.append("\"devices\":[]");
}

bool DeviceGroup::grow() {
    if (count_ >= static_cast<ListRow>(~ListRow{0}) || !members_.ensure(count_ + 1u)) {
        setStatus("memory full: the member was not added", Severity::Warning);
        return false;
    }
    return true;
}

bool DeviceGroup::addListRow(uint32_t& outId) {
    if (!grow()) return false;
    Member& m = members_[count_++];
    m = Member{};
    m.id = nextId_++;
    outId = m.id;
    markDirty();
    return true;
}

bool DeviceGroup::deleteListRow(uint32_t id) {
    Member* m = find(id);
    if (!m || m->removed) return false;
    if (isZeroMac(m->mac)) {   // a row still waiting for its device stayed local: it goes
        const ListRow at = static_cast<ListRow>(m - members_.data());
        for (ListRow j = at; j + 1u < count_; j++) members_[j] = members_[j + 1u];
        count_--;
        markDirty();
        return true;
    }
    m->removed = true;
    m->stamp = stampNow();
    edited();
    return true;
}

bool DeviceGroup::setListRowField(uint32_t id, const char* field, const char* valueJson) {
    Member* m = find(id);
    uint8_t mac[6] = {};
    const char* name = nullptr;
    if (!m || !pickedDevice(field, valueJson, mac, name)) return false;
    if (hasMember(mac)) { setStatus("that device is a member already", Severity::Warning); return false; }
    if (Member* old = findMac(mac); old && old != m) {   // a removed member picked again: its marker gives way to this row
        const ListRow at = static_cast<ListRow>(old - members_.data());
        const uint32_t keep = m->id;
        for (ListRow j = at; j + 1u < count_; j++) members_[j] = members_[j + 1u];
        count_--;
        m = find(keep);
    }
    std::memcpy(m->mac, mac, 6);
    m->removed = false;
    m->stamp = stampNow();
    mm::formatTo(m->name, sizeof(m->name), "%s", name ? name : "");
    edited();
    return true;
}

bool DeviceGroup::restoreList(const char* json, const char* key) {
    // Parsed and given room before the members change, so a refused write leaves the group as it was.
    json::JsonDoc* doc = nullptr;
    const json::JsonNode* arr = json::parseListArray(json, key, doc);
    if (!arr) return false;
    const int rows = json::arraySize(*doc, arr);
    if (rows > static_cast<int>(static_cast<ListRow>(~ListRow{0})) || !members_.ensure(static_cast<size_t>(rows))) {
        setStatus("memory full: the members were not restored", Severity::Warning);
        return false;
    }
    count_ = 0;
    Hlc* c = clock();
    for (int i = 0; i < rows; i++) restoreMember(*doc, json::element(*doc, arr, i), c);
    return true;
}

bool DeviceGroup::restoreMember(const json::JsonDoc& doc, const json::JsonNode* el, Hlc* c) {
    uint8_t mac[6];
    char text[40] = "";
    if (!el || el->type != json::JsonType::Object) return false;
    if (!json::readString(json::member(doc, el, "mac"), text, sizeof(text)) || !mdns::parseMac(text, mac)) return false;   // only a row holding a device is worth restoring
    if (findMac(mac)) return false;
    Member& m = members_[count_++];
    m = Member{};
    m.id = nextId_++;
    std::memcpy(m.mac, mac, 6);
    json::readString(json::member(doc, el, "name"), m.name, sizeof(m.name));
    m.removed = json::readBool(json::member(doc, el, "removed"));
    if (json::readString(json::member(doc, el, "stamp"), text, sizeof(text))) m.stamp = parseStamp(text);
    if (c) c->observe(m.stamp.hlc);   // later local edits are newer than every saved one
    return true;
}

bool DeviceGroup::hasMember(const uint8_t mac[6]) const MM_NONBLOCKING {
    if (isZeroMac(mac)) return false;
    for (ListRow i = 0; i < count_; i++)
        if (!members_[i].removed && std::memcmp(members_[i].mac, mac, 6) == 0) return true;
    return false;
}

bool DeviceGroup::madeHere() const {
    uint8_t mac[6];
    platform::getMacAddress(mac);
    char hex[13];
    mm::formatTo(hex, sizeof(hex), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return std::strncmp(id_, hex, 12) == 0;
}

uint32_t DeviceGroup::digest() const {
    // XOR of each part's hash, so two devices holding the members in another order agree.
    uint32_t d = fnv1a(groupName_, std::strlen(groupName_)) ^ static_cast<uint32_t>(nameStamp_.hlc) ^ nameStamp_.writer;
    for (ListRow i = 0; i < count_; i++) {
        const Member& m = members_[i];
        if (isZeroMac(m.mac)) continue;
        uint8_t bytes[6 + 8 + 4 + 1];
        std::memcpy(bytes, m.mac, 6);
        std::memcpy(bytes + 6, &m.stamp.hlc, 8);
        std::memcpy(bytes + 14, &m.stamp.writer, 4);
        bytes[18] = m.removed;
        d ^= fnv1a(bytes, sizeof(bytes));
    }
    return d;
}

ListRow DeviceGroup::membersFrom(ListRow from, uint16_t per) const {
    ListRow n = 0;
    for (ListRow i = from; i < count_ && n < per; i++) if (!isZeroMac(members_[i].mac)) n++;
    return n;
}

Stamp DeviceGroup::newest() const {
    Stamp n = nameStamp_;
    for (ListRow i = 0; i < count_; i++) if (members_[i].stamp.newerThan(n)) n = members_[i].stamp;
    return n;
}

size_t DeviceGroup::writeChunk(uint8_t* out, size_t cap, uint16_t chunk, uint16_t per) const {
    // id, name, the name's stamp, the whole definition's digest, then per member: MAC, stamp, removed, name.
    char tags[8 + 5 * 32] = ",sshii";
    const ListRow from = static_cast<ListRow>(chunk * per);
    const ListRow n = membersFrom(from, per);
    for (ListRow k = 0; k < n && std::strlen(tags) + 6 < sizeof(tags); k++) std::strcat(tags, "shiis");
    osc::Writer w(out, cap, kAddress, tags);
    w.str(id_);
    w.str(groupName_);
    w.i64(nameStamp_.hlc);
    w.i32(nameStamp_.writer);
    w.i32(digest());
    ListRow written = 0;
    for (ListRow i = from; i < count_ && written < n; i++) {
        if (isZeroMac(members_[i].mac)) continue;
        writeMember(w, members_[i]);
        written++;
    }
    return w.size();
}

void DeviceGroup::writeMember(osc::Writer& w, const Member& m) {
    char hex[13];
    mm::formatTo(hex, sizeof(hex), "%02x%02x%02x%02x%02x%02x", m.mac[0], m.mac[1], m.mac[2], m.mac[3], m.mac[4], m.mac[5]);
    w.str(hex);
    w.i64(m.stamp.hlc);
    w.i32(m.stamp.writer);
    w.i32(m.removed ? 1 : 0);
    w.str(m.name);
}

bool DeviceGroup::merge(osc::Reader& r, const char*& wantName) {
    wantName = nullptr;
    if (!wellFormed(r)) return false;   // merged whole or not at all, as OscPacket's parse refuses a malformed message
    // The caller read the id; the name and its stamp come next, then the sender's digest, then the members.
    bool changed = mergeName(r, wantName);
    const uint32_t senderDigest = r.i32();
    while (r.more()) {
        const char* hex = r.str();
        const Stamp s{r.i64(), r.i32()};
        const bool removed = r.i32() != 0;
        changed |= mergeMember(hex, s, removed, r.str());
    }
    if (changed) saveSoon(*this);
    // Trickle: a copy that agrees counts toward silence; one that differs, before or after the merge, brings the interval back to 1 s.
    if (senderDigest == digest() && !changed) { if (heard_ < 255) heard_++; }
    else if (interval_ > 1) startInterval(1);   // RFC 6206 4.2 step 6: an inconsistency resets an interval above Imin
    return changed;
}

bool DeviceGroup::chunkHasMember(osc::Reader r, const uint8_t mac[6], const char*& name) {
    name = "";
    if (!wellFormed(r)) return false;
    name = r.str();
    r.i64(); r.i32(); r.i32();   // the shared name's stamp and the sender's digest
    bool found = false;
    while (r.more()) {
        uint8_t m[6] = {};
        mdns::parseMac(r.str(), m);
        r.i64(); r.i32();
        const bool removed = r.i32() != 0;
        r.str();
        if (!removed && std::memcmp(m, mac, 6) == 0) found = true;
    }
    return found;
}

bool DeviceGroup::wellFormed(osc::Reader r) {
    r.str();
    bool valid = Stamp{r.i64(), r.i32()}.valid();
    r.i32();   // the sender's digest
    while (r.more()) {
        uint8_t mac[6];
        valid = mdns::parseMac(r.str(), mac) && !isZeroMac(mac) && valid;
        valid = Stamp{r.i64(), r.i32()}.valid() && valid;
        r.i32();
        r.str();
    }
    return valid && r.ok();
}

bool DeviceGroup::isId(const char* text) {
    size_t n = 0;
    for (; text[n]; n++)
        if (n >= kIdLen || !((text[n] >= '0' && text[n] <= '9') || (text[n] >= 'a' && text[n] <= 'f'))) return false;
    return n == kIdLen;
}

bool DeviceGroup::mergeName(osc::Reader& r, const char*& wantName) {
    const char* n = r.str();
    const Stamp ns{r.i64(), r.i32()};
    if (Hlc* c = clock()) c->observe(ns.hlc);
    if (!ns.newerThan(nameStamp_) || !n[0]) return false;
    mm::formatTo(groupName_, sizeof(groupName_), "%s", n);
    nameStamp_ = ns;
    formatStamp(nameHlc_, sizeof(nameHlc_), nameStamp_);
    if (!name() || std::strcmp(name(), groupName_) != 0) wantName = groupName_;   // the module follows when the tree has the name free
    return true;
}

bool DeviceGroup::mergeMember(const char* hex, const Stamp& s, bool removed, const char* name) {
    uint8_t mac[6];
    if (!mdns::parseMac(hex, mac) || isZeroMac(mac)) return false;
    if (Hlc* c = clock()) c->observe(s.hlc);
    Member* m = findMac(mac);
    if (m && !s.newerThan(m->stamp)) return false;   // ours is as new or newer: last writer wins
    if (!m) {
        if (!grow()) return false;
        m = &members_[count_++];
        *m = Member{};
        m->id = nextId_++;
        std::memcpy(m->mac, mac, 6);
    }
    m->stamp = s;
    m->removed = removed;
    if (name[0]) mm::formatTo(m->name, sizeof(m->name), "%s", name);
    return true;
}

Stamp DeviceGroup::stampNow() {
    Hlc* c = clock();
    return c ? c->now(selfWriter()) : Stamp{static_cast<uint64_t>(platform::millis()) << 16, selfWriter()};
}

DeviceGroup::Member* DeviceGroup::find(uint32_t id) {
    for (ListRow i = 0; i < count_; i++) if (members_[i].id == id) return &members_[i];
    return nullptr;
}

DeviceGroup::Member* DeviceGroup::findMac(const uint8_t mac[6]) {
    for (ListRow i = 0; i < count_; i++) if (std::memcmp(members_[i].mac, mac, 6) == 0) return &members_[i];
    return nullptr;
}

void DeviceGroup::edited() {
    markDirty();
    startInterval(1);
}

void DeviceGroup::startInterval(uint8_t seconds) {
    interval_ = seconds;
    elapsed_ = 0;
    heard_ = 0;
    // A moment in the second half, spread by the id and the clock so a group's members do not all send in the same second.
    const uint8_t half = static_cast<uint8_t>(seconds / 2);
    sendAt_ = static_cast<uint8_t>(half + 1 + (half ? (fnv1a(id_, std::strlen(id_)) ^ platform::millis()) % half : 0));
    if (sendAt_ > seconds) sendAt_ = seconds;
}

Hlc* DeviceGroup::clock() {
    return parent() && parent()->acceptsRole(ModuleRole::DeviceGroup) ? &static_cast<Devices*>(parent())->clock() : nullptr;
}

}  // namespace mm
