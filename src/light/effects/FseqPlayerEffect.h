#pragma once

#include "light/effects/EffectBase.h"

#include "light/util/FseqFile.h"     // the FSEQ header/sparse-range parser this effect plays back
#include "platform/platform.h"       // fsReadStreamXxx, fsList, zlibInflateAll
#include "core/util/JsonSink.h"      // the playlist's own ListSource serialization
#include "core/util/JsonUtil.h"      // restoreList: recursive reader for the persisted playlist

#include <cstdio>
#include <cstring>
#include <new>   // placement new: scanDir's heap-allocated ScanLevel

namespace mm {

/// Plays FSEQ animation files from the filesystem onto this effect's layer, with a playlist and the standard transport controls.
/// @card FseqPlayerEffect.gif
///
/// FSEQ is the Falcon Player/xLights sequence format: a flat, per-frame byte stream with no pixel semantics of its own.
///
/// @guide
///
/// ## Compressed files need ESP32
///
/// A zlib-compressed FSEQ (xLights/FPP's default) plays only on ESP32 boards; desktop and Teensy builds show a clean error instead of playing it.
///
/// Zstd-compressed files do not play on any target yet.
///
/// Export a track uncompressed if it needs to play everywhere.
///
/// @technical
///
/// ## Prior art
///
/// FPP's `FSEQ_Sequence_File_Format.txt` is the spec this plays against; the playlist follows `FixtureProfilesModule`'s `EditableListSource` shape, the transport `DemoReelEffect`'s cycle/shuffle logic.
///
/// ## The rate comes from the file by default, or the fps slider when asked
///
/// `fpsSource` picks `file` (the track's own `stepTimeMs`, default) or `manual` (the `fps` slider). Either is capped at 50 to match `tick20ms()` (@xref{why-decode-runs-in-tick20ms}), with a warning when a file asks for more.
///
/// ## Why decode runs in tick20ms
///
/// A frame read blocks, which `tick()` must never do. So `tick20ms()` decodes one due frame into `frameBuf_` instead, the same split `HttpServerModule::driveSdTransfers` uses for SD reads; `tick()` only blits what is there.
///
/// ## Sequential reads replace random-offset ones
///
/// FSEQ data appears in file order. So one open stream per track, read forward only, replaces every offset-based read the format would otherwise need; a track restart or change reopens it.
///
/// A file whose variable-length metadata pushes `channelDataOffset` past the buffered header region has that gap skipped, byte for byte, before frame 0's own read begins (`skipHeaderGap`).
///
/// ## Where animations live
///
/// Every `.fseq` file anywhere in the file manager's tree is found walking from the filesystem root, bounded to `kMaxScanDepth` levels, with no special case for `/sd`. A flash file plays exactly like an SD one.
///
/// ## Starts on its own
///
/// Becoming enabled starts playback immediately when the playlist is non-empty, in `prepare()`, with no separate "press play" step. `release()` stops and closes the stream first, so a later re-enable never resumes into memory it just freed.
///
/// ## tick(): stopped clears, paused re-blits the same frame
///
/// A stopped transport goes black every tick rather than keep the previous effect's paint. A paused one re-blits the same decoded frame every tick instead of holding a stale buffer, so a clear elsewhere self-heals.
///
/// ## Compression: zlib on ESP32 only, zstd nowhere yet
///
/// Zlib (type 2) decodes through the ESP32 ROM's `tinfl_decompress` at zero flash cost and is ESP32-only to play (every target parses it). Zstd (type 1) has no decoder yet, reported as its own status, not a bug.
///
/// ## A corrupt file degrades, never overflows
///
/// `channelCount` sizes `frameBuf_` directly, however large; the allocation is checked for success, so a hostile value fails cleanly rather than writing into a null or short buffer.
///
/// A compression block's table entry is validated (`fseq::validateBlockRange`) before any buffer is sized by it: a non-advancing entry or an overflowing byte count is refused rather than read.
///
/// ## The scan is depth-bounded recursion over a heap-per-level buffer
///
/// `scanDir` mirrors `FilesystemModule::removeTree`'s shape for the same problem (an oversized stack frame): one heap-allocated `platform::fsList` level per recursion, bounded in depth, not a member-held breadth-first queue.
class FseqPlayerEffect : public EffectBase, public ListSource {
public:
    /// The transport's three states.
    enum class PlayState : uint8_t { Stopped, Playing, Paused };
    /// The three loopMode values, public so a test can select one by name.
    static constexpr uint8_t kLoopOff = 0, kLoopOne = 1, kLoopAll = 2;
    /// The two fpsSource values, public so a test can select one by name.
    static constexpr uint8_t kFpsFromFile = 0, kFpsManual = 1;

    /// Catalog tags: a recorded show, played back.
    const char* tags() const override { return "🖼️"; }

