/// The table, the worker and the two rings behind JsonFetch.h.

#include "core/util/JsonFetch.h"

#include "core/module/Scheduler.h"
#include "core/util/JsonSink.h"

#include <cstdio>
#include <cstring>
#include <new>

namespace mm {

namespace {

/// A copy of `s` from the platform heap, which the worker frees with the same allocator.
char* copyText(const char* s) {
    if (!s) return nullptr;
    const size_t n = std::strlen(s);
    char* c = static_cast<char*>(platform::alloc(n + 1));
    if (c) std::memcpy(c, s, n + 1);
    return c;
}

/// Whether a target names this device: nothing at all.
bool isSelf(const char* target) { return !target || !*target; }

/// Whether two targets are the same, the empty and the null one both being this device.
bool sameTarget(const char* a, const char* b) {
    if (isSelf(a) || isSelf(b)) return isSelf(a) && isSelf(b);
    return std::strcmp(a, b) == 0;
}

}  // namespace

JsonFetch JsonFetch::instance_;

void JsonFetch::poll() {
    // With nothing started both steps return at once.
    JsonFetch& f = instance();
    f.takeAnswers();
    f.retireIfUnused();
}

void JsonFetch::retireIfUnused() {
    if (!started_ || busy_.load(std::memory_order_relaxed) != 0) return;
    // A feed asks again on its own, so it keeps the worker; a held reply waits for the next push, which starts it again.
    for (const Slot& s : slots_) if (s.kind == Kind::Feed) return;
    // Nothing queued and nothing on its way, so the worker waits and the join is immediate.
    stop_.store(true, std::memory_order_relaxed);
    platform::stopPinnedTask(task_);
    started_ = false;
    stop_.store(false, std::memory_order_relaxed);
}

JsonFetch::~JsonFetch() {
    if (started_) {
        stop_.store(true, std::memory_order_relaxed);
        platform::stopPinnedTask(task_);
    }
    Job j;
    while (jobs_.pop(&j, 1)) { platform::free(j.url); platform::free(j.body); }
    Answer a;
    while (answers_.pop(&a, 1)) delete a.doc;
    for (Slot& s : slots_) if (s.kind != Kind::Free) freeSlot(s);
}

const JsonFetch::Slot* JsonFetch::slotFor(const void* owner, int handle) const MM_NONBLOCKING {
    const int index = indexOf(handle);
    if (index < 0) return nullptr;
    const Slot& s = slots_[index];
    // The epoch, so a handle kept past its document's release names nothing rather than the next one in its place.
    return (s.kind != Kind::Free && s.owner == owner && s.epoch == static_cast<uint16_t>(handle >> 8)) ? &s : nullptr;
}

int JsonFetch::claim(const void* owner, Kind kind) {
    for (int i = 0; i < kMaxDocs; i++) {
        Slot& s = slots_[i];
        if (s.kind != Kind::Free) continue;
        const uint16_t epoch = s.epoch;
        s = Slot{};
        s.epoch = epoch;
        s.kind = kind;
        s.owner = owner;
        return i;
    }
    return -1;
}

void JsonFetch::freeSlot(Slot& s) {
    delete s.doc;
    platform::free(s.target);
    platform::free(s.waiting);
    const uint16_t epoch = static_cast<uint16_t>(s.epoch + 1);   // an answer on its way finds the slot changed
    s = Slot{};
    s.epoch = epoch;
}

int JsonFetch::literal(const void* owner, const char* text) {
    if (!text) return 0;
    for (int i = 0; i < kMaxDocs; i++)
        if (slots_[i].kind == Kind::Literal && slots_[i].owner == owner && slots_[i].key == text) return handleOf(i);
    const int i = claim(owner, Kind::Literal);
    if (i < 0) return 0;
    Slot& s = slots_[i];
    s.key = text;
    s.doc = new (std::nothrow) json::JsonDoc;
    if (!s.doc || !json::parse(text, *s.doc)) { freeSlot(s); return 0; }
    s.generation = 1;
    s.status = 200;
    return handleOf(i);
}

int JsonFetch::fetch(const void* owner, const char* url, uint32_t periodMs) {
    if (!url || !*url) return 0;
    for (int i = 0; i < kMaxDocs; i++) {
        Slot& s = slots_[i];
        if (s.kind != Kind::Feed || s.owner != owner || std::strcmp(s.target, url) != 0) continue;
        s.periodMs = periodMs;
        refreshIfDue(i);
        return handleOf(i);
    }
    const int i = claim(owner, Kind::Feed);
    if (i < 0) return 0;
    Slot& s = slots_[i];
    s.target = copyText(url);
    if (!s.target) { freeSlot(s); return 0; }
    s.periodMs = periodMs;
    s.dueAt = platform::millis();
    refreshIfDue(i);
    return handleOf(i);
}

int JsonFetch::replySlot(const void* owner, const char* target) {
    for (int k = 0; k < kMaxDocs; k++)
        if (slots_[k].kind == Kind::Reply && slots_[k].owner == owner && sameTarget(slots_[k].target, target)) return k;
    const int i = claim(owner, Kind::Reply);
    if (i < 0) return -1;
    if (!isSelf(target)) {
        slots_[i].target = copyText(target);
        if (!slots_[i].target) { freeSlot(slots_[i]); return -1; }
    }
    return i;
}

void JsonFetch::hand(int index, char* body) {
    Slot& reply = slots_[index];
    if (reply.inFlight) {
        // The newest waits for the answer to the one on its way.
        platform::free(reply.waiting);
        reply.waiting = body;
        return;
    }
    dispatch(index, body);
}

int JsonFetch::push(const void* owner, const char* target, int handle) {
    Slot* from = slotFor(owner, handle);
    if (!from || !from->doc || !from->doc->valid()) return 0;
    const int i = replySlot(owner, target);
    if (i < 0) return 0;
    JsonSink sink;
    json::write(sink, *from->doc, from->doc->rootNode());
    if (sink.overflowed()) { lost(i); return handleOf(i); }
    hand(i, sink.detach());
    return handleOf(i);
}

void JsonFetch::lost(int index) {
    // Answered as a request that reached nothing, so `jsonChanged` and `jsonStatus` say it did not go.
    slots_[index].status = 0;
    slots_[index].generation++;
}

int JsonFetch::post(const void* owner, const char* url, const char* body) {
    if (isSelf(url) || !body) return 0;
    const int i = replySlot(owner, url);
    if (i < 0) return 0;
    slots_[i].keepAnswer = false;
    char* copy = copyText(body);
    if (!copy) return 0;
    hand(i, copy);
    return slots_[i].inFlight ? handleOf(i) : 0;
}

int JsonFetch::outcome(const void* owner, int handle) const MM_NONBLOCKING {
    const Slot* s = slotFor(owner, handle);
    if (!s) return 0;
    return s->inFlight || s->waiting ? -1 : s->status;
}

void JsonFetch::dispatch(int index, char* body) {
    Slot& reply = slots_[index];
    if (isSelf(reply.target)) {
        Scheduler* sched = Scheduler::instance();
        const uintptr_t tag = (static_cast<uintptr_t>(index) << 16) | reply.epoch;
        if (!sched || !sched->applyLive(body, &JsonFetch::applied, tag)) { platform::free(body); lost(index); return; }
        reply.inFlight = true;
        busy_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // A bare host is a MoonLight, whose state takes the document live; a URL is any endpoint, which takes a POST.
    const char* target = reply.target;
    const bool url = std::strncmp(target, "http://", 7) == 0 || std::strncmp(target, "https://", 8) == 0;
    char stateUrl[96];
    if (!url) std::snprintf(stateUrl, sizeof(stateUrl), "http://%s/api/state?live=1", target);
    if (!send(index, url ? "POST" : "PATCH", url ? target : stateUrl, body)) { platform::free(body); lost(index); }
}

bool JsonFetch::send(int index, const char* method, const char* url, char* body) {
    Slot& s = slots_[index];
    if (!started_) {
        // One worker for every request, started by the first.
        started_ = platform::spawnPinnedTask(task_, "jsonFetch", &JsonFetch::worker, this, kWorkerStack, 1, -1);
        if (!started_) return false;
    }
    char* u = copyText(url);
    if (!u) return false;
    Job j{static_cast<uint8_t>(index), s.epoch, method, u, body, s.keepAnswer};
    if (jobs_.push(&j, 1) != 1) { platform::free(u); return false; }
    s.inFlight = true;
    busy_.fetch_add(1, std::memory_order_relaxed);
    platform::notifyTask(task_);
    return true;
}

void JsonFetch::refreshIfDue(int index) {
    Slot& s = slots_[index];
    const uint32_t now = platform::millis();
    if (s.inFlight || static_cast<int32_t>(now - s.dueAt) < 0) return;
    if (send(index, "GET", s.target, nullptr)) s.dueAt = now + s.periodMs;
}

void JsonFetch::takeAnswers() {
    Answer a;
    while (answers_.pop(&a, 1)) {
        busy_.fetch_sub(1, std::memory_order_relaxed);
        deliver(a.slot, a.epoch, a.status, a.doc);
    }
}

void JsonFetch::deliver(uint8_t index, uint16_t epoch, int status, json::JsonDoc* d) {
    Slot& s = slots_[index];
    if (s.kind == Kind::Free || s.epoch != epoch) { delete d; return; }   // released while on its way
    s.inFlight = false;
    s.status = status;
    // A failed refresh keeps the last good document, as a cache serves stale while it revalidates.
    if (d) {
        delete s.doc;
        s.doc = d;
        s.generation++;
    } else if (s.kind == Kind::Reply) {
        s.generation++;   // a reply that is no JSON still answered
    }
    if (s.kind == Kind::Reply && s.waiting) {
        char* next = s.waiting;
        s.waiting = nullptr;
        dispatch(index, next);
    }
}

void JsonFetch::applied(uintptr_t tag, const StateDocumentResult& result) {
    JsonFetch& f = instance();
    f.busy_.fetch_sub(1, std::memory_order_relaxed);
    const auto index = static_cast<uint8_t>(tag >> 16);
    const auto epoch = static_cast<uint16_t>(tag & 0xFFFF);
    if (index >= kMaxDocs) return;
    // The answer PATCH /api/state gives, so a script reads a push to itself as it reads one to another device.
    JsonSink sink;
    writeStateResult(sink, result);
    auto* d = new (std::nothrow) json::JsonDoc;
    if (d && !json::parse(sink.data(), *d)) { delete d; d = nullptr; }
    f.deliver(index, epoch, result.ok ? 200 : 400, d);
}

json::JsonDoc* JsonFetch::doc(const void* owner, int handle) {
    takeAnswers();
    Slot* s = slotFor(owner, handle);
    if (!s) return nullptr;
    if (s->kind == Kind::Feed) refreshIfDue(indexOf(handle));
    return s->doc;
}

bool JsonFetch::changed(const void* owner, int handle) {
    takeAnswers();
    Slot* s = slotFor(owner, handle);
    if (!s) return false;
    if (s->kind == Kind::Feed) refreshIfDue(indexOf(handle));
    const bool moved = s->generation != s->seen;
    s->seen = s->generation;
    return moved;
}

int JsonFetch::status(const void* owner, int handle) {
    takeAnswers();
    const Slot* s = slotFor(owner, handle);
    return s ? s->status : 0;
}

void JsonFetch::release(const void* owner) {
    takeAnswers();
    for (Slot& s : slots_)
        if (s.kind != Kind::Free && s.owner == owner) freeSlot(s);
    retireIfUnused();
}

bool JsonFetch::settle(uint32_t timeoutMs) {
    // Counted in waits rather than read from the clock, which a test may hold still.
    for (uint32_t waited = 0;; waited++) {
        takeAnswers();
        if (busy_.load(std::memory_order_relaxed) == 0) return true;
        if (waited >= timeoutMs) return false;
        platform::delayMs(1);
    }
}

JsonFetch::Answer JsonFetch::serve(const Job& j, char* answer) {
    // A post's answer is not read, so its size cannot refuse it; a fetch with no buffer to read into fails.
    const bool noBuffer = j.keep && !answer;
    const int status = noBuffer ? 0 : platform::httpFetch(j.method, j.url, j.body, kTimeoutMs,
                                                          j.keep ? answer : nullptr, j.keep ? kAnswerCap : 0);
    json::JsonDoc* d = nullptr;
    if (j.keep && status && answer[0]) {
        d = new (std::nothrow) json::JsonDoc;
        if (d && !json::parse(answer, *d)) { delete d; d = nullptr; }
    }
    platform::free(j.url);
    platform::free(j.body);
    return Answer{j.slot, j.epoch, status, d};
}

void JsonFetch::worker(void* self) {
    auto& f = *static_cast<JsonFetch*>(self);
    // One answer buffer for the task's life, in PSRAM where there is PSRAM.
    char* answer = static_cast<char*>(platform::alloc(kAnswerCap));
    while (!f.stop_.load(std::memory_order_relaxed)) {
        Job j;
        if (!f.jobs_.pop(&j, 1)) { platform::waitNotify(f.task_, 1000); continue; }
        const Answer a = serve(j, answer);
        // The ring holds every request the loop can have out, so this only waits while it takes the last ones in.
        while (f.answers_.push(&a, 1) != 1 && !f.stop_.load(std::memory_order_relaxed)) platform::delayMs(5);
    }
    platform::free(answer);
}

}  // namespace mm
