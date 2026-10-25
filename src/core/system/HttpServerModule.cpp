/// The request handlers behind HttpServerModule: connections, routes, the REST answers and every send.
///
/// Public surface and class layout live in HttpServerModule.h, so implementation edits do not cascade-recompile every TU including the header. The state wire lives in HttpServerState.cpp and the WLED shim in HttpServerWled.cpp.

#include "core/system/HttpServerModule.h"

#include "core/module/Scheduler.h"
#include "core/module/StateDocument.h"   // PATCH /api/state applies a document through it
#include "core/util/hex.h"
#include "core/util/ModuleFactory.h"
#include "core/util/JsonUtil.h"
#include "core/util/parse.h"
#include "core/util/JsonSink.h"
#include "core/util/format.h"            // formatTo: nonblocking formatting into a fixed buffer
#include "core/util/CaptivePortal.h"     // the redirect a phone on the access point follows to the UI
#include "core/util/fnv.h"               // fnv1a: the diff-on-the-wire cache's digest
#include "core/system/Sha1.h"
#include "core/system/Base64.h"
#include "core/system/FilesystemModule.h"
#include "core/system/FirmwareUpdateModule.h"
#include "core/system/SystemModule.h"      // deviceName() for the WLED /json/info shim
#include "core/moonlive/MoonLiveScriptFile.h"   // kFactoryScriptDir: where a download lands
#include "core/moonlive/script_catalog.h"        // generated: which factory scripts exist
#include "core/util/build_info.h"                      // kVersion: the tag a script is fetched from
#include "platform/platform.h"
#include "ui/ui_embedded.h"

#include <algorithm>   // std::min: a stream's span within its budget
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>   // strtol: bounded Content-Length parse
#include <cctype>    // tolower, case-insensitive header names (findHeaderCI)
#include <cerrno>    // errno / ERANGE: Content-Length overflow check
#include <cstring>
#include <cstdint>
#include <new>       // placement new: a send's source built in its heap block

