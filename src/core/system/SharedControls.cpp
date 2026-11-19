/// The shared values and looks behind SharedControls.h: merged by their stamps, written as OSC, and a look pushed in chunks and repaired on request.
#include "core/system/SharedControls.h"

#include "core/module/Scheduler.h"
#include "core/services/OscPacket.h"
#include "core/system/ControlModule.h"   // splitTarget: a parameter named as a slot names it; readPreset: the document a look pushes
#include "core/util/fnv.h"
#include "core/util/format.h"
#include "platform/platform.h"     // millis and the MAC: when a chunk or a request goes, and each device's offset

#include <algorithm>   // std::min: the last chunk is the rest
#include <cstdio>
#include <cstring>

namespace mm {

namespace {

/// Whether `param` names a control as a slot does: "Module.control", short enough to hold.
bool validParam(const char* param) {
    const size_t n = std::strlen(param);
    return n > 2 && n < SharedControls::kParamLen && std::strchr(param, '.') && param[0] != '.';
}

/// Whether `name` is a preset's name: a file name with no path in it.
bool validName(const char* name) {
    const size_t n = std::strlen(name);
    return n > 0 && n < SharedControls::kNameLen && !std::strchr(name, '/') && !std::strstr(name, "..");
}

/// Whether a control of this type holds a value a slot drives: a number, a toggle or a choice; a pin, an address or text stays the board's own.
bool valueType(ControlType t) {
    switch (t) {
        case ControlType::Uint8: case ControlType::Uint16: case ControlType::Int16: case ControlType::Int32:
        case ControlType::Bool: case ControlType::Select: case ControlType::Palette:
            return true;
        default:
            return false;
    }
}

/// Whether a group may set `m`'s controls: an effect, a modifier or a layer, which an effects preset carries, or the Drivers card itself.
bool lookModule(const MoonModule& m) {
    const ModuleRole r = m.role();
    if (r == ModuleRole::Effect || r == ModuleRole::Modifier || r == ModuleRole::Layer) return true;
    return !m.parent() && m.name() && std::strcmp(m.name(), ControlModule::kCapturable[ControlModule::kDriversRole]) == 0;
}

}  // namespace

const ControlDescriptor* SharedControls::shareable(const char* param, char (&module)[24], const char*& control) {
    control = ControlModule::splitTarget(param, module);
    Scheduler* sched = Scheduler::instance();
    MoonModule* m = control && sched ? sched->firstByName(module) : nullptr;
    if (!m || !lookModule(*m)) return nullptr;
    const ControlList& cs = m->controls();
    for (uint8_t i = 0; i < cs.count(); i++)
        if (std::strcmp(cs[i].name, control) == 0) return valueType(cs[i].type) ? &cs[i] : nullptr;
    return nullptr;
}

uint32_t SharedControls::keyOf(const char* groupId) {
    return fnv1a(groupId, std::strlen(groupId));
}

// ---- Values ---------------------------------------------------------------------------------

SharedControls::Value* SharedControls::find(uint32_t group, const char* param) {
    for (uint16_t i = 0; i < valueCount_; i++)
        if (values_[i].group == group && std::strcmp(values_[i].param, param) == 0) return &values_[i];
    return nullptr;
}

SharedControls::Value* SharedControls::oldest(uint32_t group, uint16_t& count) {
    Value* o = nullptr;
    count = 0;
    for (uint16_t i = 0; i < valueCount_; i++) {
        if (values_[i].group != group) continue;
        count++;
        if (!o || o->stamp.newerThan(values_[i].stamp)) o = &values_[i];
    }
    return o;
}

uint8_t SharedControls::groupCount() const {
    uint8_t n = 0;
    for (uint16_t i = 0; i < valueCount_; i++) {
        uint16_t j = 0;
        while (j < i && values_[j].group != values_[i].group) j++;
        if (j == i) n++;   // the first value of its group
    }
    return n;
}

void SharedControls::refreshFull() {
    full_ = false;
    for (uint16_t i = 0; i < valueCount_ && !full_; i++) {
        uint16_t count = 0;
        oldest(values_[i].group, count);
        full_ = count >= kMaxPerGroup;
    }
}

SharedControls::Value* SharedControls::add(uint32_t group, const char* param, const Stamp& s) {
    uint16_t count = 0;
    Value* o = oldest(group, count);
    Value* slot = nullptr;
    if (count >= kMaxPerGroup) {
        // The group is full: its own oldest-stamped value gives way to a newer one, so the group's members keep the same newest values.
        full_ = true;
        if (s.newerThan(o->stamp)) slot = o;
    } else if (!count && groupCount() >= kMaxGroups) {
        full_ = true;   // one group more than the table holds: its values stay unheld, and the status says so
    } else if (values_.ensure(valueCount_ + 1u)) {
        slot = &values_[valueCount_++];
    }
    if (!slot) return nullptr;
    Value& v = *slot;
    v = Value{};
    v.group = group;
    mm::formatTo(v.param, sizeof(v.param), "%s", param);
    return &v;
}

bool SharedControls::setLocal(uint32_t group, const char* param, int32_t value, const Stamp& s) {
    if (!validParam(param)) return false;
    Value* v = find(group, param);
    if (!v) v = add(group, param, s);
    if (!v) return false;
    v->value = value;
    v->stamp = s;
    return true;
}

bool SharedControls::holds(const char* param, int32_t value) const {
    for (uint16_t i = 0; i < valueCount_; i++)
        if (values_[i].value == value && std::strcmp(values_[i].param, param) == 0) return true;
    return false;
}

uint32_t SharedControls::digest(uint32_t group) const {
    // XOR of each value's hash, so two devices holding them in another order agree.
    uint32_t d = 0;
    for (uint16_t i = 0; i < valueCount_; i++) {
        const Value& v = values_[i];
        if (v.group != group) continue;
        const uint32_t stampBits = static_cast<uint32_t>(v.stamp.hlc) ^ static_cast<uint32_t>(v.stamp.hlc >> 32) ^ v.stamp.writer;
        d ^= fnv1a(v.param, std::strlen(v.param)) ^ static_cast<uint32_t>(v.value) * 2654435761u ^ stampBits;
    }
    return d;
}

size_t SharedControls::writeValues(uint8_t* out, size_t cap, const char* groupId, uint16_t& from, const char* param) const {
    // The group's id and the sender's digest, then per value: its name, its value, its stamp.
    const uint32_t group = keyOf(groupId);
    uint16_t picked[kPerMessage];
    uint8_t n = 0;
    for (; from < valueCount_ && n < kPerMessage; from++) {
        const Value& v = values_[from];
        if (v.group == group && (!param || std::strcmp(v.param, param) == 0)) picked[n++] = from;
    }
    char tags[4 + 4 * kPerMessage] = ",si";
    for (uint8_t k = 0; k < n; k++) std::strcat(tags, "sihi");
    osc::Writer w(out, cap, kValuesAddress, tags);
    w.str(groupId);
    w.i32(digest(group));
    for (uint8_t k = 0; k < n; k++) {
        const Value& v = values_[picked[k]];
        w.str(v.param);
        w.i32(static_cast<uint32_t>(v.value));
        w.i64(v.stamp.hlc);
        w.i32(v.stamp.writer);
    }
    return w.size();
}

bool SharedControls::mergeValues(osc::Reader& r, uint32_t group, Hlc* clock) {
    const uint32_t senderDigest = r.i32();
    for (uint8_t n = 0; n < kPerMessage && r.more(); n++) {   // the bound a sender writes, so a crafted message sets no more
        const char* param = r.str();
        const auto value = static_cast<int32_t>(r.i32());
        const Stamp s{r.i64(), r.i32()};
        if (!r.ok()) return true;   // a malformed message stops the merge where it breaks, and asks for no repeat
        if (!s.valid() || !validParam(param)) continue;
        if (clock) clock->observe(s.hlc);
        mergeOne(group, param, value, s);
    }
    return digest(group) == senderDigest;
}

void SharedControls::mergeOne(uint32_t group, const char* param, int32_t value, const Stamp& s) {
    Value* v = find(group, param);
    if (v && !s.newerThan(v->stamp)) return;   // ours is as new or newer: last writer wins
    if (!v) v = add(group, param, s);
    if (!v) return;   // older than every value a full table holds: stale
    v->value = value;
    v->stamp = s;
    apply(param, value);
}

void SharedControls::apply(const char* param, int32_t value) {
    Scheduler* sched = Scheduler::instance();
    char module[24];
    const char* control = nullptr;
    const ControlDescriptor* c = shareable(param, module, control);
    if (!sched || !c) return;   // a control this device lacks, or one a group may not set
    int32_t current = 0;
    if (sched->getControlWide(module, control, current) && current == value) return;
    char body[32];
    if (c->type == ControlType::Bool) mm::formatTo(body, sizeof(body), "{\"value\":%s}", value ? "true" : "false");
    else mm::formatTo(body, sizeof(body), "{\"value\":%ld}", static_cast<long>(value));
    sched->setControl(module, control, body);
}

// ---- The look -------------------------------------------------------------------------------

SharedControls::Look* SharedControls::lookOf(uint32_t group, bool make) {
    for (uint8_t i = 0; i < lookCount_; i++)
        if (looks_[i].group == group) return &looks_[i];
    if (!make || lookCount_ >= kMaxGroups || !looks_.ensure(lookCount_ + 1u)) return nullptr;
    Look& l = looks_[lookCount_++];
    l = Look{};
    l.group = group;
    return &l;
}

void SharedControls::freeText(Look& l) {
    platform::free(l.text);
    l.text = nullptr;
}

bool SharedControls::restart(Look& l, const LookHead& h) {
    char* text = static_cast<char*>(platform::alloc(h.size + 1));   // first, so a refusal leaves no half-held look behind
    freeText(l);
    const uint32_t group = l.group;
    l = Look{};
    l.group = group;
    if (!text) { mm::formatTo(lookNote_, sizeof(lookNote_), "look %s too large", h.name); return false; }   // the slot stays empty: the next copy tries afresh
    text[h.size] = '\0';
    l.text = text;
    l.stamp = h.stamp;
    mm::formatTo(l.name, sizeof(l.name), "%s", h.name);
    l.size = h.size;
    l.digest = h.digest;
    l.needDueMs = platform::millis() + spreadMs();
    return true;
}

uint32_t SharedControls::spreadMs() {
    static const uint32_t spread = [] {
        uint8_t mac[6];
        platform::getMacAddress(mac);
        return fnv1a(mac, sizeof(mac)) % kSpreadMs;
    }();
    return spread;
}

void SharedControls::writeHead(osc::Writer& w, const char* groupId, const Look& l) {
    w.str(groupId);
    w.str(l.name);
    w.i64(l.stamp.hlc);
    w.i32(l.stamp.writer);
    w.i32(l.size);
    w.i32(l.digest);
}

bool SharedControls::readHead(osc::Reader& r, LookHead& h) {
    h.name = r.str();
    h.stamp = Stamp{r.i64(), r.i32()};
    h.size = r.i32();
    h.digest = r.i32();
    return r.ok() && h.stamp.valid() && validName(h.name) && h.size && h.size <= uint32_t{kChunkBytes} * kMaxChunks;
}

void SharedControls::lookLocal(uint32_t group, const char* name, char* doc, const Stamp& s, int64_t atUs) {
    const size_t size = doc ? std::strlen(doc) : 0;
    Look* l = validName(name) && size && size <= size_t{kChunkBytes} * kMaxChunks ? lookOf(group, true) : nullptr;
    if (!l) {
        if (doc) mm::formatTo(lookNote_, sizeof(lookNote_), "look %s not shared", name);
        platform::free(doc);
        return;
    }
    freeText(*l);
    *l = Look{};
    l->group = group;
    mm::formatTo(l->name, sizeof(l->name), "%s", name);
    l->stamp = s;
    l->size = static_cast<uint32_t>(size);
    l->digest = fnv1a(doc, size);
    l->text = doc;
    l->have = allOf(l->size);
    l->send = l->have;   // every chunk, from the next read on
    l->sendDueMs = platform::millis();
    l->atUs = atUs;
    l->applied = true;   // applied here already
    lookNote_[0] = 0;
}

bool SharedControls::merge(uint8_t kind, osc::Reader& r, uint32_t group, Hlc* clock) {
    switch (kind) {
        case 0: return mergeValues(r, group, clock);
        case 1: return mergeLook(r, group, clock);
        case 2: mergeChunk(r, group, clock); return true;
        default: mergeNeed(r, group); return true;
    }
}

size_t SharedControls::writeLook(uint8_t* out, size_t cap, const char* groupId) const {
    const Look* l = nullptr;
    for (uint8_t i = 0; i < lookCount_ && !l; i++)
        if (looks_[i].group == keyOf(groupId)) l = &looks_[i];
    if (!l || !l->name[0]) return 0;
    osc::Writer w(out, cap, kLookAddress, ",sshiiii");
    writeHead(w, groupId, *l);
    w.i32(l->have);
    return w.size();
}

bool SharedControls::mergeLook(osc::Reader& r, uint32_t group, Hlc* clock) {
    LookHead h;
    const bool fits = readHead(r, h);
    r.i32();   // the chunks the sender holds: a member short of some asks for them itself
    if (!fits || !r.ok()) return true;
    if (clock) clock->observe(h.stamp.hlc);
    Look* l = lookOf(group, true);
    if (!l) return true;
    if (l->name[0] && !h.stamp.newerThan(l->stamp)) return !l->stamp.newerThan(h.stamp);   // the same look, or an older one, which Trickle answers
    restart(*l, h);   // its chunks are asked for at this device's offset
    return true;
}

bool SharedControls::readChunk(osc::Reader& r, LookHead& h, Chunk& c) {
    const bool fits = readHead(r, h);
    c.atUs = static_cast<int64_t>(r.i64());
    c.index = r.i32();
    c.data = r.blob(c.n);
    return fits && r.ok() && c.index < chunksOf(h.size) && c.n == std::min<uint32_t>(kChunkBytes, h.size - c.index * kChunkBytes);   // whole, the last one the rest
}

void SharedControls::mergeChunk(osc::Reader& r, uint32_t group, Hlc* clock) {
    LookHead h;
    Chunk c;
    if (!readChunk(r, h, c)) return;
    if (clock) clock->observe(h.stamp.hlc);
    Look* l = lookOf(group, true);
    if (!l || !accepts(*l, h)) return;
    if (!l->atUs) l->atUs = c.atUs;
    fill(*l, c.index, c.data, c.n);
}

bool SharedControls::accepts(Look& l, const LookHead& h) {
    if (l.name[0] && l.stamp.newerThan(h.stamp)) return false;   // an older look's chunk
    if (!l.name[0] || h.stamp.newerThan(l.stamp)) return restart(l, h);   // a newer look starts with it, unless memory refuses it
    return l.size == h.size && l.digest == h.digest;   // the same stamp naming another document is refused
}

void SharedControls::fill(Look& l, uint32_t index, const uint8_t* data, size_t n) {
    const uint32_t bit = 1u << index;
    l.send &= ~bit;   // sent by another: this device stays quiet about it
    if (!l.text || (l.have & bit)) return;
    std::memcpy(l.text + index * kChunkBytes, data, n);
    l.have |= bit;
    if (l.have != allOf(l.size)) return;
    l.needDueMs = 0;
    if (fnv1a(l.text, l.size) == l.digest) return;   // whole: tickLooks applies it at its time
    mm::formatTo(lookNote_, sizeof(lookNote_), "look %s damaged", l.name);
    l.have = 0;   // asked for again
    l.needDueMs = platform::millis() + spreadMs();
}

void SharedControls::mergeNeed(osc::Reader& r, uint32_t group) {
    const Stamp s{r.i64(), r.i32()};
    const uint32_t missing = r.i32();
    Look* l = r.ok() ? lookOf(group, false) : nullptr;
    if (!l || l->stamp.newerThan(s) || s.newerThan(l->stamp)) return;   // only the same look's chunks are answered
    const uint32_t answer = missing & l->have & allOf(l->size);
    if (!answer) return;
    if (!l->send) l->sendDueMs = platform::millis() + spreadMs();
    l->send |= answer;
}

size_t SharedControls::writeDue(uint8_t* out, size_t cap, const char* groupId, uint32_t nowMs) {
    Look* l = lookOf(keyOf(groupId), false);
    if (!l || !l->name[0]) return 0;
    if (l->send && static_cast<int32_t>(nowMs - l->sendDueMs) >= 0) {
        uint8_t index = 0;
        while (!(l->send & (1u << index))) index++;
        l->send &= ~(1u << index);
        const uint32_t offset = uint32_t{index} * kChunkBytes;
        osc::Writer w(out, cap, kChunkAddress, ",sshiiihib");
        writeHead(w, groupId, *l);
        w.i64(static_cast<uint64_t>(l->atUs));
        w.i32(index);
        w.blob(reinterpret_cast<const uint8_t*>(l->text) + offset, std::min<uint32_t>(kChunkBytes, l->size - offset));
        return w.size();
    }
    if (l->needDueMs && static_cast<int32_t>(nowMs - l->needDueMs) >= 0) {
        l->needDueMs = nowMs + kNeedEveryMs + spreadMs();
        osc::Writer w(out, cap, kNeedAddress, ",shii");
        w.str(groupId);
        w.i64(l->stamp.hlc);
        w.i32(l->stamp.writer);
        w.i32(allOf(l->size) & ~l->have);
        return w.size();
    }
    return 0;
}

bool SharedControls::due(const Look& l, int64_t fleetNowUs, bool synced, int64_t maxHoldUs) {
    if (l.applied || !l.text || !l.size || l.have != allOf(l.size)) return false;
    return !(synced && l.atUs > fleetNowUs && l.atUs - fleetNowUs <= maxHoldUs);   // whole, and its time come or not to be waited for
}

void SharedControls::tickLooks(int64_t fleetNowUs, bool synced, int64_t maxHoldUs) {
    Scheduler* sched = Scheduler::instance();
    for (uint8_t i = 0; sched && i < lookCount_; i++) {
        Look& l = looks_[i];
        if (!due(l, fleetNowUs, synced, maxHoldUs)) continue;
        char* copy = static_cast<char*>(platform::alloc(l.size + 1));   // the live apply frees what it takes
        if (!copy) continue;
        std::memcpy(copy, l.text, l.size + 1);
        if (!sched->applyLive(copy, nullptr, 0)) { platform::free(copy); continue; }   // the next read tries again
        l.applied = true;
        lookNote_[0] = 0;
    }
}

void SharedControls::clear() {
    for (uint8_t i = 0; i < lookCount_; i++) freeText(looks_[i]);
    valueCount_ = lookCount_ = 0;
    full_ = false;
    lookNote_[0] = 0;
}

}  // namespace mm
