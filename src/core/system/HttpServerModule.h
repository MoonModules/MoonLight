#pragma once

#include "core/module/MoonModule.h"
#include "core/util/TryLock.h"   // the cross-thread sender latch (wsLock_)
#include "core/util/ScratchBuffer.h"   // leafHashes_: the growable diff-on-the-wire value-hash cache
#include "core/util/BinaryBroadcaster.h"
#include "platform/platform.h"

#include <array>
#include <cstdint>

namespace mm {

struct StateDocumentResult;

class JsonSink;   // forward declared: the bodies in the .cpp include the real headers
class Scheduler;

/// The embedded HTTP server and WebSocket: it serves the web UI and the REST API behind it. @xref{the-rest-api} @xref{what-it-is-allowed-to-include}
///
/// @moreinfo
///
/// ## What it is allowed to include
///
/// This is core infrastructure and includes no light header; the WLED shim reads the device and its palettes through `LightOutput` and drives the palette through `Scheduler::setControl`.
///
/// ## The REST API
///
/// `GET /` serves `index.html` and the UI assets.
///
/// | Route | What it returns |
/// |-------|-----------------|
/// | `GET /api/state` | the full module-tree JSON: per entry a name, type, role, enabled flag, tick time, sizes, controls, and any status |
/// | `GET /api/system` | frame rate, tick time, free heap, free internal, largest block, uptime |
/// | `GET /api/types` | the type catalog: a stable factory name, a display name, the child roles a type accepts, and defaults from a fresh probe instance |
///
/// ## Mutations and the File Manager
///
/// A mutation is `POST /api/control`, `POST /api/modules` to create, `.../move` and `.../replace`, `POST /api/reboot`, or `DELETE /api/modules/{name}`.
///
/// The File Manager reads and writes through `/api/file?path=` and lists, creates and removes through `/api/dir?path=`; a card mounted at `/sd` takes the same routes.
///
/// ## The two WebSocket channels
///
/// `GET /ws` with `Upgrade: websocket` does the RFC 6455 handshake. `/ws` carries the JSON state and its patches, and `/wsp` the lossy binary preview, each with its own client cap.
///
/// A binary frame drains in memory-adaptive chunks per client per 20 ms tick. A large frame so crosses several ticks without a loop waiting on a socket, and still arrives as one message.
///
/// One frame is in flight per channel and a new offer meanwhile is dropped. A client is closed on a real error or a FIN, never for being slow.
///
/// ## What a preview client may send back
///
/// Inbound `/wsp` payloads go unmasked and opaque to the producer's sink: a standing frame request and a one-shot table request.
///
/// On `/ws` a client writes a control as `{"module","control","value"}`, as the input bridges do up to 30 times a second; every other mutation goes through REST.
///
/// ## State push: a diff on the wire
///
/// Snapshot then patch, the shape Redux and Firestore sync use and RFC 6902 formalises. Re-serializing the mostly static tree every second on the render thread stuttered the LEDs at 1 Hz.
///
/// A client gets a full state when it subscribes, then each second a patch of the values that changed. Each leaf's serialized value is hashed with FNV-1a and compared with the cached hash.
///
/// Value-compare rather than a dirty flag, so a value the device changes itself, telemetry, a status or a driver, is caught like a `setControl` write with no per-write bookkeeping.
///
/// ## State by subscription
///
/// Publish/subscribe per client: `{"subscribe":[…]}` names the modules a client views, by name or type, each with its descendants, and `"*"` names the whole tree.
///
/// The full state carries those modules whole and every other module as an outline: its header and children, without controls. A page so gets its root and its status bar's modules.
///
/// A connect alone sends nothing, so a page that subscribes on open gets one document, and the WLED app, which never subscribes, gets only its own frame.
///
/// A subscribe resyncs that client alone. The others first get the patch due, then the baseline is retaken, so none misses a change; a schema change resyncs everyone.
///
/// `{"resync":n}` asks for the view again, as the page does after an add, delete or move. The answer ends with `"resync":n`, so a document already under way is not taken for it.
///
/// ## Ping and Close
///
/// A Ping gets a Pong with its payload, and a Close a Close with its status code before the connection ends (RFC 6455 §5.5). A browser sends neither; a client library pings to keep alive.
///
/// The uplink is read only between frames, so the answer can never land inside a fragment, where RFC 6455 §5.4 forbids it.
///
/// ## The full state streams
///
/// Each client's document streams from its own cursor, a depth-first position resumable at a module's header, each control and each child, a fragment of about 2 KB at a time.
///
/// The first fragment opens a text message, the rest continue it and the last carries FIN: the fragmentation RFC 6455 §5.4 gives a message of unknown size.
///
/// A board so needs one fragment, or its largest control, in one block. The whole tree is more than a 1 MB board's heap can hand out at once.
///
/// The cursor resolves its path again at every step, so a tree changed between fragments still ends in valid JSON, and the schema change queues the resync that follows.
///
/// Run to completion, the same cursor writes `/api/state` and one module's JSON. Patches and the WLED frame wait while any document streams.
///
/// ## A value someone watches move
///
/// A module whose values move in front of someone, a desk's motorized faders, calls `notifyValuesChanged()`. Its changes go out on the next 20 ms tick, at most every 40 ms.
///
/// Only that module is walked and its hashes update as it goes, so the second's patch does not send the values again.
///
/// ## How a leaf is named
///
/// A leaf path is the module name and the control name, or an `@`-prefixed field for per-card header telemetry. Module names are unique tree-wide, so a path is stable.
///
/// ## When a patch cannot describe the change
///
/// One baseline covers what any client subscribes to. A patch is collected once over it, one slice per module, and each client gets the slices its subscription covers.
///
/// A full resync resends the state and retakes the baseline, on a subscribe and after any structural change, since a value patch cannot describe a reshaped tree.
///
/// A schema change forces one through a static hook, since a patch carries no hidden flags or option sets. Any `rebuildControls` triggers it: a control set, a list edit, a network callback.
///
/// A pending resync runs on the 20 ms tick and preempts an in-flight preview frame, so a new client gets its state, and its preview, within tens of milliseconds.
///
/// ## Why the drain runs on the transport tick
///
/// The drain runs on the 20 ms transport poll, not the per-render `tick`, so a socket write is never charged to the LEDs. The preview's rate is bounded by that cadence: it is a view, the LEDs are not.
///
/// ## The WLED-compatibility shim
///
/// WLED-shaped messages make a MoonLight device appear in the WLED apps and Home Assistant's WLED integration, and be controlled from them. Discovery is over mDNS.
///
/// `GET /json/info` carries a name, a MAC, LED and WiFi objects and the brand and product fields. The app keys on the brand: we interoperate rather than impersonate.
///
/// Live state goes over `/ws` as a state-and-info frame: Drivers brightness, and the active palette's color from `LightOutput::paletteRgb()`, or black without a light output.
///
/// The app's slider and toggle send a frame on the same socket, applied to Drivers brightness through the apply core that REST and Improv share.
///
/// ## Reading an uplink frame, and taking the lease
///
/// The uplink parser handles RFC 6455 framing alone. It returns a payload length, or -1 without a complete frame, and the bytes consumed, so a coalesced read walks frame by frame.
///
/// It is pure and static, so the byte handling is unit-tested without a socket.
///
/// The sender lease guards the preview's send state against this module's core-0 drain while an offloaded `PreviewDriver` streams from core 1. It try-locks: a busy transport skips a frame.
///
/// ## The per-module state route
///
/// One module's JSON, children included, is served on its own route, byte-identical to its entry in the full state; `/document` serves it as a preset's state document. A name may be percent-encoded.
///
/// ## Two firmware install paths
///
/// A firmware URL is answered at once and installed on its own task, so the browser polls for progress rather than holding a request open.
///
/// On a MoonBase device the URL is staged in NVS and the device reboots into MoonBase, which installs it; otherwise the platform fetches straight to the OTA slot.
///
/// Installing a MoonBase into the factory slot mirrors that, and is the only cable-free fix for a broken recovery image. It vets the image before erasing and does not reboot.
///
/// ## Crossing the domain boundary
///
/// The light-domain `PreviewDriver` streams each frame through this module's `BinaryBroadcaster` interface. `main.cpp`, the one file that knows both, wires them together.
///
/// ## The apply core: the REST API, callable in process
///
/// The add, set and clear-children operations are factored out of the connection, so the HTTP handlers and the Improv serial path share one apply logic and differ only in framing.
///
/// A file write ends in the same re-derive request a control write does, here rather than in a client that remembers a follow-up nudge, which `curl` and MoonDeck would not.
///
/// The re-derive asks the whole tree, and each module decides whether what it holds changed: a scripted module compares a content hash rather than re-reading its source.
///
/// ## Two guards that are static on purpose
///
/// The path decoder is the one guard every filesystem route shares: it refuses a missing or empty path, a `..` traversal and an overlong value. Both guards are static, unit-tested without a socket.
///
/// The header lookup is case-insensitive, as RFC 9112 says field names are. A case-sensitive search reads node's lowercase `content-length` as zero and writes empty files.
///
/// ## What the preview lock guards, and what it does not
///
/// The lock covers the preview channel's send state and sockets, which have producers on both cores once the multicore split engages: core 1 arms frames, core 0 drains and reaps.
///
/// Without it a partial write on one core interleaves inside the other's frame, a close lands under a concurrent write, or one side reads torn send state.
///
/// The control channel is outside it, every writer there running on core 0. The lock try-locks: the loser skips a slot, costing a preview frame and never stalling a render.
///
/// ## The served port is not the port control
///
/// `servedPort` reports the port the listener bound, or zero, and is what a module prints in its URL; the `port` control says what was asked for.
///
/// The schema hook installs without the listener, so a unit test proves it fires a resync without binding a port that may be busy.
///
/// ## How the two channel caps are sized
///
/// The control channel takes eight clients: a few viewers plus a refresh's overlap. The new socket opens before the old one's FIN arrives, and holds a slot until the old one is reaped.
///
/// At four, quick refreshes fill every slot with dead connections and the new upgrade is refused. A slot is only a descriptor and a stream cursor.
///
/// ## Why the preview cap is lower
///
/// The preview takes four. Both channels share lwIP's sixteen sockets with HTTP, mDNS, Art-Net, MQTT and OTA, so a preview per control client would starve the rest.
///
/// One browser is usual and two must work. A refused preview costs that client its preview alone, and a REST caller such as Home Assistant opens none.
///
/// ## Why the preview has its own connections
///
/// Separate TCP connections keep a 10 KB preview frame from delaying a state push, the standard remedy for head-of-line blocking. The state push has its own streams for the same reason.
///
/// ## Why the sink helpers are members
///
/// The `JsonSink` helpers are members because each reads this module's state. The sink, the SHA-1 of RFC 3174 and the base64 encoder live in headers of their own.
///
/// ## Prior art
///
/// The shim's field requirements come from the [WLED-Android client](https://github.com/Moustachauve/WLED-Android) by Christophe Gagnier, so it is the minimal object that app accepts rather than a guessed full emulation.
class HttpServerModule : public MoonModule, public BinaryBroadcaster {
public:
    /// The port it listens on, 8080 on the desktop and 80 on a device. @xref{the-served-port-is-not-the-port-control}
    uint16_t port = 8080;