    /// Bind the transport buttons, the shuffle/loop controls, the playlist and the position gauge.
    void defineControls() override {
        controls_.addButton("play");
        controls_.addButton("pause");
        controls_.addButton("stop");
        controls_.addButton("previous");
        controls_.addButton("next");
        controls_.addControl("shuffle", shuffle_);
        controls_.addSelect("loopMode", loopMode_, kLoopModeOptions, 3);
        controls_.addSelect("fpsSource", fpsSource_, kFpsSourceOptions, 2);
        controls_.addControl("fps", fps_, 1, kMaxFps);
        controls_.setHidden(controls_.count() - 1, fpsSource_ != kFpsManual);
        controls_.addList("playlist", *this);
        controls_.addProgress("position", frameIndex_, frameCount_);   // a frame count, not bytes; ProgressUnit has no plain-count variant yet
        controls_.addButton("rescan");
    }

    /// Scan the filesystem for animations once.
    void setup() override {
        EffectBase::setup();
        rescanFiles();
    }

    /// Start playing on its own once the effect becomes enabled.
    void prepare() override {
        if (state_ == PlayState::Stopped && playlistCount_ > 0) {
            visited_ = 0;
            // Set before loadTrack: its own refreshStatus() must see Playing already, though loadTrack corrects this back on its own failure paths.
            state_ = PlayState::Playing;
            loadTrack(cursor_);
        }
    }

    /// Close the open stream and force a clean stop before the base frees the frame buffers.
    void release() override {
        closeTrackStream();
        state_ = PlayState::Stopped;
        EffectBase::release();
    }

    /// Decode the next due frame off the render tick; a blocking file read, bounded to one frame per call.
    void tick20ms() MM_NONBLOCKING override {
        if (state_ != PlayState::Playing) return;
        const uint32_t now = elapsed();
        const uint32_t stepMs = effectiveStepMs();
        if (now - lastFrameMs_ < stepMs) return;
        lastFrameMs_ += stepMs;   // advance by one step, not to `now`: a stall catches up one frame at a time
        if (!decodeCurrentFrame()) return;   // a read failure above already stopped playback
        frameIndex_++;
        if (frameIndex_ >= header_.frameCount) handleEndOfFile();
    }

    /// Blit the already-decoded frame, or hold black while stopped. Pure memory work, no I/O.
    void tick() MM_NONBLOCKING override {
        if (state_ == PlayState::Stopped) {
            uint8_t* buf = buffer();
            if (buf) std::memset(buf, 0, static_cast<size_t>(nrOfLights()) * channelsPerLight());
            return;
        }
        blitFrameBuf();
    }

    /// Dispatch a transport button through a name-to-handler table, a Command row per button rather than a branch.
    void onControlChanged(const char* controlName) override {
        if (controlName) {
            for (const auto& row : kButtonHandlers) {
                if (std::strcmp(controlName, row.name) == 0) { (this->*row.handler)(); break; }
            }
        }
        EffectBase::onControlChanged(controlName);
    }

    /// Rescan when any file lands or leaves, so an upload through the File Manager reaches the playlist unasked.
    void onFileChanged(const char* path) override {
        if (path) rescanFiles();
    }

    // --- ListSource (editable): the playlist -------------------------------------------

    /// How many rows the playlist holds.
    uint8_t listRowCount() const override { return playlistCount_; }

    /// Write one row, which is also the persisted form: the stable id and the chosen file.
    void writeListRow(JsonSink& sink, uint8_t row) const override {
        sink.appendf("{\"id\":%lu,\"file\":", static_cast<unsigned long>(tracks_[row].id));
        sink.writeJsonString(tracks_[row].file);
        sink.append("}");
    }

    /// Emitted once per list: the full paths the scan found.
    void writeListOptionSets(JsonSink& sink) const override {
        sink.append("\"files\":[");
        for (uint8_t i = 0; i < fileCount_; i++) {
            if (i) sink.append(",");
            sink.writeJsonString(fileOptions_[i]);
        }
        sink.append("]");
    }

    /// One row's editable fields: the file picker and a "play this row now" button.
    void writeListRowDetail(JsonSink& sink, uint8_t row) const override {
        sink.append("{\"fields\":[");
        sink.appendf("{\"name\":\"file\",\"type\":\"select\",\"value\":%d,\"optionsRef\":\"files\"},",
                     fileIndexOf(tracks_[row].file));
        sink.append("{\"name\":\"play\",\"type\":\"button\"}");
        sink.append("]}");
    }

    /// The playlist accepts add, delete, reorder and field edits.
    bool isEditableList() const override { return true; }

