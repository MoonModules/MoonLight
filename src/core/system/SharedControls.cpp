/// The shared values and recalls behind SharedControls.h: merged by their stamps, written as OSC, and a recalled look fetched from its maker.
#include "core/system/SharedControls.h"

#include "core/module/Scheduler.h"
#include "core/services/OscPacket.h"
#include "core/system/ControlModule.h"   // splitTarget: a parameter named as a slot names it; kPresetDir: where a preset is served
#include "core/util/JsonFetch.h"         // the fetch and the live apply, off the render path
#include "core/util/fnv.h"
#include "core/util/format.h"

#include <cctype>   // std::isalnum: the characters a URL carries as they are
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

/// Whether `c` is one of RFC 3986's unreserved characters, which a URL carries as they are.
bool unreserved(unsigned char c) {
    return std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~';
}

/// `s` percent-encoded into `out`, false when it does not fit.
bool percentEncode(const char* s, char* out, size_t cap) {
    size_t at = 0;
    for (; *s; s++) {
        const auto c = static_cast<unsigned char>(*s);
        const int n = unreserved(c) ? std::snprintf(out + at, cap - at, "%c", c) : std::snprintf(out + at, cap - at, "%%%02X", c);
        if (n < 0 || at + static_cast<size_t>(n) >= cap) return false;
        at += static_cast<size_t>(n);
    }
    return true;
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

SharedControls::Recall* SharedControls::recallOf(uint32_t group) {
    for (uint8_t i = 0; i < recallCount_; i++)
        if (recalls_[i].group == group) return &recalls_[i];
    if (recallCount_ >= kMaxGroups || !recalls_.ensure(recallCount_ + 1u)) return nullptr;
    Recall& r = recalls_[recallCount_++];
    r = Recall{};
    r.group = group;
    return &r;
}

void SharedControls::recallLocal(uint32_t group, const char* name, const uint8_t ip[4], uint16_t port, const Stamp& s) {
    Recall* r = validName(name) ? recallOf(group) : nullptr;
    if (!r) return;
    mm::formatTo(r->name, sizeof(r->name), "%s", name);
    std::memcpy(r->ip, ip, 4);
    r->port = port;
    r->stamp = s;
    r->applied = true;   // applied here already
}

size_t SharedControls::writeRecall(uint8_t* out, size_t cap, const char* groupId) const {
    const uint32_t group = keyOf(groupId);
    const Recall* r = nullptr;
    for (uint8_t i = 0; i < recallCount_ && !r; i++)
        if (recalls_[i].group == group) r = &recalls_[i];
    if (!r || !r->name[0]) return 0;
    // The group's id, the look's name, its stamp, and where its maker serves it.
    osc::Writer w(out, cap, kLookAddress, ",sshiii");
    w.str(groupId);
    w.str(r->name);
    w.i64(r->stamp.hlc);
    w.i32(r->stamp.writer);
    uint32_t ip = 0;
    std::memcpy(&ip, r->ip, 4);
    w.i32(ip);
    w.i32(r->port);
    return w.size();
}

bool SharedControls::mergeRecall(osc::Reader& r, uint32_t group, Hlc* clock) {
    const char* name = r.str();
    const Stamp s{r.i64(), r.i32()};
    const uint32_t ip = r.i32();
    const auto port = static_cast<uint16_t>(r.i32());
    if (!r.ok() || !s.valid() || !validName(name)) return true;
    if (clock) clock->observe(s.hlc);
    Recall* held = recallOf(group);
    if (!held) return true;
    if (held->name[0] && !s.newerThan(held->stamp)) return !held->stamp.newerThan(s);   // the same recall again, or an older one, which Trickle answers
    mm::formatTo(held->name, sizeof(held->name), "%s", name);
    std::memcpy(held->ip, &ip, 4);
    held->port = port;
    held->stamp = s;
    held->applied = false;
    return true;
}

bool SharedControls::urlOf(const Recall& r, char* out, size_t cap) {
    char file[3 * kNameLen + 1];
    if (!percentEncode(r.name, file, sizeof(file))) return false;
    const int n = std::snprintf(out, cap, "http://%u.%u.%u.%u:%u/api/file?path=%s/%s.json", r.ip[0], r.ip[1], r.ip[2], r.ip[3],
                                static_cast<unsigned>(r.port), ControlModule::kPresetDir, file);
    return n > 0 && static_cast<size_t>(n) < cap;
}

void SharedControls::tickFetch() {
    if (applyHandle_) {
        const int outcome = JsonFetch::instance().outcome(this, applyHandle_);
        if (outcome != -1) endFetch(outcome == 200 ? nullptr : "not applied");
        return;
    }
    if (fetchHandle_) { advanceFetch(); return; }
    startFetch();
}

void SharedControls::startFetch() {
    for (uint8_t i = 0; i < recallCount_; i++) {
        Recall& r = recalls_[i];
        if (r.applied || !r.name[0]) continue;
        r.applied = true;   // tried once per stamp; a later recall tries again
        mm::formatTo(fetchName_, sizeof(fetchName_), "%s", r.name);
        char url[160];
        if (!urlOf(r, url, sizeof(url))) { endFetch("not fetched"); return; }
        fetchHandle_ = JsonFetch::instance().fetch(this, url, UINT32_MAX / 2);   // once: a recall is a moment, not a feed
        if (!fetchHandle_) endFetch("not fetched");
        return;
    }
}

void SharedControls::advanceFetch() {
    JsonFetch& f = JsonFetch::instance();
    const int outcome = f.outcome(this, fetchHandle_);
    if (outcome == -1) return;   // on its way
    json::JsonDoc* doc = outcome == 200 ? f.doc(this, fetchHandle_) : nullptr;
    applyHandle_ = doc ? f.push(this, nullptr, fetchHandle_) : 0;   // applied live between frames, as a script's push to its own device is
    if (!applyHandle_) endFetch("not fetched");
}

void SharedControls::endFetch(const char* failed) {
    if (failed) mm::formatTo(fetchNote_, sizeof(fetchNote_), "look %s %s", fetchName_, failed);
    else fetchNote_[0] = 0;
    JsonFetch::instance().release(this);   // released at once: the next recall fetches again
    fetchHandle_ = applyHandle_ = 0;
}

void SharedControls::clear() {
    if (fetchHandle_ || applyHandle_) JsonFetch::instance().release(this);
    fetchHandle_ = applyHandle_ = 0;
    valueCount_ = recallCount_ = 0;
    full_ = false;
    fetchNote_[0] = 0;
}

}  // namespace mm