    /// The scheduler every control write is applied through.
    void setScheduler(Scheduler* s) { scheduler_ = s; }
    /// Where the UI assets are served from.
    void setUiPath(const char* path) { uiPath_ = path; }

    /// Stream one binary frame to every client, the producer pushing payload bytes and this prepending the header. @xref{the-two-websocket-channels}
    bool sendBufferedFrame(const uint8_t* header, size_t headerLen,
                           const uint8_t* body, size_t bodyLen) override;
    /// True when no preview frame is draining.
    bool bufferedSendIdle() const override { return !previewSend_.active; }
    /// Drop the in-flight frame, closing any client left mid-message, whose stream would desync.
    void cancelBufferedSend() override {
        if (previewSend_.active) {
            const size_t total = previewSend_.hdrLen + previewSend_.bodyLen;
            for (int i = 0; i < MAX_PREVIEW_CLIENTS; i++)
                if (previewClients_[i].valid() &&
                    previewSend_.sent[i] > 0 && previewSend_.sent[i] < total) {
                    previewClients_[i].close();
                    // Every close notifies the producer, or the dead slot's standing request steers on.
                    if (clientSink_) clientSink_->onClientGone(i);
                }
        }
        previewSend_.active = false;
    }


    /// How many preview clients are connected.
    int subscriberCount() const override {
        int n = 0;
        for (const auto& pc : previewClients_) if (pc.valid()) n++;
        return n;
    }