    /// Append a row, defaulting to the first scanned file if one exists.
    bool addListRow(uint32_t& outId) override {
        if (playlistCount_ >= kMaxTracks) return false;
        Track& t = tracks_[playlistCount_];
        t = Track{};
        t.id = nextId_++;
        if (fileCount_ > 0) copyBounded(t.file, sizeof(t.file), fileNames_[0]);
        playlistCount_++;
        outId = t.id;
        return true;
    }

    /// Remove a row by id, clamping the cursor back onto the shrunk list.
    bool deleteListRow(uint32_t id) override {
        const int i = indexOf(id);
        if (i < 0) return false;
        for (uint8_t j = static_cast<uint8_t>(i); j + 1 < playlistCount_; j++) tracks_[j] = tracks_[j + 1];
        playlistCount_--;
        if (cursor_ >= playlistCount_ && playlistCount_ > 0) cursor_ = static_cast<uint8_t>(playlistCount_ - 1);
        return true;
    }

    /// Move a row by id, which never changes that id.
    bool moveListRow(uint32_t id, uint8_t to) override {
        const int i = indexOf(id);
        if (i < 0) return false;
        if (to >= playlistCount_) to = static_cast<uint8_t>(playlistCount_ - 1);
        const Track moved = tracks_[i];
        if (to > i) for (int j = i; j < to; j++) tracks_[j] = tracks_[j + 1];
        else        for (int j = i; j > to; j--) tracks_[j] = tracks_[j - 1];
        tracks_[to] = moved;
        return true;
    }

    /// Edit a row's file, or act on its "play" field: jump to this row and start playing now.
    bool setListRowField(uint32_t id, const char* field, const char* valueJson) override {
        const int i = indexOf(id);
        if (i < 0) return false;
        if (std::strcmp(field, "file") == 0) {
            const int idx = mm::json::parseInt(valueJson, "value");
            if (idx < 0 || idx >= fileCount_) return false;
            copyBounded(tracks_[i].file, sizeof(tracks_[i].file), fileNames_[idx]);
            return true;
        }
        if (std::strcmp(field, "play") == 0) {
            cursor_ = static_cast<uint8_t>(i);
            visited_ = 0;
            state_ = PlayState::Playing;   // set before loadTrack; see prepare()'s own comment
            loadTrack(cursor_);
            return true;
        }
        return false;
    }

    /// Restore the playlist from persisted JSON, keeping each row's stable id.
    bool restoreList(const char* json, const char* key) override {
        mm::json::JsonDoc doc;
        if (!mm::json::parse(json, doc)) return false;
        const mm::json::JsonNode* arr = mm::json::member(doc, doc.rootNode(), key);
        if (!arr || arr->type != mm::json::JsonType::Array) return false;
        playlistCount_ = 0;
        const int n = mm::json::arraySize(doc, arr);
        for (int r = 0; r < n && playlistCount_ < kMaxTracks; r++) {
            const mm::json::JsonNode* row = mm::json::element(doc, arr, r);
            Track& t = tracks_[playlistCount_];
            t = Track{};
            t.id = static_cast<uint32_t>(mm::json::readInt(mm::json::member(doc, row, "id"), 0));
            mm::json::readString(mm::json::member(doc, row, "file"), t.file, sizeof(t.file));
            if (t.id >= nextId_) nextId_ = t.id + 1;   // never reissue a persisted id
            playlistCount_++;
        }
        return true;
    }

    // --- Test seams: inject state directly, since every real file call needs a test root to succeed on desktop ---