namespace mm {


// The UI file named `name`, from the table embed_ui.cmake generates, or null.
static const ui::EmbeddedFile* embeddedFile(const char* name) {
    for (const ui::EmbeddedFile& e : ui::kFiles)
        if (std::strcmp(name, e.name) == 0) return &e;
    return nullptr;
}

void HttpServerModule::defineControls() {
    controls_.addControl("port", port);
}

void HttpServerModule::setup() {
    instance_ = this;
    if (server_.open(port)) {
        boundPort_ = port;   // the port actually serving, frozen until release; the `port` control applies at the next open
    } else {
        boundPort_ = 0;
        std::printf("HTTP server failed to open port %u\n", port);
    }
    // A schema change carries hidden flags and option sets the value-patch cannot, so any module's rebuildControls() flips the WS full-resync flag through this hook.
    MoonModule::setSchemaChangedHook(&HttpServerModule::onSchemaChanged);
    // A module whose values someone watches move, a desk's motors, asks for them ahead of the second.
    MoonModule::setValuesChangedHook(&HttpServerModule::onValuesChanged);
}

void HttpServerModule::release() {
    // Drop the in-flight sends before the clients go: the preview frame borrows its buffer, a state stream owns its fragment.
    cancelBufferedSend();
    for (auto& cs : controlSends_) endSendSlot(cs);
    for (int i = 0; i < kMaxWsClients; i++) dropCarry(i);
    endValueWalk();
    for (auto& ws : wsClients_) ws.close();
    for (auto& c : parked_) c.close();
    releaseTransfers();
    for (auto& a : sends_) { endSendSlot(a); a.conn.close(); }
    // Every close site notifies the producer (see cancelBufferedSend), else ghost standing requests keep the driver gathering frames for nobody.
    for (int i = 0; i < kMaxPreviewClients; i++) {
        if (previewClients_[i].valid() && clientSink_) clientSink_->onClientGone(i);
        previewClients_[i].close();
    }
    server_.close();
    boundPort_ = 0;
    if (instance_ == this) {
        MoonModule::setSchemaChangedHook(nullptr);
        MoonModule::setValuesChangedHook(nullptr);
        instance_ = nullptr;
    }
    MoonModule::release();   // chain: uniform override-and-chain (no buffers or children, but the convention holds)
}

void HttpServerModule::tick20ms() MM_NONBLOCKING {
    // Drain every send on the 20 ms transport cadence, before accept so a connection burst cannot starve it; sockets stay off the render tick (architecture.md § Parallelism).
    drainSends();
    // Inbound WS frames before the push, so the subscription a page sends on open shapes its first document: control writes, subscriptions, and the WLED app's {on,bri}.
    pollUplinks();
    // A pending full resync is served here, not on the 1 s tick, so a connect reaches its first preview within tens of ms; the flag gates the expensive serialize.
    if (fullResyncPending_ || anyClientResync() || soonCount_) {
        const SecretsHidden hide(anyWsClientOnAccessPoint());   // one patch serves every client; a stream hides per client
        if (fullResyncPending_ || anyClientResync()) pushStateToWebSockets();
        // A module that asked goes out now rather than on the next second.
        if (soonCount_) pushSoonPatch();
    }
    advanceValueWalk();
    serveConnections();
    driveTransfers();
}

// One entry from the tick, so the connection work, which blocks by nature, is one call on the render path.
void HttpServerModule::serveConnections() {
    // Accept a bounded batch per tick, since a page load opens ~8 connections at once and one per tick lets the backlog fill and drop the WS.
    constexpr int kAcceptsPerTick = 8;
    // Bound the batch by wall clock too, since a stalled peer can burn TcpConnection::write's 8 s ceiling per connection and stack past the task WDT.
    constexpr uint32_t kAcceptBudgetMs = 100;
    const uint32_t batchStart = platform::millis();
    serveParked();
    for (int i = 0; i < kAcceptsPerTick; i++) {
        auto conn = server_.accept();
        if (!conn.valid()) break;   // backlog drained (the usual case: 0 or 1 pending)
        if (!handleConnection(conn)) park(conn);
        if (platform::millis() - batchStart >= kAcceptBudgetMs) break;   // a slow client stalled the batch
    }
}

// A request whose first packet was lost arrives a resend later, hundreds of milliseconds after the connection opened, so it waits here instead of being dropped.
void HttpServerModule::park(platform::TcpConnection& conn) {
    for (int i = 0; i < kParkedSlots; i++) {
        if (parked_[i].valid()) continue;
        parked_[i] = std::move(conn);
        parkedAt_[i] = platform::millis();
        return;
    }
    conn.close();   // every slot taken: dropped, as an unparked one always was
}

// Each parked connection gets one read without waiting: served once its request is in, closed once kParkMs has passed without one.
void HttpServerModule::serveParked() {
    const uint32_t now = platform::millis();
    for (int i = 0; i < kParkedSlots; i++) {
        if (!parked_[i].valid()) continue;
        if (handleConnection(parked_[i], 0)) parked_[i].close();
        else if (now - parkedAt_[i] >= kParkMs) parked_[i].close();
    }
}

// The buffer lives from claim to finishTransfer, so idle slots cost nothing; internal RAM, since the SD host bounces a PSRAM buffer per sector.
HttpServerModule::FileTransfer* HttpServerModule::claimTransfer(platform::TcpConnection& conn, char* buf) {
    for (auto& t : transfers_) {
        if (t.active) continue;
        t.buf = buf ? buf : static_cast<char*>(platform::allocInternal(kTransferBufBytes + kUploadPathBytes));
        if (t.buf) return &t;
        sendResponse(conn, 507, "application/json", "{\"error\":\"out of memory\"}");
        return nullptr;
    }
    // Both slots busy: answered rather than blocking for one to free.
    sendResponse(conn, 503, "application/json", "{\"error\":\"another transfer is in progress\"}");
    return nullptr;
}

void HttpServerModule::finishTransfer(FileTransfer& t, bool closeConn) {
    if (closeConn) t.conn.close();
    t.active = false;
    t.waiting = false;
    t.wstream = platform::FsWriteStream{};
    if (t.buf) { platform::free(t.buf); t.buf = nullptr; }
}

// release()'s own cleanup: an active slot holds a file handle, a socket and a buffer, none freed by going out of scope.
void HttpServerModule::releaseTransfers() {
    for (auto& t : transfers_) {
        if (!t.active) continue;
        if (t.kind != FileTransfer::Kind::Request) abandonUpload(t);
        finishTransfer(t, true);
    }
}

void HttpServerModule::abandonUpload(FileTransfer& t) {
    if (t.kind == FileTransfer::Kind::Firmware) platform::otaEnd(t.ota, false);
    else platform::fsWriteStreamClose(t.wstream, false);
}

// An upload slot's bounded chunk, socket to storage or the firmware writer, then the rate guard. It uses t.buf, not a stack-local: a 4 KB frame here once panicked the board.
bool HttpServerModule::driveUpload(FileTransfer& t, uint32_t now) {
    if (t.remaining > 0) {
        const size_t want = t.remaining < kTransferBufBytes ? t.remaining : kTransferBufBytes;
        const int n = t.conn.read(reinterpret_cast<uint8_t*>(t.buf), want);
        if (n > 0) {
            if (t.kind == FileTransfer::Kind::Firmware) {
                // A refused image is read to its end and dropped, so the answer reaches a sender still sending rather than a reset.
                if (!t.refused && !platform::otaWrite(t.ota, reinterpret_cast<const uint8_t*>(t.buf), static_cast<size_t>(n))) t.refused = true;
            } else if (!platform::fsWriteStreamWrite(t.wstream, t.buf, static_cast<size_t>(n))) {
                platform::fsWriteStreamClose(t.wstream, false);
                sendResponse(t.conn, 500, "application/json", "{\"error\":\"write failed\"}");
                finishTransfer(t, true);
                return false;
            }
            t.remaining -= static_cast<size_t>(n);
            moved(t, static_cast<size_t>(n));
            return true;
        }
        if (n == 0) {   // peer closed with body remaining
            abandonUpload(t);
            finishTransfer(t, true);   // the peer is already gone: no response to send
            return false;
        }
    }
    if (t.active && t.remaining == 0 && t.kind == FileTransfer::Kind::Firmware) {
        const bool ok = !t.refused && platform::otaEnd(t.ota, true);
        answerFirmware(t.conn, ok, t.target);   // the application's success reboots here
        finishTransfer(t, true);
        return false;
    }
    if (t.active && t.remaining == 0) {
        if (sendingFile(uploadPath(t))) {   // committed once its readers are done, retried every tick
            if (!t.waiting) { t.waiting = true; t.waitSinceMs = now; }
            if (now - t.waitSinceMs < kTransferStallMs) return false;
            // A reader that holds the file a whole stall window: the upload gives up, so its client retries rather than waits on it.
            abandonUpload(t);
            sendResponse(t.conn, 503, "application/json", "{\"error\":\"the file is being read, retry\"}");
            finishTransfer(t, true);
            return false;
        }
        const bool ok = platform::fsWriteStreamClose(t.wstream, true);
        if (ok) applyFileChanged(uploadPath(t));   // the write succeeded, so what was built from it may be stale
        sendResponse(t.conn, ok ? 200 : 500, "application/json",
                    ok ? "{\"ok\":true}" : "{\"error\":\"write failed\"}");
        finishTransfer(t, true);
        return false;
    }
    // A rate guard, not a size or time cap on the transfer itself: only a peer that stalled or trickles trips it.
    if (t.active && tooSlow(t, now)) {
        abandonUpload(t);
        finishTransfer(t, true);
    }
    return false;
}

bool HttpServerModule::sendingFile(const char* path) const {
    for (const auto& s : sends_)
        if (s.busy && s.source == Send::Source::File && s.block && std::strcmp(static_cast<const FileSource*>(s.block)->path, path) == 0) return true;
    return false;
}

bool HttpServerModule::uploadWaiting(const char* path) {
    for (auto& t : transfers_)
        if (t.active && t.kind == FileTransfer::Kind::Upload && t.remaining == 0 && std::strcmp(uploadPath(t), path) == 0) return true;
    return false;
}

bool HttpServerModule::tooSlow(FileTransfer& t, uint32_t now) {
    if (now - t.windowStartMs < kTransferStallMs) return false;
    // A body with less than the minimum left owes only what it has.
    const size_t owed = std::min(kMinBodyBytesPerWindow, t.remaining + t.windowBytes);
    const bool slow = t.windowBytes < owed;
    t.windowStartMs = now;
    t.windowBytes = 0;
    return slow;
}

// A request slot's bounded chunk: whatever of the body has arrived, then the request routed once whole. A body held back by a delayed ACK or a resend costs the sender time, never a frame.
bool HttpServerModule::driveRequest(FileTransfer& t, uint32_t now) {
    if (t.remaining > 0) {
        const int n = t.conn.read(reinterpret_cast<uint8_t*>(t.buf + t.bufLen), t.remaining);
        if (n > 0) {
            t.bufLen += static_cast<size_t>(n);
            t.remaining -= static_cast<size_t>(n);
            moved(t, static_cast<size_t>(n));
            return true;
        }
        if (n == 0) { finishTransfer(t, true); return false; }   // peer closed with body remaining: no one to answer
        if (tooSlow(t, now)) {
            sendResponse(t.conn, 400, "application/json", "{\"error\":\"incomplete request body\"}");
            finishTransfer(t, true);
        }
        return false;
    }
    t.buf[t.bufLen] = 0;
    routeRequest(t.conn, t.buf, t.bufLen);
    finishTransfer(t, true);
    return false;
}

// Several bounded chunks per slot per tick, not one, dispatched to the upload or request leg while storage and the peer keep up; drainSends' non-blocking shape, for what comes in.
void HttpServerModule::driveTransfers() {
    const uint32_t budgetStart = platform::millis();
    for (auto& t : transfers_) {
        if (!t.active) continue;
        for (;;) {
            const uint32_t now = platform::millis();
            const bool progressed = t.kind == FileTransfer::Kind::Request ? driveRequest(t, now) : driveUpload(t, now);
            if (!t.active) break;           // finished or failed this chunk
            if (!progressed) break;         // no data moved: nothing more to do until next tick
            if (now - budgetStart >= kTransferTickBudgetMs) break;
        }
    }
}

void HttpServerModule::tick1s() MM_NONBLOCKING {
    const SecretsHidden hide(anyWsClientOnAccessPoint());   // one push serves every client
    pushStateToWebSockets();
}

bool HttpServerModule::onAccessPoint(const platform::TcpConnection& conn) {
    uint8_t peer[4] = {};
    return conn.peerIPv4(peer) && captive::fromAccessPoint(peer);
}

bool HttpServerModule::anyWsClientOnAccessPoint() const MM_NONBLOCKING {
    for (int i = 0; i < kMaxWsClients; i++)
        if (wsOnAccessPoint_[i] && wsClients_[i].valid()) return true;
    return false;
}

// Whether the query holds the parameter `name=value` whole, as a query parameter is read: `xlive=1` and `live=10` are other parameters.
static bool queryHas(const char* query, const char* name, const char* value) {
    const size_t n = std::strlen(name), v = std::strlen(value);
    for (const char* p = query; p && *p;) {
        const char* end = std::strchr(p, '&');
        const size_t len = end ? static_cast<size_t>(end - p) : std::strlen(p);
        if (len == n + 1 + v && std::strncmp(p, name, n) == 0 && p[n] == '=' && std::strncmp(p + n + 1, value, v) == 0) return true;
        p = end ? end + 1 : nullptr;
    }
    return false;
}

// Whether the request line is `prefix`, the method and the state path, ending there or going on with a query.
static bool isStatePath(const char* req, const char* prefix) {
    const size_t n = std::strlen(prefix);
    return std::strncmp(req, prefix, n) == 0 && (req[n] == ' ' || req[n] == '?');
}

// A cheap scan of the query for a "/sd" path, which only sizes the Content-Length ceiling; parseFilePath is the real guard, downstream.
static bool requestNamesSdPath(const char* req) {
    const char* query = std::strchr(req, '?');
    const char* lineEnd = std::strstr(req, "\r\n");
    if (!query || (lineEnd && query > lineEnd)) return false;
    const char* queryEnd = std::strchr(query, ' ');
    char line[160];
    const size_t len = static_cast<size_t>((queryEnd ? queryEnd : query + std::strlen(query)) - query - 1);
    if (len >= sizeof(line)) return false;
    std::memcpy(line, query + 1, len);
    line[len] = '\0';
    char path[160];
    return HttpServerModule::parseFilePath(line, path, sizeof(path)) && platform::isSdPath(path);
}

// A module name from its path segment, percent-decoded (a space arrives as %20) up to `/` or `?`, and never '+' as a space, which keeps "A+B" reachable; its length.
static size_t decodeModuleName(const char* name, char* out, size_t outLen) {
    size_t i = 0;
    for (const char* p = name; *p && *p != '/' && *p != '?' && i + 1 < outLen; p++) {
        const int hi = *p == '%' && p[1] ? hexDigit(p[1]) : -1, lo = hi >= 0 && p[2] ? hexDigit(p[2]) : -1;
        if (lo >= 0) { out[i++] = static_cast<char>((hi << 4) | lo); p += 2; }
        else out[i++] = *p;
    }
    out[i] = 0;
    return i;
}

// The module name a /api/modules/<name>/... route carries, decoded; false when it does not fit a module's name, refused rather than truncated, since a truncated name could match another module.
static bool routeModuleName(const char* segment, char (&out)[MoonModule::kNameLen]) {
    char decoded[3 * MoonModule::kNameLen] = {};   // a percent-encoded byte takes three characters of the segment
    if (std::strcspn(segment, "/?") >= sizeof(decoded)) return false;
    const size_t n = decodeModuleName(segment, decoded, sizeof(decoded));
    if (n >= sizeof(out)) return false;
    std::memcpy(out, decoded, n + 1);
    return true;
}

// The uploads that stream their body to storage as it comes, never held whole: a file, a firmware image and a MoonBase image.
static bool isStreamingUpload(const char* req) {
    return std::strncmp(req, "POST /api/file", 14) == 0 ||
           std::strncmp(req, "POST /api/firmware/upload", 25) == 0 ||
           // The trailing space keeps `moonbase-update-url`, a small JSON body read whole, out of the match.
           std::strncmp(req, "POST /api/firmware/moonbase-update ", 35) == 0;
}

bool HttpServerModule::handleConnection(platform::TcpConnection& conn, int patienceMs) {
    uint8_t buf[2048];
    int totalRead = 0;

    // read() is non-blocking, so a newly accepted connection gets a short bounded wait for its request: five polls 1 ms apart, a yield each on a 100 Hz FreeRTOS tick. An idle connection costs at most that.
    for (int empties = 0; totalRead < static_cast<int>(sizeof(buf) - 1);) {
        int n = conn.read(buf + totalRead, sizeof(buf) - 1 - totalRead);
        if (n > 0) {
            totalRead += n;
            buf[totalRead] = 0;
            if (std::strstr(reinterpret_cast<char*>(buf), "\r\n\r\n")) break;
            empties = 0;                 // got data: reset the patience counter
        } else if (n == 0) {
            return true;                      // peer closed
        } else {                          // -1 = nothing pending yet
            if (totalRead > 0) break;    // had a partial then nothing more: process it
            if (++empties > patienceMs) break;   // no bytes within the patience: the caller decides
            platform::delayMs(1);
        }
    }

    if (totalRead == 0) return false;   // nothing arrived: the caller parks it rather than dropping it
    buf[totalRead] = 0;
    auto* req = reinterpret_cast<char*>(buf);

    auto* headerEnd = std::strstr(req, "\r\n\r\n");
    const char* clh = headerEnd ? findHeaderCI(req, "Content-Length:") : nullptr;
    if (clh) {
        // An /sd upload rides FAT32's own ceiling, 4 GiB-1 (UINT32_MAX), via strtoull not strtoul: `unsigned long` is only 32-bit on this RV32 target, the same width as that ceiling itself.
        const bool sdUpload = std::strncmp(req, "POST /api/file", 14) == 0 && requestNamesSdPath(req);
        const unsigned long long kContentLenMax = sdUpload ? 0xFFFFFFFFULL : 8ULL * 1024 * 1024;
        const char* valStart = clh + 15;
        while (*valStart == ' ' || *valStart == '\t') valStart++;   // skip OWS after the colon
        char* valEnd = nullptr;
        errno = 0;
        const unsigned long long parsed = std::strtoull(valStart, &valEnd, 10);
        const bool consumedDigits = valEnd != valStart;
        const bool endsCleanly = *valEnd == '\r' || *valEnd == '\n' || *valEnd == ' ' ||
                                 *valEnd == '\t' || *valEnd == '\0';
        if (!consumedDigits || !endsCleanly || errno == ERANGE) {
            sendResponse(conn, 400, "application/json",
                         "{\"error\":\"invalid content-length\"}");
            return true;
        }
        // A well-formed number past the ceiling is too large, not malformed: 413, not 400.
        if (parsed > kContentLenMax) {
            sendResponse(conn, 413, "application/json",
                         "{\"error\":\"request body too large\"}");
            return true;
        }
        if (!isStreamingUpload(req)) {
            const size_t headerSize = static_cast<size_t>(headerEnd + 4 - req);
            const size_t whole = headerSize + static_cast<size_t>(parsed);
            // A state document holds an effects stack and outgrows the request buffer; every other body is read whole within it, so an oversized one is 413, never a truncated parse.
            const bool state = isStatePath(req, "PATCH /api/state") || isStatePath(req, "POST /api/state");
            if (whole > (state ? headerSize + kStateDocumentMax : sizeof(buf) - 1)) {
                sendResponse(conn, 413, "application/json", state ? "{\"error\":\"document too large\"}" : "{\"error\":\"request body too large\"}");
                return true;
            }
            // A body not all in yet, held back by a delayed ACK or a resend, waits in a slot rather than in the frame, and is routed once whole.
            if (static_cast<size_t>(totalRead) < whole) {
                deferRequest(conn, req, static_cast<size_t>(totalRead), whole);
                return true;
            }
            req[whole] = 0;   // the body ends here: this server closes every connection, so nothing pipelined after it is read
            totalRead = static_cast<int>(whole);
        }
    }
    routeRequest(conn, req, static_cast<size_t>(totalRead));
    return true;
}

// A request whose body is still on its way: its text so far is copied into a buffer sized to the whole, which a slot completes.
void HttpServerModule::deferRequest(platform::TcpConnection& conn, const char* req, size_t got, size_t whole) {
    char* text = static_cast<char*>(platform::alloc(whole + 1));
    if (!text) { sendResponse(conn, 507, "application/json", "{\"error\":\"out of memory\"}"); return; }
    std::memcpy(text, req, got);
    FileTransfer* t = claimTransfer(conn, text);
    if (!t) { platform::free(text); return; }
    t->kind = FileTransfer::Kind::Request;
    t->bufLen = got;
    t->remaining = whole - got;
    t->windowStartMs = platform::millis();
    t->windowBytes = 0;
    t->active = true;
    t->conn = std::move(conn);
}

// Whether a request names a JavaScript module, the one kind of UI file the embedded bundle folds into app.js.
static bool isScriptName(const char* path) {
    const size_t n = std::strlen(path);
    return n > 3 && std::strcmp(path + n - 3, ".js") == 0;
}

// One request of `len` bytes, its body whole or, for an upload, its first part: the method and path pick the handler.
void HttpServerModule::routeRequest(platform::TcpConnection& conn, char* req, size_t len) {
    // A client on the device's own access point has not shown it knows the network's password, so it sees no stored one.
    const SecretsHidden hide(onAccessPoint(conn));
    uint8_t local[4] = {};
    conn.localIPv4(local);   // the address it reached, which the captive redirect decides by
    auto* headerEnd = std::strstr(req, "\r\n\r\n");
    const char* clh = headerEnd ? findHeaderCI(req, "Content-Length:") : nullptr;
    const bool hasContentLen = clh != nullptr;   // header PRESENT: an explicit 0 is a legitimate empty write
    const size_t contentLen = clh ? static_cast<size_t>(std::strtoull(clh + 15, nullptr, 10)) : 0;   // validated by handleConnection
    const int totalRead = static_cast<int>(len);   // a byte count, never strlen: an uploaded image holds zero bytes

    // Parse method and path, into members rather than locals: one request routes at a time, and on the stack they pushed this frame past 512 bytes.
    char (&method)[8] = routeMethod_;
    char (&path)[128] = routePath_;
    std::memset(method, 0, sizeof(method));
    std::memset(path, 0, sizeof(path));
    std::sscanf(req, "%7s %127s", method, path);
    // Strip the query string (RFC 3986 §3.4) before routing; the web installer's Inject button hands off `/?deviceModel=<name>` (see SystemModule.md).
    char* queryStart = std::strchr(path, '?');
    if (queryStart) *queryStart = 0;

    // WebSocket upgrade: `/ws` is the control plane (JSON state), `/wsp` the lossy binary preview, separate so a large preview frame cannot delay a state push.
    const bool isWs  = std::strcmp(path, "/ws") == 0;
    const bool isWsp = std::strcmp(path, "/wsp") == 0;
    if (std::strcmp(method, "GET") == 0 && (isWs || isWsp) &&
        findHeaderCI(req, "Upgrade: websocket")) {
        handleWebSocketUpgrade(conn, req, isWsp);
        return; // don't close: connection is now a WebSocket
    }

    // The body starts after the headers
    char* body = headerEnd ? const_cast<char*>(headerEnd) + 4 : nullptr;

    // A phone on the access point asking for its own captive-check page gets the UI, which is what shows it the sign-in screen.
    if (std::strcmp(method, "GET") == 0) {
        const char* host = findHeaderCI(req, "Host:");
        if (host) { host += 5; while (*host == ' ') host++; }
        if (captive::redirects(local, host, path)) {   // `local` was read when the connection came in
            conn.write(reinterpret_cast<const uint8_t*>(captive::kRedirect), std::strlen(captive::kRedirect));
            return;
        }
    }

    // Route
    if (std::strcmp(method, "GET") == 0) {
        const char* ifNoneMatch = findHeaderCI(req, "If-None-Match:");
        if (std::strcmp(path, "/") == 0) serveFile(conn, *embeddedFile("index.html"), ifNoneMatch);
        // Every UI file at `/<name>`, from the one list embed_ui.cmake generates.
        else if (const ui::EmbeddedFile* file = embeddedFile(path + 1)) serveFile(conn, *file, ifNoneMatch);
        else if (std::strcmp(path, "/api/state") == 0) serveState(conn);
        else if (std::strcmp(path, "/api/system") == 0) serveSystem(conn);
        else if (std::strcmp(path, "/api/types") == 0) serveTypes(conn, ifNoneMatch);
        // GET /api/scripts → the MoonLive factory catalog (names per role + the tag to fetch from).
        else if (std::strcmp(path, "/api/scripts") == 0) serveScriptCatalog(conn);
        // GET /api/modules/<name> → that module's JSON as /api/state carries it, for issue reports; /document → the module as a state document.
        else if (std::strncmp(path, "/api/modules/", 13) == 0) serveModule(conn, path + 13);
        // File Manager: GET /api/dir?path=<rel>[&hidden=1] → one directory's children as JSON [{name,isDir,size}] (the lazy tree loads a node's children on expand).
        else if (std::strcmp(path, "/api/dir") == 0) serveDirListing(conn, queryStart ? queryStart + 1 : "");
        // File Manager: GET /api/file?path=<rel> → the file's contents (text, size-capped).
        else if (std::strcmp(path, "/api/file") == 0) serveFileContents(conn, queryStart ? queryStart + 1 : "");
        // HLS: GET /hls/<file> → the segments the HlsDriver's ffmpeg writes under /.hls/, with video MIME types and no-cache (the playlist mutates every second).
        else if (std::strncmp(path, "/hls/", 5) == 0) serveHlsFile(conn, path + 5);
        // WLED shim: the WLED apps and Home Assistant discover a device via mDNS `_wled._tcp` and validate it by GETting /json/info, so a minimal WLED-shaped info makes MoonLight appear there.
        else if (std::strcmp(path, "/json/info") == 0) serveWledInfo(conn);
        // WLED state and the combined state+info (`/json/si`) for the app's device card: on/off, brightness and the segment's primary color as the card tint.
        else if (std::strcmp(path, "/json/state") == 0) serveWledState(conn);
        else if (std::strcmp(path, "/json/si") == 0) serveWledStateInfo(conn);
        // python-wled (HA) fetches the full `/json` and needs Info.fs, State.nl, State.udpn and State.lor, so serveWledDeviceJson writes that shape; /json/info and /json/state stay minimal.
        else if (std::strcmp(path, "/json") == 0) serveWledDeviceJson(conn);
        // /presets.json: python-wled fetches it after /json and a 404 aborts HA's config flow; the truthy-but-empty `{"0":{}}` yields zero presets where `{}` fails its `not presets` guard.
        else if (std::strcmp(path, "/presets.json") == 0) serveWledPresets(conn);
        // A desktop running from a checkout serves the UI's own modules, which the embedded bundle holds inside app.js: scripts only, so the folder's other files stay private.
        else if (isScriptName(path) && serveUiFromDisk(conn, path + 1)) {}
        else sendResponse(conn, 404, "text/plain", "Not found");
    } else if (std::strcmp(method, "POST") == 0) {
        // POST /api/modules/<name>/move with body {"to":N}. Strict-suffix check: path must end with "/move" exactly (rejects "/movex").
        const size_t pathLen = std::strlen(path);
        const bool isMoveRoute =
            std::strncmp(path, "/api/modules/", 13) == 0 &&
            pathLen > 18 &&
            std::strcmp(path + pathLen - 5, "/move") == 0;
        // POST /api/modules/<name>/replace with body {"type":"<TypeName>"}. Strict-suffix check, same as the move route.
        const bool isReplaceRoute =
            std::strncmp(path, "/api/modules/", 13) == 0 &&
            pathLen > 21 &&
            std::strcmp(path + pathLen - 8, "/replace") == 0;
        if (std::strcmp(path, "/api/control") == 0 && body) {
            handleSetControl(conn, body);
        } else if (std::strcmp(path, "/api/state") == 0 && body) {
            // POST for a client that cannot send PATCH: the same document, the same engine.
            if (!hasContentLen) { sendResponse(conn, 411, "application/json", "{\"error\":\"length required\"}"); return; }
            handleApplyState(conn, body, queryStart && queryHas(queryStart + 1, "live", "1"));
        } else if (std::strcmp(path, "/api/file") == 0 && body) {
            // File Manager: POST /api/file?path=<rel> streams the body to an atomic write; `body` holds the bytes that came with the headers, and a transfer slot pulls the rest off the socket.
            const size_t initialLen = static_cast<size_t>(totalRead) - static_cast<size_t>(body - req);
            // No Content-Length (chunked) is 411, since acting on it would commit an empty file; keyed on header absence, so an explicit `Content-Length: 0` stays a valid empty write.
            if (!hasContentLen) {
                sendResponse(conn, 411, "application/json", "{\"error\":\"length required\"}");
                return;
            }
            handleWriteFile(conn, queryStart ? queryStart + 1 : "", body, initialLen,
                            static_cast<size_t>(contentLen));
        } else if (std::strcmp(path, "/api/dir") == 0) {
            // File Manager: POST /api/dir?path=<rel> → mkdir; the path is the whole operation, so it rides the query like /api/file.
            handleMakeDir(conn, queryStart ? queryStart + 1 : "");
        } else if (std::strcmp(path, "/api/modules") == 0 && body) {
            handleAddModule(conn, body);
        } else if (std::strncmp(path, "/api/list/", 10) == 0 && pathLen > 16 && std::strcmp(path + pathLen - 6, "/apply") == 0) {
            // A row's action as a POST on its sub-resource, the custom-method shape REST design guides give an action: POST /api/list/<module>/<control>/<row>/apply.
            handleListApplyRow(conn, path + 10, pathLen - 10 - 6);
        } else if (std::strncmp(path, "/api/list/", 10) == 0) {
            // Editable list: POST /api/list/<module>/<control> appends a new row and returns its stable id. The row's fields are then set via PATCH /api/list/.../<id>.
            handleListAddRow(conn, path + 10);
        } else if ((isMoveRoute || isReplaceRoute) && body) {
            char nameBuf[MoonModule::kNameLen] = {};
            if (!routeModuleName(path + 13, nameBuf)) sendResponse(conn, 400, "application/json", "{\"error\":\"module name too long\"}");
            else if (isMoveRoute) handleMoveModule(conn, nameBuf, body);
            else handleReplaceModule(conn, nameBuf, body);
        } else if (std::strcmp(path, "/json/state") == 0 && body) {
            // WLED-compatibility: the native WLED app POSTs {on,bri,…} here; we map it onto the Drivers brightness control.
            handleWledState(conn, body);
        } else if (std::strcmp(path, "/api/reboot") == 0) {
            handleReboot(conn);
        } else if (std::strcmp(path, "/api/firmware/url") == 0 && body) {
            handleFirmwareUrl(conn, body);
        } else if (std::strcmp(path, "/api/firmware/moonbase") == 0) {
            // Reboot into MoonBase with nothing staged, for install-from-file: the browser re-POSTs the image to MoonBase once it answers.
            handleBootMoonBase(conn);
        } else if (std::strcmp(path, "/api/firmware/moonbase-update-url") == 0 && body) {
            // Install a new MoonBase the device fetches itself, so a release asset comes straight from GitHub.
            handleMoonBaseUrl(conn, body);
        } else if (std::strcmp(path, "/api/firmware/moonbase-update") == 0 && body) {
            // Install a new MOONBASE from an uploaded image: the one direction the other firmware routes cannot go, since only the running app may write the factory slot.
            const size_t initialLen = static_cast<size_t>(totalRead) - static_cast<size_t>(body - req);
            if (!hasContentLen) {
                sendResponse(conn, 411, "application/json", "{\"error\":\"length required\"}");
                return;
            }
            handleFirmwareUpload(conn, platform::OtaTarget::MoonBase, body, initialLen, static_cast<size_t>(contentLen));
        } else if (std::strcmp(path, "/api/firmware/upload") == 0 && body) {
            // OTA from an uploaded .bin body: the browser POSTs the image to the device, which streams it into the OTA partition like /api/file.
            const size_t initialLen = static_cast<size_t>(totalRead) - static_cast<size_t>(body - req);
            if (!hasContentLen) { sendResponse(conn, 411, "application/json", "{\"error\":\"length required\"}"); return; }   // as every upload route
            handleFirmwareUpload(conn, platform::OtaTarget::App, body, initialLen, static_cast<size_t>(contentLen));
        } else {
            sendResponse(conn, 404, "text/plain", "Not found");
        }
    } else if (std::strcmp(method, "PATCH") == 0) {
        // Editable list: PATCH /api/list/<module>/<control>/<id> edits one row: a field ({"field":F,"value":V}) or a reorder ({"to":N}).
        if (std::strcmp(path, "/api/state") == 0 && body) {
            if (!hasContentLen) { sendResponse(conn, 411, "application/json", "{\"error\":\"length required\"}"); return; }
            handleApplyState(conn, body, queryStart && queryHas(queryStart + 1, "live", "1"));
        } else if (std::strncmp(path, "/api/list/", 10) == 0 && body) {
            handleListPatchRow(conn, path + 10, body);
        } else {
            sendResponse(conn, 404, "text/plain", "Not found");
        }
    } else if (std::strcmp(method, "DELETE") == 0) {
        // DELETE /api/modules/ModuleName
        if (std::strncmp(path, "/api/modules/", 13) == 0) {
            handleDeleteModule(conn, path + 13);
        } else if (std::strncmp(path, "/api/list/", 10) == 0) {
            // Editable list: DELETE /api/list/<module>/<control>/<id> removes one row.
            handleListDeleteRow(conn, path + 10);
        } else if (std::strcmp(path, "/api/dir") == 0) {
            // File Manager: DELETE /api/dir?path=<rel> removes a file or a whole folder tree.
            handleRemoveEntry(conn, queryStart ? queryStart + 1 : "");
        } else {
            sendResponse(conn, 404, "text/plain", "Not found");
        }
    } else if (std::strcmp(method, "OPTIONS") == 0) {
        // CORS preflight: a cross-origin POST with a JSON Content-Type sends OPTIONS first, and the reply is 204 for ANY path since the device's HTTP surface is tiny and LAN-only.
        sendPreflightResponse(conn);
    } else {
        sendResponse(conn, 405, "text/plain", "Method not allowed");
    }

    conn.close();
}

void HttpServerModule::sendPreflightResponse(platform::TcpConnection& conn) {
    // 204 No Content with the Access-Control-Allow-* headers; Max-Age caches the preflight for an hour.
    const char* response =
        "HTTP/1.1 204 No Content\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, PATCH, DELETE, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type\r\n"
        "Access-Control-Max-Age: 3600\r\n"
        "Connection: close\r\n"
        "\r\n";
    conn.write(reinterpret_cast<const uint8_t*>(response), std::strlen(response));
}

// The reason phrase for each status this server answers (RFC 9110 section 15); a code not listed reads "Error", which a client ignores.
static const char* reasonPhrase(int status) {
    static constexpr struct { int code; const char* text; } kReasons[] = {
        {200, "OK"}, {202, "Accepted"}, {400, "Bad Request"}, {403, "Forbidden"}, {404, "Not Found"},
        {405, "Method Not Allowed"}, {409, "Conflict"}, {411, "Length Required"}, {413, "Content Too Large"},
        {500, "Internal Server Error"}, {501, "Not Implemented"}, {503, "Service Unavailable"}, {507, "Insufficient Storage"},
    };
    for (const auto& r : kReasons) if (r.code == status) return r.text;
    return "Error";
}

void HttpServerModule::sendResponse(platform::TcpConnection& conn, int status, const char* contentType, const char* body) {
    const char* statusText = reasonPhrase(status);
    char header[256];
    int bodyLen = static_cast<int>(std::strlen(body));
    int headerLen = std::snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n",
        status, statusText, contentType, bodyLen);
    conn.write(reinterpret_cast<const uint8_t*>(header), headerLen);
    conn.write(reinterpret_cast<const uint8_t*>(body), bodyLen);
}