    /// Register the producer that receives this channel's inbound client messages (opaque bytes).
    void setClientMessageSink(ClientMessageSink* sink) override { clientSink_ = sink; }

    /// Parse and unmask one client frame from a `/wsp` read, returning its payload length or -1. @xref{reading-an-uplink-frame-and-taking-the-lease}
    static int parsePreviewUplink(const uint8_t* buf, int n, uint8_t out[8], int* consumed);
    /// The longest text frame a client writes, a MIDI batch included. @xref{what-a-preview-client-may-send-back}
    static constexpr size_t kWsTextMax = 400;
    /// The longest control frame payload, a Ping's or a Close's (RFC 6455 §5.5).
    static constexpr size_t kWsControlMax = 125;
    /// Unmask a client frame's payload: `maskAt` points at the four-byte key, the payload following it (RFC 6455 §5.3).
    static void unmask(const uint8_t* maskAt, size_t len, uint8_t* out) {
        for (size_t i = 0; i < len; i++) out[i] = maskAt[4 + i] ^ maskAt[i & 3];
    }
    /// Hand each complete masked text frame in `buf` to `onText`, and each Ping or Close to `onControl` unmasked, returning the bytes consumed. A frame cut off at the end is left unconsumed.
    template <class OnText, class OnControl>
    static size_t walkWsFrames(const uint8_t* buf, size_t total, OnText&& onText, OnControl&& onControl) {
        size_t off = 0;
        while (off + 6 <= total) {
            const uint8_t* fr = buf + off;
            const uint8_t opcode = fr[0] & 0x0f;
            const bool masked = fr[1] & 0x80;
            size_t len = fr[1] & 0x7f;
            size_t hdr = 2;
            if (len == 126) {
                if (off + 4 > total) break;
                len = (size_t(fr[2]) << 8) | fr[3];
                hdr = 4;
            } else if (len == 127) {
                return total;                             // a frame over 64 KB is not ours: drop the read
            }
            if (!masked) return total;                    // a client frame must be masked (RFC 6455): drop the read
            const size_t frameLen = hdr + 4 + len;        // header, mask key, payload
            if (off + frameLen > total) break;            // the rest arrives in a later read
            if (opcode == 0x1 && len < kWsTextMax) {
                char body[kWsTextMax];
                unmask(fr + hdr, len, reinterpret_cast<uint8_t*>(body));
                body[len] = 0;
                onText(body);
            } else if ((opcode == 0x8 || opcode == 0x9) && len <= kWsControlMax && (fr[0] & 0x80)) {
                // A control frame is at most 125 bytes and never fragmented (RFC 6455 §5.5); one that breaks either is malformed and skipped.
                uint8_t payload[kWsControlMax];
                unmask(fr + hdr, len, payload);
                onControl(opcode, payload, len);
            }
            off += frameLen;
        }
        return off;
    }


    /// Take the sender lease, guarding the send state against this module's own drain and push. @xref{what-the-preview-lock-guards-and-what-it-does-not}
    bool tryAcquireSend() override { return wsLock_.tryAcquire(); }
    /// Give the sender lease back.
    void releaseSend() override { wsLock_.release(); }

    /// False: disabling the server through the UI would leave no way to re-enable it.
    bool respectsEnabled() const MM_NONBLOCKING override { return false; }

    /// False: this is the server that renders the state, so it does not list itself as a card.
    bool appearsInUi() const override { return false; }