    /// Test seam: inject a parsed header directly, skipping the real file read.
    void setHeaderForTest(const fseq::FseqHeader& h) {
        header_ = h;
        frameCount_ = h.frameCount;
        frameBuf_.resize(h.channelCount);
        frameIndex_ = 0;
        currentBlockIndex_ = 0;
        currentBlockFirstFrame_ = 0;
        currentBlockFrameCount_ = 0;
    }
    /// Force the transport state directly.
    void setStateForTest(PlayState s) { state_ = s; }
    /// The current transport state.
    PlayState stateForTest() const { return state_; }
    /// The playlist row loaded, or about to load.
    uint8_t cursorForTest() const { return cursor_; }
    /// The frame position within the current track.
    uint32_t frameIndexForTest() const { return frameIndex_; }
    /// Run end-of-file handling without waiting out the real frame cadence.
    void handleEndOfFileForTest() { handleEndOfFile(); }
    /// Whether a full shuffle+loopOff pass has visited every playlist row.
    bool allTracksVisitedForTest() const { return allTracksVisited(); }
    /// Mark a row visited directly, as if its track had just loaded for real.
    void markVisitedForTest(uint8_t idx) { markVisited(idx); }
    /// Force the loop mode directly, one of kLoopOff/kLoopOne/kLoopAll below.
    void setLoopModeForTest(uint8_t m) { loopMode_ = m; }
    /// Force the shuffle flag directly.
    void setShuffleForTest(bool s) { shuffle_ = s; }
    /// Force the playback rate directly.
    void setFpsForTest(uint8_t fps) { fps_ = fps; }
    /// Force the fps source directly, one of kFpsFromFile/kFpsManual above.
    void setFpsSourceForTest(uint8_t source) { fpsSource_ = source; }
    /// The milliseconds-per-frame decode is actually pacing on right now.
    uint32_t effectiveStepMsForTest() const { return effectiveStepMs(); }
    /// Run one tick20ms decode step without waiting out the real frame cadence.
    void decodeOneFrameForTest() { lastFrameMs_ = elapsed() - effectiveStepMs(); tick20ms(); }
    /// Test seam for the scan's directory-join, the one piece of the scan desktop can exercise directly.
    static void joinPathForTest(const char* dir, const char* name, char* out, size_t outCap) {
        joinPath(dir, name, out, outCap);
    }

private:
    static constexpr const char* kLoopModeOptions[] = {"off", "one", "all"};
    static constexpr const char* kFpsSourceOptions[] = {"file", "manual"};
    static constexpr uint8_t kMaxTracks = 64;         // a playlist, not a media library: bounded, no heap
    static constexpr uint8_t kMaxFiles = 64;
    static constexpr uint8_t kMaxScanDepth = 8;       // matches FilesystemModule::removeTree's own bound
    /// 50, not the usual control range: tick20ms's own ~50 Hz cadence is the decode ceiling (@xref{why-decode-runs-in-tick20ms}).
    static constexpr uint8_t kMaxFps = 50;
    /// The slowest step the decode cadence can honor: 1000 / kMaxFps, so a file-sourced rate above it clamps here.
    static constexpr uint32_t kMinStepMs = 1000u / kMaxFps;
    /// A ceiling against a corrupt block-length field: real exports ran up to ~300KB/block, so 4MB keeps headroom.
    static constexpr size_t kMaxCompressedBlockBytes = 4 * 1024 * 1024;

    struct Track { uint32_t id = 0; char file[96] = {}; };   ///< the full filesystem path, not a bare name
    Track    tracks_[kMaxTracks] = {};
    uint8_t  playlistCount_ = 0;
    uint32_t nextId_ = 1;
    uint8_t  cursor_ = 0;

    bool     shuffle_ = false;
    uint8_t  loopMode_ = kLoopAll;
    uint8_t  fpsSource_ = kFpsFromFile;   ///< kFpsFromFile reads the track's own stepTimeMs; kFpsManual reads fps_
    uint8_t  fps_ = 30;   ///< the manual playback rate; 1000/fps milliseconds per frame

    PlayState       state_ = PlayState::Stopped;
    fseq::FseqHeader header_ = {};
    uint32_t        frameIndex_ = 0;
    uint32_t        frameCount_ = 0;
    uint32_t        lastFrameMs_ = 0;
    uint64_t        visited_ = 0;   ///< per-pass bitset over playlist rows, for shuffle+loopOff
    Random8         rng_;

    /// The one open track stream, plus its buffered header: @xref{sequential-reads-replace-random-offset-ones}.
    platform::FsReadStream stream_{};
    bool     streamOpen_ = false;
    uint8_t  headBuf_[4096] = {};   ///< the header, block table and sparse table of virtually any real file
    size_t   headLen_ = 0;          ///< bytes actually filled in headBuf_
    size_t   headPos_ = 0;          ///< how far into headBuf_ sequential reads have already consumed

    ScratchBuffer<uint8_t> frameBuf_{*this};   ///< the one decoded frame tick() blits
    ScratchBuffer<uint8_t> compBuf_{*this};    ///< one compression block's compressed bytes
    ScratchBuffer<uint8_t> blockBuf_{*this};   ///< that block, decompressed
    uint32_t currentBlockIndex_ = 0;           ///< which compression-block-table entry is loaded
    uint32_t currentBlockFirstFrame_ = 0;      ///< that block's first frame
    uint32_t currentBlockFrameCount_ = 0;      ///< how many frames blockBuf_ covers

    char        fileNames_[kMaxFiles][96] = {};   ///< full filesystem paths, found by the scan
    const char* fileOptions_[kMaxFiles] = {};
    uint8_t     fileCount_ = 0;

    /// Sized for "playing: " + a 95-char path + " (999/999)", with headroom; setStatus holds the pointer, so this outlives the call.
    char statusBuf_[128] = {};