// The /api/file endpoints: a file body is not a control value; parseFilePath vets the `path=<rel>` query (rejects "..", roots at the mount), the one traversal guard every filesystem entry shares.

// Case-insensitive header lookup (MSVC has no strcasestr), matching only at the start of a header line and stopping at the blank line, so lookalikes and body bytes never match.
const char* HttpServerModule::findHeaderCI(const char* hay, const char* needle) {
    const size_t n = std::strlen(needle);
    const char* line = hay;
    while (line && *line) {
        if (line[0] == '\r' && line[1] == '\n') break;   // blank line: end of the header section
        size_t i = 0;
        while (i < n && std::tolower(static_cast<unsigned char>(line[i])) ==
                        std::tolower(static_cast<unsigned char>(needle[i]))) i++;
        if (i == n) return line;
        const char* nl = std::strchr(line, '\n');
        line = nl ? nl + 1 : nullptr;
    }
    return nullptr;
}

// Copy the `path=` query value into `out` (decoding %XX and '+'), rooted at the mount; false on a missing path or a ".." traversal.
bool HttpServerModule::parseFilePath(const char* query, char* out, size_t cap) {
    const char* p = query ? std::strstr(query, "path=") : nullptr;
    if (!p) return false;
    p += 5;                                   // past "path="
    size_t i = 0;
    // The path may be its own query (stop at '&') and percent-encoded ('/' → %2F, ' ' → %20).
    while (*p && *p != '&' && i + 1 < cap) {
        char c = *p;
        if (c == '%' && p[1] && p[2]) {       // %XX → byte
            const int hi = hexDigit(p[1]), lo = hexDigit(p[2]);
            if (hi >= 0 && lo >= 0) { c = static_cast<char>((hi << 4) | lo); p += 2; }
        } else if (c == '+') {
            c = ' ';
        }
        out[i++] = c;
        p++;
    }
    // Reject an overlong path rather than route on a truncated prefix: if the loop stopped because the buffer filled, the decoded value is incomplete.
    if (*p && *p != '&') return false;
    out[i] = 0;
    if (i == 0 || std::strstr(out, "..")) return false;   // empty or traversal → reject
    // One spelling per file: the filesystems read "//a" as "/a", so a repeated slash collapses and two spellings never pass as two files.
    size_t w = 0;
    for (size_t r = 0; out[r]; r++)
        if (out[r] != '/' || w == 0 || out[w - 1] != '/') out[w++] = out[r];
    out[w] = 0;
    if (out[0] != '/') {                                  // relative → root at the mount
        char rooted[160];
        const int n = std::snprintf(rooted, sizeof(rooted), "/%s", out);
        if (n <= 0 || static_cast<size_t>(n) >= cap) return false;
        std::strncpy(out, rooted, cap - 1); out[cap - 1] = 0;
    }
    return true;
}

// GET /api/dir: one directory's children as a JSON array, single level (platform::fsList) since the lazy tree fetches per expanded node, streamed like serveState; dotfiles only with `hidden=1`.
namespace {
struct DirListState {
    JsonSink* sink;
    bool showHidden;
    bool first = true;
};
void dirListTrampoline(const char* name, bool isDir, uint32_t size, void* user) {
    auto* st = static_cast<DirListState*>(user);
    if (!st->showHidden && name[0] == '.') return;          // dotfile convention
    if (!st->first) st->sink->append(",");
    st->first = false;
    st->sink->append("{\"name\":");
    st->sink->writeJsonString(name);
    st->sink->appendf(",\"isDir\":%s,\"size\":%lu}",
                      isDir ? "true" : "false", static_cast<unsigned long>(size));
}
}  // namespace

void HttpServerModule::serveDirListing(platform::TcpConnection& conn, const char* query) {
    char path[160];
    if (!parseFilePath(query, path, sizeof(path))) {
        sendResponse(conn, 400, "application/json", "{\"error\":\"bad path\"}");
        return;
    }
    const char* header =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n";
    conn.write(reinterpret_cast<const uint8_t*>(header), std::strlen(header));

    JsonSink sink(conn);
    DirListState st{&sink, query && std::strstr(query, "hidden=1") != nullptr, true};
    sink.append("[");
    platform::fsList(path, &dirListTrampoline, &st);
    sink.append("]");
    sink.flush();
}

// POST /api/dir?path=<rel> → mkdir; parseFilePath vets the path, and a create is a filesystem action, not a stored control.
void HttpServerModule::handleMakeDir(platform::TcpConnection& conn, const char* query) {
    char path[160];
    if (!parseFilePath(query, path, sizeof(path))) {
        sendResponse(conn, 400, "application/json", "{\"error\":\"bad path\"}");
        return;
    }
    if (platform::fsMkdir(path)) sendResponse(conn, 200, "application/json", "{\"ok\":true}");
    else sendResponse(conn, 500, "application/json", "{\"error\":\"mkdir failed\"}");
}

// DELETE /api/dir?path=<rel> → remove a file or a directory with everything in it, since fsRemove takes only an empty one; the File Manager's double-armed delete is the confirmation.
void HttpServerModule::handleRemoveEntry(platform::TcpConnection& conn, const char* query) {
    char path[160];
    if (!parseFilePath(query, path, sizeof(path))) {
        sendResponse(conn, 400, "application/json", "{\"error\":\"bad path\"}");
        return;
    }
    // A path that names nothing is the client's mistake, 404 as REST answers it, where a removal that fails is the device's.
    if (!platform::fsExists(path)) {
        sendResponse(conn, 404, "application/json", "{\"error\":\"not found\"}");
        return;
    }
    if (FilesystemModule::removeTree(path)) {
        // A removed file is a change to persistent state like a written one: a module derived from it must notice the file is gone.
        applyFileChanged(path);
        sendResponse(conn, 200, "application/json", "{\"ok\":true}");
    } else {
        sendResponse(conn, 500, "application/json", "{\"error\":\"delete failed\"}");
    }
}

// A download resumes in a send slot a chunk per tick, NUL-safe. The slot is claimed and the file opened before the header. A busy slot, a missing buffer or a failed open answers 503, 507 or 500, never a 200 with no body.
void HttpServerModule::streamFsFile(platform::TcpConnection& conn, const char* path,
                                    const char* mime, const char* extraHeaders) {
    // The saved config holds the WiFi passwords in plain text, which a client on the access point does not see.
    if (SecretsHidden::active() && std::strstr(path, "/.config")) {
        sendResponse(conn, 403, "application/json", "{\"error\":\"config files are not served through the access point\"}");
        return;
    }
    const int64_t size = platform::fsSize(path);
    if (size < 0) { sendResponse(conn, 404, "application/json", "{\"error\":\"not found\"}"); return; }
    if (uploadWaiting(path)) { sendResponse(conn, 503, "application/json", "{\"error\":\"the file is being replaced, retry\"}"); return; }
    char header[224];
    const int hn = std::snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %lld\r\n%s"
        "Connection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n", mime, static_cast<long long>(size), extraHeaders);

    Send* slot = claimSend(conn);
    if (!slot) return;
    // Internal RAM, since the SD host bounces a PSRAM buffer per sector.
    auto* f = static_cast<FileSource*>(platform::allocInternal(sizeof(FileSource)));
    if (!f) { sendResponse(conn, 507, "application/json", "{\"error\":\"out of memory\"}"); return; }
    new (f) FileSource;
    std::snprintf(f->path, sizeof(f->path), "%s", path);
    if (!platform::fsReadStreamOpen(f->stream, path)) {
        platform::free(f);
        sendResponse(conn, 500, "application/json", "{\"error\":\"read failed\"}");
        return;
    }
    if (!conn.write(reinterpret_cast<const uint8_t*>(header), static_cast<size_t>(hn))) {
        platform::fsReadStreamClose(f->stream);
        platform::free(f);
        return;
    }
    f->left = static_cast<size_t>(size);
    slot->block = f;
    slot->source = Send::Source::File;
    slot->conn = std::move(conn);
    slot->busy = true;
    slot->lastActivityMs = platform::millis();
}

void HttpServerModule::serveFileContents(platform::TcpConnection& conn, const char* query) {
    char path[160];
    if (!parseFilePath(query, path, sizeof(path))) {
        sendResponse(conn, 400, "application/json", "{\"error\":\"bad path\"}");
        return;
    }
    streamFsFile(conn, path, "text/plain", "");
}