    /// The port control, and the rest of this module's own surface.
    void defineControls() override;
    /// Open the listener and install the schema hook.
    void setup() override;
    /// Close every connection and unwire the hook.
    void release() override;
    void tick20ms() MM_NONBLOCKING override;
    void tick1s() MM_NONBLOCKING override;

    /// What an apply-core operation did, which a transport maps onto its own error reporting.
    enum class OpResult : uint8_t {
        Ok,
        NameInUse,       ///< add of a name some module already holds
        ModuleNotFound,  ///< module / parent name not in the tree
        ControlNotFound, ///< module exists but has no such control (a distinct 404)
        UnknownType,     ///< factory doesn't know the type
        BadRequest,      ///< missing field, top-level add, parent rejected child
        OutOfRange,      ///< numeric value outside bounds
        Malformed,       ///< value didn't parse (such as an IPv4)
        ReadOnly,        ///< tried to write a display-only control, or remove a module the user cannot
        Refused,         ///< the state engine refused the document, its reason in the result the caller passed
    };
    /// Create a module named `id`, refused when that name is in use, or without an id named after its type with a free suffix; `outName` receives the name. @xref{the-apply-core-the-rest-api-callable-in-process}
    OpResult applyAddModule(const char* typeName, const char* id, const char* parentId, char* outName = nullptr, size_t outNameLen = 0, StateDocumentResult* refusal = nullptr);
    /// Write one control, the single path every transport's control write ends in.
    OpResult applySetControl(const char* moduleName, const char* controlName, const char* valueJson);
    /// Remove a module and everything under it; a top-level or non-editable one is refused.
    OpResult applyDeleteModule(const char* moduleName, StateDocumentResult* refusal = nullptr);
    /// Swap a module for a fresh one of `typeName` in its place, named `wantName`, else its custom name, else the new type's default, made free.
    OpResult applyReplaceModule(const char* moduleName, const char* typeName, const char* wantName, StateDocumentResult* refusal = nullptr);

    /// A file changed, so ask the tree to re-derive whatever was built from it.
    void applyFileChanged(const char* path);

    /// Decode a `path=` query value into `out`, rooted at the mount, false when it is unsafe. @xref{two-guards-that-are-static-on-purpose}
    static bool parseFilePath(const char* query, char* out, size_t cap);

    /// Find a header by name, case-insensitively, as RFC 9112 requires.
    static const char* findHeaderCI(const char* hay, const char* needle);
    /// What a replaced module is called: the requested name, else a custom one, else its own default.
    static const char* replacementName(const char* requested, const char* current,
                                       const char* oldDefault);

    /// Apply a WLED state body onto the Drivers controls, the entry every WLED path drives.
    void applyWledState(const char* body);

    /// Test seams for the wire diff, so a unit test proves it without a socket.
    uint16_t buildStatePatchForTest(JsonSink& sink) { return buildStatePatch(sink); }
    /// Re-baseline the leaf hashes, for a test.
    void baselineLeafHashesForTest() { baselineLeafHashes(true); }
    /// Ask for a full resync, for a test.
    void requestFullResyncForTest() { requestFullResync(); }
    /// Whether a full resync is pending, for a test.
    bool fullResyncPendingForTest() const { return fullResyncPending_; }
    /// Clear the pending resync, for a test.
    void clearFullResyncForTest() { fullResyncPending_ = false; }
    /// Whether one client alone waits for a full state, for a test.
    bool clientResyncPendingForTest(int client) const { return resyncClient_[client]; }
    /// The port the live server is actually bound to, or 0 when none is up.
    static uint16_t servedPort() { return instance_ ? instance_->boundPort_ : 0; }

    /// Build the early patch of the modules that asked for one, for a test.
    uint16_t buildSoonPatchForTest(JsonSink& sink) { return buildSoonPatch(sink); }

    /// Install the schema-changed and values-changed hooks without opening the listener, which a unit test needs.
    void installHooksForTest() {
        instance_ = this;
        MoonModule::setSchemaChangedHook(&HttpServerModule::onSchemaChanged);
        MoonModule::setValuesChangedHook(&HttpServerModule::onValuesChanged);
    }

private:
    platform::TcpServer server_;
    Scheduler* scheduler_ = nullptr;
    const char* uiPath_ = "src/ui";

    // Eight: a few viewers plus the overlap while a refresh's old socket is still being reaped. @xref{how-the-two-channel-caps-are-sized}
    static constexpr int MAX_WS_CLIENTS = 8;
    platform::TcpConnection wsClients_[MAX_WS_CLIENTS];
    bool wsOnAccessPoint_[MAX_WS_CLIENTS] = {};   // each client's side, read when it connected

    // Four, deliberately fewer: the two arrays share one lwIP socket budget with every other service. @xref{why-the-preview-cap-is-lower}
    static constexpr int MAX_PREVIEW_CLIENTS = 4;
    platform::TcpConnection previewClients_[MAX_PREVIEW_CLIENTS];
    static constexpr int kParkedSlots = 4;          // connections open with no request yet
    static constexpr uint32_t kParkMs = 3000;       // how long one waits for its request before it is closed
    platform::TcpConnection parked_[kParkedSlots];
    uint32_t parkedAt_[kParkedSlots] = {};

