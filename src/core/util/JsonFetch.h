#pragma once

#include "core/module/StateDocument.h"   // what a push to this device answers
#include "core/util/JsonUtil.h"
#include "core/util/SpscRing.h"
#include "platform/platform.h"

#include <atomic>
#include <cstdint>

/// JSON documents held by handle: parsed from a script's own text, fetched from a URL and kept fresh, or answered by a push to this device or another.
///
/// What a script's `json` values are, and what a compiled module calls for the same reach.
namespace mm {

class JsonFetch {
public:
    /// How many documents the device holds at once, across every script.
    static constexpr uint8_t kMaxDocs = 16;
    /// The largest answer kept, refused beyond rather than cut: a weather or news answer is a few KB, and a scoreboard of hundreds is a mistake.
    static constexpr size_t kAnswerCap = platform::hasPsram ? 16 * 1024 : 4 * 1024;
    /// How long one request may take, connect and handshake included.
    static constexpr uint32_t kTimeoutMs = 10000;
    /// The worker's stack: the measured peak, a TLS handshake through a P-384 chain at about 4.4 KB, with room for what that run did not reach.
    static constexpr size_t kWorkerStack = 6 * 1024;

    /// The one table every script shares.
    static JsonFetch& instance() MM_NONBLOCKING { return instance_; }

    /// A document parsed from `text` while its program lives; the same text answers the same handle, so a script may ask every tick. Zero when it is not JSON or the table is full.
    int literal(const void* owner, const char* text);
    /// A document fetched from `url`, answered with the last good one and fetched again when read after `periodMs`, as an HTTP cache revalidates; 0 when the table is full.
    int fetch(const void* owner, const char* url, uint32_t periodMs);
    /// Send the document behind `handle` to `target` and answer a handle to the reply. Null or empty is this device, which applies it between frames; a bare host is another MoonLight's state, and a URL is any REST endpoint.
    int push(const void* owner, const char* target, int handle);
    /// POST `body` to `url`, keeping only the status, for a caller with nothing to read in the answer. A handle to the reply, or 0 when it could not go.
    int post(const void* owner, const char* url, const char* body);
    /// How the push or post behind `handle` went, as of the answers the loop last took in. It reads -1 while on its way, else the HTTP status, 0 when nothing was reached.
    int outcome(const void* owner, int handle) const MM_NONBLOCKING;

    /// The document behind a handle, or null while none has arrived.
    json::JsonDoc* doc(const void* owner, int handle);
    /// Whether a new document arrived since this was last asked.
    bool changed(const void* owner, int handle);
    /// The HTTP status of the last answer, 0 while none came.
    int status(const void* owner, int handle);

    /// Free every document `owner` holds, as its program goes.
    void release(const void* owner);

    /// Wait until every request is answered and taken in, for a test; false on timeout.
    bool settle(uint32_t timeoutMs);

    /// Once a second from the loop: take the answers in, and stop the worker when nothing uses it, so a device running no JSON script carries no stack for it.
    static void poll();
    /// Whether the worker task runs, for a test.
    bool workerRunning() const { return started_; }

    /// The worker stops and every document goes.
    ~JsonFetch();

private:
    enum class Kind : uint8_t { Free, Literal, Feed, Reply };
    struct Slot {
        Kind kind = Kind::Free;
        const void* owner = nullptr;
        const char* key = nullptr;     ///< a literal's text, which the program owns
        char* target = nullptr;        ///< a feed's URL or a reply's target, owned here
        char* waiting = nullptr;       ///< the newest push to a busy target, sent when its answer comes
        json::JsonDoc* doc = nullptr;
        uint32_t periodMs = 0;
        uint32_t dueAt = 0;
        uint32_t generation = 0;       ///< documents arrived
        uint32_t seen = 0;             ///< the generation `changed` last answered for
        int status = 0;
        uint16_t epoch = 0;            ///< bumped on release, so a late answer for a freed slot is dropped
        bool inFlight = false;
        bool keepAnswer = true;        ///< false for a post, whose caller reads the status alone
    };
    struct Job { uint8_t slot; uint16_t epoch; const char* method; char* url; char* body; bool keep; };
    struct Answer { uint8_t slot; uint16_t epoch; int status; json::JsonDoc* doc; };

    const Slot* slotFor(const void* owner, int handle) const MM_NONBLOCKING;
    Slot* slotFor(const void* owner, int handle) {
        return const_cast<Slot*>(static_cast<const JsonFetch*>(this)->slotFor(owner, handle));
    }
    /// A slot's handle, four bytes as a script slot is: its place plus one in the low byte, its generation above, so one kept past a release names nothing.
    int handleOf(int index) const { return (static_cast<int>(slots_[index].epoch) << 8) | (index + 1); }
    /// The place a handle names, or -1.
    static int indexOf(int handle) MM_NONBLOCKING {
        const int index = (handle & 0xFF) - 1;
        return handle > 0 && index >= 0 && index < kMaxDocs ? index : -1;
    }
    int claim(const void* owner, Kind kind);
    void freeSlot(Slot& s);
    void refreshIfDue(int index);
    bool send(int index, const char* method, const char* url, char* body);
    void dispatch(int index, char* body);
    void lost(int index);
    int replySlot(const void* owner, const char* target);
    /// Send a push, or let it wait while one to its target is on its way, replacing any that waited: the coalescing a UI does for its redraws.
    void hand(int index, char* body);
    /// Stop the worker once no feed is held and nothing is on its way; the stop is the loop's, made while the worker waits, so the join is immediate.
    void retireIfUnused();
    void takeAnswers();
    void deliver(uint8_t index, uint16_t epoch, int status, json::JsonDoc* d);
    static void applied(uintptr_t tag, const StateDocumentResult& result);
    static Answer serve(const Job& j, char* answer);
    /// The worker task, since a DNS lookup and a TLS handshake take seconds: requests come in through one ring and parsed answers go back through another.
    static void worker(void* self);

    static JsonFetch instance_;
    Slot slots_[kMaxDocs];
    SpscRing<Job, 8> jobs_;
    SpscRing<Answer, 8> answers_;
    platform::WorkerTask task_;
    bool started_ = false;
    std::atomic<uint32_t> generation_{0};   ///< bumped by each stop, so a worker a timed-out join left running ends rather than serving beside the next
    std::atomic<uint32_t> busy_{0};   ///< requests sent whose answers the loop has still to take in
};

}  // namespace mm