// GET /hls/<name>: an HLS artifact from /.hls/, streamed like serveFileContents with video MIME types, no-cache (the set changes every second) and a flat-name guard (no '/', no '..').
void HttpServerModule::serveHlsFile(platform::TcpConnection& conn, const char* name) {
    if (!name[0] || std::strlen(name) > 80 || std::strchr(name, '/') || std::strstr(name, "..")) {
        sendResponse(conn, 400, "application/json", "{\"error\":\"bad name\"}");
        return;
    }
    char path[96];   // "/.hls/" + <=80 name fits; the length guard above keeps snprintf exact
    std::snprintf(path, sizeof(path), "/.hls/%s", name);
    const char* dot = std::strrchr(name, '.');
    const char* mime = "application/octet-stream";
    if (dot && std::strcmp(dot, ".m3u8") == 0) mime = "application/vnd.apple.mpegurl";
    else if (dot && std::strcmp(dot, ".ts") == 0) mime = "video/mp2t";
    else if (dot && (std::strcmp(dot, ".mp4") == 0 || std::strcmp(dot, ".m4s") == 0)) mime = "video/mp4";

    // RAM first, then the filesystem: a platform that keeps segments in memory (the P4) answers here, one whose encoder writes to disk (desktop ffmpeg) declines.
    const uint8_t* ram = nullptr;
    size_t ramLen = 0;
    if (platform::hlsSegment(name, &ram, &ramLen)) {
        // Copied out and released at once: the platform holds one segment for one reader, and the playlist lives in a static buffer the next request rewrites.
        auto* copy = static_cast<char*>(platform::alloc(ramLen ? ramLen : 1));
        if (copy) std::memcpy(copy, ram, ramLen);
        platform::hlsSegmentRelease();
        if (!copy) { sendResponse(conn, 507, "application/json", "{\"error\":\"out of memory\"}"); return; }
        char header[224];
        std::snprintf(header, sizeof(header),
            "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
            "Cache-Control: no-cache\r\nConnection: close\r\n"
            "Access-Control-Allow-Origin: *\r\n\r\n", mime, ramLen);
        answerBlock(conn, header, copy, ramLen);
        return;
    }
    streamFsFile(conn, path, mime, "Cache-Control: no-cache\r\n");
}

// The body is whole, its request ended at it by handleConnection or completed in a slot.
void HttpServerModule::handleApplyState(platform::TcpConnection& conn, const char* body, bool live) {
    if (!scheduler_) { sendResponse(conn, 503, "application/json", "{\"error\":\"not ready\"}"); return; }
    answerStateDocument(conn, body, live);
}

void HttpServerModule::answerStateDocument(platform::TcpConnection& conn, const char* doc, bool live) {
    // `?live=1` drives rather than configures, as a show building its look on another device does: nothing it changes is marked to save.
    const StateDocumentResult r = applyStateDocument(*scheduler_, doc, live ? StateSource::Live : StateSource::Request);
    // A growing sink, since a path of escaped names has no small fixed bound.
    JsonSink sink;
    writeStateResult(sink, r);
    sendResponse(conn, r.ok ? 200 : 400, "application/json", sink.overflowed() ? "{\"error\":\"out of memory\"}" : sink.data());
}

// Claims a slot, opens the stream, writes the body already buffered, and leaves the rest to driveTransfers; the response goes out once the file commits.
void HttpServerModule::startUpload(platform::TcpConnection& conn, const char* path,
                                   const char* initialBody, size_t initialLen, size_t contentLen) {
    FileTransfer* t = claimTransfer(conn);
    if (!t) return;
    if (!platform::fsWriteStreamOpen(t->wstream, path)) {
        finishTransfer(*t, false);
        sendResponse(conn, 500, "application/json", "{\"error\":\"write failed\"}");
        return;
    }
    // Hand the stream at most Content-Length of the buffered bytes, since a pipelined next request may follow the body and must not land in the file.
    const size_t initial = initialLen < contentLen ? initialLen : contentLen;
    if (initial > 0 && !platform::fsWriteStreamWrite(t->wstream, initialBody, initial)) {
        platform::fsWriteStreamClose(t->wstream, false);
        finishTransfer(*t, false);
        sendResponse(conn, 500, "application/json", "{\"error\":\"write failed\"}");
        return;
    }
    std::snprintf(uploadPath(*t), kUploadPathBytes, "%s", path);
    t->kind = FileTransfer::Kind::Upload;
    t->remaining = contentLen - initial;
    t->windowStartMs = platform::millis();
    t->windowBytes = 0;
    t->active = true;
    t->conn = std::move(conn);
}

static constexpr size_t kUploadMax = 256 * 1024;   // 256 KB: sanity bound on one upload to flash

void HttpServerModule::handleWriteFile(platform::TcpConnection& conn, const char* query,
                                       const char* initialBody, size_t initialLen, size_t contentLen) {
    char path[160];
    if (!parseFilePath(query, path, sizeof(path))) {
        sendResponse(conn, 400, "application/json", "{\"error\":\"bad path\"}");
        return;
    }
    // Reject up front if it would not fit, rather than failing mid-write; free is taken conservatively, with no credit for an overwrite.
    uint64_t total = 0, used = 0;
    if (platform::isSdPath(path)) {
        if (!platform::sdCardMounted()) {
            sendResponse(conn, 503, "application/json", "{\"error\":\"no card mounted\"}");
            return;
        }
        total = platform::sdCardTotal();
        used = platform::sdCardUsed();
    } else {
        if (contentLen > kUploadMax) {
            sendResponse(conn, 413, "application/json", "{\"error\":\"file too large\"}");
            return;
        }
        total = platform::filesystemTotal();
        used = platform::filesystemUsed();
    }
    const uint64_t freeBytes = total > used ? total - used : 0;
    if (total > 0 && contentLen > freeBytes) {
        char msg[96];
        std::snprintf(msg, sizeof(msg), "{\"error\":\"not enough space (%llu free)\"}",
                      static_cast<unsigned long long>(freeBytes));
        sendResponse(conn, 507, "application/json", msg);   // 507 Insufficient Storage
        return;
    }
    startUpload(conn, path, initialBody, initialLen, contentLen);
}

// See the header for WHY this exists and why it is whole-tree. Here is only the how.
void HttpServerModule::applyFileChanged(const char* path) {
    // A written /.config/<Type>.json applies live, queued for the render task: setup() here crashed the ESP32 and a network apply could cut the socket before the response.
    if (auto* fs = FilesystemModule::instance()) fs->requestConfigApply(path);
    // A user script of a name the firmware also ships is a fork; record its origin here, where every writer passes, so the binding can flag an updated shipped copy.
    moonlive::noteForkedFrom(path);
    // A script that landed leaves the missing list here, not at a compile that may never come.
    moonlive::noteScriptLanded(path);
    if (!scheduler_) return;
    scheduler_->notifyFileChanged(path);
    // requestPrepareTree, never prepareTree: the flag is consumed at the frame boundary, so a multi-file upload costs one sweep.
    scheduler_->requestPrepareTree();
}

// A firmware image moves through a transfer slot as a file upload does, its bytes pushed into the platform's writer as they arrive, so no frame waits on the sender.
void HttpServerModule::handleFirmwareUpload(platform::TcpConnection& conn, platform::OtaTarget target, const char* initialBody,
                                            size_t initialLen, size_t contentLen) {
    if constexpr (!platform::hasOta) {
        sendResponse(conn, 501, "application/json", "{\"error\":\"OTA not supported on this platform\"}");
        return;
    }
    if (target == platform::OtaTarget::MoonBase) {
        if (!platform::otaHasMoonBase()) {
            sendResponse(conn, 409, "application/json", "{\"error\":\"no MoonBase on this device\"}");
            return;
        }
        // Running from MoonBase means the factory slot is executing and cannot be written; this answers with the reason, and otaBegin is the real guard.
        if (platform::otaRunningMoonBase()) {
            sendResponse(conn, 409, "application/json", "{\"error\":\"running from MoonBase, boot the app first\"}");
            return;
        }
    }
    // One OTA at a time, as handleFirmwareUrl guards: both write g_ota* state.
    if (otaInFlight()) {
        sendResponse(conn, 409, "application/json", "{\"error\":\"ota already in progress\"}");
        return;
    }
    FileTransfer* t = claimTransfer(conn);
    if (!t) return;
    g_otaBytesTotal = static_cast<uint32_t>(contentLen);   // the UI's "Y KB", Content-Length up front
    // An image refused at the start is drained like one refused mid-write, so the answer reaches a sender still sending rather than a reset.
    t->refused = !platform::otaBegin(t->ota, target, contentLen, g_otaStatus, sizeof(g_otaStatus), &g_otaBytesRead);
    t->kind = FileTransfer::Kind::Firmware;
    t->target = target;
    // The bytes that came with the request go first, at most Content-Length of them.
    const size_t initial = initialLen < contentLen ? initialLen : contentLen;
    if (initial && !t->refused && !platform::otaWrite(t->ota, reinterpret_cast<const uint8_t*>(initialBody), initial)) t->refused = true;
    t->remaining = contentLen - initial;
    t->windowStartMs = platform::millis();
    t->windowBytes = 0;
    t->active = true;
    t->conn = std::move(conn);
}

void HttpServerModule::answerFirmware(platform::TcpConnection& conn, bool ok, platform::OtaTarget target) {
    if (!ok) {
        char msg[128];
        std::snprintf(msg, sizeof(msg), "{\"error\":\"%.90s\"}", g_otaStatus);
        sendResponse(conn, 500, "application/json", msg);
        return;
    }
    if (target == platform::OtaTarget::MoonBase) {
        // No reboot: the running app is untouched; re-read then re-bind, since the card's version buffer is filled at setup.
        if (auto* fw = static_cast<FirmwareUpdateModule*>(findModuleByName("Firmware"))) {
            fw->readMoonBaseVersion();
            fw->rebuildControls();
        }
        sendResponse(conn, 200, "application/json", "{\"ok\":true}");
        return;
    }
    // The boot pointer flipped, so the 200 goes out first and the browser sees a clean "flashed" instead of an aborted socket.
    FilesystemModule::flushPending();
    sendResponse(conn, 200, "application/json", "{\"ok\":true}");
    conn.close();
    platform::delayMs(200);
    platform::reboot();   // noreturn: boots the flashed image
}

void HttpServerModule::handleMoonBaseUrl(platform::TcpConnection& conn, const char* body) {
    if constexpr (!platform::hasOta) {
        sendResponse(conn, 501, "application/json", "{\"error\":\"OTA not supported on this platform\"}");
        return;
    }
    if (!platform::otaHasMoonBase()) {
        sendResponse(conn, 409, "application/json", "{\"error\":\"no MoonBase on this device\"}");
        return;
    }
    if (platform::otaRunningMoonBase()) {
        sendResponse(conn, 409, "application/json",
                     "{\"error\":\"running from MoonBase, boot the app first\"}");
        return;
    }
    if (otaInFlight()) {
        sendResponse(conn, 409, "application/json", "{\"error\":\"ota already in progress\"}");
        return;
    }
    char url[512] = {};
    mm::json::parseString(body, "url", url, sizeof(url));
    if (url[0] == 0) {
        sendResponse(conn, 400, "application/json", "{\"error\":\"url required\"}");
        return;
    }
    if (std::strncmp(url, "http://", 7) != 0 && std::strncmp(url, "https://", 8) != 0) {
        sendResponse(conn, 400, "application/json",
                     "{\"error\":\"url must start with http:// or https://\"}");
        return;
    }
    g_otaBytesRead = 0;
    g_otaBytesTotal = 0;
    // 202 and return: the install runs on its own task, so the browser polls progress instead of holding a request open.
    const bool started = platform::otaFetchMoonBaseUrl(url, g_otaStatus, sizeof(g_otaStatus),
                                                       &g_otaBytesRead, &g_otaBytesTotal);
    if (!started) {
        char msg[128];
        std::snprintf(msg, sizeof(msg), "{\"error\":\"%.90s\"}", g_otaStatus);
        sendResponse(conn, 500, "application/json", msg);
        return;
    }
    sendResponse(conn, 202, "application/json", "{\"ok\":true}");
}

// The content type a UI file name's extension names, for a file served from disk.
static const char* uiContentType(const char* name) {
    const char* ext = std::strrchr(name, '.');
    if (!ext) return "application/octet-stream";
    if (std::strcmp(ext, ".html") == 0) return "text/html";
    if (std::strcmp(ext, ".css") == 0) return "text/css";
    if (std::strcmp(ext, ".js") == 0) return "application/javascript";
    if (std::strcmp(ext, ".png") == 0) return "image/png";
    return "application/octet-stream";
}

bool HttpServerModule::serveUiFromDisk(platform::TcpConnection& conn, const char* name) {
    char* data = nullptr;
    size_t len = 0;
    if (!platform::uiFolderFile(name, data, len)) return false;
    char header[256];
    std::snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "Cache-Control: no-cache\r\n"
        "\r\n",
        uiContentType(name), len);
    answerBlock(conn, header, data, len);
    return true;
}

void HttpServerModule::answerBlock(platform::TcpConnection& conn, const char* header, char* data, size_t len) {
    Send* slot = claimSend(conn);
    if (!slot || !conn.write(reinterpret_cast<const uint8_t*>(header), std::strlen(header))) {
        platform::free(data);
        return;
    }
    // The block leaves as the body and is freed once it is out.
    slot->owned = data;
    slot->body = reinterpret_cast<const uint8_t*>(data);
    slot->bodyLeft = len;
    slot->conn = std::move(conn);
    slot->busy = true;
    slot->lastActivityMs = platform::millis();
}

void HttpServerModule::answerJson(platform::TcpConnection& conn, JsonSink& sink) {
    if (sink.overflowed()) { sendResponse(conn, 500, "application/json", "{\"error\":\"out of memory\"}"); return; }
    const size_t len = sink.size();
    char header[192];
    std::snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n"
        "Connection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n", len);
    answerBlock(conn, header, sink.detach(), len);
}

// Whether a request's If-None-Match names this file's tag, or `*` (RFC 9110 section 13.1.2).
static bool etagMatches(const char* ifNoneMatch, const char* etag) {
    if (!ifNoneMatch) return false;
    const char* value = ifNoneMatch + std::strlen("If-None-Match:");
    const char* eol = std::strstr(value, "\r\n");
    const char* hit = std::strstr(value, etag);
    if (hit && (!eol || hit < eol)) return true;
    const char* star = std::strchr(value, '*');
    return star && (!eol || star < eol);
}

void HttpServerModule::serveFile(platform::TcpConnection& conn, const ui::EmbeddedFile& file, const char* ifNoneMatch) {
    // A desktop running from a checkout serves its folder's copy, so an edit shows without a rebuild.
    if (serveUiFromDisk(conn, file.name)) return;
    // A browser holding this exact file is told so, and loads it from its cache.
    if (etagMatches(ifNoneMatch, file.etag)) {
        char notModified[128];
        int n = std::snprintf(notModified, sizeof(notModified),
            "HTTP/1.1 304 Not Modified\r\nETag: %s\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n", file.etag);
        conn.write(reinterpret_cast<const uint8_t*>(notModified), static_cast<size_t>(n));
        return;
    }
    const char* contentType = file.contentType;
    // Fall back to the embedded copy (ESP32, or no files on disk).
    char header[256];
    int headerLen = std::snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "%s"
        "ETag: %s\r\n"
        "Connection: close\r\n"
        "Cache-Control: no-cache\r\n"
        "\r\n",
        contentType, file.len,
        file.gzipped ? "Content-Encoding: gzip\r\n" : "", file.etag);
    Send* slot = claimSend(conn);
    if (!slot) return;
    if (!conn.write(reinterpret_cast<const uint8_t*>(header), headerLen)) return;
    slot->body = file.data;
    slot->bodyLeft = file.len;
    slot->conn = std::move(conn);
    slot->busy = true;
    slot->lastActivityMs = platform::millis();
}