    // Every upload, and every /sd download, resumes here across ticks, a bounded slice at a time, drainPreviewSend's shape: neither a large file nor a slow client holds the render loop.
    struct FileTransfer {
        bool active = false;
        bool isUpload = false;
        platform::TcpConnection conn;
        platform::FsWriteStream wstream;   // valid while isUpload
        platform::FsReadStream  rstream;   // valid while !isUpload
        size_t remaining = 0;              // upload: body bytes still expected; download: bytes left to send
        char* buf = nullptr;               // kTransferBufBytes from the claim to the finish, so an idle server holds none
        size_t bufLen = 0;                 // download only: the read chunk, drained across ticks by writeSome
        size_t bufSent = 0;
        uint32_t lastActivityMs = 0;       // a stall guard, not a size or time cap on the transfer itself
        char path[160] = {};               // upload only: the file applyFileChanged announces once it commits
    };
    static constexpr int kTransferSlots = 2;
    static constexpr size_t kTransferBufBytes = 4096;
    static constexpr uint32_t kTransferStallMs = 10000;   // no progress this long: abort, free the slot
    static constexpr uint32_t kTransferTickBudgetMs = 3;  // several chunks while storage/peer keep up, not one
    std::array<FileTransfer, kTransferSlots> transfers_{};
    /// A free slot with its buffer, or null once it has answered: 503 when both slots are busy, 507 when the buffer does not fit.
    FileTransfer* claimTransfer(platform::TcpConnection& conn);
    /// Advance every active slot by one bounded chunk; called once per tick, the drainXxxSend's sibling.
    void driveTransfers();
    /// One upload slot's bounded chunk: socket to storage, then the stall guard. Returns whether real bytes moved.
    bool driveUpload(FileTransfer& t, uint32_t now);
    /// One download slot's bounded chunk: storage to socket, then the stall guard. Returns whether real bytes moved.
    bool driveDownload(FileTransfer& t, uint32_t now);
    /// Close the connection when asked, free the buffer and the slot; the caller closed the stream.
    void finishTransfer(FileTransfer& t, bool closeConn);
    /// Close every active slot's stream and socket; release()'s own cleanup, since going out of scope frees neither.
    void releaseTransfers();

    ClientMessageSink* clientSink_ = nullptr;   // the producer's inbound-message sink (PreviewDriver) @xref{crossing-the-domain-boundary}

    // One resumable frame: a copied header plus a pointer into the caller's stable body buffer. @xref{why-the-preview-has-its-own-connections}
    struct PreviewSend {
        // 24, not 16: a payload over 64 KB needs the ten-byte length form plus the app headers.
        uint8_t hdr[24] = {};                 // WS + app header, copied (caller's may be a stack local)
        size_t hdrLen = 0;
        const uint8_t* body = nullptr;        // the frame body; ownsBody says who frees it
        size_t bodyLen = 0;
        size_t sent[MAX_PREVIEW_CLIENTS] = {};  // per-PREVIEW-client cursor over [hdr ++ body]; a slow client lags
        bool active = false;
        // Borrowed, not owned: the producer keeps it alive and cancels before a resize frees it.
    };
    PreviewSend previewSend_;
public:
    /// What one control client views: modules named at any depth by name or type, each bringing its descendants. @xref{state-by-subscription}
    struct Subscription {
        static constexpr uint8_t kMaxModules = 8;   ///< a page names its root and five types it reads beside it
        bool received = false;             ///< the client has subscribed, which is what its first document answers
        bool all = false;                  ///< `"*"`: the whole tree
        uint8_t count = 0;                 ///< how many names are held
        uint32_t names[kMaxModules] = {};  ///< each name's FNV-1a, a module name or a type name of any length
        /// Whether `mod` or one of its ancestors is named, by its name or its type.
        bool covers(const MoonModule* mod) const;
        /// Whether `s` is one of the names held.
        bool names_(const char* s) const;
        /// Read `{"subscribe":[…]}`, false when the body holds no such array.
        bool parse(const char* body);
        /// Whether two subscriptions name the same modules.
        bool operator==(const Subscription& o) const;
    };

    /// A depth-first position in the module tree, which one serialization resumes from. @xref{the-full-state-streams}
    struct TreeCursor {
        static constexpr uint8_t kMaxDepth = 8;   ///< deeper than any tree MoonLight builds (root, module, child, grandchild); a deeper child would end its parent's object childless
        const Subscription* sub = nullptr;   ///< null: every module in full
        MoonModule* single = nullptr;        ///< one module alone, without the `{"modules":[…]}` wrapper
        uint8_t idx[kMaxDepth] = {};         ///< the child index at each level of the path
        uint8_t depth = 0;                   ///< levels on the path, 0 before the document begins
        uint8_t phase = 0;                   ///< the deepest module's part in progress: header, controls or children
        uint8_t ctrl = 0;                    ///< the next control, in the controls part
        bool wroteRoot = false;              ///< a root went out, so the next one takes a comma
        bool done = false;                   ///< the document has ended
        static constexpr uint8_t kHeader = 0, kControls = 1, kChildren = 2;   ///< the values of `phase`
        uint32_t answers = 0;                ///< the `resync` request the streamed state answers, echoed at its end; 0 for none
    };
    /// Append from `c` until the sink holds `target` bytes or the document ends, true once it has ended; 0 means no target.
    bool writeTreeFragment(JsonSink& sink, TreeCursor& c, size_t target);
    /// One step where the cursor's path names no module: close what is open and move on, true once the document has ended.
    bool stepPast(JsonSink& sink, TreeCursor& c);
    /// One step of the module at the cursor: its header, a control, or its children.
    void stepInto(JsonSink& sink, TreeCursor& c, MoonModule* mod);