    /// A plain bounded copy, not snprintf: GCC's -Wrestrict flags snprintf between two member arrays of the same `this`.
    static void copyBounded(char* dst, size_t dstCap, const char* src) {
        if (dstCap == 0) return;
        size_t n = std::strlen(src);
        if (n >= dstCap) n = dstCap - 1;
        std::memcpy(dst, src, n);
        dst[n] = '\0';
    }
    /// dir + "/" + name, built by hand for the same -Wrestrict reason as copyBounded; skips the slash when dir already ends in one.
    static void joinPath(const char* dir, const char* name, char* out, size_t outCap) {
        const size_t dirLen = std::strlen(dir);
        size_t n = dirLen < outCap ? dirLen : (outCap > 0 ? outCap - 1 : 0);
        std::memcpy(out, dir, n);
        const bool needsSlash = dirLen == 0 || dir[dirLen - 1] != '/';
        if (needsSlash && n < outCap) { out[n] = '/'; n++; }
        if (n < outCap) copyBounded(out + n, outCap - n, name);
        else if (outCap > 0) out[outCap - 1] = '\0';
    }

    int indexOf(uint32_t id) const {
        for (uint8_t i = 0; i < playlistCount_; i++) if (tracks_[i].id == id) return i;
        return -1;
    }
    int fileIndexOf(const char* file) const {
        for (uint8_t i = 0; i < fileCount_; i++) if (std::strcmp(fileOptions_[i], file) == 0) return i;
        return -1;   // no scanned file matches, e.g. one deleted after a row picked it
    }
    void markVisited(uint8_t idx) { if (idx < 64) visited_ |= (uint64_t{1} << idx); }

    /// Milliseconds per frame: the manual fps slider, or the track's own stepTimeMs capped at kMaxFps.
    uint32_t effectiveStepMs() const {
        if (fpsSource_ == kFpsManual) {
            if (fps_ == 0) return 1;   // defensive only: the control's own min bound keeps this unreachable
            const uint32_t ms = 1000u / fps_;
            return ms ? ms : 1;
        }
        if (header_.stepTimeMs == 0) return kMinStepMs;   // an unauthored rate defaults to the cap, not a division by zero
        return header_.stepTimeMs < kMinStepMs ? kMinStepMs : header_.stepTimeMs;
    }

    /// Warn when the file's own rate asks for more than kMaxFps, which file-sourced timing cannot honor; true if it did.
    bool warnIfFileRateExceedsCap() {
        if (fpsSource_ != kFpsFromFile || header_.stepTimeMs == 0 || header_.stepTimeMs >= kMinStepMs) return false;
        const unsigned fileFps = 1000u / header_.stepTimeMs;
        std::snprintf(statusBuf_, sizeof(statusBuf_),
                      "file requests %ufps, capped at %u", fileFps, static_cast<unsigned>(kMaxFps));
        setStatus(statusBuf_, Severity::Warning);
        return true;
    }

    bool allTracksVisited() const {
        if (playlistCount_ == 0) return true;
        const uint64_t full = playlistCount_ >= 64 ? ~uint64_t{0} : ((uint64_t{1} << playlistCount_) - 1);
        return (visited_ & full) == full;
    }

    /// The `play` button: start a stopped transport, or resume a paused one.
    void handlePlay() {
        if (state_ == PlayState::Stopped) {
            if (playlistCount_ == 0) return;
            visited_ = 0;
            state_ = PlayState::Playing;   // set before loadTrack; see prepare()'s own comment
            loadTrack(cursor_);
            return;
        }
        if (state_ != PlayState::Paused) return;
        lastFrameMs_ = elapsed();   // rebase, so a long pause does not fire a catch-up burst
        state_ = PlayState::Playing;
    }

    /// The `pause` button: only takes effect while playing.
    void handlePause() {
        if (state_ != PlayState::Playing) return;
        state_ = PlayState::Paused;
        refreshStatus();
    }

    /// The `stop` button: close the stream and reset position, only while playing or paused.
    void handleStop() {
        if (state_ != PlayState::Playing && state_ != PlayState::Paused) return;
        closeTrackStream();   // a later play reopens and reads frame 0 forward
        state_ = PlayState::Stopped;
        frameIndex_ = 0;
        visited_ = 0;
        refreshStatus();
    }

    /// The `previous` button, wrapped to the uniform no-argument shape `kButtonHandlers` rows need.
    void handlePrevious() { moveCursor(-1); }
    /// The `next` button, under the same wrapper as `handlePrevious`.
    void handleNext() { moveCursor(+1); }

    /// Move the cursor by one step, shuffled or sequential.
    void moveCursor(int direction) {
        if (playlistCount_ == 0) return;
        advanceCursor(direction);
        if (state_ == PlayState::Stopped) return;
        loadTrack(cursor_);   // success leaves state_ untouched; failure forces it to Stopped
    }

    /// Pick the next playlist row, shuffled or sequential, the same shape as DemoReelEffect::advance().
    void advanceCursor(int direction) {
        if (playlistCount_ == 0) return;
        uint8_t next;
        if (shuffle_ && playlistCount_ > 1) {
            do { next = rng_.below(playlistCount_); } while (next == cursor_);
        } else {
            next = static_cast<uint8_t>((cursor_ + playlistCount_ + direction) % playlistCount_);
        }
        cursor_ = next;
    }