// Below the headroom one send goes at a time rather than none. No send waits forever, and the queue stays one socket's buffer, not the 52 KB a page load once built. A holder that moves nothing for kTransferStallMs passes the lease on.
bool HttpServerModule::maySend(const void* who) {
    if (sendHasHeadroom()) return true;
    const uint32_t now = platform::millis();
    if (lowRamSender_ && lowRamSender_ != who && now - lowRamProgressMs_ <= kTransferStallMs) {
        if (!lowRamWaiter_ || now - lowRamWaitMs_ > kTransferStallMs) { lowRamWaiter_ = who; lowRamWaitMs_ = now; }
        return false;
    }
    // Turns go in order: a send that asked while another held the lease goes before one asking again, as the preview does with every frame.
    if (!lowRamSender_ && lowRamWaiter_ && lowRamWaiter_ != who && now - lowRamWaitMs_ <= kTransferStallMs) return false;
    if (lowRamWaiter_ == who) lowRamWaiter_ = nullptr;
    if (lowRamSender_ != who) { lowRamSender_ = who; lowRamProgressMs_ = now; }
    return true;
}

// Apply-core: set one control's value from the same `{"value":8}` body the HTTP handler gets; transport-free, returning an OpResult the caller maps to its own reporting.
HttpServerModule::OpResult HttpServerModule::applySetControl(
        const char* moduleName, const char* controlName, const char* valueJson) {
    // The generic control-set is a Scheduler primitive shared with Improv, the WLED bridge and InfraredService; this wrapper maps its result onto the HTTP OpResult.
    if (!scheduler_) return OpResult::ModuleNotFound;
    switch (scheduler_->setControl(moduleName, controlName, valueJson)) {
        // A schema change from this set is handled centrally: Scheduler::setControl calls rebuildControls(), which fires the schema-changed hook, so no per-path resync is needed.
        case Scheduler::SetControlResult::Ok:              return OpResult::Ok;
        case Scheduler::SetControlResult::ModuleNotFound:  return OpResult::ModuleNotFound;
        case Scheduler::SetControlResult::ControlNotFound: return OpResult::ControlNotFound;
        case Scheduler::SetControlResult::OutOfRange:      return OpResult::OutOfRange;
        case Scheduler::SetControlResult::Malformed:       return OpResult::Malformed;
        case Scheduler::SetControlResult::ReadOnly:        return OpResult::ReadOnly;
    }
    return OpResult::ModuleNotFound;   // unreachable; keeps -Wreturn-type happy
}

void HttpServerModule::handleSetControl(platform::TcpConnection& conn, const char* body) {
    // Parse {"module":"Noise","control":"scale","value":8}; the apply-core reads the value out of `body` itself, so it sees the exact JSON the API got.
    char moduleName[32] = {};
    char controlName[32] = {};
    mm::json::parseString(body, "module", moduleName, sizeof(moduleName));
    mm::json::parseString(body, "control", controlName, sizeof(controlName));

    switch (applySetControl(moduleName, controlName, body)) {
        case OpResult::Ok:
            sendResponse(conn, 200, "application/json", "{\"ok\":true}");
            return;
        case OpResult::ModuleNotFound:
            sendResponse(conn, 404, "application/json", "{\"error\":\"module not found\"}");
            return;
        case OpResult::ControlNotFound:
            sendResponse(conn, 404, "application/json", "{\"error\":\"control not found\"}");
            return;
        case OpResult::OutOfRange:
            sendResponse(conn, 400, "application/json", "{\"error\":\"value out of range\"}");
            return;
        case OpResult::Malformed:
            sendResponse(conn, 400, "application/json", "{\"error\":\"value malformed\"}");
            return;
        case OpResult::ReadOnly:
            sendResponse(conn, 400, "application/json", "{\"error\":\"control is read-only\"}");
            return;
        default:
            sendResponse(conn, 400, "application/json", "{\"error\":\"bad request\"}");
            return;
    }
}

// The Scheduler owns the tree walk (firstByName); this adds only the scheduler_ null-guard the handlers rely on, since it is unset until setScheduler().
MoonModule* HttpServerModule::findModuleByName(const char* name) {
    return scheduler_ ? scheduler_->firstByName(name) : nullptr;
}

void HttpServerModule::serveSystem(platform::TcpConnection& conn) {
    const char* header =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n";
    conn.write(reinterpret_cast<const uint8_t*>(header), std::strlen(header));

    JsonSink sink(conn);
    // maxBlock is internal-only, since all-memory reports ~8 MB on PSRAM boards; `allocated`/`allocPeak` mean the same on a board and a laptop, where free heap does not.
    sink.appendf(
        "{\"fps\":%u,\"tickTimeUs\":%u,\"freeHeap\":%u,\"freeInternal\":%u,\"maxBlock\":%u,\"maxExec\":%u,"
        "\"allocated\":%u,\"allocPeak\":%u,\"allocBlocks\":%u,\"uptime\":%u,\"modules\":[",
        static_cast<unsigned>(scheduler_ ? scheduler_->fps() : 0),
        static_cast<unsigned>(scheduler_ ? scheduler_->tickTimeUs() : 0),
        static_cast<unsigned>(platform::freeHeap()),
        static_cast<unsigned>(platform::freeInternalHeap()),
        static_cast<unsigned>(platform::maxInternalAllocBlock()),
        static_cast<unsigned>(platform::maxExecAllocBlock()),
        static_cast<unsigned>(platform::allocatedBytes()),
        static_cast<unsigned>(platform::allocatedPeak()),
        static_cast<unsigned>(platform::allocatedCount()),
        static_cast<unsigned>(scheduler_ ? scheduler_->elapsed() / 1000 : 0));

    // Per-module timing (walk tree recursively)
    if (scheduler_) {
        bool first = true;
        for (uint8_t i = 0; i < scheduler_->moduleCount(); i++) {
            writeModuleMetricsJson(sink, scheduler_->module(i), first);
        }
    }

    sink.append("]}");
    sink.flush();
}

void HttpServerModule::writeModuleMetricsJson(JsonSink& sink, MoonModule* mod, bool& first) {
    if (!mod) return;
    sink.appendf(
        "%s{\"name\":\"%s\",\"us\":%u,\"classSize\":%u,\"heap\":%u",
        first ? "" : ",",
        mod->name() ? mod->name() : "?",
        static_cast<unsigned>(mod->tickTimeUs()),
        static_cast<unsigned>(mod->classSize()),
        static_cast<unsigned>(mod->dynamicBytes()));
    writeStatus(sink, mod);
    sink.append("}");
    first = false;
    for (uint8_t i = 0; i < mod->childCount(); i++) {
        writeModuleMetricsJson(sink, mod->child(i), first);
    }
}

// Apply-core: every structural change is a one-member state document, so one engine does the checks, lifecycle and save; opResultOf keeps its refusal for the response.
static HttpServerModule::OpResult opResultOf(const StateDocumentResult& r, StateDocumentResult* refusal) {
    if (r.ok) return HttpServerModule::OpResult::Ok;
    if (refusal) *refusal = r;
    return HttpServerModule::OpResult::Refused;
}

// A refusal answered as `PATCH /api/state` answers one: the engine's error and where it is.
void HttpServerModule::sendRefusal(platform::TcpConnection& conn, const StateDocumentResult& r) {
    char resp[192];
    JsonSink sink(resp, sizeof(resp));
    writeStateResult(sink, r);
    sendResponse(conn, 400, "application/json", sink.overflowed() ? "{\"error\":\"refused\"}" : resp);
}

// `"name":{"type":"T"}`, the member that creates a module.
static void writeTypedMember(JsonSink& sink, const char* name, const char* typeName) {
    sink.writeJsonString(name);
    sink.append(":{\"type\":");
    sink.writeJsonString(typeName);
    sink.append("}");
}

HttpServerModule::OpResult HttpServerModule::applyAddModule(const char* typeName, const char* id, const char* parentId,
                                                            char* outName, size_t outNameLen, StateDocumentResult* refusal) {
    if (!typeName || typeName[0] == 0 || !scheduler_) return OpResult::BadRequest;
    // The top level is the fixed set main.cpp wires, so an add names a parent.
    if (!parentId || parentId[0] == 0) return OpResult::BadRequest;
    auto* parent = findModuleByName(parentId);
    if (!parent) return OpResult::ModuleNotFound;
    char name[MoonModule::kNameLen] = {};
    if (id && id[0] != 0) {
        if (findModuleByName(id)) return OpResult::NameInUse;
        std::snprintf(name, sizeof(name), "%s", id);
        if (std::strlen(id) >= sizeof(name)) return OpResult::BadRequest;   // the document's name rule, said before it truncates
    } else {
        const char* base = ModuleFactory::defaultNameOf(typeName);
        if (!base) return OpResult::UnknownType;
        if (!scheduler_->freeName(base, name, sizeof(name))) return OpResult::NameInUse;
    }
    JsonSink body;
    writeTypedMember(body, name, typeName);
    const OpResult r = opResultOf(applyStateAt(*scheduler_, *parent, body.data()), refusal);
    if (r == OpResult::Ok && outName && outNameLen > 0) std::snprintf(outName, outNameLen, "%s", name);
    return r;
}

void HttpServerModule::handleAddModule(platform::TcpConnection& conn, const char* body) {
    char typeName[32] = {};
    char id[32] = {};
    char parentId[32] = {};
    mm::json::parseString(body, "type", typeName, sizeof(typeName));
    mm::json::parseString(body, "id", id, sizeof(id));
    mm::json::parseString(body, "parent_id", parentId, sizeof(parentId));

    // The created module's name rides back so the UI can select and focus it; written through writeJsonString, since a client-supplied `id` may hold a quote.
    char createdName[MoonModule::kNameLen] = {};
    StateDocumentResult why;
    switch (applyAddModule(typeName, id, parentId, createdName, sizeof(createdName), &why)) {
        case OpResult::Ok: {
            // A 15-character name fully \uXXXX-escaped plus the wrapper.
            char resp[128];
            JsonSink sink(resp, sizeof(resp));
            sink.append("{\"ok\":true,\"name\":");
            sink.writeJsonString(createdName);
            sink.append("}");
            sendResponse(conn, 200, "application/json", resp);
            return;
        }
        case OpResult::NameInUse:
            sendResponse(conn, 409, "application/json", "{\"error\":\"that name is used elsewhere in the tree\"}");
            return;
        case OpResult::ModuleNotFound:
            sendResponse(conn, 404, "application/json", "{\"error\":\"parent not found\"}");
            return;
        case OpResult::UnknownType:
            sendResponse(conn, 400, "application/json", "{\"error\":\"unknown type\"}");
            return;
        case OpResult::Refused:
            sendRefusal(conn, why);
            return;
        case OpResult::BadRequest:
        default:
            sendResponse(conn, 400, "application/json",
                         "{\"error\":\"missing type, a name too long, or no parent_id (the top level is fixed in main.cpp)\"}");
            return;
    }
}

// The module a delete or replace acts on, refused when it is top-level (wired in main.cpp and held by the scheduler) or one the user cannot remove.
HttpServerModule::OpResult HttpServerModule::editableChild(const char* moduleName, MoonModule*& mod) {
    mod = findModuleByName(moduleName);
    if (!mod) return OpResult::ModuleNotFound;
    if (!mod->parent() || !scheduler_) return OpResult::BadRequest;
    return mod->userEditable() ? OpResult::Ok : OpResult::ReadOnly;
}

HttpServerModule::OpResult HttpServerModule::applyDeleteModule(const char* moduleName, StateDocumentResult* refusal) {
    MoonModule* mod = nullptr;
    const OpResult r = editableChild(moduleName, mod);
    if (r != OpResult::Ok) return r;
    JsonSink body;
    body.writeJsonString(mod->name());
    body.append(":null");
    return opResultOf(applyStateAt(*scheduler_, *mod->parent(), body.data()), refusal);
}

void HttpServerModule::handleDeleteModule(platform::TcpConnection& conn, const char* path) {
    char moduleName[MoonModule::kNameLen] = {};
    if (!routeModuleName(path, moduleName)) {
        sendResponse(conn, 400, "application/json", "{\"error\":\"module name too long\"}");
        return;
    }
    StateDocumentResult why;
    switch (applyDeleteModule(moduleName, &why)) {
        case OpResult::Ok: sendResponse(conn, 200, "application/json", "{\"ok\":true}"); return;
        case OpResult::Refused: sendRefusal(conn, why); return;
        case OpResult::ModuleNotFound: sendResponse(conn, 404, "application/json", "{\"error\":\"module not found\"}"); return;
        case OpResult::ReadOnly: sendResponse(conn, 400, "application/json", "{\"error\":\"module not deletable\"}"); return;
        default: sendResponse(conn, 400, "application/json", "{\"error\":\"cannot delete top-level module\"}"); return;
    }
}

/// The name for a replaced module: the caller's request, else a custom current name, else null for the new type's default (never an empty string).
const char* HttpServerModule::replacementName(const char* requested, const char* current,
                                              const char* oldDefault) {
    if (requested && requested[0] != 0) return requested;
    if (current && oldDefault && std::strcmp(current, oldDefault) != 0) return current;
    return nullptr;
}

// Exactly the parent's children, in order, with the replacement in `old`'s place: a new name removes the old module, the same name re-types it in place.
static void writeChildrenReplacing(JsonSink& doc, MoonModule& old, const char* name, const char* typeName) {
    MoonModule* parent = old.parent();
    doc.append("\"$patch\":\"replace\"");
    for (uint8_t i = 0; i < parent->childCount(); i++) {
        MoonModule* c = parent->child(i);
        if (!c) continue;
        doc.append(",");
        if (c == &old) writeTypedMember(doc, name, typeName);
        else { doc.writeJsonString(c->name()); doc.append(":{}"); }
    }
}

HttpServerModule::OpResult HttpServerModule::applyReplaceModule(const char* moduleName, const char* typeName, const char* wantName,
                                                                StateDocumentResult* refusal) {
    MoonModule* mod = nullptr;
    const OpResult found = editableChild(moduleName, mod);
    if (found != OpResult::Ok) return found;
    if (!typeName || typeName[0] == 0) return OpResult::BadRequest;
    // Copied out, since the factory hands every default name out of one shared buffer and the next call overwrites it.
    char typeDefault[MoonModule::kNameLen] = {};
    const char* fresh = ModuleFactory::defaultNameOf(typeName);
    if (!fresh) return OpResult::UnknownType;
    std::snprintf(typeDefault, sizeof(typeDefault), "%s", fresh);
    const char* keep = replacementName(wantName, mod->name(), ModuleFactory::defaultNameOf(mod->typeName()));
    // A name other than the slot's own is made free, as an add without an id is.
    const char* want = keep ? keep : typeDefault;
    char name[MoonModule::kNameLen] = {};
    if (std::strcmp(want, mod->name()) == 0) std::snprintf(name, sizeof(name), "%s", want);
    else if (!scheduler_->freeName(want, name, sizeof(name))) return OpResult::NameInUse;

    JsonSink doc;
    writeChildrenReplacing(doc, *mod, name, typeName);
    return opResultOf(applyStateAt(*scheduler_, *mod->parent(), doc.data()), refusal);
}