    /// Set a client slot's subscription, as its `{"subscribe":…}` frame does, for a test.
    void subscribeForTest(int client, const char* body) { applySubscription(client, body); }
    /// Mark a client's stream open without draining it, for a test of what waits while a document is under way.
    void holdStreamForTest(int client) { streams_[client].active = true; }
    /// Collect the next patch once and write each of the first `count` client slots its share, for a test with no socket.
    void buildClientPatchesForTest(JsonSink* outs, int count);

private:
    // The resumable fragment a stream sends at a time: small enough for a fragmented heap, large enough that frame headers stay noise.
    static constexpr size_t kStreamFragmentBytes = 2048;
    // One client's full state in flight: the cursor, and the fragment leaving as one WebSocket frame. @xref{the-full-state-streams}
    struct StateStream {
        bool active = false;
        Subscription sub;          // copied at the start, so a subscribe arriving mid-document cannot switch the view halfway
        TreeCursor cursor;
        char* frame = nullptr;     // the fragment in flight, owned
        size_t frameLen = 0;
        uint8_t hdr[10] = {};
        size_t hdrLen = 0;
        size_t sent = 0;           // over hdr ++ frame
        bool first = true;         // the next frame opens the message
        bool last = false;         // the frame in flight ends it
    };
    StateStream streams_[MAX_WS_CLIENTS];
    Subscription subs_[MAX_WS_CLIENTS];
    bool resyncClient_[MAX_WS_CLIENTS] = {};   // this client alone needs a full state: it subscribed, changed what it views, or asked again
    uint32_t resyncId_[MAX_WS_CLIENTS] = {};   // the id of the request a client's next document answers
    bool fragmentRefused_ = false;   // a fragment found no memory; logged once per run of refusals
    // Whether a frame may go to this client now: no fragment of its stream is half written. The uplink is read only then, so a Ping is always answered between frames (RFC 6455 §5.4).
    bool atFrameBoundary(int client) const;
    // Answer a Ping with a Pong, or a Close with a Close and the end of the connection. @xref{ping-and-close}
    void onControlFrame(int client, uint8_t opcode, const uint8_t* payload, size_t len);
    // Write one unfragmented control frame, `opcode` with FIN, carrying `payload`.
    static bool sendWsControlFrame(platform::TcpConnection& conn, uint8_t opcode, const uint8_t* payload, size_t len);
    // Guards the preview channel alone, whose producers sit on two cores under the split.
    mutable TryLock wsLock_;
    // Drain one chunk per client, finishing when every live client holds the whole frame. @xref{why-the-drain-runs-on-the-transport-tick}
    void drainPreviewSend();
    // Advance every open stream by one bounded chunk, generating each fragment as the previous one leaves. Called from tick20ms.
    void drainStateSend();
    // One client's stream, as far as `budget` bytes and the socket allow.
    void drainStream(int client, size_t budget);
    // A fragment found no memory: close the client, which reconnects and is sent its document again.
    void refuseFragment(int client);
    // Give each client due a document its stream, from the baseline taken this pass.
    void startStreams();
    // Generate a stream's next fragment and its frame header, false when even one fragment does not fit in memory.
    bool nextStreamFrame(int client);
    // Free a stream's fragment and close it.
    void endStream(StateStream& s);
    // Whether any client is mid-document, which holds every other frame on the control channel.
    bool anyStreamActive() const;
    // Whether some client alone waits for a full state.
    bool anyClientResync() const;
    // Store a client's subscription, resyncing when it changed.
    void applySubscription(int client, const char* body);
    // The module at a cursor's path, or null when the deepest index has run past its siblings.
    MoonModule* moduleAt(const TreeCursor& c) const;
    // Derived from free contiguous memory, so a tight board takes small bites and a roomy one drains fast.
    size_t drainChunkBytes() const;

    // The wire diff's cache: one path hash and one value hash per leaf, eight bytes each. @xref{how-a-leaf-is-named}
    struct LeafHash { uint32_t path = 0; uint32_t value = 0; };
    // Growable rather than fixed: sized to the exact leaf count each baseline, so no leaf is dropped.
    ScratchBuffer<LeafHash> leafHashes_{*this};
    uint16_t leafHashCount_ = 0;
    bool fullResyncPending_ = true;    // send a full state next push (set on connect / structural change)
    // Emit only the leaves whose value hash moved, returning how many there were. @xref{state-push-a-diff-on-the-wire}
    uint16_t buildStatePatch(JsonSink& sink);
    // One module's changed leaves, a slice of the patch entries that each client takes when its subscription covers the module.
    struct PatchSlice { const MoonModule* mod; uint32_t begin; uint32_t end; uint16_t count; };
    // Sized at each baseline to the modules it covers, one slice each.
    ScratchBuffer<PatchSlice> patchSlices_{*this};
    // The changed leaves over what `walk` visits, as comma-led entries in one slice per module, returning how many there were.
    template <class Walk> uint16_t collectPatch(JsonSink& entries, uint16_t& sliceCount, Walk&& walk);
    // Wrap the slices `sub` covers as one patch frame, null covering all, returning how many entries it holds.
    uint16_t writeClientPatch(JsonSink& out, const JsonSink& entries, uint16_t sliceCount, const Subscription* sub);
    // Collect over `walk`, then send each control client the share its subscription covers; a client about to get a full state is skipped when `skipResyncing`.
    template <class Walk> void sendPatches(Walk&& walk, bool skipResyncing = false);
    // Whether some client's subscription covers `mod`, so its leaves are worth hashing.
    bool anyClientCovers(const MoonModule* mod) const;