    /// End of the current track: loop it, advance to the next, or stop a completed pass.
    void handleEndOfFile() {
        if (loopMode_ == kLoopOne) {
            loadTrack(cursor_);   // re-opens from frame 0: a forward-only stream cannot rewind in place
            return;
        }
        if (loopMode_ == kLoopOff && allTracksVisited()) {
            closeTrackStream();
            state_ = PlayState::Stopped;
            frameIndex_ = 0;
            visited_ = 0;
            refreshStatus();
            return;
        }
        advanceCursor(+1);
        loadTrack(cursor_);   // loopAll wraps forever; loopOff stops on the NEXT end-of-file once every row has played
    }

    /// Close the open stream, if any; safe to call when nothing is open.
    void closeTrackStream() {
        if (streamOpen_) platform::fsReadStreamClose(stream_);
        streamOpen_ = false;
        headLen_ = 0;
        headPos_ = 0;
    }

    /// Open `path` fresh and buffer its header region; false when the file can't be opened or has no bytes.
    bool openTrackStream(const char* path) {
        closeTrackStream();
        if (!platform::fsReadStreamOpen(stream_, path)) return false;
        streamOpen_ = true;
        size_t got = 0;
        while (got < sizeof(headBuf_)) {
            const int n = platform::fsReadStreamRead(stream_, reinterpret_cast<char*>(headBuf_) + got, sizeof(headBuf_) - got);
            if (n <= 0) break;
            got += static_cast<size_t>(n);
        }
        headLen_ = got;
        headPos_ = 0;
        return got > 0;
    }

    /// Pull `n` sequential file bytes into `dst`: the buffered header region first, the live stream after.
    bool readBytes(uint8_t* dst, size_t n) {
        size_t fromHead = 0;
        if (headPos_ < headLen_) {
            fromHead = n < (headLen_ - headPos_) ? n : (headLen_ - headPos_);
            std::memcpy(dst, headBuf_ + headPos_, fromHead);
            headPos_ += fromHead;
        }
        const size_t remain = n - fromHead;
        if (remain == 0) return true;
        if (!streamOpen_) return false;
        size_t got = 0;
        while (got < remain) {
            const int r = platform::fsReadStreamRead(stream_, reinterpret_cast<char*>(dst + fromHead + got), remain - got);
            if (r <= 0) return false;
            got += static_cast<size_t>(r);
        }
        return true;
    }

    /// Discards `n` live-stream bytes between the buffered header region and `channelDataOffset`, for a file whose variable-length metadata outgrows `headBuf_`.
    bool skipHeaderGap(size_t n) {
        uint8_t discard[256];
        while (n > 0) {
            const size_t chunk = n < sizeof(discard) ? n : sizeof(discard);
            if (!readBytes(discard, chunk)) return false;
            n -= chunk;
        }
        return true;
    }

    /// Open and parse one playlist row's header; forces Stopped and reports the reason on failure.
    bool loadTrack(uint8_t idx) {
        if (idx >= playlistCount_) { state_ = PlayState::Stopped; return false; }
        cursor_ = idx;
        if (!openTrackStream(tracks_[cursor_].file)) {
            setStatus("file read failed", Severity::Error);
            state_ = PlayState::Stopped;
            frameIndex_ = 0;
            return false;
        }
        fseq::FseqHeader hdr;
        const fseq::ParseError err = fseq::parseHeader(headBuf_, headLen_, hdr);
        const char* errMsg = nullptr;
        switch (err) {
            case fseq::ParseError::BadMagic:               errMsg = "not an FSEQ file"; break;
            case fseq::ParseError::UnsupportedVersion:     errMsg = "unsupported FSEQ version"; break;
            case fseq::ParseError::UnsupportedCompression: errMsg = "compressed (zstd) FSEQ not supported yet"; break;
            case fseq::ParseError::Truncated:              errMsg = "FSEQ header truncated or unreadable"; break;
            default: break;   // None, or TooManySparseRanges (non-fatal, handled below)
        }
        if (errMsg) {
            setStatus(errMsg, Severity::Error);
            state_ = PlayState::Stopped;
            frameIndex_ = 0;
            return false;
        }
        header_ = hdr;
        frameCount_ = header_.frameCount;
        frameBuf_.resize(header_.channelCount);
        if (!frameBuf_) {
            setStatus("out of memory for this track's frame size", Severity::Error);
            state_ = PlayState::Stopped;
            frameIndex_ = 0;
            return false;
        }
        headPos_ = header_.channelDataOffset;   // skip the header, land exactly where frame 0 starts
        if (headPos_ > headLen_ && !skipHeaderGap(headPos_ - headLen_)) {
            setStatus("FSEQ header truncated or unreadable", Severity::Error);
            state_ = PlayState::Stopped;
            frameIndex_ = 0;
            return false;
        }
        currentBlockIndex_ = 0;
        currentBlockFirstFrame_ = 0;
        currentBlockFrameCount_ = 0;
        frameIndex_ = 0;
        lastFrameMs_ = elapsed();
        markVisited(cursor_);
        rebuildControls();   // the "position" progress total (frameCount_) just changed
        if (!warnIfFileRateExceedsCap()) {
            if (err == fseq::ParseError::TooManySparseRanges) setStatus("FSEQ sparse range table clipped", Severity::Warning);
            else refreshStatus();
        }
        return true;
    }