void HttpServerModule::handleReplaceModule(platform::TcpConnection& conn, const char* moduleName, const char* body) {
    char typeName[32] = {};
    mm::json::parseString(body, "type", typeName, sizeof(typeName));
    // An optional name for the replacement, the counterpart of `id` on create.
    char wantName[32] = {};
    mm::json::parseString(body, "name", wantName, sizeof(wantName));
    StateDocumentResult why;
    switch (applyReplaceModule(moduleName, typeName, wantName, &why)) {
        case OpResult::Ok: sendResponse(conn, 200, "application/json", "{\"ok\":true}"); return;
        case OpResult::Refused: sendRefusal(conn, why); return;
        case OpResult::ModuleNotFound: sendResponse(conn, 404, "application/json", "{\"error\":\"module not found\"}"); return;
        case OpResult::ReadOnly: sendResponse(conn, 400, "application/json", "{\"error\":\"module not editable\"}"); return;
        case OpResult::UnknownType: sendResponse(conn, 400, "application/json", "{\"error\":\"unknown type\"}"); return;
        case OpResult::NameInUse: sendResponse(conn, 409, "application/json", "{\"error\":\"no free name\"}"); return;
        default: sendResponse(conn, 400, "application/json", typeName[0] ? "{\"error\":\"top-level modules cannot be replaced\"}" : "{\"error\":\"missing type\"}"); return;
    }
}

void HttpServerModule::serveModule(platform::TcpConnection& conn, const char* name) {
    char decoded[MoonModule::kNameLen] = {};
    if (!routeModuleName(name, decoded)) {
        sendResponse(conn, 400, "application/json", "{\"error\":\"module name too long\"}");
        return;
    }

    // appearsInUi() gates this like /api/state, since a module that is not a card has no `{ }` link; `/document` asks for the state document PATCH /api/state takes.
    const char* rest = std::strchr(name, '/');
    const bool document = rest && std::strncmp(rest, "/document", 9) == 0 && (rest[9] == 0 || rest[9] == '?');
    MoonModule* mod = decoded[0] ? findModuleByName(decoded) : nullptr;
    if (mod && !mod->appearsInUi()) mod = nullptr;
    if (!mod) {
        sendResponse(conn, 404, "application/json", "{\"error\":\"module not found\"}");
        return;
    }

    const char* header =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n";
    conn.write(reinterpret_cast<const uint8_t*>(header), std::strlen(header));

    // The SAME writer /api/state uses, so the two can never disagree about a module's shape; a document comes from the writer a preset save uses.
    JsonSink sink(conn);
    if (document) {
        sink.append("{");
        writeStateMember(sink, *mod);
        sink.append("}");
    } else {
        writeModuleJson(sink, mod);
    }
    sink.flush();
}

// The ref the UI downloads factory scripts from. A -dev build fetches from its own commit (kBuildId), which cannot drift like a branch; a `+` dirty suffix is stripped, and an unpushed commit 404s as a failed download.
static void writeScriptTag(JsonSink& sink) {
    const char* v = kVersion;
    const bool dev = std::strstr(v, "-dev") != nullptr;
    if (dev) {
        char commit[24];
        std::snprintf(commit, sizeof(commit), "%s", kBuildId);
        for (char* c = commit; *c; c++) if (*c == '+') { *c = '\0'; break; }
        sink.writeJsonString(commit[0] ? commit : "main");
    } else {
        char tag[32];
        std::snprintf(tag, sizeof(tag), "v%s", v);
        sink.writeJsonString(tag);
    }
}

// GET /api/scripts: the factory script names (none of the text), which the UI fetches from GitHub at `tag`, the firmware's own version, and posts to /api/file.
void HttpServerModule::serveScriptCatalog(platform::TcpConnection& conn) {
    const char* header =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n";
    conn.write(reinterpret_cast<const uint8_t*>(header), std::strlen(header));

    JsonSink sink(conn);
    sink.append("{\"tag\":");
    writeScriptTag(sink);
    sink.append(",\"dir\":");
    sink.writeJsonString(moonlive::kFactoryScriptDir);

    // Each name carries `dim` and `tags` from its source, for the picker, and `folder`, where under moonlive/ the UI fetches it from; the device keeps scripts flat.
    auto emit = [&sink](const char* key, const char* const* names, const unsigned char* dims,
                        const char* const* tags, const unsigned char* folders, size_t count) {
        sink.appendf(",\"%s\":{\"names\":[", key);
        for (size_t i = 0; i < count; i++) {
            if (i) sink.append(",");
            sink.writeJsonString(names[i]);
        }
        sink.append("],\"dim\":[");
        for (size_t i = 0; i < count; i++) sink.appendf(i ? ",%u" : "%u", unsigned(dims[i]));
        sink.append("],\"tags\":[");
        for (size_t i = 0; i < count; i++) {
            if (i) sink.append(",");
            sink.writeJsonString(tags[i]);
        }
        sink.append("],\"folder\":[");
        for (size_t i = 0; i < count; i++) {
            if (i) sink.append(",");
            sink.writeJsonString(moonlive::kCatalogFolders[folders[i]]);
        }
        sink.append("]}");
    };
    emit("effects", moonlive::kEffectCatalog,
         moonlive::kEffectCatalogDim, moonlive::kEffectCatalogTags, moonlive::kEffectCatalogFolder,
         moonlive::kEffectCatalogCount);
    emit("layouts", moonlive::kLayoutCatalog,
         moonlive::kLayoutCatalogDim, moonlive::kLayoutCatalogTags, moonlive::kLayoutCatalogFolder,
         moonlive::kLayoutCatalogCount);
    emit("modifiers", moonlive::kModifierCatalog,
         moonlive::kModifierCatalogDim, moonlive::kModifierCatalogTags, moonlive::kModifierCatalogFolder,
         moonlive::kModifierCatalogCount);
    emit("services", moonlive::kServiceCatalog,
         moonlive::kServiceCatalogDim, moonlive::kServiceCatalogTags, moonlive::kServiceCatalogFolder,
         moonlive::kServiceCatalogCount);
    emit("palettes", moonlive::kPaletteCatalog,
         moonlive::kPaletteCatalogDim, moonlive::kPaletteCatalogTags, moonlive::kPaletteCatalogFolder,
         moonlive::kPaletteCatalogCount);
    sink.append("}");
    sink.flush();
}

void HttpServerModule::typesEtag(char (&out)[16]) {
    Fnv1a f;
    for (const char* part : {kVersion, kBuildId, kBuildDate, kFirmwareName}) f.add(part, std::strlen(part));
    for (uint8_t i = 0; i < ModuleFactory::typeCount(); i++)
        if (const char* name = ModuleFactory::typeName(i)) f.add(name, std::strlen(name) + 1);   // the terminator keeps "ab","c" from reading as "a","bc"
    std::snprintf(out, sizeof(out), "\"%08x\"", static_cast<unsigned>(f.h));
}

void HttpServerModule::serveTypes(platform::TcpConnection& conn, const char* ifNoneMatch) {
    char tag[16];
    typesEtag(tag);
    if (etagMatches(ifNoneMatch, tag)) {
        char notModified[128];
        const int n = std::snprintf(notModified, sizeof(notModified),
            "HTTP/1.1 304 Not Modified\r\nETag: %s\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n", tag);
        conn.write(reinterpret_cast<const uint8_t*>(notModified), static_cast<size_t>(n));
        return;
    }
    Send* slot = claimSend(conn);
    if (!slot) return;
    auto* t = static_cast<TypesSource*>(platform::alloc(sizeof(TypesSource)));
    if (!t) { sendResponse(conn, 507, "application/json", "{\"error\":\"out of memory\"}"); return; }
    new (t) TypesSource;
    char header[192];
    const int hn = std::snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nETag: %s\r\nCache-Control: no-cache\r\n"
        "Connection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n", tag);
    if (!conn.write(reinterpret_cast<const uint8_t*>(header), static_cast<size_t>(hn))) { platform::free(t); return; }
    slot->block = t;
    slot->source = Send::Source::Types;
    slot->conn = std::move(conn);
    slot->busy = true;
    slot->lastActivityMs = platform::millis();
}

void HttpServerModule::writeTypeJson(JsonSink& sink, uint8_t i, bool first) {
    const char* name = ModuleFactory::typeName(i);
    const ModuleRole role = ModuleFactory::typeRole(i);
    const char* docPath = ModuleFactory::typeDocPath(i);
    const char* tags = ModuleFactory::typeTags(i);
    const char* childRoles = ModuleFactory::typeAcceptsChildRoles(i);
    // displayNameFor returns a pointer into a shared static buffer, so copy it before another factory call overwrites it.
    char displayName[16];
    mm::formatTo(displayName, sizeof(displayName), "%s", ModuleFactory::displayNameFor(name, role));
    sink.appendf("%s{\"name\":\"%s\",\"displayName\":\"%s\",\"role\":\"%s\","
                 "\"docPath\":\"%s\",\"tags\":\"%s\",\"dim\":%u,"
                 "\"acceptsChildRoles\":\"%s\",\"defaults\":{",
                 first ? "" : ",", name, displayName, roleName(role),
                 docPath ? docPath : "", tags ? tags : "",
                 static_cast<unsigned>(ModuleFactory::typeDim(i)),
                 childRoles ? childRoles : "");
    writeTypeDefaults(sink, name);
    sink.append("}}");
}

void HttpServerModule::writeTypeDefaults(JsonSink& sink, const char* typeName) {
    MoonModule* probe = ModuleFactory::create(typeName);
    if (!probe) return;
    probe->defineControls();
    auto& cs = probe->controls();
    bool first = true;
    for (uint8_t i = 0; i < cs.count(); i++) {
        auto& c = cs[i];
        // hasDefault filters out Password and the read-only/Progress types (no input to seed); the rest emit `"name":value`, rendered in Control.cpp.
        if (!hasDefault(c.type)) continue;
        sink.appendf("%s\"%s\":", first ? "" : ",", c.name);
        writeControlValue(sink, c);
        first = false;
    }
    probe->release();
    delete probe;
}

void HttpServerModule::handleMoveModule(platform::TcpConnection& conn, const char* moduleName, const char* body) {
    auto* mod = findModuleByName(moduleName);
    if (!mod) {
        sendResponse(conn, 404, "application/json", "{\"error\":\"module not found\"}");
        return;
    }
    auto* parent = mod->parent();
    if (!parent) {
        sendResponse(conn, 400, "application/json", "{\"error\":\"top-level modules cannot be reordered\"}");
        return;
    }
    int to = mm::json::parseInt(body, "to");
    if (to < 0 || to >= parent->childCount()) {
        sendResponse(conn, 400, "application/json", "{\"error\":\"to out of range\"}");
        return;
    }
    if (!parent->moveChildTo(mod, static_cast<uint8_t>(to))) {
        // Already at position N or another no-op: not an error, but reported so the UI avoids a refetch storm on rapid drags.
        sendResponse(conn, 200, "application/json", "{\"ok\":true,\"noop\":true}");
        return;
    }
    // A move changes the parent's child ordering: mark the parent dirty so its file is rewritten with the new order (same as add/delete handlers).
    parent->markDirty();
    FilesystemModule::noteDirty();
    if (scheduler_) scheduler_->requestPrepareTree();
    requestFullResync();   // structural change (see requestFullResync)
    sendResponse(conn, 200, "application/json", "{\"ok\":true}");
}

// Resolve `/api/list/<module>/<control>[/<row>]` into the editable List source, the row id and whether a row was named (digits are an id, else a name); nullptr after the 4xx on failure.
ListSource* HttpServerModule::resolveEditableList(platform::TcpConnection& conn, const char* tail,
                                                  uint32_t& outId, bool& outHasId) {
    // Split the tail into "<module>/<control>[/<row>]" on '/'. Names have no '/', so two slashes at most: module, control, and an optional row.
    char moduleName[32] = {};
    char controlName[32] = {};
    outHasId = false;
    outId = 0;
    const char* s1 = std::strchr(tail, '/');
    if (!s1) { sendResponse(conn, 400, "application/json", "{\"error\":\"bad list path\"}"); return nullptr; }
    const size_t mLen = static_cast<size_t>(s1 - tail);
    if (mLen == 0 || mLen >= sizeof(moduleName)) {
        sendResponse(conn, 400, "application/json", "{\"error\":\"bad module\"}"); return nullptr;
    }
    std::memcpy(moduleName, tail, mLen);
    const char* cStart = s1 + 1;
    const char* s2 = std::strchr(cStart, '/');
    const size_t cLen = s2 ? static_cast<size_t>(s2 - cStart) : std::strlen(cStart);
    if (cLen == 0 || cLen >= sizeof(controlName)) {
        sendResponse(conn, 400, "application/json", "{\"error\":\"bad control\"}"); return nullptr;
    }
    std::memcpy(controlName, cStart, cLen);
    const char* rowName = nullptr;
    if (s2 && s2[1]) {   // a row segment follows the control
        // Bounded parse like the Content-Length one: an id is all digits and fits 32 bits, so "/5abc" is a name and an overflow a clean 400.
        const char* idStart = s2 + 1;
        char* idEnd = nullptr;
        errno = 0;
        const unsigned long parsed = std::strtoul(idStart, &idEnd, 10);
        if (idEnd != idStart && *idEnd == '\0') {
            if (errno == ERANGE || parsed > 0xFFFFFFFFul) {
                sendResponse(conn, 400, "application/json", "{\"error\":\"bad id\"}"); return nullptr;
            }
            outId = static_cast<uint32_t>(parsed);
        } else {
            rowName = idStart;
        }
        outHasId = true;
    }

    MoonModule* mod = findModuleByName(moduleName);
    if (!mod) { sendResponse(conn, 404, "application/json", "{\"error\":\"module not found\"}"); return nullptr; }
    auto& cs = mod->controls();
    for (uint8_t i = 0; i < cs.count(); i++) {
        if (cs[i].type == ControlType::List && std::strcmp(cs[i].name, controlName) == 0) {
            auto* src = static_cast<ListSource*>(cs[i].ptr);
            if (!src || !src->isEditableList()) {
                sendResponse(conn, 400, "application/json", "{\"error\":\"list not editable\"}");
                return nullptr;
            }
            char name[64] = {};
            if (rowName && (!decodeModuleName(rowName, name, sizeof(name)) || !src->listRowNamed(name, outId))) {
                sendResponse(conn, 404, "application/json", "{\"error\":\"row not found\"}");
                return nullptr;
            }
            listMutationModule_ = mod;   // remembered so afterListMutation marks IT dirty (persistence)
            return src;
        }
    }
    sendResponse(conn, 404, "application/json", "{\"error\":\"control not found\"}");
    return nullptr;
}

// After a list mutation: persist the owning module and let consumers re-resolve what they read.
void HttpServerModule::afterListMutation() {
    // markDirty plus noteDirty, as the module handlers do: noteDirty alone leaves the subtree clean, so the list would not persist.
    if (listMutationModule_) listMutationModule_->markDirty();
    FilesystemModule::noteDirty();
    if (scheduler_) {
        // Rebuild every module's controls: a list mutation can change what others present, such as a fixture profile adding to every driver's `fixture` Select, else a new option is unselectable.
        for (uint8_t i = 0; i < scheduler_->moduleCount(); i++)
            if (auto* m = scheduler_->module(i)) m->rebuildControls();
        // Each module re-resolves what it reads from the list; not prepareTree(), which reinitializes every driver's output and blanks a strip for a tick.
        if (listMutationModule_) scheduler_->notifyListChanged(*listMutationModule_);
        // The rebuildControls() calls above fire the schema-changed hook, so connected clients re-read the schema without an explicit resync.
    }
}

void HttpServerModule::handleListAddRow(platform::TcpConnection& conn, const char* tail) {
    uint32_t id; bool hasId;
    ListSource* src = resolveEditableList(conn, tail, id, hasId);
    if (!src) return;   // response already sent
    uint32_t newId = 0;
    if (!src->addListRow(newId)) {
        sendResponse(conn, 409, "application/json", "{\"error\":\"list full or add refused\"}");
        return;
    }
    afterListMutation();
    char body[48];
    std::snprintf(body, sizeof(body), "{\"ok\":true,\"id\":%lu}", static_cast<unsigned long>(newId));
    sendResponse(conn, 200, "application/json", body);
}

