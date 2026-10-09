/// @defgroup http_server_impl HTTP server implementation
/// The request handlers behind HttpServerModule, and the compatibility shims they serve.
///
/// Public surface and class layout live in HttpServerModule.h, so implementation edits do not cascade-recompile every TU including the header.
///
/// @moreinfo
///
/// ## Why an upload needs two timeouts
///
/// The upload drain blocks rendering on `tick20ms()` until the transfer ends or a bound trips, which is accepted for a user-initiated transient upload.
///
/// `kUploadIdleMs` resets on every read so a steady upload never trips it, and `kUploadHardMs` caps the whole request so a slowloris trickle cannot hold the freeze open.
///
/// The hard cap sits above a legitimate worst case of 256 KB at ~50 KB/s, about 5 s.
///
/// ## Why firmware gets a larger ceiling
///
/// A firmware image is MB-scale and takes minutes over weak WiFi, so it carries its own ceiling: three minutes covers 1.5 MB at 10 KB/s with margin.
///
/// The ceiling stays small because it also bounds the worst-case render freeze.
///
/// ## Why the WLED shim reports a sentinel version
///
/// The `ver` field is a sentinel, not the MoonLight version.
///
/// Home Assistant parses WLED tags as CalVer, so a real `2.1.0-dev` compares lower than `16.0.1` and HA offers an update whose `.bin` would brick the device.
///
/// Under CalVer `99.0.0` outranks any WLED tag, so HA's update check stays silent.
///
/// The real version belongs on the MQTT update topic, where the question is whether MoonLight shipped a release.
///
/// ## Why an Ethernet device sends no wifi object
///
/// The `wifi` block feeds HA's diagnostic sensors, and an Ethernet device has no AP, so `info.wifi` (optional in python-wled) is omitted.
///
/// Omitting it makes HA create no WiFi sensors, as a real WLED on Ethernet does, instead of greyed-out zero rows.
/// @{

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
#include "core/system/ControlModule.h"   // look presets on /presets.json (HA WLED integration)
#include "core/system/FilesystemModule.h"
#include "core/system/FirmwareUpdateModule.h"
#include "core/system/SystemModule.h"      // deviceName() for the WLED /json/info shim
#include "core/moonlive/MoonLiveScriptFile.h"   // kFactoryScriptDir: where a download lands
#include "core/moonlive/script_catalog.h"        // generated: which factory scripts exist
#include "core/util/build_info.h"                      // kVersion: the tag a script is fetched from
#include "core/util/LightOutput.h"   // the device shape and the palettes as colors, for the WLED shim
#include "platform/platform.h"
#include "ui/ui_embedded.h"

#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>   // strtol: bounded Content-Length parse
#include <cctype>    // tolower, case-insensitive header names (findHeaderCI)
#include <cerrno>    // errno / ERANGE: Content-Length overflow check
#include <cstring>
#include <cstdint>