    /// Decode frame `frameIndex_` into `frameBuf_`: the one blocking file read `tick20ms` makes, bounded to one frame or compression block.
    bool decodeCurrentFrame() {
        const uint8_t* rawFrame;
        if (header_.compressionType == fseq::CompressionType::Zlib) {
            rawFrame = frameFromCompressedBlock(frameIndex_);
            if (!rawFrame) {
                setStatus("file read failed mid-file", Severity::Error);
                state_ = PlayState::Stopped;
                frameIndex_ = 0;
                return false;
            }
            std::memcpy(frameBuf_.data(), rawFrame, header_.channelCount);
        } else if (!readBytes(frameBuf_.data(), header_.channelCount)) {
            setStatus("file read failed mid-file", Severity::Error);
            state_ = PlayState::Stopped;
            frameIndex_ = 0;
            return false;
        }
        return true;
    }

    /// Blit the decoded frame onto the layer buffer, sparse or dense; a no-op before any frame has decoded.
    void blitFrameBuf() {
        if (!frameBuf_.data() || frameCount_ == 0) return;
        uint8_t* buf = buffer();
        if (!buf) return;
        const size_t bufBytes = static_cast<size_t>(nrOfLights()) * channelsPerLight();
        if (header_.sparseRangeCount > 0) {
            // Unmapped channels go dark: the sparse table only addresses the fixtures a show touches.
            std::memset(buf, 0, bufBytes);
            fseq::expandSparseFrame(header_, frameBuf_.data(), header_.channelCount, buf, bufBytes);
        } else {
            const size_t n = static_cast<size_t>(header_.channelCount) < bufBytes ? header_.channelCount : bufBytes;
            std::memcpy(buf, frameBuf_.data(), n);
        }
    }

    /// The raw bytes for `frameIndex` out of the current (or a freshly loaded) compression block.
    const uint8_t* frameFromCompressedBlock(uint32_t frameIndex) {
        while (frameIndex >= currentBlockFirstFrame_ + currentBlockFrameCount_) {
            if (!loadNextBlock()) return nullptr;
        }
        // A block table not starting at frame 0 is malformed; fail rather than underflow this subtraction.
        if (frameIndex < currentBlockFirstFrame_) return nullptr;
        const size_t offset = static_cast<size_t>(frameIndex - currentBlockFirstFrame_) * header_.channelCount;
        return blockBuf_.data() + offset;
    }

    /// Read, decompress and install the next compression block; its byte count sets this call's real cost.
    bool loadNextBlock() {
        if (currentBlockIndex_ >= header_.compressionBlockCount) return false;
        const size_t entryOffset = 32 + static_cast<size_t>(currentBlockIndex_) * 8;
        if (entryOffset + 8 > headLen_) return false;   // the block table lives inside the buffered header region
        const uint32_t blockFirstFrame = fseq::detail::rdU32(headBuf_ + entryOffset);
        const uint32_t blockByteLen = fseq::detail::rdU32(headBuf_ + entryOffset + 4);

        const bool hasNext = currentBlockIndex_ + 1 < header_.compressionBlockCount;
        uint32_t nextFirstFrame = 0;
        if (hasNext) {
            if (entryOffset + 12 > headLen_) return false;
            nextFirstFrame = fseq::detail::rdU32(headBuf_ + entryOffset + 8);
        }
        uint32_t blockFrameCount = 0;
        size_t decompLen = 0;
        if (!fseq::validateBlockRange(blockFirstFrame, header_.frameCount, hasNext, nextFirstFrame,
                                       header_.channelCount, blockFrameCount, decompLen))
            return false;
        if (blockByteLen == 0 || blockByteLen > kMaxCompressedBlockBytes || blockFrameCount == 0) return false;

        compBuf_.resize(blockByteLen);
        if (!compBuf_ || !readBytes(compBuf_.data(), blockByteLen)) return false;

        blockBuf_.resize(decompLen);
        if (!blockBuf_) return false;
        size_t outLen = 0;
        if (!platform::zlibInflateAll(compBuf_.data(), blockByteLen, blockBuf_.data(), decompLen, outLen) ||
            outLen != decompLen)
            return false;

        currentBlockFirstFrame_ = blockFirstFrame;
        currentBlockFrameCount_ = blockFrameCount;
        currentBlockIndex_++;
        return true;
    }

