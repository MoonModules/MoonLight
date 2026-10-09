#pragma once

#include "core/module/MoonModule.h"
#include "core/util/TryLock.h"   // the cross-thread sender latch (wsLock_)
#include "core/util/ScratchBuffer.h"   // leafHashes_: the growable diff-on-the-wire value-hash cache
#include "core/util/BinaryBroadcaster.h"
#include "platform/platform.h"

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
/// This is core infrastructure, and includes no light header.
/// The WLED-compatibility shim reads the device shape and the palettes as colors through `LightOutput`, which the light domain implements, and drives the palette through `Scheduler::setControl`.
///
/// The implementation lives in the `.cpp`, and this header is the interface alone. The `port` control defaults to 8080 on the desktop and 80 on an ESP32.
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
/// A mutation is `POST /api/control` with a module, control and value; `POST /api/modules` to create; `POST /api/modules/{name}/move` to reorder and `.../replace` to swap; `POST /api/reboot`; or `DELETE /api/modules/{name}`.
///
/// The File Manager reads and writes through `GET|POST /api/file?path=`, and lists, creates and removes through `/api/dir?path=`. The path rides the query, so a filesystem operation carries its target in the request rather than in a stored control.
///
/// Every JSON response streams through a `JsonSink`, so there is no fixed-buffer ceiling and a tree of any size serializes correctly.
///
/// ## The two WebSocket channels
///
/// `GET /ws` with an `Upgrade: websocket` header does the RFC 6455 handshake, SHA-1 and base64.
/// Traffic is split by class across two channels sharing one lwIP socket budget, each with its own cap. `/ws` carries the control plane, the JSON state and its patches, and `/wsp` the lossy binary preview stream.
///
/// Every binary message takes one path: the resumable buffered send.
/// It drains a memory-adaptive chunk per client per 20 ms tick, from a stable caller-owned buffer. A large frame is therefore delivered across wall-clock ticks without any loop waiting on a socket, while staying one atomic WebSocket message.
/// One buffered send is in flight per slot at a time, newest-wins: a new offer arriving while one is active is dropped. A client is closed only on a real error or a FIN, never for being slow.
///
/// ## What a preview client may send back
///
/// Inbound `/wsp` payloads are unmasked and handed opaquely to the registered producer sink, whose vocabulary is a standing frame request and a one-shot table request.
/// On `/ws` a client may write a control as `{"module","control","value"}`, which the input bridges use at up to 30 writes a second; every other mutation goes through REST.
///
/// ## State push: a diff on the wire
///
/// This is the recognizable snapshot-then-patch model, the shape Redux and Firestore sync use and RFC 6902 formalises.
///
/// The state a client needs is the full module tree, around 30 KB and mostly unchanging option and detail metadata. Re-serializing all of it every second, inline on the render thread, stole render budget and stuttered the LEDs at 1 Hz.
///
/// So a client gets the full state once on connect, chunk-drained through the resumable sender and off the render tick.
/// Each second after that it gets a patch carrying only the values that changed. Change is found by value-compare rather than a dirty flag: each leaf's value is serialized, hashed with FNV-1a, and compared against a cached hash.
/// A value the device mutates itself, telemetry or a status or a driver, is therefore caught exactly like a `setControl` write. No per-write instrumentation is needed.
///
/// ## A value someone watches move
///
/// A module whose value moves in front of someone, a desk's motorized faders, calls `notifyValuesChanged()`. Its changed leaves then go out on the next 20 ms tick in a patch of their own, at most every 40 ms, rather than on the next second.
/// Only that module is walked, and the hashes update as it goes, so the second's patch does not send the values again.
///
/// ## How a leaf is named
///
/// A leaf path is the module name and the control name, or an `@`-prefixed field for live per-card header telemetry. Module names are unique tree-wide, so a path is stable.
///
/// ## When a patch cannot describe the change
///
/// The hash cache is one global baseline rather than one per client, held in a growable scratch buffer.
/// A full resync re-sends the whole state and re-baselines. That happens on connect and after any structural change, since a value patch cannot describe a reshaped tree.
///
/// A schema change forces one too, through a static schema-changed hook. The value patch carries no changed hidden flags or option sets. Any `rebuildControls` triggers it: a control set, a list mutation, an asynchronous WiFi or Hue callback.
///
/// A pending resync is fast-pathed on the 20 ms tick rather than only the 1 s one, and preempts an in-flight preview frame.
/// A freshly connected client therefore gets its state, and so its preview, within tens of milliseconds rather than up to a second.
///
/// ## What the diff buys
///
/// The net effect is a per-second push falling from about 34 KB to one or two. The expensive full-tree serialize runs only on connect or a schema change.
/// The UI applies a patch in place, with no rebuild, and re-renders on a full frame. That is the sub-hot-path rule applied: a periodic tick shares the render thread, so its work must be cheap or skipped when nothing changed.
///
/// ## Why the drain runs on the transport tick
///
/// The resumable drain runs on the 20 ms transport poll and deliberately not the per-render `tick`. Pushing preview bytes to a socket is therefore never charged to the LED render hot path.
/// The LED output is never delayed by the preview. The preview frame rate is instead bounded by the 20 ms drain cadence, which is the right trade: the preview is a view and the LEDs are not.
///
/// ## The WLED-compatibility shim
///
/// A small set of WLED-shaped messages make a MoonLight device appear in the native WLED apps and Home Assistant's WLED integration, and be controlled from them.
///
/// Discovery is over mDNS.
/// Validation is a minimal `GET /json/info` carrying a name, a MAC, LED and WiFi objects, and the brand and product fields. The app keys on the brand to accept a device: we interoperate rather than impersonate, and this is no full WLED emulation.
///
/// Live state is pushed over `/ws` as a state-and-info frame. The state mirrors the Drivers brightness control, and its color is the active palette's, from `LightOutput::paletteRgb()`, or black on a device with no light output.
///
/// Control is bidirectional over that same socket. The app's slider and toggle send a frame that is read and applied to Drivers brightness through the shared apply core, the path REST and Improv also use.
///
/// ## Reading an uplink frame, and taking the lease
///
/// The uplink parser handles RFC 6455 framing alone, and the payload's meaning belongs to the registered sink. It returns a payload length, or -1 when the buffer holds no complete frame.
/// It also reports how many bytes the whole frame consumed, so a caller can walk a read that coalesced several. It is pure and static, so the byte handling is unit-tested without a socket.
///
/// The sender lease guards the send state and the socket writes against this module's own core-0 drain and state push, while an offloaded `PreviewDriver` streams from core 1.
/// It try-locks rather than waits: a busy transport returns false and the producer skips that frame.
///
/// ## The per-module state route
///
/// One module's JSON is served on its own route, byte-identical to that module's entry in the full state, children included.
/// An issue report carries it from the card's `{ }` popup, and `/document` on the same route serves the module as the state document a preset holds. A name in the path may be percent-encoded, since a module name can carry a space.
///
/// ## Two firmware install paths
///
/// A firmware URL is answered at once and installed on its own task, so the browser polls for progress rather than holding a request open.
/// On a MoonBase device the URL is staged in NVS and the device reboots into MoonBase, which installs it unattended; otherwise the platform fetches straight to the OTA slot.
///
/// Installing a new MoonBase into the factory slot is the mirror of that, and the only way to fix a broken recovery image without a cable.
/// It vets the image before erasing anything and deliberately does not reboot, so the running app is untouched. The URL form of it is what makes a release asset installable without the browser relaying most of a megabyte.
///
/// ## Crossing the domain boundary
///
/// This module exposes the `BinaryBroadcaster` interface, and the light-domain `PreviewDriver` holds a pointer to it and streams each frame's bytes through it.
/// `main.cpp` wires the two together, being the only file that knows both, and the preview's point budget and wire format are the driver's concern.
///
/// ## The apply core: the REST API, callable in process
///
/// The add, set and clear-children operations are factored out of the connection so any transport can drive them.
/// Two do today: the HTTP handlers, thin wrappers mapping a result onto a status code, and the Improv serial path, which applies a pushed op on the main loop.
/// That gives the apply logic one home, and transports differ only in how they frame a request and report its result.
///
/// A file write is the second way persistent state changes, so it ends in the same request that a control write does.
/// Putting it here rather than in whichever client remembers to send a follow-up nudge is what makes it reliable, since `curl` and MoonDeck would not.
///
/// The re-derive asks the whole tree rather than consulting a path-to-module registry, which would need an association nothing else in the system keeps.
/// A module decides for itself whether what it holds actually changed. `prepare` is the cold path that exists to be re-entered, and a scripted module compares a content hash rather than re-reading its source.
/// The path is accepted for diagnostics and a future narrower dispatch; today every successful write asks the same question.
///
/// ## Two guards that are static on purpose
///
/// The filesystem path decoder is the single guard every filesystem entry shares, for reads, writes, listings, directory creation and deletion. It refuses a missing or empty path, a `..` traversal, and an overlong value that would fill the buffer.
///
/// The header lookup is case-insensitive because RFC 9112 says field names are. Browsers send one spelling and node's client another, and the case-sensitive search this replaced silently read a length of zero and committed empty files with a 200.
///
/// Both are public and static so the byte handling is unit-tested without a socket fixture.
///
/// ## What the preview lock guards, and what it does not
///
/// The lock covers the preview channel's shared state, its send bookkeeping and its sockets, because those have producers on two cores once the multicore split engages.
/// Core 1 arms frames and streams the coordinate table from the offloaded driver's tick, while core 0 touches the same sockets in the drain, the uplink reap and admission.
///
/// Three things go wrong without it. A partial write on one core interleaves with the other inside a single frame and corrupts the framing. A close lands under a concurrent write on the same descriptor. Or one side observes torn send state.
///
/// The control channel is deliberately outside its scope, since every writer there runs on core 0.
/// It try-locks and never blocks: whichever core loses the race skips its slot, which the hot-path rule requires. A lost race costs one preview frame, or defers a reap or an admission by a tick, and never stalls a render or an encode.
///
/// ## The served port is not the port control
///
/// `servedPort` reports what the listener actually bound, or zero when none is up, and it is the one true source for any module that must print its own URL. The `port` control is mutable and says what was asked for, which is a different question.
///
/// The schema hook can be installed without opening the listener.
/// A unit test proving the hook fires a resync needs no socket, and binding a port under test is flaky when that port is busy. A release unwires it exactly as it would after a real setup.
///
/// ## How the two channel caps are sized
///
/// The control channel allows eight clients, sized for a few concurrent viewers plus the transient overlap when one refreshes.
/// A browser opens the new socket before the OS delivers the old one's FIN, so both briefly hold a slot. The dead one is reaped within a tick or two, on its next failed send.
/// At four, a couple of quick refreshes filled every slot with not-yet-reaped connections and the new upgrade was rejected outright. Eight leaves headroom so a refresh always lands a slot, and a slot is only a descriptor and a small cursor.
///
/// ## Why the preview cap is lower
///
/// The preview channel allows four, deliberately fewer. Both arrays draw on one lwIP socket budget of sixteen, shared with HTTP, mDNS, Art-Net, MQTT and OTA.
/// A preview socket per control client would consume the whole budget at the cap and starve the rest. Four is sized from observed use: one browser almost always, two often enough that it must simply work, more only occasionally.
/// A REST caller such as Home Assistant never opens a preview socket at all. A refused upgrade costs that client only its preview, leaving its control connection untouched.
///
/// ## Why the preview has its own connections
///
/// They are separate TCP connections, so a 10 KB preview frame can never delay a state push. That is the standard remedy for the head-of-line blocking one socket carrying both traffic classes produces.
///
/// The state push has its own send slot for the same reason. Sharing the preview's routed the full state to the preview channel and starved every control client of its resync.
/// The preview's body is borrowed rather than owned, the producer keeping its pixel buffer alive and cancelling before a resize frees it.
/// The header buffer is 24 bytes and not 16: a payload over 64 KB takes the ten-byte length form, and the app headers add eleven more. At 16, every uncapped-size frame was silently refused.
///
/// ## Why the sink helpers are members
///
/// The `JsonSink` helpers below are private members rather than free functions, because each reads this module's own state or calls another of its members.
///
/// Three pieces live in their own headers, all in the same namespace so the call sites are unchanged.
/// They are the sink itself with its escaping writer, the SHA-1 of RFC 3174 for the handshake, and the base64 encoder the handshake and password obfuscation share.
///
/// ## Prior art
///
/// The shim's exact field requirements were reverse-engineered from the [WLED-Android client](https://github.com/Moustachauve/WLED-Android) by Christophe Gagnier.
/// Its mDNS browse, its `/json/info` validation with a non-empty MAC check, its models and its live-state socket client told us precisely what the app reads.
/// Knowing that is why the shim is the minimal accepted object rather than a guessed full emulation.
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
    /// Hand each complete masked text frame in `buf` to `onText`, returning how many bytes it consumed; a frame cut off at the end is left unconsumed.
    template <class OnText>
    static size_t walkWsTextFrames(const uint8_t* buf, size_t total, OnText&& onText) {
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
                const uint8_t* mask = fr + hdr;
                char body[kWsTextMax];
                for (size_t i = 0; i < len; i++) body[i] = static_cast<char>(fr[hdr + 4 + i] ^ mask[i & 3]);
                body[len] = 0;
                onText(body);
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
    void baselineLeafHashesForTest() { baselineLeafHashes(); }
    /// Ask for a full resync, for a test.
    void requestFullResyncForTest() { requestFullResync(); }
    /// Whether a full resync is pending, for a test.
    bool fullResyncPendingForTest() const { return fullResyncPending_; }
    /// Clear the pending resync, for a test.
    void clearFullResyncForTest() { fullResyncPending_ = false; }
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
    // The same shape as PreviewSend, but draining to the control channel and owning its body.
    struct StateSend {
        uint8_t hdr[16] = {};
        size_t hdrLen = 0;
        const uint8_t* body = nullptr;
        size_t bodyLen = 0;
        size_t sent[MAX_WS_CLIENTS] = {};
        bool active = false;
    };
    StateSend stateSend_;
    // Guards the preview channel alone, whose producers sit on two cores under the split.
    mutable TryLock wsLock_;
    // Queue an owned text body into the state slot, freeing it and refusing when one is in flight.
    bool startBufferedTextSend(char* ownedBody, size_t bodyLen);
    // Drain one chunk per client, finishing when every live client holds the whole frame. @xref{why-the-drain-runs-on-the-transport-tick}
    void drainPreviewSend();
    // Same, for the in-flight full-state send to /ws clients. Called from tick20ms.
    void drainStateSend();
    // Derived from free contiguous memory, so a tight board takes small bites and a roomy one drains fast.
    size_t drainChunkBytes() const;

    // The wire diff's cache: one path hash and one value hash per leaf, eight bytes each. @xref{how-a-leaf-is-named}
    struct LeafHash { uint32_t path = 0; uint32_t value = 0; };
    // Growable rather than fixed: sized to the exact leaf count each baseline, so no leaf is dropped.
    ScratchBuffer<LeafHash> leafHashes_{*this};
    uint16_t leafHashCount_ = 0;
    bool fullResyncPending_ = true;    // send a full state next push (set on connect / structural change)
    // Emit only the leaves whose value hash moved, returning how many there were. @xref{state-push-a-diff-on-the-wire} @xref{what-the-diff-buys}
    uint16_t buildStatePatch(JsonSink& sink);
    // The same, over the leaves `walk` visits, which is the whole tree or the modules that asked early.
    template <class Walk> uint16_t buildPatch(JsonSink& sink, Walk&& walk);
    // Send one patch frame to every control-channel client.
    void sendPatch(const JsonSink& sink);

    // Modules whose changed values go out ahead of the periodic patch, by name since one may be deleted before the tick.
    static constexpr uint8_t kSoonModules = 4;
    // At most 25 early patches a second: smooth for a motor, and half the frames of one per tick. @xref{a-value-someone-watches-move}
    static constexpr uint32_t kSoonMs = 40;
    char soon_[kSoonModules][16] = {};   // a module name's own size
    uint8_t soonCount_ = 0;
    uint32_t soonSentMs_ = 0;
    // Patch the modules that asked, and forget them.
    uint16_t buildSoonPatch(JsonSink& sink);
    // Send the early patch, held while a full state drains and paced by kSoonMs.
    void pushSoonPatch();
    // Routes a module's request to the live instance.
    static void onValuesChanged(MoonModule* mod);
    // Re-hash every leaf without emitting, so the next patch reports only changes since now.
    void baselineLeafHashes();
    // Visit every UI leaf in the state's own order, templated so the lambda inlines.
    template <class Fn> void forEachStateLeaf(Fn&& fn);
    template <class Fn> void visitModuleLeaves(MoonModule* mod, Fn&& fn);
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
    // Streams the remainder off the socket, so an upload of any size never needs one buffer.
    void handleWriteFile(platform::TcpConnection& conn, const char* query,
                         const char* initialBody, size_t initialLen, size_t contentLen);
    // One directory's children, single-level, which is what the lazy tree loads a node from.
    void serveDirListing(platform::TcpConnection& conn, const char* query);
    void handleMakeDir(platform::TcpConnection& conn, const char* query);      // POST /api/dir?path=
    void handleRemoveEntry(platform::TcpConnection& conn, const char* query);  // DELETE /api/dir?path=

    // JSON state @xref{why-the-sink-helpers-are-members}
    void serveState(platform::TcpConnection& conn);
    void buildStateJson(JsonSink& sink);
    void writeModuleJson(JsonSink& sink, MoonModule* mod);
    void writeControls(JsonSink& sink, MoonModule* mod);
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
    void pollWledStateFromWebSockets();             ///< read app's slider/toggle sent over /ws
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