namespace mm {

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

// Static sink for the schema-changed hook: routes a module's rebuildControls() to the live instance's resync flag, like FilesystemModule::noteDirty.
void HttpServerModule::onSchemaChanged() {
    if (instance_) instance_->requestFullResync();
}

void HttpServerModule::onValuesChanged(MoonModule* mod) {
    if (!instance_ || !mod || !mod->name()) return;
    HttpServerModule& self = *instance_;
    for (uint8_t i = 0; i < self.soonCount_; i++)
        if (std::strcmp(self.soon_[i], mod->name()) == 0) return;
    // A full list leaves the rest to the periodic patch, which carries every change anyway.
    if (self.soonCount_ >= kSoonModules) return;
    mm::formatTo(self.soon_[self.soonCount_++], sizeof(self.soon_[0]), "%s", mod->name());
}

void HttpServerModule::release() {
    // Drop the in-flight sends before the clients go: the preview frame borrows its buffer, the state frame owns its JSON body.
    cancelBufferedSend();
    if (stateSend_.active) {
        platform::free(const_cast<uint8_t*>(stateSend_.body));
        stateSend_.body = nullptr;
        stateSend_.active = false;
    }
    for (auto& ws : wsClients_) ws.close();
    for (auto& c : parked_) c.close();
    // Every close site notifies the producer (see cancelBufferedSend), else ghost standing requests keep the driver gathering frames for nobody.
    for (int i = 0; i < MAX_PREVIEW_CLIENTS; i++) {
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
    MoonModule::release();   // chain: uniform override-and-chain (no buffers/children today, but the convention holds)
}

void HttpServerModule::tick20ms() MM_NONBLOCKING {
    // Drain the in-flight preview frame on the 20 ms transport cadence, before accept so a connection burst cannot starve it; sockets stay off the render tick (architecture.md § Parallelism).
    drainPreviewSend();
    drainStateSend();
    // A pending full resync is served here, not on the 1 s tick, so a connect reaches its first preview within tens of ms; the flag gates the expensive serialize.
    if (fullResyncPending_ || soonCount_) {
        const SecretsHidden hide(anyWsClientOnAccessPoint());   // one push serves every client
        if (fullResyncPending_) pushStateToWebSockets();
        // A module that asked goes out now rather than on the next second.
        if (soonCount_) pushSoonPatch();
    }
    // Inbound WS frames: the WLED app sets on/off and brightness with an {on,bri} text frame over /ws.
    pollWledStateFromWebSockets();
    serveConnections();
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

void HttpServerModule::tick1s() MM_NONBLOCKING {
    const SecretsHidden hide(anyWsClientOnAccessPoint());   // one push serves every client
    pushStateToWebSockets();
}

bool HttpServerModule::onAccessPoint(const platform::TcpConnection& conn) {
    uint8_t peer[4] = {};
    return conn.peerIPv4(peer) && captive::fromAccessPoint(peer);
}

bool HttpServerModule::anyWsClientOnAccessPoint() const MM_NONBLOCKING {
    for (int i = 0; i < MAX_WS_CLIENTS; i++)
        if (wsOnAccessPoint_[i] && wsClients_[i].valid()) return true;
    return false;
}

bool HttpServerModule::handleConnection(platform::TcpConnection& conn, int patienceMs) {
    // A client on the device's own access point has not shown it knows the network's password, so it sees no stored one.
    const SecretsHidden hide(onAccessPoint(conn));
    uint8_t local[4] = {};
    conn.localIPv4(local);   // the address it reached, which the captive redirect decides by
    uint8_t buf[2048];
    int totalRead = 0;

    // read() is non-blocking, so a just-accepted connection gets a short bounded wait (~5 ms) for its request; an idle connection costs at most that.
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

    // The body can land a segment after the headers, so retry briefly; one still incomplete gets 400, never a truncated parse into the permissive JSON helpers.
    auto* headerEnd = std::strstr(req, "\r\n\r\n");
    int contentLen = 0;   // declared body length (0 if no Content-Length); used by the streaming route
    bool hasContentLen = false;   // header PRESENT (an explicit 0 is a legitimate empty write)
    if (headerEnd) {
        auto* clh = findHeaderCI(req, "Content-Length:");
        if (clh) {
            hasContentLen = true;
            // Bounded parse, not atoi: strtol with an end pointer rejects non-numeric, trailing junk, ERANGE and negatives, and the ceiling (8 MB) exceeds any firmware image; failures get 400.
            constexpr long kContentLenMax = 8L * 1024 * 1024;
            const char* valStart = clh + 15;
            while (*valStart == ' ' || *valStart == '\t') valStart++;   // skip OWS after the colon
            char* valEnd = nullptr;
            errno = 0;
            const long parsed = std::strtol(valStart, &valEnd, 10);
            const bool consumedDigits = valEnd != valStart;
            const bool endsCleanly = *valEnd == '\r' || *valEnd == '\n' || *valEnd == ' ' ||
                                     *valEnd == '\t' || *valEnd == '\0';
            if (!consumedDigits || !endsCleanly || errno == ERANGE ||
                parsed < 0 || parsed > kContentLenMax) {
                sendResponse(conn, 400, "application/json",
                             "{\"error\":\"invalid content-length\"}");
                return true;
            }
            contentLen = static_cast<int>(parsed);
            int headerSize = static_cast<int>(headerEnd + 4 - req);
            int bodyNeeded = headerSize + contentLen;
            // Only streaming routes pull a body larger than buf off the socket; every other route parses it whole, so an oversized body gets 413, never a truncated read.
            const bool isStreamingRoute =
                std::strncmp(req, "POST /api/file", 14) == 0 ||
                std::strncmp(req, "POST /api/firmware/upload", 25) == 0 ||
                // A MoonBase image (~750 KB) streams too; the trailing space keeps `moonbase-update-url`, a small JSON body read whole, out of the match.
                std::strncmp(req, "POST /api/firmware/moonbase-update ", 35) == 0 ||
                // A state document holding an effects stack outgrows the request buffer; the trailing space keeps a longer path out.
                std::strncmp(req, "PATCH /api/state ", 17) == 0 ||
                std::strncmp(req, "POST /api/state ", 16) == 0;
            if (bodyNeeded > static_cast<int>(sizeof(buf) - 1)) {
                if (!isStreamingRoute) {
                    sendResponse(conn, 413, "application/json",
                                 "{\"error\":\"request body too large\"}");
                    return true;
                }
                bodyNeeded = static_cast<int>(sizeof(buf) - 1);   // streaming: buffer the prefix only
            }
            for (int empties = 0; totalRead < bodyNeeded;) {
                int n = conn.read(buf + totalRead, sizeof(buf) - 1 - totalRead);
                if (n > 0) { totalRead += n; empties = 0; }
                else if (n == 0) break;                    // peer closed
                else { if (++empties > 50) break; platform::delayMs(1); }  // ~50 ms for the body
            }
            buf[totalRead] = 0;
            if (totalRead < bodyNeeded) {                  // body never fully arrived
                sendResponse(conn, 400, "application/json",
                             "{\"error\":\"incomplete request body\"}");
                return true;
            }
        }
    }

    // Parse method and path
    char method[8] = {};
    char path[128] = {};
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
        return true; // don't close: connection is now a WebSocket
    }

    // The body starts after the headers
    char* body = headerEnd ? const_cast<char*>(headerEnd) + 4 : nullptr;

    // A phone on the access point asking for its own captive-check page gets the UI, which is what shows it the sign-in screen.
    if (std::strcmp(method, "GET") == 0) {
        const char* host = findHeaderCI(req, "Host:");
        if (host) { host += 5; while (*host == ' ') host++; }
        if (captive::redirects(local, host, path)) {   // `local` was read when the connection came in
            conn.write(reinterpret_cast<const uint8_t*>(captive::kRedirect), std::strlen(captive::kRedirect));
            return true;
        }
    }

    // Route
    if (std::strcmp(method, "GET") == 0) {
        if (std::strcmp(path, "/") == 0) serveFile(conn, "index.html", "text/html");
        else if (std::strcmp(path, "/app.js") == 0) serveFile(conn, "app.js", "application/javascript");
        else if (std::strcmp(path, "/install-picker.js") == 0) serveFile(conn, "install-picker.js", "application/javascript");
        else if (std::strcmp(path, "/semver.js") == 0) serveFile(conn, "semver.js", "application/javascript");
        else if (std::strcmp(path, "/prism.js") == 0) serveFile(conn, "prism.js", "application/javascript");
        else if (std::strcmp(path, "/preview3d.js") == 0) serveFile(conn, "preview3d.js", "application/javascript");
        else if (std::strcmp(path, "/preview-adapt.js") == 0) serveFile(conn, "preview-adapt.js", "application/javascript");
        else if (std::strcmp(path, "/migrate.js") == 0) serveFile(conn, "migrate.js", "application/javascript");
        else if (std::strcmp(path, "/style.css") == 0) serveFile(conn, "style.css", "text/css");
        else if (std::strcmp(path, "/moonmodules-logo.png") == 0) serveFile(conn, "moonmodules-logo.png", "image/png");
        else if (std::strcmp(path, "/api/state") == 0) serveState(conn);
        else if (std::strcmp(path, "/api/system") == 0) serveSystem(conn);
        else if (std::strcmp(path, "/api/types") == 0) serveTypes(conn);
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
            if (!hasContentLen) { sendResponse(conn, 411, "application/json", "{\"error\":\"length required\"}"); return true; }
            handleApplyState(conn, body, static_cast<size_t>(totalRead) - static_cast<size_t>(body - req),
                             static_cast<size_t>(contentLen));
        } else if (std::strcmp(path, "/api/file") == 0 && body) {
            // File Manager: POST /api/file?path=<rel> streams the body to an atomic write; `body` holds the buffered bytes and handleWriteFile pulls the rest off the socket.
            const size_t initialLen = static_cast<size_t>(totalRead) - static_cast<size_t>(body - req);
            // No Content-Length (chunked) is 411, since acting on it would commit an empty file; keyed on header absence, so an explicit `Content-Length: 0` stays a valid empty write.
            if (!hasContentLen) {
                sendResponse(conn, 411, "application/json", "{\"error\":\"length required\"}");
                return true;
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
        } else if (isMoveRoute && body) {
            char nameBuf[32] = {};
            size_t nameLen = pathLen - 13 - 5;  // strip "/api/modules/" prefix and "/move" suffix
            // Reject rather than truncate: a truncated name could match a different module than the client intended.
            if (nameLen >= sizeof(nameBuf)) {
                sendResponse(conn, 400, "application/json", "{\"error\":\"module name too long\"}");
            } else {
                std::memcpy(nameBuf, path + 13, nameLen);
                nameBuf[nameLen] = 0;
                handleMoveModule(conn, nameBuf, body);
            }
        } else if (isReplaceRoute && body) {
            char nameBuf[32] = {};
            size_t nameLen = pathLen - 13 - 8;  // strip "/api/modules/" prefix and "/replace" suffix
            if (nameLen >= sizeof(nameBuf)) {
                sendResponse(conn, 400, "application/json", "{\"error\":\"module name too long\"}");
            } else {
                std::memcpy(nameBuf, path + 13, nameLen);
                nameBuf[nameLen] = 0;
                handleReplaceModule(conn, nameBuf, body);
            }
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
                return true;
            }
            handleMoonBaseUpload(conn, body, initialLen, static_cast<size_t>(contentLen));
        } else if (std::strcmp(path, "/api/firmware/upload") == 0 && body) {
            // OTA from an uploaded .bin body: the browser POSTs the image to the device, which streams it into the OTA partition like /api/file.
            const size_t initialLen = static_cast<size_t>(totalRead) - static_cast<size_t>(body - req);
            handleFirmwareUpload(conn, body, initialLen, static_cast<size_t>(contentLen));
        } else {
            sendResponse(conn, 404, "text/plain", "Not found");
        }
    } else if (std::strcmp(method, "PATCH") == 0) {
        // Editable list: PATCH /api/list/<module>/<control>/<id> edits one row: a field ({"field":F,"value":V}) or a reorder ({"to":N}).
        if (std::strcmp(path, "/api/state") == 0 && body) {
            if (!hasContentLen) { sendResponse(conn, 411, "application/json", "{\"error\":\"length required\"}"); return true; }
            handleApplyState(conn, body, static_cast<size_t>(totalRead) - static_cast<size_t>(body - req),
                             static_cast<size_t>(contentLen));
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
            // File Manager: DELETE /api/dir?path=<rel> → remove a file or empty dir.
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
    return true;
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

void HttpServerModule::sendResponse(platform::TcpConnection& conn, int status, const char* contentType, const char* body) {
    const char* statusText =
        status == 200 ? "OK" :
        status == 202 ? "Accepted" :
        status == 400 ? "Bad Request" :
        status == 404 ? "Not Found" :
        status == 405 ? "Method Not Allowed" :
        status == 409 ? "Conflict" :
        status == 500 ? "Internal Server Error" :
        status == 501 ? "Not Implemented" :
        "Error";
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
static constexpr size_t kUploadMax = 256 * 1024;   // 256 KB: sanity bound on one upload

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
    if (FilesystemModule::removeTree(path)) {
        // A removed file is a change to persistent state like a written one: a module derived from it must notice the file is gone.
        applyFileChanged(path);
        sendResponse(conn, 200, "application/json", "{\"ok\":true}");
    } else {
        sendResponse(conn, 500, "application/json", "{\"error\":\"delete failed\"}");
    }
}

// Stream a file to the socket in 1 KB chunks with a Content-Length: no whole-file buffer, and NUL-safe where sendResponse's strlen is not; `extraHeaders` carries caller lines.
void HttpServerModule::streamFsFile(platform::TcpConnection& conn, const char* path,
                                    const char* mime, const char* extraHeaders) {
    // The saved config holds the WiFi passwords in plain text, which a client on the access point does not see.
    if (SecretsHidden::active() && std::strstr(path, "/.config")) {
        sendResponse(conn, 403, "application/json", "{\"error\":\"config files are not served through the access point\"}");
        return;
    }
    const long size = platform::fsSize(path);
    if (size < 0) { sendResponse(conn, 404, "application/json", "{\"error\":\"not found\"}"); return; }
    char header[224];
    const int hn = std::snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %ld\r\n%s"
        "Connection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n", mime, size, extraHeaders);
    if (!conn.write(reinterpret_cast<const uint8_t*>(header), static_cast<size_t>(hn))) return;
    char chunk[1024];
    for (long offset = 0; offset < size;) {
        const size_t want = static_cast<size_t>(size - offset) < sizeof(chunk)
                          ? static_cast<size_t>(size - offset) : sizeof(chunk);
        const int got = platform::fsReadAt(path, offset, chunk, want);
        if (got <= 0) break;   // read error / early EOF: the client sees a short (truncated) body
        // write() fails on a socket error or its deadline; stop, since retrying every remaining chunk would burn a deadline each on the render thread.
        if (!conn.write(reinterpret_cast<const uint8_t*>(chunk), static_cast<size_t>(got))) return;
        offset += got;
    }
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
        char header[224];
        const int hn = std::snprintf(header, sizeof(header),
            "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
            "Cache-Control: no-cache\r\nConnection: close\r\n"
            "Access-Control-Allow-Origin: *\r\n\r\n", mime, ramLen);
        // One write, not streamFsFile's 1 KB loop, since chunking a resident RAM segment only gives each piece a fresh deadline; release on every exit.
        if (hn > 0 && hn < static_cast<int>(sizeof(header)) &&
            conn.write(reinterpret_cast<const uint8_t*>(header), static_cast<size_t>(hn))) {
            conn.write(ram, ramLen);
        }
        platform::hlsSegmentRelease();
        return;
    }
    streamFsFile(conn, path, mime, "Cache-Control: no-cache\r\n");
}

// Source for the streamed upload: the body bytes already in the request buffer, then the rest straight off the socket, so the device never holds the whole upload in RAM.
namespace {
// The drain blocks rendering, and the two bounds cap for how long: idle scales with a steady transfer, hard caps the total against a slowloris. @xref{why-an-upload-needs-two-timeouts}
constexpr uint32_t kUploadIdleMs = 5000;    // max gap between successful reads before abort
constexpr uint32_t kUploadHardMs = 60000;   // absolute whole-request ceiling (anti-slowloris)
// A firmware push is MB-scale and bounds a SLOW transfer where kUploadIdleMs bounds a stalled one, kept tight because it caps the render freeze. @xref{why-firmware-gets-a-larger-ceiling}
constexpr uint32_t kFirmwareUploadHardMs = 180000;  // 3 min absolute ceiling for a firmware push
struct UploadSource {
    platform::TcpConnection* conn;
    const char* initial;      // body bytes already read into the request buffer
    size_t initialLeft;       // how many of those remain to hand out
    size_t remaining;         // total body bytes still to deliver (Content-Length − delivered)
    uint32_t hardDeadline;    // absolute millis by which the whole body must arrive
};
size_t uploadPull(char* out, size_t cap, void* user, bool* abort) {
    auto* s = static_cast<UploadSource*>(user);
    if (s->remaining == 0) return 0;   // all body delivered → clean EOF
    // Whole-request ceiling, checked on every pull: a paced trickler keeping one byte ready would never trip a cap tested only in the wait loop.
    if (static_cast<int32_t>(platform::millis() - s->hardDeadline) >= 0) { *abort = true; return 0; }
    // Drain the already-buffered prefix first.
    if (s->initialLeft) {
        const size_t n = s->initialLeft < cap ? s->initialLeft : cap;
        std::memcpy(out, s->initial, n);
        s->initial += n; s->initialLeft -= n; s->remaining -= n;
        return n;
    }
    // Then the socket, bounded by the idle and hard deadlines; an early close or lapse sets *abort so the temp file is discarded, while 0 is a clean end.
    const size_t want = s->remaining < cap ? s->remaining : cap;
    const uint32_t deadline = platform::millis() + kUploadIdleMs;
    for (;;) {
        const int r = s->conn->read(reinterpret_cast<uint8_t*>(out), want);
        if (r > 0) { s->remaining -= static_cast<size_t>(r); return static_cast<size_t>(r); }
        if (r == 0) { *abort = true; return 0; }                 // peer closed with body remaining
        // Idle timeout; the hard cap is enforced at the top of uploadPull, so it covers the pacing case this wait loop cannot.
        if (static_cast<int32_t>(platform::millis() - deadline) >= 0) { *abort = true; return 0; }
        platform::delayMs(1);
    }
}
}  // namespace


// The whole body into `out` (contentLen + 1 bytes): the buffered prefix, then the socket under the upload limits; false when it stopped short.
static bool pullWholeBody(platform::TcpConnection& conn, const char* initialBody, size_t initialLen, size_t contentLen, char* out) {
    const size_t initial = initialLen < contentLen ? initialLen : contentLen;
    UploadSource src{&conn, initialBody, initial, contentLen, platform::millis() + kUploadHardMs};
    size_t got = 0;
    bool abort = false;
    for (size_t n = 1; got < contentLen && !abort && n > 0; got += n) n = uploadPull(out + got, contentLen - got, &src, &abort);
    out[got] = 0;
    return got >= contentLen;
}

void HttpServerModule::handleApplyState(platform::TcpConnection& conn, const char* initialBody, size_t initialLen,
                                        size_t contentLen) {
    if (!scheduler_) { sendResponse(conn, 503, "application/json", "{\"error\":\"not ready\"}"); return; }
    if (contentLen > kStateDocumentMax) { sendResponse(conn, 413, "application/json", "{\"error\":\"document too large\"}"); return; }
    char* doc = static_cast<char*>(platform::alloc(contentLen + 1));
    if (!doc) { sendResponse(conn, 507, "application/json", "{\"error\":\"out of memory\"}"); return; }
    if (!pullWholeBody(conn, initialBody, initialLen, contentLen, doc)) {
        platform::free(doc);
        sendResponse(conn, 400, "application/json", "{\"error\":\"incomplete request body\"}");
        return;
    }
    const StateDocumentResult r = applyStateDocument(*scheduler_, doc);
    platform::free(doc);
    // A growing sink, since a path of escaped names has no small fixed bound.
    JsonSink sink;
    writeStateResult(sink, r);
    sendResponse(conn, r.ok ? 200 : 400, "application/json", sink.overflowed() ? "{\"error\":\"out of memory\"}" : sink.data());
}

void HttpServerModule::handleWriteFile(platform::TcpConnection& conn, const char* query,
                                       const char* initialBody, size_t initialLen, size_t contentLen) {
    char path[160];
    if (!parseFilePath(query, path, sizeof(path))) {
        sendResponse(conn, 400, "application/json", "{\"error\":\"bad path\"}");
        return;
    }
    if (contentLen > kUploadMax) {
        sendResponse(conn, 413, "application/json", "{\"error\":\"file too large\"}");
        return;
    }
    // Reject up front if it would not fit the free space, rather than failing mid-write; free is taken conservatively, with no credit for an overwrite.
    const size_t total = platform::filesystemTotal();
    const size_t used = platform::filesystemUsed();
    const size_t freeBytes = total > used ? total - used : 0;
    if (total > 0 && contentLen > freeBytes) {
        char msg[96];
        std::snprintf(msg, sizeof(msg), "{\"error\":\"not enough space (%lu free)\"}",
                      static_cast<unsigned long>(freeBytes));
        sendResponse(conn, 507, "application/json", msg);   // 507 Insufficient Storage
        return;
    }
    // Hand the source at most Content-Length of the buffered bytes, since a pipelined next request may follow the body and must not land in the file.
    const size_t initial = initialLen < contentLen ? initialLen : contentLen;
    UploadSource src{&conn, initialBody, initial, contentLen,
                     platform::millis() + kUploadHardMs};
    if (platform::fsWriteStream(path, &uploadPull, &src)) {
        applyFileChanged(path);   // the write succeeded, so what was built from it may be stale
        sendResponse(conn, 200, "application/json", "{\"ok\":true}");
    } else {
        sendResponse(conn, 500, "application/json", "{\"error\":\"write failed\"}");
    }
}

// See the header for WHY this exists and why it is whole-tree. Here is only the how.
void HttpServerModule::applyFileChanged(const char* path) {
    // A written /.config/<Type>.json applies live, queued for the render task: setup() here crashed the ESP32 and a network apply could cut the socket before the response.
    if (auto* fs = FilesystemModule::instance()) fs->requestConfigApply(path);
    // A user script of a name the firmware also ships is a fork; record its origin here, where every writer passes, so the binding can flag an updated shipped copy.
    moonlive::noteForkedFrom(path);
    if (!scheduler_) return;
    scheduler_->notifyFileChanged(path);
    // requestPrepareTree, never prepareTree: the flag is consumed at the frame boundary, so a multi-file upload costs one sweep.
    scheduler_->requestPrepareTree();
}

// OTA from an uploaded .bin body: stream it into the OTA partition (platform::otaWriteStream) with the same uploadPull as file upload; the 200 goes out before the reboot.
void HttpServerModule::handleFirmwareUpload(platform::TcpConnection& conn, const char* initialBody,
                                            size_t initialLen, size_t contentLen) {
    if constexpr (!platform::hasOta) {
        sendResponse(conn, 501, "application/json", "{\"error\":\"OTA not supported on this platform\"}");
        return;
    }
    // Same 409 concurrency guard as handleFirmwareUrl: one OTA at a time (both write g_ota* state).
    if (otaInFlight()) {
        sendResponse(conn, 409, "application/json", "{\"error\":\"ota already in progress\"}");
        return;
    }
    const size_t initial = initialLen < contentLen ? initialLen : contentLen;
    // Firmware gets the MB-scale ceiling kFirmwareUploadHardMs: a 1.5 MB push over WiFi outruns kUploadHardMs and would abort mid-flash.
    UploadSource src{&conn, initialBody, initial, contentLen,
                     platform::millis() + kFirmwareUploadHardMs};
    g_otaBytesTotal = static_cast<uint32_t>(contentLen);   // the UI's "Y KB" (Content-Length up front)
    g_otaBytesRead = 0;                                    // clear any stale count from a prior OTA
    // otaWriteStream commits the image and flips the boot pointer without rebooting, so the 200 goes out first and the browser sees a clean "flashed" instead of an aborted socket.
    const bool ok = platform::otaWriteStream(&uploadPull, &src, contentLen,
                                             g_otaStatus, sizeof(g_otaStatus), &g_otaBytesRead);
    if (!ok) {
        char msg[96];
        std::snprintf(msg, sizeof(msg), "{\"error\":\"ota failed: %.60s\"}", g_otaStatus);
        sendResponse(conn, 500, "application/json", msg);
        return;
    }
    FilesystemModule::flushPending();
    sendResponse(conn, 200, "application/json", "{\"ok\":true}");
    conn.close();
    platform::delayMs(200);
    platform::reboot();  // noreturn: boots the flashed image
}

void HttpServerModule::handleMoonBaseUpload(platform::TcpConnection& conn, const char* initialBody,
                                            size_t initialLen, size_t contentLen) {
    if constexpr (!platform::hasOta) {
        sendResponse(conn, 501, "application/json", "{\"error\":\"OTA not supported on this platform\"}");
        return;
    }
    if (!platform::otaHasMoonBase()) {
        sendResponse(conn, 409, "application/json", "{\"error\":\"no MoonBase on this device\"}");
        return;
    }
    // Running from MoonBase means the factory slot is executing and cannot be written; this check exists for the message, while otaWriteMoonBase is the real guard and stays.
    if (platform::otaRunningMoonBase()) {
        sendResponse(conn, 409, "application/json",
                     "{\"error\":\"running from MoonBase, boot the app first\"}");
        return;
    }
    if (otaInFlight()) {
        sendResponse(conn, 409, "application/json", "{\"error\":\"ota already in progress\"}");
        return;
    }
    const size_t initial = initialLen < contentLen ? initialLen : contentLen;
    UploadSource src{&conn, initialBody, initial, contentLen,
                     platform::millis() + kFirmwareUploadHardMs};
    g_otaBytesTotal = static_cast<uint32_t>(contentLen);
    g_otaBytesRead = 0;
    const bool ok = platform::otaWriteMoonBase(&uploadPull, &src, contentLen,
                                               g_otaStatus, sizeof(g_otaStatus), &g_otaBytesRead);
    if (!ok) {
        // Drain before answering: replying while ~750 KB is in flight shows the peer a reset instead of the reason, and uploadPull's deadlines bound the drain.
        char discard[512];
        for (bool done = false; !done;) {
            bool abort = false;
            if (uploadPull(discard, sizeof(discard), &src, &abort) == 0 || abort) done = true;
        }
        char msg[128];
        std::snprintf(msg, sizeof(msg), "{\"error\":\"%.90s\"}", g_otaStatus);
        sendResponse(conn, 500, "application/json", msg);
        return;
    }
    // No reboot: the running app is untouched; re-read then re-bind, since the card's version buffer is filled at setup.
    if (auto* fw = static_cast<FirmwareUpdateModule*>(findModuleByName("Firmware"))) {
        fw->readMoonBaseVersion();
        fw->rebuildControls();
    }
    sendResponse(conn, 200, "application/json", "{\"ok\":true}");
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

void HttpServerModule::serveFile(platform::TcpConnection& conn, const char* filename, const char* contentType) {
    // Try disk first (desktop development: live editing without rebuild)
    char filepath[256];
    std::snprintf(filepath, sizeof(filepath), "%s/%s", uiPath_, filename);

    FILE* f = std::fopen(filepath, "rb");
    if (f) {
        std::fseek(f, 0, SEEK_END);
        long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);

        char header[256];
        int headerLen = std::snprintf(header, sizeof(header),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %ld\r\n"
            "Connection: close\r\n"
            "Cache-Control: no-cache\r\n"
            "\r\n",
            contentType, size);
        if (!conn.write(reinterpret_cast<const uint8_t*>(header), headerLen)) { std::fclose(f); return; }

        uint8_t chunk[1024];
        while (size > 0) {
            size_t toRead = size > static_cast<long>(sizeof(chunk)) ? sizeof(chunk) : static_cast<size_t>(size);
            size_t bytesRead = std::fread(chunk, 1, toRead, f);
            if (bytesRead == 0) break;
            // Stop on a write failure (socket error or deadline), else every remaining chunk burns a deadline of render-thread time.
            if (!conn.write(chunk, bytesRead)) break;
            size -= static_cast<long>(bytesRead);
        }
        std::fclose(f);
        return;
    }

    // Fall back to embedded data (ESP32 or no disk files): text assets are embedded gzipped (see embed_ui.cmake) and served with Content-Encoding: gzip, the PNG raw.
    const uint8_t* data = nullptr;
    size_t dataLen = 0;
    bool gzipped = false;
    if (std::strcmp(filename, "index.html") == 0) { data = ui::indexHtml; dataLen = ui::indexHtmlLen; gzipped = true; }
    else if (std::strcmp(filename, "app.js") == 0) { data = ui::appJs; dataLen = ui::appJsLen; gzipped = true; }
    else if (std::strcmp(filename, "install-picker.js") == 0) { data = ui::installPickerJs; dataLen = ui::installPickerJsLen; gzipped = true; }
    else if (std::strcmp(filename, "semver.js") == 0) { data = ui::semverJs; dataLen = ui::semverJsLen; gzipped = true; }
    else if (std::strcmp(filename, "prism.js") == 0) { data = ui::prismJs; dataLen = ui::prismJsLen; gzipped = true; }
    else if (std::strcmp(filename, "preview3d.js") == 0) { data = ui::preview3dJs; dataLen = ui::preview3dJsLen; gzipped = true; }
    else if (std::strcmp(filename, "preview-adapt.js") == 0) { data = ui::previewAdaptJs; dataLen = ui::previewAdaptJsLen; gzipped = true; }
    else if (std::strcmp(filename, "migrate.js") == 0) { data = ui::migrateJs; dataLen = ui::migrateJsLen; gzipped = true; }
    else if (std::strcmp(filename, "style.css") == 0) { data = ui::styleCss; dataLen = ui::styleCssLen; gzipped = true; }
    else if (std::strcmp(filename, "moonmodules-logo.png") == 0) { data = ui::logoPng; dataLen = ui::logoPngLen; }

    if (!data) {
        sendResponse(conn, 404, "text/plain", "File not found");
        return;
    }

    char header[256];
    int headerLen = std::snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "%s"
        "Connection: close\r\n"
        "Cache-Control: no-cache\r\n"
        "\r\n",
        contentType, dataLen,
        gzipped ? "Content-Encoding: gzip\r\n" : "");
    conn.write(reinterpret_cast<const uint8_t*>(header), headerLen);
    conn.write(data, dataLen);
}

void HttpServerModule::serveState(platform::TcpConnection& conn) {
    const char* header =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n";
    conn.write(reinterpret_cast<const uint8_t*>(header), std::strlen(header));

    JsonSink sink(conn);
    buildStateJson(sink);
    sink.flush();
}

void HttpServerModule::buildStateJson(JsonSink& sink) {
    sink.append("{\"modules\":[");

    if (scheduler_) {
        bool first = true;
        for (uint8_t m = 0; m < scheduler_->moduleCount(); m++) {
            auto* mod = scheduler_->module(m);
            // Skip modules whose appearsInUi() is false (HttpServerModule, FilesystemModule): the one mechanism for "not a card in /api/state".
            if (!mod || !mod->appearsInUi()) continue;
            if (!first) sink.append(",");
            first = false;
            writeModuleJson(sink, mod);
        }
    }

    sink.append("]}");
}

// The diff-on-the-wire cache keeps an 8-byte FNV-1a {path,value} hash per leaf, not the value; a collision skips one update at worst.

// The diff-on-the-wire core: visit every UI leaf the periodic push sends, in buildStateJson order, as fn(pathHash, valueHash, path, valueSink); names are unique tree-wide, so "<module>/<name>" is stable.
template <class Fn>
void HttpServerModule::forEachStateLeaf(Fn&& fn) {
    if (!scheduler_) return;
    // `fn`, not std::forward: forwarding inside a loop moves the callable on the first module, leaving later modules a moved-from object.
    for (uint8_t m = 0; m < scheduler_->moduleCount(); m++)
        if (auto* mod = scheduler_->module(m))
            if (mod->appearsInUi()) visitModuleLeaves(mod, fn);
}

template <class Fn>
void HttpServerModule::visitModuleLeaves(MoonModule* mod, Fn&& fn) {
    char path[80];
    // Header telemetry the UI shows per card, `@`-prefixed so it cannot collide with a control name; only fields that change per tick (timing, memory) ride the patch.
    auto leaf = [&](const char* fieldPath, const char* valueJson) {
        JsonSink vs; vs.append(valueJson);
        fn(fnv1a(fieldPath, std::strlen(fieldPath)), fnv1a(vs.data(), vs.size()), fieldPath, vs);
    };
    char num[24];
    std::snprintf(path, sizeof(path), "%s/@tickTimeUs", mod->name());
    std::snprintf(num, sizeof(num), "%u", static_cast<unsigned>(mod->tickTimeUs())); leaf(path, num);
    std::snprintf(path, sizeof(path), "%s/@dynamicBytes", mod->name());
    std::snprintf(num, sizeof(num), "%u", static_cast<unsigned>(mod->dynamicBytes())); leaf(path, num);
    // Status and severity ride the patch too, since a driver can fault at any moment and only the diff push runs every second; an unchanged value costs nothing.
    {
        JsonSink sv;
        // writeJsonString already emits the quotes and escapes, so add none: manual quotes make invalid JSON that the browser rejects as a whole patch frame.
        sv.writeJsonString(mod->status() ? mod->status() : "");
        std::snprintf(path, sizeof(path), "%s/@status", mod->name());
        leaf(path, sv.data());
    }
    {
        static const char* sevStr[] = {"status", "warning", "error"};
        JsonSink sv;
        sv.appendf("\"%s\"", sevStr[static_cast<int>(mod->severity())]);
        std::snprintf(path, sizeof(path), "%s/@severity", mod->name());
        leaf(path, sv.data());
    }
    // Each control's value.
    auto& ctrls = mod->controls();
    for (uint8_t i = 0; i < ctrls.count(); i++) {
        auto& c = ctrls[i];
        std::snprintf(path, sizeof(path), "%s/%s", mod->name(), c.name);
        JsonSink vs;
        if (c.type == ControlType::Password) writeObfuscatedPassword(vs, static_cast<const char*>(c.ptr));   // as the full state shows it
        else writeControlValue(vs, c);
        fn(fnv1a(path, std::strlen(path)), fnv1a(vs.data(), vs.size()), path, vs);
    }
    // `fn`, not std::forward, for the same reason as the caller: forwarding in a loop moves the callable into the first child.
    for (uint8_t i = 0; i < mod->childCount(); i++)
        if (auto* ch = mod->child(i)) visitModuleLeaves(ch, fn);
}

// Look up a leaf's cached value-hash by path-hash, or nullptr; a linear scan over the flat cache is a handful of int compares for ~92 leaves.
HttpServerModule::LeafHash* HttpServerModule::findLeaf(uint32_t pathHash) {
    for (uint16_t i = 0; i < leafHashCount_; i++)
        if (leafHashes_[i].path == pathHash) return &leafHashes_[i];
    return nullptr;
}

void HttpServerModule::baselineLeafHashes() {
    // Count the leaves, size the buffer exactly, then fill from scratch (resize does not preserve); off the hot path, on a full-state resync only.
    uint16_t n = 0;
    forEachStateLeaf([&](uint32_t, uint32_t, const char*, JsonSink&) { n++; });
    leafHashes_.resize(n);
    leafHashCount_ = 0;
    forEachStateLeaf([&](uint32_t ph, uint32_t vh, const char*, JsonSink&) {
        if (leafHashCount_ < leafHashes_.count()) leafHashes_[leafHashCount_++] = {ph, vh};
    });
}

uint16_t HttpServerModule::buildStatePatch(JsonSink& sink) {
    return buildPatch(sink, [&](auto&& fn) { forEachStateLeaf(fn); });
}

uint16_t HttpServerModule::buildSoonPatch(JsonSink& sink) {
    const uint16_t changed = buildPatch(sink, [&](auto&& fn) {
        for (uint8_t i = 0; i < soonCount_; i++)
            // No appearsInUi test: the interface drops a patch for a module it never saw.
            if (MoonModule* mod = scheduler_ ? scheduler_->firstByName(soon_[i]) : nullptr) visitModuleLeaves(mod, fn);
    });
    soonCount_ = 0;
    return changed;
}

void HttpServerModule::pushSoonPatch() {
    // A small frame written into a full state mid-drain would land inside that message, so it waits.
    if (fullResyncPending_ || stateSend_.active) return;
    const uint32_t now = platform::millis();
    if (now - soonSentMs_ < kSoonMs) return;
    soonSentMs_ = now;
    JsonSink sink;
    // The leaves' hashes update as they go, so the second's patch does not send them again.
    if (buildSoonPatch(sink) > 0) sendPatch(sink);
}

void HttpServerModule::sendPatch(const JsonSink& sink) {
    for (auto& ws : wsClients_) {
        if (!ws.valid()) continue;
        if (!sendWsTextFrame(ws, sink.data(), static_cast<int>(sink.size()))) ws.close();
    }
}

template <class Walk>
uint16_t HttpServerModule::buildPatch(JsonSink& sink, Walk&& walk) {
    sink.append("{\"patch\":[");
    uint16_t changed = 0;
    walk([&](uint32_t ph, uint32_t vh, const char* path, JsonSink& vs) {
        LeafHash* h = findLeaf(ph);
        if (h && h->value == vh) return;              // unchanged: the common case, emit nothing
        if (h) h->value = vh;                          // known leaf, value changed → update cache
        // A leaf missing from the baseline cannot happen, since every structural mutation re-baselines first; emit it without growing the cache, because ScratchBuffer::resize is non-preserving and would discard every hash.
        if (changed++) sink.append(",");
        sink.append("{\"path\":\"");
        sink.append(path);
        sink.append("\",\"value\":");
        sink.append(vs.data());                        // the already-serialized value
        sink.append("}");
    });
    sink.append("]}");
    return changed;
}

void HttpServerModule::writeModuleJson(JsonSink& sink, MoonModule* mod) {
    // Per-module header: name, role, enabled, tickTimeUs (fps/ms display), classSize (static C++ object bytes) + dynamicBytes (heap), controls
    const char* roleStr = roleName(mod->role());
    const char* type = mod->typeName();
    if (!type) type = "";
    // `enabled` is published for every module, and the ones that keep running regardless say so here, so the UI can leave out a switch that would do nothing.
    sink.appendf(
        "{\"name\":\"%s\",\"type\":\"%s\",\"role\":\"%s\",\"enabled\":%s,"
        "\"respectsEnabled\":%s,"
        "\"tickTimeUs\":%u,\"classSize\":%u,\"dynamicBytes\":%u",
        mod->name() ? mod->name() : "",
        type,
        roleStr,
        mod->enabledSetting() ? "true" : "false",
        mod->respectsEnabled() ? "true" : "false",
        static_cast<unsigned>(mod->tickTimeUs()),
        static_cast<unsigned>(mod->classSize()),
        static_cast<unsigned>(mod->dynamicBytes()));
    // The flag as set, and whether safe mode holds the module anyway, so the UI shows why its switch does nothing.
    if (mod->held()) sink.append(",\"held\":true");
    // The instance's own tags (a scripted module's come from its script), since /api/types answers per type; emitted only when non-empty.
    if (const char* tg = mod->tags(); tg && tg[0]) {
        sink.append(",\"tags\":");
        sink.writeJsonString(tg);
    }
    writeStatus(sink, mod);
    // userEditable is omitted when true (the UI treats absent as editable); modules that opt out, like PreviewDriver, hide their delete/replace affordance.
    if (!mod->userEditable()) sink.append(",\"userEditable\":false");
    sink.append(",\"controls\":[");
    writeControls(sink, mod);
    sink.append("]");

    // Children
    uint8_t cc = mod->childCount();
    if (cc > 0) {
        sink.append(",\"children\":[");
        for (uint8_t i = 0; i < cc; i++) {
            if (i > 0) sink.append(",");
            writeModuleJson(sink, mod->child(i));
        }
        sink.append("]");
    }

    sink.append("}");
}

void HttpServerModule::writeStatus(JsonSink& sink, MoonModule* mod) {
    // Only emit when the module has a status; severity strings are stable wire format ("status", "warning", "error"), documented in HttpServerModule.md.
    const char* s = mod->status();
    if (!s) return;
    static const char* sevStr[] = {"status", "warning", "error"};
    // writeJsonString emits its own quotes and escapes a `"` or `\` that a raw %s would turn into invalid JSON; severity is a fixed vocabulary and stays a plain %s.
    sink.append(",\"status\":");
    sink.writeJsonString(s);
    sink.appendf(",\"severity\":\"%s\"", sevStr[static_cast<int>(mod->severity())]);
}

void HttpServerModule::writeControls(JsonSink& sink, MoonModule* mod) {
    auto& ctrls = mod->controls();
    for (uint8_t i = 0; i < ctrls.count(); i++) {
        if (i > 0) sink.append(",");
        auto& c = ctrls[i];
        // Common wrapper for every control; per-type VALUE + EXTRAS live in Control.cpp, but Password is handled here since its API form is XOR-obfuscated + base64 and writeControlValue stays sink-neutral.
        sink.appendf("{\"name\":\"%s\",\"type\":\"%s\",\"value\":",
                     c.name, controlTypeName(c.type));
        if (c.type == ControlType::Password) {
            // Not readable at a glance in `curl /api/state`, and the UI's hold-to-peek reverses it.
            writeObfuscatedPassword(sink, static_cast<char*>(c.ptr));
        } else {
            writeControlValue(sink, c);
        }
        writeControlMetadata(sink, c);
        // Emit optional flags only when set (common case is false; omit to save bytes).
        if (c.readonly) sink.append(",\"readonly\":true");
        if (c.minMode) sink.appendf(",\"minMode\":%u", static_cast<unsigned>(c.minMode));   // the mode the UI needs before it shows this
        if (c.numberField) sink.append(",\"numberField\":true");   // render a plain number input, not a slider
        if (c.hex) sink.append(",\"hex\":true");   // that number input reads and writes hexadecimal
        if (c.switchRow) sink.append(",\"switchRow\":true");
        if (c.displayStrip) sink.append(",\"displayStrip\":true");
        // The target rides with all three surface kinds, since a switch drives something too (switch1 is the global on/off) and its popup should say so.
        if (c.fader || c.encoder || c.switchRow) {
            if (c.fader)        sink.append(",\"fader\":true");
            else if (c.encoder) sink.append(",\"encoder\":true");
            if (c.surfaceTarget) { sink.append(",\"target\":"); sink.writeJsonString(c.surfaceTarget); }
        }
        // An editable List shows add/delete/reorder and inline row editors; its rows carry a stable id and field descriptors.
        if (c.type == ControlType::List) {
            const auto* ls = static_cast<const ListSource*>(c.ptr);
            if (ls && ls->isEditableList()) sink.append(",\"editable\":true");
            if (ls && ls->listRowsFixed()) sink.append(",\"fixedRows\":true");
            if (ls && ls->listAsPads()) {
                sink.append(",\"pads\":true");
                const uint8_t gc = ls->listGridCols(), gr = ls->listGridRows();
                if (gc && gr) sink.appendf(",\"gridCols\":%u,\"gridRows\":%u",
                                           static_cast<unsigned>(gc), static_cast<unsigned>(gr));
            }
        }
        sink.append(c.hidden ? ",\"hidden\":true}" : "}");
    }
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

// WLED-compatibility `/json/info`: the field set the WLED apps and HA validate after `_wled._tcp` discovery, with `brand:"WLED"` to interoperate and `product:"MoonModules"` to say what this is.
void HttpServerModule::serveWledInfo(platform::TcpConnection& conn) {
    const char* header =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n";
    conn.write(reinterpret_cast<const uint8_t*>(header), std::strlen(header));

    // Identity: the deviceName (from SystemModule), the live IP, the MAC.
    const char* name; uint8_t mac[6]; uint8_t ip[4];
    resolveWledIdentity(name, mac, ip);
    (void)ip;  // serveWledInfo doesn't need the IP; keep the call uniform.

    // The WLED-Android app requires only `name`, `leds{}`, `wifi{}` and a non-empty `mac` (model/wledapi/Info.kt), else it drops the device silently, so these are the smallest shapes.
    JsonSink sink(conn);
    writeWledInfoBody(sink, name, mac);
    sink.flush();
}

// /presets.json: look presets in WLED's format, keyed by 1-based slot (python-wled discards 0) with a `{"0":{}}` floor; Drivers and Layouts presets stay out since they rewire pins or geometry.
void HttpServerModule::serveWledPresets(platform::TcpConnection& conn) {
    auto* control = static_cast<ControlModule*>(findModuleByName("Control"));
    if (!control) { sendResponse(conn, 200, "application/json", "{\"0\":{}}"); return; }

    JsonSink sink;
    sink.append("{");
    bool any = false;
    for (uint8_t i = 0; i < control->presetCount(); i++) {
        if (!control->isLookOnly(i)) continue;
        const char* name = control->presetName(i);
        if (!name || !name[0]) continue;
        // Slot+1: WLED slot 0 is reserved, and python-wled drops it.
        sink.appendf("%s\"%u\":{\"n\":", any ? "," : "", static_cast<unsigned>(i + 1));
        sink.writeJsonString(name);
        sink.append("}");
        any = true;
    }
    if (!any) sink.append("\"0\":{}");   // non-empty, or python-wled reads it as no response at all
    sink.append("}");
    sendResponse(conn, 200, "application/json", sink.data());
}

// The deviceName / IP / MAC lookup the WLED shim needs at four call sites, so a change to how identity is discovered updates one place.
void HttpServerModule::resolveWledIdentity(const char*& name, uint8_t mac[6], uint8_t ip[4],
                                           const char* nameFallback) {
    name = nameFallback;
    if (MoonModule* sys = findModuleByName("System")) {
        const char* dn = static_cast<SystemModule*>(sys)->deviceName();
        if (dn && dn[0]) name = dn;
    }
    for (int i = 0; i < 6; i++) mac[i] = 0;
    platform::getMacAddress(mac);
    for (int i = 0; i < 4; i++) ip[i] = 0;
    platform::localIPv4(ip);
}

// The WLED info object (no HTTP header), shared by /json/info and /json/si; the 💫 marker prefixes only the WLED-compat name HA reads, never the real deviceName.
void HttpServerModule::writeWledName(JsonSink& sink, const char* name) {
    char prefixed[80];
    std::snprintf(prefixed, sizeof(prefixed), "\xF0\x9F\x92\xAB %s", name ? name : "");
    sink.writeJsonString(prefixed);
}

void HttpServerModule::writeWledInfoBody(JsonSink& sink, const char* name, const uint8_t mac[6]) {
    sink.appendf("{\"name\":");
    writeWledName(sink, name);
    // The real light count and the wifi signal, so the WLED app card shows the true device shape; signal maps rssi to 0-100 as WLED does.
    const unsigned ledCount = lightSummary().lightCount;
    const int rssi = platform::wifiStaRssi();
    int signal = (rssi == 0) ? 0 : (2 * (rssi + 100));
    if (signal < 0) signal = 0; else if (signal > 100) signal = 100;
    sink.appendf(",\"mac\":\"%02x%02x%02x%02x%02x%02x\","
                 "\"leds\":{\"count\":%u},\"wifi\":{\"rssi\":%d,\"signal\":%d},"
                 "\"brand\":\"WLED\",\"product\":\"MoonModules\"}",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                 ledCount, rssi, signal);
}

// The WLED state object: `on`+`bri` mirror Drivers, and `seg[0].col[0]` is the active palette's full-value color, stable across consumers and never double-dimmed with HA's `state.bri × seg.bri`.
void HttpServerModule::writeWledStateBody(JsonSink& sink) {
    const uint8_t bri = driversBrightness(scheduler_);
    const LightOutput* out = LightOutput::active();
    const RGB pc = out ? out->paletteRgb(driversPalette(scheduler_)) : RGB{0, 0, 0};
    // python-wled parses this response through State.from_dict, so nl/udpn/lor/transition/ps/pl/mainseg are required, and seg[0].on must be present since HA reads segment .on, not top-level state.on.
    const char* onStr = driversOn(scheduler_) ? "true" : "false";
    // seg[0].pal is the active palette index, sharing the Drivers `palette` control with col[0], so HA's palette dropdown and color picker are two views of one value.
    const uint8_t pal = driversPalette(scheduler_);
    // The applied look as a WLED preset slot (1-based, as /presets.json), or -1; without it HA's preset dropdown reads unknown even while a preset is active.
    int currentPs = -1;
    if (auto* control = static_cast<ControlModule*>(findModuleByName("Control"))) {
        const char* look = control->currentLook();
        if (look && look[0]) {
            for (uint8_t i = 0; i < control->presetCount(); i++) {
                if (!control->isLookOnly(i)) continue;
                const char* n = control->presetName(i);
                if (n && std::strcmp(n, look) == 0) { currentPs = i + 1; break; }
            }
        }
    }
    sink.appendf("{\"on\":%s,\"bri\":%u,\"transition\":7,\"ps\":%d,\"pl\":-1,"
                 "\"nl\":{},\"udpn\":{},\"lor\":0,\"mainseg\":0,"
                 // seg[0].bri = 255 so HA's segment.bri × state.bri / 255 shows the master value; fx=0 ("Solid") pairs with pal as python-wled expects, else the light entity stays unavailable.
                 "\"seg\":[{\"id\":0,\"on\":%s,\"bri\":255,\"fx\":0,\"pal\":%u,\"col\":[[%u,%u,%u]]}]}",
                 onStr, bri, currentPs, onStr, pal, pc.r, pc.g, pc.b);
}

void HttpServerModule::serveWledState(platform::TcpConnection& conn) {
    const char* header =
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Connection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n";
    conn.write(reinterpret_cast<const uint8_t*>(header), std::strlen(header));
    JsonSink sink(conn);
    writeWledStateBody(sink);
    sink.flush();
}

// /json: the full blob HA's python-wled fetches, requiring `info.fs`, `state.nl`, `state.udpn`, `state.lor` and `ver` >= 0.14.0; separate from /json/info and /json/state, which stay the Android-validated minimum.
void HttpServerModule::serveWledDeviceJson(platform::TcpConnection& conn) {
    const char* header =
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Connection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n";
    conn.write(reinterpret_cast<const uint8_t*>(header), std::strlen(header));

    const char* name; uint8_t mac[6]; uint8_t ip[4];
    resolveWledIdentity(name, mac, ip);

    JsonSink sink(conn);
    // state: reuse writeWledStateBody under "state" so one writer decides which seg[0] fields HA reads.
    sink.appendf("{\"state\":");
    writeWledStateBody(sink);
    // `ver` is a sentinel and an Ethernet device omits `wifi` entirely. @xref{why-the-wled-shim-reports-a-sentinel-version} @xref{why-an-ethernet-device-sends-no-wifi-object}
    const bool onEth = platform::ethConnected();
    const int rssi = platform::wifiStaRssi();
    uint8_t bssid[6] = {};
    platform::wifiStaBssid(bssid);
    const int channel = platform::wifiStaChannel();
    int signal = (rssi == 0) ? 0 : (2 * (rssi + 100));
    if (signal < 0) signal = 0; else if (signal > 100) signal = 100;
    // The real pipeline shape and render rate.
    const LightSummary& ls = lightSummary();
    const LightOutput* out = LightOutput::active();
    const unsigned ledCount = ls.lightCount;
    const unsigned renderFps = scheduler_ ? scheduler_->fps() : 0;
    const char* rgbw = (ls.channelsPerLight >= 4) ? "true" : "false";
    sink.appendf(",\"info\":{\"ver\":\"99.0.0\",\"vid\":2410150,\"name\":");
    writeWledName(sink, name);
    sink.appendf(",\"mac\":\"%02x%02x%02x%02x%02x%02x\","
                 "\"ip\":\"%u.%u.%u.%u\",\"arch\":\"esp32\","
                 "\"brand\":\"WLED\",\"product\":\"MoonModules\",\"release\":\"MoonModules\","
                 // lc and seglc are capability codes, not counts: 1 (RGB_COLOR) gives HA a brightness slider and color picker, and a LED count would leave the light entity without color modes.
                 "\"leds\":{\"count\":%u,\"fps\":%u,\"rgbw\":%s,\"wv\":false,\"cct\":false,"
                 "\"maxpwr\":0,\"maxseg\":1,\"pwr\":0,\"lc\":1,\"seglc\":[1]},",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                 ip[0], ip[1], ip[2], ip[3],
                 ledCount, renderFps, rgbw);
    // wifi: only for a Wi-Fi device (omitted on Ethernet; see the comment above the getters).
    if (!onEth) {
        sink.appendf("\"wifi\":{\"bssid\":\"%02x:%02x:%02x:%02x:%02x:%02x\","
                     "\"rssi\":%d,\"channel\":%d,\"signal\":%d},",
                     bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5],
                     rssi, channel, signal);
    }
    // pmt is the presets-modified time HA uses to re-fetch /presets.json, stamped by ControlModule on every preset change, with 1 as the no-ControlModule fallback.
    unsigned pmt = 1;
    if (auto* control = static_cast<ControlModule*>(findModuleByName("Control")))
        pmt = static_cast<unsigned>(control->presetsRevision());   // >= 1 once setup's rescan ran
    if (pmt == 0) pmt = 1;   // python-wled treats 0 as "no presets support"
    sink.appendf("\"fs\":{\"t\":256,\"u\":32,\"pmt\":%u},"
                 // uptime + pmt drive python-wled's presets change-detect: both non-zero and stable stop HA refetching /presets.json on every state update.
                 "\"freeheap\":%u,\"uptime\":%u,\"udpport\":21324,\"live\":false,"
                 // ws=-1 tells python-wled WebSocket updates are unsupported, so HA polls over HTTP instead of opening /ws, whose MoonLight-native frames flood its log with `MissingField: filesystem`.
                 "\"lm\":\"\",\"lip\":\"\",\"ws\":-1,"
                 // palcount is the real count, built-ins plus the scripted tail, matching palettes[] below; fxcount stays 1 and cpal/umpal are 0.
                 "\"fxcount\":1,\"palcount\":%u,\"cpalcount\":0,\"umpalcount\":0,\"str\":false}",
                 // Non-zero or HA rejects the device: the desktop's freeHeap() reports 0, so a nominal figure stands in.
                 pmt,
                 static_cast<unsigned>(platform::freeHeap() ? platform::freeHeap() : 32768u),
                 static_cast<unsigned>(platform::millis() / 1000u),
                 static_cast<unsigned>(out ? out->paletteCount() : 0));
    // effects stays one real entry ("Solid") since this shim drives a single Layer; palettes is the real list, indexed to match seg[0].pal and the Drivers `palette` control.
    sink.appendf(",\"effects\":[\"Solid\"],\"palettes\":[");
    if (out) out->writePaletteNames(sink);
    sink.appendf("]}");
    sink.flush();
}

// /json/si: the combined {state, info} the WLED app reads in one call for its card.
void HttpServerModule::serveWledStateInfo(platform::TcpConnection& conn) {
    const char* header =
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Connection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n";
    conn.write(reinterpret_cast<const uint8_t*>(header), std::strlen(header));

    const char* name; uint8_t mac[6]; uint8_t ip[4];
    resolveWledIdentity(name, mac, ip);
    (void)ip;  // /json/si's info body carries no IP field; keep the call uniform.

    JsonSink sink(conn);
    sink.appendf("{\"state\":");
    writeWledStateBody(sink);
    sink.appendf(",\"info\":");
    writeWledInfoBody(sink, name, mac);
    sink.appendf("}");
    sink.flush();
}

// Apply a WLED state-set body ({on?, bri?}) to Drivers through the apply-core, from POST /json/state and the inbound WebSocket; `on` is master power, so toggling off keeps the brightness level.
void HttpServerModule::applyWledState(const char* body) {
    // ps is a preset slot from HA's dropdown (1-based, as /presets.json); applyLookByName re-checks look-only, so a crafted request naming a Drivers or Layouts preset is refused here, not hidden.
    if (mm::json::hasKey(body, "ps")) {
        const int slot = mm::json::parseInt(body, "ps");
        auto* control = static_cast<ControlModule*>(findModuleByName("Control"));
        if (control && slot > 0 && slot <= control->presetCount()) {
            const char* name = control->presetName(static_cast<uint8_t>(slot - 1));
            if (name) control->applyLookByName(name);
        }
    }
    if (mm::json::hasKey(body, "on")) {
        applySetControl("Drivers", "on",
                        mm::json::parseBool(body, "on") ? "{\"value\":true}" : "{\"value\":false}");
    }
    if (mm::json::hasKey(body, "bri")) {
        int bri = mm::json::parseInt(body, "bri");
        if (bri < 0) bri = 0;
        if (bri > 255) bri = 255;
        char valueJson[32];
        std::snprintf(valueJson, sizeof(valueJson), "{\"value\":%d}", bri);
        applySetControl("Drivers", "brightness", valueJson);
    }
    // seg[0].pal is the palette index HA writes from its dropdown, mapped to the Drivers `palette` control and parsed from the segment object so a top-level "pal" cannot hijack it.
    const LightOutput* out = LightOutput::active();
    const char* segStart = std::strstr(body, "\"seg\":");
    const char* palStart = segStart ? std::strstr(segStart, "\"pal\":") : nullptr;
    if (palStart && out) {
        int pal = mm::parseIntStr(palStart + 6);
        if (pal < 0) pal = 0;
        // Clamp to the full count, built-ins plus the scripted tail, the list served as `palettes[]`, so a scripted index the device offered is not snapped back.
        if (pal >= out->paletteCount()) pal = out->paletteCount() - 1;
        char valueJson[24];
        std::snprintf(valueJson, sizeof(valueJson), "{\"value\":%d}", pal);
        applySetControl("Drivers", "palette", valueJson);
    }
    // seg[0].col[0] is [r,g,b] from HA's color picker, mapped to the nearest palette; the value channel is ignored since HA's brightness slider sends `bri`.
    const char* colStart = std::strstr(body, "\"col\":[[");
    if (colStart && out) {
        int r = 0, g = 0, b = 0;
        if (std::sscanf(colStart + 8, "%d,%d,%d", &r, &g, &b) == 3) {
            const uint8_t rc = static_cast<uint8_t>(r < 0 ? 0 : (r > 255 ? 255 : r));
            const uint8_t gc = static_cast<uint8_t>(g < 0 ? 0 : (g > 255 ? 255 : g));
            const uint8_t bc = static_cast<uint8_t>(b < 0 ? 0 : (b > 255 ? 255 : b));
            const uint8_t idx = out->nearestPalette(RGB{rc, gc, bc});
            char valueJson[24];
            std::snprintf(valueJson, sizeof(valueJson), "{\"value\":%u}", static_cast<unsigned>(idx));
            applySetControl("Drivers", "palette", valueJson);
        }
    }
}

// POST /json/state: the WLED app's HTTP control channel; apply, then echo the resulting state, which the app expects.
void HttpServerModule::handleWledState(platform::TcpConnection& conn, const char* body) {
    applyWledState(body);
    serveWledState(conn);
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

void HttpServerModule::handleDeleteModule(platform::TcpConnection& conn, const char* moduleName) {
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

void HttpServerModule::serveModule(platform::TcpConnection& conn, const char* name) {
    char decoded[24] = {};
    const size_t i = decodeModuleName(name, decoded, sizeof(decoded));

    // appearsInUi() gates this like /api/state, since a module that is not a card has no `{ }` link; `/document` asks for the state document PATCH /api/state takes.
    const char* rest = std::strchr(name, '/');
    const bool document = rest && std::strncmp(rest, "/document", 9) == 0 && (rest[9] == 0 || rest[9] == '?');
    MoonModule* mod = i ? findModuleByName(decoded) : nullptr;
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
    // A -dev build fetches from its own commit (kBuildId), which cannot drift like a branch; a `+` dirty suffix is stripped, and an unpushed commit 404s as a failed download.
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
    sink.append(",\"dir\":");
    sink.writeJsonString(moonlive::kFactoryScriptDir);

    // `dim` and `tags` ride with each name so a picker can show a script before download; they are copied from its source at build time, for display only.
    auto emit = [&sink](const char* key, const char* folder,
                        const char* const* names, const unsigned char* dims,
                        const char* const* tags, size_t count) {
        sink.appendf(",\"%s\":{\"folder\":\"%s\",\"names\":[", key, folder);
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
        sink.append("]}");
    };
    emit("effects", moonlive::kEffectFolder, moonlive::kEffectCatalog,
         moonlive::kEffectCatalogDim, moonlive::kEffectCatalogTags,
         moonlive::kEffectCatalogCount);
    emit("layouts", moonlive::kLayoutFolder, moonlive::kLayoutCatalog,
         moonlive::kLayoutCatalogDim, moonlive::kLayoutCatalogTags,
         moonlive::kLayoutCatalogCount);
    emit("modifiers", moonlive::kModifierFolder, moonlive::kModifierCatalog,
         moonlive::kModifierCatalogDim, moonlive::kModifierCatalogTags,
         moonlive::kModifierCatalogCount);
    emit("services", moonlive::kServiceFolder, moonlive::kServiceCatalog,
         moonlive::kServiceCatalogDim, moonlive::kServiceCatalogTags,
         moonlive::kServiceCatalogCount);
    emit("palettes", moonlive::kPaletteFolder, moonlive::kPaletteCatalog,
         moonlive::kPaletteCatalogDim, moonlive::kPaletteCatalogTags,
         moonlive::kPaletteCatalogCount);
    sink.append("}");
    sink.flush();
}

void HttpServerModule::serveTypes(platform::TcpConnection& conn) {
    const char* header =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n";
    conn.write(reinterpret_cast<const uint8_t*>(header), std::strlen(header));

    JsonSink sink(conn);
    sink.append("{\"types\":[");
    bool first = true;
    for (uint8_t i = 0; i < ModuleFactory::typeCount(); i++) {
        const char* name = ModuleFactory::typeName(i);
        if (!name) continue;
        ModuleRole role = ModuleFactory::typeRole(i);
        const char* roleStr = roleName(role);
        const char* docPath = ModuleFactory::typeDocPath(i);
        const char* tags = ModuleFactory::typeTags(i);
        uint8_t dim = ModuleFactory::typeDim(i);
        const char* childRoles = ModuleFactory::typeAcceptsChildRoles(i);
        // displayNameFor returns a pointer into a shared static buffer, so copy it before another factory call overwrites it.
        char displayName[16];
        std::strncpy(displayName, ModuleFactory::displayNameFor(name, role), sizeof(displayName) - 1);
        displayName[sizeof(displayName) - 1] = 0;
        sink.appendf("%s{\"name\":\"%s\",\"displayName\":\"%s\",\"role\":\"%s\","
                     "\"docPath\":\"%s\",\"tags\":\"%s\",\"dim\":%u,"
                     "\"acceptsChildRoles\":\"%s\",\"defaults\":{",
                     first ? "" : ",", name, displayName, roleStr,
                     docPath ? docPath : "", tags ? tags : "",
                     static_cast<unsigned>(dim),
                     childRoles ? childRoles : "");
        writeTypeDefaults(sink, name);
        sink.append("}}");
        first = false;
    }
    sink.append("]}");
    sink.flush();
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

    // A preview client lands in its own smaller array (see MAX_PREVIEW_CLIENTS) and needs none of the control-plane resync bookkeeping, since it receives only the driver's binary frames.
    if (previewChannel) {
        // Busy is the common case (core 1 streams most ticks) and refusing caused reconnect storms, so admit with a SIZE_MAX cursor: the stream starts at the next whole frame.
        LockGuard admitLease{wsLock_};
        for (int i = 0; i < MAX_PREVIEW_CLIENTS; i++) {
            if (previewClients_[i].valid()) continue;
            previewClients_[i] = std::move(conn);
            // The slot turns over: the producer drops its predecessor's standing request, and the new client announces its own wishes (pull model).
            if (clientSink_) clientSink_->onClientGone(i);
            // A frame mid-drain to other clients keeps draining; this slot marks itself done so the newcomer is never spliced into a half-sent message.
            previewSend_.sent[i] = !admitLease ? SIZE_MAX
                                 : (previewSend_.active ? previewSend_.hdrLen + previewSend_.bodyLen : 0);
            return;
        }
        conn.close();   // preview cap reached: the client keeps /ws and simply shows no preview
        return;
    }

    // Store connection as WebSocket client.
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (!wsClients_[i].valid()) {
            wsOnAccessPoint_[i] = onAccessPoint(conn);   // read once: a socket's peer does not change
            wsClients_[i] = std::move(conn);
            // A full state mid-drain is what this newcomer needs too: its cursor starts at 0, so it receives the whole in-flight message with no splice or cancel.
            stateSend_.sent[i] = 0;
            // A new client needs the full state, not a patch against a baseline it never received; the global cache resyncs everyone, which is cheap since connects are rare.
            requestFullResync();
            return;
        }
    }
    // No slot: close. MAX_WS_CLIENTS sits above the realistic count plus a refresh's overlap, since the new socket opens before the old FIN lands; the browser's backoff retries a full moment.
    conn.close();
}

void HttpServerModule::pushStateToWebSockets() {
    bool hasClients = false;
    for (auto& ws : wsClients_) {
        if (ws.valid()) { hasClients = true; break; }
    }
    if (!hasClients) return;

    if (fullResyncPending_) {
        // Full state (~30 KB) drains in chunks on tick20ms, not a blocking write; a prior one finishes first, and fullResyncPending_ stays true until a start is accepted.
        if (stateSend_.active) return;
        JsonSink sink;
        buildStateJson(sink);
        // A sink out of heap holds a truncated document; send it anyway and clear the resync flag, since returning early freezes the whole UI where a partial tree keeps updating.
        if (sink.overflowed()) {
            setStatus("state too large for free memory: some modules may not show", Severity::Warning);
        }
        const size_t len = sink.size();
        char* owned = sink.detach();   // move ownership to the sender (frees on drain-complete)
        if (owned && startBufferedTextSend(owned, len)) {
            baselineLeafHashes();       // the full state IS the new baseline: next tick patches from here
            fullResyncPending_ = false;   // cleared only on a confirmed accept; a failed start retries
        }
    } else {
        // PATCH, the steady state: only changed leaves (~1-2 KB) go out inline; it waits while a full state drains, since a small frame would interleave inside that WS message.
        if (stateSend_.active) return;
        JsonSink sink;
        if (buildStatePatch(sink) > 0) sendPatch(sink);
        // changed == 0 → nothing to send this second (an idle device); the common quiet case.
    }

    // Push a WLED-shaped {state, info} frame for the WLED app on this /ws; each consumer ignores the other's frame (the UI keys on `modules`, the app on `state`/`info`).
    pushWledStateToWebSockets();
}

// Build and push the WLED {state, info} object to every WS client. Shares the same body writers as /json/si.
void HttpServerModule::pushWledStateToWebSockets() {
    if (stateSend_.active) return;   // never interleave with the chunked full-state drain
    bool hasClients = false;
    for (auto& ws : wsClients_) if (ws.valid()) { hasClients = true; break; }
    if (!hasClients) return;

    const char* name; uint8_t mac[6]; uint8_t ip[4];
    resolveWledIdentity(name, mac, ip);
    (void)ip;  // the WS-push info body carries no IP field; keep the call uniform.

    JsonSink sink;
    sink.appendf("{\"state\":");
    writeWledStateBody(sink);
    sink.appendf(",\"info\":");
    writeWledInfoBody(sink, name, mac);
    sink.appendf("}");

    for (auto& ws : wsClients_) {
        if (!ws.valid()) continue;
        if (!sendWsTextFrame(ws, sink.data(), static_cast<int>(sink.size()))) ws.close();
    }
}

// Read one frame per client and apply a WLED state-set ({on}/{bri}) to Drivers; client frames are masked (RFC 6455 §5.3), so unmask in place and skip non-text frames.
void HttpServerModule::pollWledStateFromWebSockets() {
    // Reap cleanly closed preview clients, whose slot otherwise counts against the cap until a send fails: read() == 0 is a peer FIN; no lease is needed on this core.
    for (int i = 0; i < MAX_PREVIEW_CLIENTS; i++) {
        auto& pc = previewClients_[i];
        if (!pc.valid()) continue;
        uint8_t buf[64];
        const int n = pc.read(buf, sizeof(buf));
        if (n == 0) {                                  // clean close (peer FIN): free the slot NOW
            pc.close();
            if (clientSink_) clientSink_->onClientGone(i);
            continue;
        }
        if (n > 0 && clientSink_) {
            // Walk the read, since TCP coalesces several small requests into one buffer; each complete frame's unmasked payload goes to the sink in order.
            for (int off = 0; off < n; ) {
                uint8_t payload[8];
                int used = 0;
                const int len = parsePreviewUplink(buf + off, n - off, payload, &used);
                if (used <= 0) break;                  // nothing parseable left (or a partial tail)
                if (len > 0) clientSink_->onClientMessage(i, payload, len);
                off += used;
            }
        }
    }

    for (int c = 0; c < MAX_WS_CLIENTS; c++) {
        auto& ws = wsClients_[c];
        if (!ws.valid()) { if (carryClient_ == c) carryLen_ = 0; continue; }
        uint8_t f[768];
        size_t have = 0;
        // A frame the last read cut in half starts this one, so the stream never loses its framing.
        if (carryClient_ == c && carryLen_) { std::memcpy(f, carry_, carryLen_); have = carryLen_; carryLen_ = 0; }
        const int n = ws.read(f + have, sizeof(f) - have);   // non-blocking: -1 when nothing is pending
        // A clean peer close (FIN) frees the slot now, so a reconnect burst finds it free.
        if (n == 0) { ws.close(); continue; }
        const size_t total = have + static_cast<size_t>(n > 0 ? n : 0);
        if (total == 0) continue;
        const size_t used = walkWsTextFrames(f, total, [&](const char* body) {
            // A control write from the interface's input bridges, which keeps them off a TCP connection per write.
            if (mm::json::hasKey(body, "module") && mm::json::hasKey(body, "control")) {
                char moduleName[32] = {}, controlName[32] = {};
                mm::json::parseString(body, "module", moduleName, sizeof(moduleName));
                mm::json::parseString(body, "control", controlName, sizeof(controlName));
                applySetControl(moduleName, controlName, body);
            } else if (mm::json::hasKey(body, "on") || mm::json::hasKey(body, "bri") || mm::json::hasKey(body, "ps")) {
                applyWledState(body);   // the WLED app's {on, bri, ps}
            }
        });
        if (used < total && total - used <= sizeof(carry_)) {
            std::memcpy(carry_, f + used, total - used);
            carryLen_ = total - used;
            carryClient_ = c;
        }
    }
}

bool HttpServerModule::sendWsTextFrame(platform::TcpConnection& conn, const char* data, int len) {
    uint8_t header[10];
    int headerLen = 0;

    header[0] = 0x81; // FIN + text opcode
    if (len < 126) {
        header[1] = static_cast<uint8_t>(len);
        headerLen = 2;
    } else if (len < 65536) {
        header[1] = 126;
        header[2] = static_cast<uint8_t>((len >> 8) & 0xFF);
        header[3] = static_cast<uint8_t>(len & 0xFF);
        headerLen = 4;
    } else {
        return false; // too large
    }

    if (!conn.write(header, headerLen)) return false;
    return conn.write(reinterpret_cast<const uint8_t*>(data), len);
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
    if (previewSend_.active) return false;

    const size_t totalLen = headerLen + bodyLen;   // WS payload length = app header + body
    // Build the WS binary header into previewSend_.hdr, followed by the app header, so the cursor streams them as one span.
    const size_t wsLen = writeWsFrameHeader(previewSend_.hdr, 0x82, totalLen);
    // sizeof(hdr)=16 holds the 10-byte WS form plus the preview app headers (≤10 bytes); the guard stops a larger header overrunning.
    if (wsLen + headerLen > sizeof(previewSend_.hdr)) return false;
    // memcpy, not a byte loop: the compiler cannot see that wsLen is at most 10 and would warn (-Wstringop-overflow), while memcpy lets it check against the guard above.
    std::memcpy(previewSend_.hdr + wsLen, header, headerLen);

    previewSend_.hdrLen = wsLen + headerLen;
    previewSend_.body = body;        // borrowed: PreviewDriver keeps the pixel buffer alive
    previewSend_.bodyLen = bodyLen;
    for (int i = 0; i < MAX_PREVIEW_CLIENTS; i++) previewSend_.sent[i] = 0;
    previewSend_.active = true;
    // Do not drain here on the render thread, where a variable-cost writeSome would hitch the LEDs; drainPreviewSend() pushes bytes on tick20ms.
    return true;
}

// Queue a text frame whose body this module owns through the same resumable slot, so the state JSON drains in chunks on tick20ms, not a blocking write.
bool HttpServerModule::startBufferedTextSend(char* ownedBody, size_t bodyLen) {
    // A send already in flight: drop this one and free its buffer: the next second's state is fresher.
    if (stateSend_.active) { platform::free(ownedBody); return false; }
    // No app header for the state frame (the JSON is the whole payload), just the WS text header.
    const size_t wsLen = writeWsFrameHeader(stateSend_.hdr, 0x81, bodyLen);
    stateSend_.hdrLen = wsLen;
    stateSend_.body = reinterpret_cast<const uint8_t*>(ownedBody);
    stateSend_.bodyLen = bodyLen;
    for (auto& c : stateSend_.sent) c = 0;
    stateSend_.active = true;
    return true;   // drained on tick20ms, never a blocking write on the render tick
}

// Per-client cursor over [hdr ++ body]: write what the socket takes (one chunk), advance, resume next tick; an error closes that client, the send ends when every client is done.
void HttpServerModule::drainPreviewSend() {
    // Core-0 side of the sender lease core 1 holds while arming a frame; try_lock, not a wait, since tick20ms must not block, so we drain next tick.
    LockGuard lease{wsLock_};
    if (!lease) return;
    if (!previewSend_.active) return;
    const size_t total = previewSend_.hdrLen + previewSend_.bodyLen;
    const size_t chunk = drainChunkBytes();
    bool anyLiveClient = false;
    bool allDone = true;
    for (int i = 0; i < MAX_PREVIEW_CLIENTS; i++) {
        auto& ws = previewClients_[i];
        if (!ws.valid()) continue;
        anyLiveClient = true;
        size_t& cur = previewSend_.sent[i];
        size_t budget = chunk;   // bound bytes pushed to THIS client this tick → bounded tick cost
        while (cur < total && budget > 0) {
            // Source the next byte run from hdr (cursor < hdrLen) or body (cursor >= hdrLen).
            const uint8_t* src;
            size_t span;
            if (cur < previewSend_.hdrLen) { src = previewSend_.hdr + cur; span = previewSend_.hdrLen - cur; }
            else { src = previewSend_.body + (cur - previewSend_.hdrLen); span = total - cur; }
            if (span > budget) span = budget;
            int n = ws.writeSome(src, span);
            if (n < 0) {                         // real error: drop this client and its requests
                ws.close();
                if (clientSink_) clientSink_->onClientGone(i);
                break;
            }
            if (n == 0) break;                   // WouldBlock: leave the rest for next tick (no spin)
            cur += static_cast<size_t>(n);
            budget -= static_cast<size_t>(n);
        }
        if (ws.valid() && cur < total) allDone = false;
    }
    // Done when every live client finished, or no client remains to send to.
    if (!anyLiveClient || allDone) previewSend_.active = false;
}

// The same cursor drain for the full-state frame over the control clients; core-0 only, so unlike the preview slot it needs no sender lease.
void HttpServerModule::drainStateSend() {
    if (!stateSend_.active) return;
    const size_t total = stateSend_.hdrLen + stateSend_.bodyLen;
    const size_t chunk = drainChunkBytes();
    bool anyLiveClient = false;
    bool allDone = true;
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        auto& ws = wsClients_[i];
        if (!ws.valid()) continue;
        anyLiveClient = true;
        size_t& cur = stateSend_.sent[i];
        size_t budget = chunk;
        while (cur < total && budget > 0) {
            const uint8_t* src;
            size_t span;
            if (cur < stateSend_.hdrLen) { src = stateSend_.hdr + cur; span = stateSend_.hdrLen - cur; }
            else { src = stateSend_.body + (cur - stateSend_.hdrLen); span = total - cur; }
            if (span > budget) span = budget;
            int n = ws.writeSome(src, span);
            if (n < 0) { ws.close(); break; }    // real error, drop this client
            if (n == 0) break;                   // WouldBlock, resume next tick
            cur += static_cast<size_t>(n);
            budget -= static_cast<size_t>(n);
        }
        if (ws.valid() && cur < total) allDone = false;
    }
    if (!anyLiveClient || allDone) {
        platform::free(const_cast<uint8_t*>(stateSend_.body));   // the frame owns its JSON body
        stateSend_.body = nullptr;
        stateSend_.active = false;
    }
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

/// @}