    /// One directory level, collected: the same shape as FilesystemModule::removeTree's DirLevel.
    struct ScanLevel {
        static constexpr uint8_t kMax = 64;
        char names[kMax][40];
        bool isDir[kMax];
        uint8_t count = 0;
    };

    static void collectScanEntry(const char* name, bool isDir, uint32_t, void* user) {
        auto* lvl = static_cast<ScanLevel*>(user);
        if (lvl->count >= ScanLevel::kMax) return;
        if (!name || name[0] == '.' || std::strlen(name) >= sizeof(lvl->names[0])) return;
        std::snprintf(lvl->names[lvl->count], sizeof(lvl->names[0]), "%s", name);
        lvl->isDir[lvl->count] = isDir;
        lvl->count++;
    }

    /// Recurse into `path`, depth-bounded, one heap-allocated level at a time (@xref{the-scan-is-depth-bounded-recursion-over-a-heap-per-level-buffer}).
    void scanDir(const char* path, uint8_t depth) {
        if (depth > kMaxScanDepth || fileCount_ >= kMaxFiles) return;
        auto* raw = platform::alloc(sizeof(ScanLevel));
        if (!raw) return;
        ScanLevel* lvlp = new (raw) ScanLevel;
        ScanLevel& lvl = *lvlp;
        struct Freer { ScanLevel* p; ~Freer() { p->~ScanLevel(); platform::free(p); } } freer{lvlp};
        platform::fsList(path, &collectScanEntry, &lvl);
        for (uint8_t i = 0; i < lvl.count && fileCount_ < kMaxFiles; i++) {
            char child[96];
            joinPath(path, lvl.names[i], child, sizeof(child));
            if (lvl.isDir[i]) {
                scanDir(child, static_cast<uint8_t>(depth + 1));
            } else if (hasExtension(lvl.names[i], ".fseq")) {
                copyBounded(fileNames_[fileCount_++], sizeof(fileNames_[0]), child);
            }
        }
    }

    /// Re-scan the filesystem root for `.fseq` files into the shared option set.
    void rescanFiles() {
        fileCount_ = 0;
        scanDir("/", 0);
        for (uint8_t i = 0; i < fileCount_; i++) fileOptions_[i] = fileNames_[i];
        rebuildControls();
        // Fires the resync directly: rebuildControls()'s own change-detection does not hash a List's row/option-set content.
        notifySchemaChanged();
    }

    /// One row of `onControlChanged`'s dispatch table: a button's name and its handler.
    struct ButtonHandler { const char* name; void (FseqPlayerEffect::*handler)(); };
    /// Every transport button, in no particular order, declared this late so rescanFiles is already in scope.
    static constexpr ButtonHandler kButtonHandlers[] = {
        {"play", &FseqPlayerEffect::handlePlay},
        {"pause", &FseqPlayerEffect::handlePause},
        {"stop", &FseqPlayerEffect::handleStop},
        {"previous", &FseqPlayerEffect::handlePrevious},
        {"next", &FseqPlayerEffect::handleNext},
        {"rescan", &FseqPlayerEffect::rescanFiles},
    };

    /// Case-insensitive suffix check, since a file may carry ".FSEQ" from a different export tool.
    static bool hasExtension(const char* name, const char* ext) {
        const size_t nameLen = std::strlen(name);
        const size_t extLen = std::strlen(ext);
        if (nameLen < extLen) return false;
        for (size_t i = 0; i < extLen; i++) {
            char a = name[nameLen - extLen + i], b = ext[i];
            if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
            if (a != b) return false;
        }
        return true;
    }

    /// Show the transport state, the current file, and its position in the playlist.
    void refreshStatus() {
        if (playlistCount_ == 0) { setStatus("no tracks in playlist"); return; }
        const char* label = state_ == PlayState::Playing ? "playing"
                           : state_ == PlayState::Paused  ? "paused"
                                                           : "stopped";
        // Copied to a local first: GCC's -Wrestrict cannot see past two member arrays of the same `this`.
        char fileCopy[sizeof(Track::file)];
        copyBounded(fileCopy, sizeof(fileCopy), tracks_[cursor_].file);
        std::snprintf(statusBuf_, sizeof(statusBuf_), "%s: %s (%u/%u)", label, fileCopy,
                      static_cast<unsigned>(cursor_ + 1), static_cast<unsigned>(playlistCount_));
        setStatus(statusBuf_);
    }
};

} // namespace mm