    // Modules whose changed values go out ahead of the periodic patch, by name since one may be deleted before the tick.
    static constexpr uint8_t kSoonModules = 4;
    // At most 25 early patches a second: smooth for a motor, and half the frames of one per tick. @xref{a-value-someone-watches-move}
    static constexpr uint32_t kSoonMs = 40;
    char soon_[kSoonModules][16] = {};   // a module name's own size
    uint8_t soonCount_ = 0;
    uint32_t soonSentMs_ = 0;
    // Patch the modules that asked over the whole tree, and forget them, for a test.
    uint16_t buildSoonPatch(JsonSink& sink);
    // Visit the leaves of the modules that asked, and forget them.
    template <class Fn> void forEachSoonLeaf(Fn&& fn, bool everything = false);
    // Send the early patch, held while a full state drains and paced by kSoonMs.
    void pushSoonPatch();
    // Routes a module's request to the live instance.
    static void onValuesChanged(MoonModule* mod);
    // Re-hash every subscribed leaf without emitting, so the next patch reports only changes since now; `everything` covers the whole tree, for a test.
    void baselineLeafHashes(bool everything = false);
    // Visit every UI leaf some client subscribes to, in the state's own order, templated so the lambda inlines; `everything` visits the whole tree, for a test.
    template <class Fn> void forEachStateLeaf(Fn&& fn, bool everything = false);
    // One module's own leaves, `fn` taking the module first so the patch knows whose slice each belongs to.
    template <class Fn> void visitOwnLeaves(MoonModule* mod, Fn&& fn);
    // A subtree's leaves, skipping a module no client covers unless `everything`.
    template <class Fn> void visitModuleLeaves(MoonModule* mod, Fn&& fn, bool everything = false);
    LeafHash* findLeaf(uint32_t pathHash);
    // The next push must be a full state and a fresh baseline. @xref{when-a-patch-cannot-describe-the-change}
    void requestFullResync() { fullResyncPending_ = true; }

    // Routes any module's schema change to the live instance's resync request.
    static void onSchemaChanged();
    static inline HttpServerModule* instance_ = nullptr;
    // One frame cut in half by a read waits here for the rest, for one client at a time: the bridges' writes are small.
    uint8_t  carry_[kWsTextMax + 16] = {};
    size_t   carryLen_ = 0;
    int      carryClient_ = -1;
    uint16_t boundPort_ = 0;   // the port open() actually bound; 0 when no server is live

    // HTTP handling
    /// Serve one connection; false when no request arrived within `patienceMs`, the connection left open to be parked.
    bool handleConnection(platform::TcpConnection& conn, int patienceMs = 5);
    void park(platform::TcpConnection& conn);
    void serveParked();
    void serveConnections();
    /// Whether `conn`'s client is on the device's own access point, which hides stored secrets from it.
    static bool onAccessPoint(const platform::TcpConnection& conn);
    bool anyWsClientOnAccessPoint() const MM_NONBLOCKING;
    void sendResponse(platform::TcpConnection& conn, int status, const char* contentType, const char* body);
    /// Answer a refused document as `PATCH /api/state` does: the engine's error and where it is, with status 400.
    void sendRefusal(platform::TcpConnection& conn, const StateDocumentResult& r);
    /// The child module a delete or replace may act on: found, not top-level, and one the user can remove.
    OpResult editableChild(const char* moduleName, MoonModule*& mod);
    void sendPreflightResponse(platform::TcpConnection& conn);
    void serveFile(platform::TcpConnection& conn, const char* filename, const char* contentType);

    // A file body is not a control value, so the File Manager has its own vetted endpoints. @xref{mutations-and-the-file-manager}
    void serveFileContents(platform::TcpConnection& conn, const char* query);
    /// The one streamed-file sender both file routes share: fs path, MIME, extra header lines.
    void streamFsFile(platform::TcpConnection& conn, const char* path, const char* mime,
                      const char* extraHeaders);
    /// One HLS artifact (playlist / segment) from /.hls/, video MIME + no-cache; flat names only.
    void serveHlsFile(platform::TcpConnection& conn, const char* name);
    // Checks an upload against its storage's limits, then streams it through a transfer slot, so an upload of any size never needs one buffer.
    void handleWriteFile(platform::TcpConnection& conn, const char* query,
                         const char* initialBody, size_t initialLen, size_t contentLen);
    // Claims a transfer slot for a checked upload, writes the bytes already buffered and hands the rest to driveTransfers.
    void startUpload(platform::TcpConnection& conn, const char* path,
                     const char* initialBody, size_t initialLen, size_t contentLen);
    // One directory's children, single-level, which is what the lazy tree loads a node from.
    void serveDirListing(platform::TcpConnection& conn, const char* query);
    void handleMakeDir(platform::TcpConnection& conn, const char* query);      // POST /api/dir?path=
    void handleRemoveEntry(platform::TcpConnection& conn, const char* query);  // DELETE /api/dir?path=