void HttpServerModule::handleListPatchRow(platform::TcpConnection& conn, const char* tail, const char* jsonBody) {
    uint32_t id; bool hasId;
    ListSource* src = resolveEditableList(conn, tail, id, hasId);
    if (!src) return;
    if (!hasId) { sendResponse(conn, 400, "application/json", "{\"error\":\"row id required\"}"); return; }
    // A PATCH is either a reorder ({"to":N}) or a field edit ({"field":F,"value":V}).
    if (mm::json::hasKey(jsonBody, "to")) {
        int to = mm::json::parseInt(jsonBody, "to");
        // Bound before the uint8_t cast, since a value over 255 would wrap into a valid-looking wrong index; moveListRow validates the rest against the row count.
        if (to < 0 || to > 255 || !src->moveListRow(id, static_cast<uint8_t>(to))) {
            sendResponse(conn, 400, "application/json", "{\"error\":\"move failed\"}");
            return;
        }
    } else {
        char field[32] = {};
        mm::json::parseString(jsonBody, "field", field, sizeof(field));
        if (!field[0]) { sendResponse(conn, 400, "application/json", "{\"error\":\"field required\"}"); return; }
        if (!src->setListRowField(id, field, jsonBody)) {
            sendResponse(conn, 400, "application/json", "{\"error\":\"field edit failed\"}");
            return;
        }
    }
    afterListMutation();
    sendResponse(conn, 200, "application/json", "{\"ok\":true}");
}

void HttpServerModule::handleListApplyRow(platform::TcpConnection& conn, const char* tail, size_t tailLen) {
    char rowPath[160] = {};
    if (tailLen >= sizeof(rowPath)) { sendResponse(conn, 400, "application/json", "{\"error\":\"bad list path\"}"); return; }
    std::memcpy(rowPath, tail, tailLen);
    uint32_t id; bool hasId;
    ListSource* src = resolveEditableList(conn, rowPath, id, hasId);
    if (!src) return;
    if (!hasId) { sendResponse(conn, 400, "application/json", "{\"error\":\"row required\"}"); return; }
    if (!src->applyListRow(id)) {
        sendResponse(conn, 400, "application/json", "{\"error\":\"apply failed: the module's status says why\"}");
        return;
    }
    // A row's action changes the tree, not the list, so the list owner is neither saved nor every module rebuilt.
    sendResponse(conn, 200, "application/json", "{\"ok\":true}");
}

void HttpServerModule::handleListDeleteRow(platform::TcpConnection& conn, const char* tail) {
    uint32_t id; bool hasId;
    ListSource* src = resolveEditableList(conn, tail, id, hasId);
    if (!src) return;
    if (!hasId) { sendResponse(conn, 400, "application/json", "{\"error\":\"row id required\"}"); return; }
    if (!src->deleteListRow(id)) {
        sendResponse(conn, 400, "application/json", "{\"error\":\"delete failed (bad id or protected)\"}");
        return;
    }
    afterListMutation();
    sendResponse(conn, 200, "application/json", "{\"ok\":true}");
}

// Reboot into MoonBase; 409 when this table has none or it already runs, since the UI offers the button only when the `moonbase` control exists.
void HttpServerModule::handleBootMoonBase(platform::TcpConnection& conn) {
    if (!platform::otaHasMoonBase() || platform::otaRunningMoonBase()) {
        sendResponse(conn, 409, "application/json", "{\"error\":\"no MoonBase on this device\"}");
        return;
    }
    // This route means MoonBase with nothing staged, so a URL left by a power cut between staging and the boot switch must not fire.
    platform::moonbaseClearStagedUrl();
    FilesystemModule::flushPending();
    sendResponse(conn, 200, "application/json", "{\"ok\":true,\"moonbase\":true}");
    conn.close();
    platform::delayMs(200);
    platform::otaBootMoonBase();
    platform::reboot();  // noreturn
}

void HttpServerModule::handleReboot(platform::TcpConnection& conn) {
    FilesystemModule::flushPending();
    sendResponse(conn, 200, "application/json", "{\"ok\":true}");
    // Best-effort: close the socket and give LWIP a brief window to push the FIN and payload out before the restart, so the browser sees a clean 200.
    conn.close();
    platform::delayMs(200);
    platform::reboot();  // noreturn
}

void HttpServerModule::handleFirmwareUrl(platform::TcpConnection& conn, const char* body) {
    if constexpr (!platform::hasOta) {
        sendResponse(conn, 501, "application/json",
                     "{\"error\":\"OTA not supported on this platform\"}");
        return;
    }

    // Reject a second concurrent OTA with 409, since both would write g_otaStatus and garble the progress; successful OTAs reboot, so only an error re-enables an attempt.
    if (otaInFlight()) {
        sendResponse(conn, 409, "application/json",
                     "{\"error\":\"ota already in progress\"}");
        return;
    }

    char url[512] = {};
    mm::json::parseString(body, "url", url, sizeof(url));
    if (url[0] == 0) {
        sendResponse(conn, 400, "application/json", "{\"error\":\"url required\"}");
        return;
    }
    // Cheap URL-shape sanity: only http(s). Stops accidental file:// or protocol-relative things from reaching the platform layer.
    if (std::strncmp(url, "http://", 7) != 0 && std::strncmp(url, "https://", 8) != 0) {
        sendResponse(conn, 400, "application/json",
                     "{\"error\":\"url must start with http:// or https://\"}");
        return;
    }
    // A device without TLS has no firmware fetcher at all, so it says so before starting: the UI then fetches the image itself and uploads it.
    if (!platform::httpsAvailable()) {
        sendResponse(conn, 501, "application/json",
                     "{\"error\":\"this device cannot fetch a URL: upload the image\"}");
        return;
    }

    // A MoonBase device runs from its one app slot, so stage the URL in NVS and reboot into MoonBase to install; 202 with {"moonbase":true} marks this flow.
    if (platform::otaHasMoonBase() && !platform::otaRunningMoonBase()) {
        // The staged URL crosses into MoonBase through a 256-byte NVS read, so a longer one would fail silently and park the device in MoonBase; reject it here.
        if (std::strlen(url) > 255) {
            sendResponse(conn, 400, "application/json",
                         "{\"error\":\"url too long for the MoonBase handoff (max 255)\"}");
            return;
        }
        if (!platform::moonbaseStageInstallUrl(url)) {
            sendResponse(conn, 500, "application/json",
                         "{\"error\":\"could not stage install url\"}");
            return;
        }
        std::snprintf(g_otaStatus, sizeof(g_otaStatus), "rebooting");
        FilesystemModule::flushPending();
        sendResponse(conn, 202, "application/json", "{\"ok\":true,\"moonbase\":true}");
        conn.close();
        platform::delayMs(200);
        platform::otaBootMoonBase();
        platform::reboot();  // noreturn, boots MoonBase, which installs and reboots back
    }

    // Seed the shared globals so the first WS push shows "starting", not a prior failed attempt's error.
    std::snprintf(g_otaStatus, sizeof(g_otaStatus), "starting");
    g_otaBytesRead = 0;
    g_otaBytesTotal = 0;

    if (!platform::http_fetch_to_ota(url, g_otaStatus, sizeof(g_otaStatus),
                                     &g_otaBytesRead, &g_otaBytesTotal)) {
        // The platform may have already written an error string; pass it through.
        char err[128];
        std::snprintf(err, sizeof(err),
                      "{\"error\":\"%s\"}", g_otaStatus[0] ? g_otaStatus : "ota start failed");
        sendResponse(conn, 500, "application/json", err);
        return;
    }
    // 202 Accepted: task running; UI polls FirmwareUpdate.update_status.
    sendResponse(conn, 202, "application/json", "{\"ok\":true}");
}

void HttpServerModule::handleWebSocketUpgrade(platform::TcpConnection& conn, const char* req,
                                              bool previewChannel) {
    // Extract Sec-WebSocket-Key
    const char* keyHeader = findHeaderCI(req, "Sec-WebSocket-Key: ");
    if (!keyHeader) { conn.close(); return; }
    keyHeader += 19;
    char wsKey[32] = {};
    int ki = 0;
    while (*keyHeader && *keyHeader != '\r' && ki < 31) {
        wsKey[ki++] = *keyHeader++;
    }
    wsKey[ki] = 0;

    // RFC 6455: accept = base64(SHA1(client_key + magic_GUID)), a fixed GUID from the spec.
    char concat[128];
    std::snprintf(concat, sizeof(concat), "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", wsKey);
    uint8_t sha1Hash[20];
    sha1(reinterpret_cast<const uint8_t*>(concat), std::strlen(concat), sha1Hash);
    char acceptKey[32];
    base64Encode(std::span<const uint8_t>(sha1Hash), std::span(acceptKey));

    // Send 101 response
    char response[256];
    int respLen = std::snprintf(response, sizeof(response),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n"
        "\r\n",
        acceptKey);
    conn.write(reinterpret_cast<const uint8_t*>(response), respLen);

    // A preview client lands in its own smaller array (see kMaxPreviewClients) and needs none of the control-plane resync bookkeeping, since it receives only the driver's binary frames.
    if (previewChannel) {
        // Busy is the common case (core 1 streams most ticks) and refusing caused reconnect storms, so admit with a SIZE_MAX cursor: the stream starts at the next whole frame.
        LockGuard admitLease{wsLock_};
        for (int i = 0; i < kMaxPreviewClients; i++) {
            if (previewClients_[i].valid()) continue;
            previewClients_[i] = std::move(conn);
            // The slot turns over: the producer drops its predecessor's standing request, and the new client announces its own wishes (pull model).
            if (clientSink_) clientSink_->onClientGone(i);
            // A frame mid-drain to other clients keeps draining; this slot's send stays idle, so the newcomer starts at the next whole frame and is never spliced into a half-sent message.
            previewSends_[i].busy = false;
            previewSends_[i].headLeft = previewSends_[i].bodyLeft = 0;
            return;
        }
        conn.close();   // preview cap reached: the client keeps /ws and simply shows no preview
        return;
    }

    // Store connection as WebSocket client.
    for (int i = 0; i < kMaxWsClients; i++) {
        if (!wsClients_[i].valid()) {
            wsOnAccessPoint_[i] = onAccessPoint(conn);   // read once: a socket's peer does not change
            wsClients_[i] = std::move(conn);
            // A fresh slot: no stream, no frame its last client left half read, and no state until the client subscribes, which requests the resync its first document answers.
            endSendSlot(controlSends_[i]);
            dropCarry(i);
            subs_[i] = Subscription{};
            resyncClient_[i] = false;
            resyncId_[i] = 0;
            return;
        }
    }
    // No slot: close. kMaxWsClients sits above the realistic count plus a refresh's overlap, since the new socket opens before the old FIN lands; the browser's backoff retries a full moment.
    conn.close();
}

// Read one frame per client and apply a WLED state-set ({on}/{bri}) to Drivers; client frames are masked (RFC 6455 §5.3), so unmask in place and skip non-text frames.
void HttpServerModule::pollUplinks() {
    for (int i = 0; i < kMaxPreviewClients; i++)
        if (previewClients_[i].valid()) pollPreviewClient(i);
    for (int c = 0; c < kMaxWsClients; c++) pollControlClient(c);
}

void HttpServerModule::pollPreviewClient(int i) {
    auto& pc = previewClients_[i];
    uint8_t buf[64];
    const int n = pc.read(buf, sizeof(buf));
    // A clean close frees the slot now, which otherwise counts against the cap until a send fails; no lease is needed on this core.
    if (n == 0) {
        pc.close();
        if (clientSink_) clientSink_->onClientGone(i);
        return;
    }
    if (n <= 0 || !clientSink_) return;
    // TCP coalesces small requests into one read, so each complete frame's payload goes to the sink in order.
    for (int off = 0; off < n; ) {
        uint8_t payload[8];
        int used = 0;
        const int len = parsePreviewUplink(buf + off, n - off, payload, &used);
        if (used <= 0) break;                  // nothing parseable left (or a partial tail)
        if (len > 0) clientSink_->onClientMessage(i, payload, len);
        off += used;
    }
}

void HttpServerModule::pollControlClient(int c) {
    auto& ws = wsClients_[c];
    if (!ws.valid()) {
        dropCarry(c);
        if (subs_[c].received) subs_[c] = Subscription{};   // a gone client stops widening what the patch walks
        return;
    }
    uint8_t f[768];
    size_t have = 0;
    // A frame the last read cut in half starts this one, so the stream never loses its framing.
    if (carry_[c]) {
        std::memcpy(f, carry_[c], carryLen_[c]);
        have = carryLen_[c];
        dropCarry(c);
    }
    const int n = ws.read(f + have, sizeof(f) - have);   // non-blocking: -1 when nothing is pending
    if (n == 0) { ws.close(); return; }                   // a clean close frees the slot for a reconnect burst
    const size_t total = have + static_cast<size_t>(n > 0 ? n : 0);
    if (total == 0) return;
    const size_t used = walkWsFrames(f, total, [&](const char* body) { onTextFrame(c, body); },
        [&](uint8_t opcode, const uint8_t* payload, size_t len) { onControlFrame(c, opcode, payload, len); });
    if (used == total) return;
    const size_t rest = total - used;
    // Past the longest frame a client writes, or with no memory to hold it, the stream cannot resume. It fails with 1002 (RFC 6455 section 7.4.1) and the client reconnects clean.
    auto* keep = rest <= kCarryMax ? static_cast<uint8_t*>(platform::alloc(rest)) : nullptr;
    if (!keep) {
        queueFrame(c, 0x88, "\x03\xea", 2);
        controlSends_[c].closing = true;
        return;
    }
    std::memcpy(keep, f + used, rest);
    carry_[c] = keep;
    carryLen_[c] = static_cast<uint16_t>(rest);
}

void HttpServerModule::dropCarry(int client) {
    if (carry_[client]) platform::free(carry_[client]);
    carry_[client] = nullptr;
    carryLen_[client] = 0;
}

void HttpServerModule::onTextFrame(int c, const char* body) {
    using mm::json::hasKey;
    if (hasKey(body, "module") && hasKey(body, "control")) {
        // A control write from the interface's input bridges, which keeps them off a TCP connection per write.
        char moduleName[32] = {}, controlName[32] = {};
        mm::json::parseString(body, "module", moduleName, sizeof(moduleName));
        mm::json::parseString(body, "control", controlName, sizeof(controlName));
        applySetControl(moduleName, controlName, body);
    } else if (hasKey(body, "subscribe")) {
        applySubscription(c, body);   // what this client views
    } else if (hasKey(body, "resync")) {
        // The page asks for its view again after an add, delete or move, and the document echoes the id.
        if (!subs_[c].received) return;
        resyncClient_[c] = true;
        resyncId_[c] = static_cast<uint32_t>(mm::json::parseInt(body, "resync"));
    } else if (hasKey(body, "on") || hasKey(body, "bri") || hasKey(body, "ps")) {
        applyWledState(body);   // the WLED app's {on, bri, ps}
    }
}

void HttpServerModule::onControlFrame(int client, uint8_t opcode, const uint8_t* payload, size_t len) {
    // Queued behind the frame leaving, so the answer goes out between frames, where a control frame may.
    if (opcode == 0x9) {
        queueFrame(client, 0x8A, reinterpret_cast<const char*>(payload), len);
        return;
    }
    // A Close is echoed with its status code and the connection ends once it is out; a one-byte body carries no code (§5.5.1), so it is answered empty.
    queueFrame(client, 0x88, reinterpret_cast<const char*>(payload), len >= 2 ? 2 : 0);
    controlSends_[client].closing = true;
}