    // JSON state @xref{why-the-sink-helpers-are-members}
    void serveState(platform::TcpConnection& conn);
    void buildStateJson(JsonSink& sink);
    void writeModuleJson(JsonSink& sink, MoonModule* mod);
    // A module's fields before its controls, which an outline carries alone.
    void writeModuleHeader(JsonSink& sink, MoonModule* mod);
    static void writeControl(JsonSink& sink, const ControlDescriptor& c);
    // Emit a module's status and severity where set, shared so the two endpoints stay in sync.
    static void writeStatus(JsonSink& sink, MoonModule* mod);

    // Control setter
    void handleSetControl(platform::TcpConnection& conn, const char* body);
    /// Apply a state document from the request body, which may outgrow the request buffer: `PATCH /api/state`.
    void handleApplyState(platform::TcpConnection& conn, const char* initialBody, size_t initialLen, size_t contentLen);

    // Delegates to the scheduler's own canonical tree walk, guarding a null scheduler first.
    MoonModule* findModuleByName(const char* name);

    // System metrics
    void serveSystem(platform::TcpConnection& conn);
    /// The WLED shim's routes: one lists the device, the others carry and accept its state. @xref{the-wled-compatibility-shim}
    void serveWledInfo(platform::TcpConnection& conn);
    void serveWledState(platform::TcpConnection& conn);
    void serveWledStateInfo(platform::TcpConnection& conn);
    void serveWledDeviceJson(platform::TcpConnection& conn);   ///< /json, the surface Home Assistant's WLED integration reads
    void serveWledPresets(platform::TcpConnection& conn);      ///< /presets.json, the looks Home Assistant lists
    void handleWledState(platform::TcpConnection& conn, const char* body);
    /// Read every client's uplink: preview requests on `/wsp`, and on `/ws` control writes, subscriptions, resync requests, Ping, Close and the WLED app's frame.
    void pollUplinks();
    void pollPreviewClient(int client);
    void pollControlClient(int client);
    // What a text frame from a control client asks for.
    void onTextFrame(int client, const char* body);
    void writeWledInfoBody(JsonSink& sink, const char* name, const uint8_t mac[6]);
    void writeWledName(JsonSink& sink, const char* name);   // 💫-prefixed WLED name (HA marker)
    void writeWledStateBody(JsonSink& sink);
    /// The device's name, MAC and live address, resolved once for every WLED handler to share. @xref{prior-art}
    void resolveWledIdentity(const char*& name, uint8_t mac[6], uint8_t ip[4],
                             const char* nameFallback = "MoonLight");
    void writeModuleMetricsJson(JsonSink& sink, MoonModule* mod, bool& first);

    // Module CRUD
    void handleAddModule(platform::TcpConnection& conn, const char* body);
    void handleDeleteModule(platform::TcpConnection& conn, const char* moduleName);
    void handleReplaceModule(platform::TcpConnection& conn, const char* moduleName, const char* body);
    void serveTypes(platform::TcpConnection& conn);
    // The compiled-in script catalog, so the UI can offer a script the device does not hold yet.
    void serveScriptCatalog(platform::TcpConnection& conn);

    /// One module's JSON, identical to its entry in the full state, its name possibly encoded. @xref{the-per-module-state-route}
    void serveModule(platform::TcpConnection& conn, const char* name);
    void writeTypeDefaults(JsonSink& sink, const char* typeName);
    void handleMoveModule(platform::TcpConnection& conn, const char* moduleName, const char* body);
    // The editable-list primitive, the tail naming a module, a control and optionally a row.
    void handleListAddRow(platform::TcpConnection& conn, const char* tail);
    void handleListPatchRow(platform::TcpConnection& conn, const char* tail, const char* jsonBody);
    void handleListDeleteRow(platform::TcpConnection& conn, const char* tail);
    void handleListApplyRow(platform::TcpConnection& conn, const char* tail, size_t tailLen);
    ListSource* resolveEditableList(platform::TcpConnection& conn, const char* tail,
                                    uint32_t& outId, bool& outHasId);
    MoonModule* listMutationModule_ = nullptr;  // module whose list a CRUD op resolved to (for markDirty)
    void afterListMutation();
    void handleReboot(platform::TcpConnection& conn);
    void handleBootMoonBase(platform::TcpConnection& conn);
    /// Take a firmware URL and answer at once, the install running on its own task. @xref{two-firmware-install-paths}
    void handleFirmwareUrl(platform::TcpConnection& conn, const char* body);
    void handleFirmwareUpload(platform::TcpConnection& conn, const char* initialBody,
                              size_t initialLen, size_t contentLen);   // POST /api/firmware/upload
    /// Install a MoonBase image into the factory slot, vetting it before erasing anything.
    void handleMoonBaseUpload(platform::TcpConnection& conn, const char* initialBody,
                              size_t initialLen, size_t contentLen);   // POST /api/firmware/moonbase-update
    /// The same install, fetched by the device itself from a URL.
    void handleMoonBaseUrl(platform::TcpConnection& conn, const char* body);   // POST /api/firmware/moonbase-update-url

    // WebSocket
    /// Complete the handshake, `previewChannel` putting the connection on the lossy channel.
    void handleWebSocketUpgrade(platform::TcpConnection& conn, const char* req,
                                bool previewChannel = false);
    void pushStateToWebSockets();
    void pushWledStateToWebSockets();   // WLED-app {state,info} frame on /ws (see impl)
    static bool sendWsTextFrame(platform::TcpConnection& conn, const char* data, int len);
};

} // namespace mm