int HttpServerModule::parsePreviewUplink(const uint8_t* buf, int n, uint8_t out[8], int* consumed) {
    if (consumed) *consumed = 0;
    // One masked client frame [0x81|0x82][0x80|len][mask x4][payload]; anything but a small masked data frame is refused, with bounds first since these are network bytes.
    if (n < 6) return -1;                                  // header(2) + mask(4) is the minimum
    const uint8_t op = buf[0] & 0x0F;
    if (op != 0x01 && op != 0x02) return -1;               // text/binary only
    if (!(buf[1] & 0x80)) return -1;                       // client frames must be masked (RFC 6455)
    const int len = buf[1] & 0x7F;
    if (len > 8 || n < 6 + len) return -1;                 // small request payloads only
    if (consumed) *consumed = 6 + len;
    const uint8_t* mask = buf + 2;
    for (int i = 0; i < len; i++) out[i] = buf[6 + i] ^ mask[i & 3];
    return len;
}

// Build a WS frame header (FIN + `opcode`, unmasked, 7/16/64-bit length form) into `h`; returns its length, shared by the preview and state sends.
static size_t writeWsFrameHeader(uint8_t* h, uint8_t opcode, size_t payloadLen) {
    h[0] = opcode;
    if (payloadLen < 126) { h[1] = static_cast<uint8_t>(payloadLen); return 2; }
    if (payloadLen < 65536) {
        h[1] = 126; h[2] = static_cast<uint8_t>((payloadLen >> 8) & 0xFF);
        h[3] = static_cast<uint8_t>(payloadLen & 0xFF); return 4;
    }
    h[1] = 127;
    for (int i = 0; i < 8; i++)
        h[2 + i] = static_cast<uint8_t>((static_cast<uint64_t>(payloadLen) >> (56 - 8 * i)) & 0xFF);
    return 10;
}

bool HttpServerModule::sendBufferedFrame(const uint8_t* header, size_t headerLen,
                                         const uint8_t* body, size_t bodyLen) {
    // Drop-new backpressure: one frame in flight, and a caller asking during a send is told busy, which the producer reads as the link being behind and sheds frame rate.
    if (previewFrame_.active) return false;

    const size_t totalLen = headerLen + bodyLen;   // WS payload length = app header + body
    // Build the WS binary header into previewFrame_.hdr, followed by the app header, so each send streams them as one head.
    const size_t wsLen = writeWsFrameHeader(previewFrame_.hdr, 0x82, totalLen);
    // sizeof(hdr)=24 holds the 10-byte WS form plus the preview app headers; the guard stops a larger header overrunning.
    if (wsLen + headerLen > sizeof(previewFrame_.hdr)) return false;
    // memcpy, not a byte loop: the compiler cannot see that wsLen is at most 10 and would warn (-Wstringop-overflow), while memcpy lets it check against the guard above.
    std::memcpy(previewFrame_.hdr + wsLen, header, headerLen);

    previewFrame_.hdrLen = wsLen + headerLen;
    previewFrame_.body = body;        // borrowed: PreviewDriver keeps the pixel buffer alive
    previewFrame_.bodyLen = bodyLen;
    const uint32_t now = platform::millis();
    for (int i = 0; i < kMaxPreviewClients; i++) {
        if (!previewClients_[i].valid()) continue;
        Send& s = previewSends_[i];
        s.head = previewFrame_.hdr;
        s.headLeft = previewFrame_.hdrLen;
        s.body = body;
        s.bodyLeft = bodyLen;
        s.busy = true;
        s.lastActivityMs = now;
    }
    previewFrame_.active = true;
    // Do not drain here on the render thread, where a variable-cost writeSome would hitch the LEDs; drainSends() pushes bytes on tick20ms.
    return true;
}

void HttpServerModule::cancelBufferedSend() {
    const size_t whole = previewFrame_.hdrLen + previewFrame_.bodyLen;
    for (int i = 0; i < kMaxPreviewClients; i++) {
        Send& s = previewSends_[i];
        // A client left mid-message would read the next frame as the rest of this one.
        if (s.busy && s.headLeft + s.bodyLeft < whole && previewClients_[i].valid()) {
            previewClients_[i].close();
            // Every close notifies the producer, or the dead slot's standing request steers on.
            if (clientSink_) clientSink_->onClientGone(i);
        }
        endSendSlot(s);
    }
    previewFrame_.active = false;
    endSend(&previewFrame_);
}

void HttpServerModule::finishPreviewFrame() {
    if (!previewFrame_.active) return;
    for (const auto& s : previewSends_)
        if (s.busy) return;
    previewFrame_.active = false;
    endSend(&previewFrame_);
}

HttpServerModule::Send* HttpServerModule::claimSend(platform::TcpConnection& conn) {
    for (auto& a : sends_)
        if (!a.busy) return &a;
    // Every slot busy: answered rather than written whole, which would hold the render loop for the transfer.
    sendResponse(conn, 503, "application/json", "{\"error\":\"every send slot is busy\"}");
    return nullptr;
}

bool HttpServerModule::startTree(Send& s, const Subscription* sub, bool ws, bool hide, uint32_t answers) {
    auto* t = static_cast<TreeSource*>(platform::alloc(sizeof(TreeSource)));
    if (!t) return false;
    new (t) TreeSource;
    if (sub) {
        t->sub = *sub;
        t->cursor.sub = &t->sub;
    }
    t->cursor.answers = answers;
    t->ws = ws;
    t->hide = hide;
    s.block = t;
    s.source = Send::Source::Tree;
    s.busy = true;
    s.lastActivityMs = platform::millis();
    return true;
}

void HttpServerModule::queueFrame(int client, uint8_t opcode, const char* payload, size_t len) {
    Send& s = controlSends_[client];
    auto& ws = wsClients_[client];
    if (!ws.valid()) return;
    uint8_t hdr[10];
    const size_t hl = writeWsFrameHeader(hdr, opcode, len);
    // One frame always queues; more behind it only within the backlog, past which the client has stopped reading and is closed, to reconnect to a fresh document.
    const bool over = s.queuedLen && s.queuedLen + hl + len > kFrameBacklogMax;
    char* grown = over ? nullptr : static_cast<char*>(platform::alloc(s.queuedLen + hl + len));
    if (!grown) {
        endSendSlot(s);
        ws.close();
        return;
    }
    if (s.queued) {
        std::memcpy(grown, s.queued, s.queuedLen);
        platform::free(s.queued);
    }
    std::memcpy(grown + s.queuedLen, hdr, hl);
    if (len) std::memcpy(grown + s.queuedLen + hl, payload, len);
    s.queued = grown;
    s.queuedLen += hl + len;
    if (!s.busy) {
        s.busy = true;
        s.lastActivityMs = platform::millis();
    }
}

void HttpServerModule::endSendSlot(Send& s) {
    if (s.owned) platform::free(s.owned);
    if (s.queued) platform::free(s.queued);
    if (s.block) {
        if (s.source == Send::Source::File) platform::fsReadStreamClose(static_cast<FileSource*>(s.block)->stream);
        platform::free(s.block);
    }
    endSend(&s);
    s.source = Send::Source::None;
    s.busy = s.closing = false;
    s.head = s.body = nullptr;
    s.headLeft = s.bodyLeft = 0;
    s.owned = s.queued = nullptr;
    s.block = nullptr;
    s.queuedLen = 0;
}

HttpServerModule::Refill HttpServerModule::refill(Send& s) {
    if (s.owned) { platform::free(s.owned); s.owned = nullptr; }
    // Queued frames go at a frame boundary, which this is: a Pong may sit between a document's fragments (RFC 6455 section 5.4), and no data frame queues while one streams.
    if (s.queued) {
        s.owned = s.queued;
        s.body = reinterpret_cast<const uint8_t*>(s.queued);
        s.bodyLeft = s.queuedLen;
        s.queued = nullptr;
        s.queuedLen = 0;
        return Refill::More;
    }
    if (s.closing) return Refill::End;
    if (s.source == Send::Source::File) {
        auto* f = static_cast<FileSource*>(s.block);
        if (f->left == 0) return Refill::End;
        const size_t want = f->left < sizeof(f->buf) ? f->left : sizeof(f->buf);
        const int got = platform::fsReadStreamRead(f->stream, f->buf, want);
        if (got <= 0) return Refill::Failed;   // the header promised more: the close tells the reader its body is short
        f->left -= static_cast<size_t>(got);
        s.body = reinterpret_cast<const uint8_t*>(f->buf);
        s.bodyLeft = static_cast<size_t>(got);
        return Refill::More;
    }
    if (s.source == Send::Source::Types) {
        auto* t = static_cast<TypesSource*>(s.block);
        if (t->last) return Refill::End;
        JsonSink sink;
        if (t->next == 0) sink.append("{\"types\":[");
        // One type a refill: building it to read its defaults is the costly part.
        const uint8_t count = ModuleFactory::typeCount();
        while (t->next < count && !ModuleFactory::typeName(t->next)) t->next++;
        if (t->next < count) {
            writeTypeJson(sink, t->next, !t->wrote);
            t->wrote = true;
            t->next++;
        }
        if (t->next >= count) {
            sink.append("]}");
            t->last = true;
        }
        if (sink.overflowed()) return Refill::Failed;
        s.bodyLeft = sink.size();
        s.owned = sink.detach();
        s.body = reinterpret_cast<const uint8_t*>(s.owned);
        return Refill::More;
    }
    if (s.source == Send::Source::Tree) {
        auto* t = static_cast<TreeSource*>(s.block);
        if (t->last) return Refill::End;
        JsonSink sink;
        bool done;
        {
            // Per reader: one on the device's own access point gets its passwords empty, the others as stored.
            const SecretsHidden hide(t->hide);
            done = writeTreeFragment(sink, t->cursor, fragmentBytes());
        }
        // A truncated fragment would end the document in invalid JSON.
        if (sink.overflowed()) { refuseFragment(); return Refill::Failed; }
        s.bodyLeft = sink.size();
        s.owned = sink.detach();
        s.body = reinterpret_cast<const uint8_t*>(s.owned);
        if (t->ws) {
            // RFC 6455 section 5.4: the first frame carries the text opcode, the rest continue it, and the last one sets FIN.
            const uint8_t opcode = static_cast<uint8_t>((t->first ? 0x01 : 0x00) | (done ? 0x80 : 0x00));
            s.head = t->hdr;
            s.headLeft = writeWsFrameHeader(t->hdr, opcode, s.bodyLeft);
            t->first = false;
        }
        t->last = done;
        fragmentRefused_ = false;
        return Refill::More;
    }
    return Refill::End;
}

void HttpServerModule::drainSend(Send& s, platform::TcpConnection& conn, const void* who, size_t budget, uint32_t tickStart) {
    if (!s.busy) return;
    if (!conn.valid()) { endSendSlot(s); return; }
    const uint32_t now = platform::millis();
    // Short of internal RAM a send waits its turn, which is not a stall.
    if (!maySend(who)) { s.lastActivityMs = now; return; }
    // An answer's own connection closes once the answer is out; a WebSocket client's stays open for the next.
    const bool answer = &conn == &s.conn;
    while (budget > 0) {
        if (!s.headLeft && !s.bodyLeft) {
            // A refill writes a fragment or reads a chunk, the costly part, so it waits for the next tick once this one's time is spent.
            if (platform::millis() - tickStart >= kTransferTickBudgetMs) { s.lastActivityMs = now; return; }
            const Refill r = refill(s);
            if (r == Refill::More) continue;
            const bool close = answer || s.closing || r == Refill::Failed;
            endSendSlot(s);
            if (close) conn.close();
            return;
        }
        // A write queues its bytes in internal RAM until acknowledged, so it takes only what lies above the floor WiFi needs to receive; below it the send waits, within the stall window.
        const size_t room = sendRoom();
        if (room == 0) break;
        const bool inHead = s.headLeft > 0;
        const size_t span = std::min({budget, room, inHead ? s.headLeft : s.bodyLeft});
        const int n = conn.writeSome(inHead ? s.head : s.body, span);
        if (n < 0) { endSendSlot(s); conn.close(); return; }   // a real error: drop this client
        if (n == 0) break;                                      // the socket is full: the rest goes next tick
        if (inHead) { s.head += n; s.headLeft -= static_cast<size_t>(n); }
        else { s.body += n; s.bodyLeft -= static_cast<size_t>(n); }
        budget -= static_cast<size_t>(n);
        s.lastActivityMs = now;
        sent(who, now);
    }
    // A liveness guard, not a size or time cap on the answer: only a peer that stopped reading trips it.
    if (now - s.lastActivityMs > kTransferStallMs) { endSendSlot(s); conn.close(); }
}

void HttpServerModule::drainSends() {
    const uint32_t start = platform::millis();
    const size_t chunk = drainChunkBytes();
    {
        // Core 1 arms preview frames under this lease; try_lock, not a wait, since tick20ms must not block, so the preview drains next tick.
        LockGuard lease{wsLock_};
        if (lease) {
            for (int i = 0; i < kMaxPreviewClients; i++) {
                const bool was = previewClients_[i].valid();
                // The frame is one send to the lease, shared by every preview client.
                drainSend(previewSends_[i], previewClients_[i], &previewFrame_, chunk, start);
                // Every close notifies the producer, or the dead slot's standing request steers on.
                if (was && !previewClients_[i].valid() && clientSink_) clientSink_->onClientGone(i);
            }
            finishPreviewFrame();
        }
    }
    // Control clients and answers take turns from a rotating start, so the tick's time budget favors no slot.
    constexpr int kTurns = kMaxWsClients + kSendSlots;
    for (int k = 0; k < kTurns; k++) {
        const int i = (sendTurn_ + k) % kTurns;
        if (i < kMaxWsClients) {
            drainSend(controlSends_[i], wsClients_[i], &controlSends_[i], chunk, start);
        } else {
            Send& a = sends_[i - kMaxWsClients];
            drainSend(a, a.conn, &a, chunk, start);
        }
    }
    sendTurn_ = (sendTurn_ + 1) % kTurns;
}

void HttpServerModule::refuseFragment() {
    // Out of memory for one fragment: the reader is closed, a page reconnects and is sent its document again, and the serial log says why, once per run of refusals.
    if (!fragmentRefused_) std::printf("state stream: no memory for a %u-byte fragment (largest block %u), client closed\n", static_cast<unsigned>(fragmentBytes()), static_cast<unsigned>(platform::maxAllocBlock()));
    fragmentRefused_ = true;
}

size_t HttpServerModule::fragmentBytes() const {
    const size_t chunk = drainChunkBytes();
    return chunk < kFragmentCeilBytes ? chunk : kFragmentCeilBytes;
}

// Per-client chunk cap per tick from free contiguous memory, between a floor (progress) and a ceiling (tick occupancy), so a tight board takes small bites.
size_t HttpServerModule::drainChunkBytes() const {
    constexpr size_t kFloor = 2048;     // always make real progress, even on a fragmented board
    constexpr size_t kCeil  = 65536;    // cap tick occupancy regardless of how much RAM is free
    const size_t block = platform::maxAllocBlock();
    // 0 means unlimited (desktop): no ceiling, since writeSome stops at the socket buffer and TCP paces the drain.
    if (block == 0) return static_cast<size_t>(1) << 30;
    size_t chunk = block / 8;           // a fraction of the largest contiguous block
    if (chunk < kFloor) chunk = kFloor;
    if (chunk > kCeil)  chunk = kCeil;
    return chunk;
}

} // namespace mm
