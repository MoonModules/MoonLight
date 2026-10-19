#pragma once

#include "light/effects/ChannelDataSink.h"   // where the decoded frame lands, held and placed in wiring order
#include "light/effects/EffectBase.h"

#include "light/util/FseqFile.h"     // the FSEQ header/sparse-range parser this effect plays back
#include "platform/platform.h"       // fsReadStreamXxx, fsList, zlibInflateAll
#include "core/util/JsonSink.h"      // the playlist's own ListSource serialization
#include "core/util/JsonUtil.h"      // restoreList: recursive reader for the persisted playlist

#include <cstdio>
#include <cstring>

namespace mm {

/// Plays FSEQ animation files from the filesystem onto this effect's layer, with a playlist and the standard transport controls.
/// @card FseqPlayerEffect.gif
///
/// FSEQ is the Falcon Player/xLights sequence format: a flat, per-frame byte stream with no pixel semantics of its own. The playlist follows `FixtureProfilesModule`'s `EditableListSource` shape, the transport `DemoReelEffect`'s cycle/shuffle logic.
///
/// @attribution FPP
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
class FseqPlayerEffect : public EffectBase, public ListSource {
public:
    /// The transport's three states.
    enum class PlayState : uint8_t { Stopped, Playing, Paused };
    /// The three loopMode values, public so a test can select one by name.
    static constexpr uint8_t kLoopOff = 0, kLoopOne = 1, kLoopAll = 2;
    /// The two fpsSource values, public so a test can select one by name.
    static constexpr uint8_t kFpsFromFile = 0, kFpsManual = 1;

    /// Free the playlist's path strings, the one memory a disabled player keeps.
    ~FseqPlayerEffect() override { clearTracks(); }

    /// Catalog tags: a recorded show, played back.
    const char* tags() const override { return "🖼️"; }

    /// Bind the transport buttons, the shuffle/loop controls, the playlist and the position gauge.
    void defineControls() override {
        /// Starts a stopped transport, or resumes a paused one.
        controls_.addButton("play");
        /// Holds the current frame on screen.
        controls_.addButton("pause");
        /// Stops playback, resets the position and blanks the lights.
        controls_.addButton("stop");
        /// Jumps to the previous track in the playlist.
        controls_.addButton("previous");
        /// Jumps to the next track in the playlist.
        controls_.addButton("next");
        /// Picks the next track at random instead of in playlist order.
        controls_.addControl("shuffle", shuffle_);
        /// At a track's end: stop, repeat it, or repeat the playlist.
        controls_.addSelect("loopMode", loopMode_, kLoopModeOptions, 3);
        controls_.addSelect("fpsSource", fpsSource_, kFpsSourceOptions, 2);
        controls_.addControl("fps", fps_, 1, kMaxFps);
        controls_.setHidden(controls_.count() - 1, fpsSource_ != kFpsManual);
        /// The tracks to play, in order; each row picks a scanned .fseq file.
        controls_.addList("playlist", *this);
        /// How far into the current track playback is, in frames.
        controls_.addProgress("position", frameIndex_, frameCount_, ProgressUnit::Count);
        /// Looks through the filesystem for .fseq files again.
        controls_.addButton("rescan");
    }

    /// Scan the filesystem for animations once.
    void setup() override {
        EffectBase::setup();
        rng_.seed(platform::micros());   // a different shuffle order each boot
        if (enabled()) rescanFiles();
    }

    /// Size the sink to the layer, then start playing on its own once the effect becomes enabled.
    void prepare() override {
        sink_.prepare(/*senderMapped=*/true);   // an FSEQ frame is already in wiring order, the same shape NetworkReceiveEffect's sender-mapped frames take, so a serpentine layout is not applied twice
        sizeExpandBuf();
        if (!fileNames_) rescanFiles();   // the scan cache is freed on release, so an enabled effect rebuilds it
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
        fileCount_ = 0;   // the names below are freed with the scratch buffers
        EffectBase::release();
    }

    /// Render last tick's decoded frame, then decode the next due one: the blocking read lives here, never in `tick()`, at most one frame per call.
    void tick20ms() MM_NONBLOCKING override {
        if (state_ != PlayState::Playing) return;
        const uint32_t now = elapsed();
        const uint32_t stepMs = effectiveStepMs();
        if (now - lastFrameMs_ < stepMs) return;
        lastFrameMs_ += stepMs;   // advance by one step, not to `now`: a stall catches up one frame at a time
        if (aheadFrameReady_) { writeFrameToSink(); aheadFrameReady_ = false; }
        if (frameIndex_ >= header_.frameCount) { handleEndOfFile(); return; }   // last frame already rendered above
        if (!decodeCurrentFrame()) return;   // a read failure above already stopped playback
        frameIndex_++;
    }

    /// Hand the sink's held frame to the output; tick20ms() is the only place new data reaches it.
    void tick() MM_NONBLOCKING override { sink_.tick(); }

    /// An FSEQ frame is in wiring order.
    bool deliversWiringOrder() const MM_NONBLOCKING override { return sink_.wiringOrder(); }
    /// The sink hands its held frame to the output.
    void copyWiringFrame(uint8_t* out, size_t bytes, bool force) override { sink_.copyWiringFrame(out, bytes, force); }

    /// Dispatch a transport button through a name-to-handler table, a Command row per button rather than a branch; a changed rate source rebuilds the controls so `fps` shows or hides.
    void onControlChanged(const char* controlName) override {
        if (controlName) {
            if (std::strcmp(controlName, "fpsSource") == 0) rebuildControls();
            for (const auto& row : kButtonHandlers) {
                if (std::strcmp(controlName, row.name) == 0) { (this->*row.handler)(); break; }
            }
        }
        EffectBase::onControlChanged(controlName);
    }

    /// Rescan when a show lands or leaves, so an upload through the File Manager reaches the playlist unasked. An unrelated file, or a disabled effect, skips the whole-tree walk.
    void onFileChanged(const char* path) override {
        if (!enabled() || (path && !hasExtension(path, ".fseq"))) return;
        rescanFiles();
    }

    // --- ListSource (editable): the playlist -------------------------------------------

    /// How many rows the playlist holds.
    uint8_t listRowCount() const override { return playlistCount_; }

    /// Write one row, which is also the persisted form: the stable id and the chosen file.
    void writeListRow(JsonSink& sink, uint8_t row) const override {
        sink.appendf("{\"id\":%lu,\"file\":", static_cast<unsigned long>(tracks_[row].id));
        sink.writeJsonString(trackFile(row));
        sink.append("}");
    }

    /// Emitted once per list: the full paths the scan found.
    void writeListOptionSets(JsonSink& sink) const override {
        sink.append("\"files\":[");
        for (uint8_t i = 0; i < fileCount_; i++) {
            if (i) sink.append(",");
            sink.writeJsonString(fileName(i));
        }
        sink.append("]");
    }

    /// One row's editable fields: the file picker and a "play this row now" button.
    void writeListRowDetail(JsonSink& sink, uint8_t row) const override {
        sink.append("{\"fields\":[");
        sink.appendf("{\"name\":\"file\",\"type\":\"select\",\"value\":%d,\"optionsRef\":\"files\"},",
                     fileIndexOf(trackFile(row)));
        sink.append("{\"name\":\"play\",\"type\":\"button\"}");
        sink.append("]}");
    }

    /// The playlist accepts add, delete, reorder and field edits.
    bool isEditableList() const override { return true; }

    /// Append a row, defaulting to the first scanned file if one exists.
    bool addListRow(uint32_t& outId) override {
        if (playlistCount_ >= kMaxTracks) return false;
        Track& t = tracks_[playlistCount_];
        if (fileCount_ > 0 && !setTrackFile(t, fileName(0))) return false;
        t.id = nextId_++;
        playlistCount_++;
        outId = t.id;
        return true;
    }

    /// Remove a row by id, clamping the cursor back onto the shrunk list.
    bool deleteListRow(uint32_t id) override {
        const int i = indexOf(id);
        if (i < 0) return false;
        platform::free(tracks_[i].file);
        for (uint8_t j = static_cast<uint8_t>(i); j + 1 < playlistCount_; j++) tracks_[j] = tracks_[j + 1];
        playlistCount_--;
        tracks_[playlistCount_] = Track{};   // the vacated slot no longer owns the path it was shifted from
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
            return setTrackFile(tracks_[i], fileName(static_cast<uint8_t>(idx)));
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
        clearTracks();
        const int n = mm::json::arraySize(doc, arr);
        for (int r = 0; r < n && playlistCount_ < kMaxTracks; r++) {
            const mm::json::JsonNode* row = mm::json::element(doc, arr, r);
            Track& t = tracks_[playlistCount_];
            t.id = static_cast<uint32_t>(mm::json::readInt(mm::json::member(doc, row, "id"), 0));
            char path[kPathCap];
            mm::json::readString(mm::json::member(doc, row, "file"), path, sizeof(path));
            if (path[0]) setTrackFile(t, path);
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
        sizeExpandBuf();
        frameIndex_ = 0;
        currentBlockIndex_ = 0;
        currentBlockFirstFrame_ = 0;
        currentBlockFrameCount_ = 0;
        aheadFrameReady_ = false;
    }
    /// The scratch bytes held right now across every runtime buffer: zero once released.
    size_t scratchBytesForTest() const {
        return fileNames_.bytes() + headBuf_.bytes() + frameBuf_.bytes() + expandBuf_.bytes() + compBuf_.bytes() + blockBuf_.bytes() + zlibWork_.bytes();
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
    static bool joinPathForTest(const char* dir, const char* name, char* out, size_t outCap) {
        return joinPath(dir, name, out, outCap);
    }

private:
    static constexpr const char* kLoopModeOptions[] = {"off", "one", "all"};
    static constexpr const char* kFpsSourceOptions[] = {"file", "manual"};
    static constexpr uint8_t kMaxTracks = 64;         // a playlist, not a media library: bounded, no heap
    static constexpr uint8_t kMaxFiles = 64;
    static constexpr size_t  kHeadBytes = 4096;
    static constexpr uint8_t kMaxScanDepth = 8;       // matches FilesystemModule::removeTree's own bound
    /// 50, not the usual control range: tick20ms's own ~50 Hz cadence is the decode ceiling.
    static constexpr uint8_t kMaxFps = 50;
    /// The slowest step the decode cadence can honor: 1000 / kMaxFps, so a file-sourced rate above it clamps here.
    static constexpr uint32_t kMinStepMs = 1000u / kMaxFps;
    /// A ceiling against a corrupt block-length field: real exports ran up to ~300KB/block, so 4MB keeps headroom.
    static constexpr size_t kMaxCompressedBlockBytes = 4 * 1024 * 1024;

    /// One path budget for a scanned name, a joined path and `Track::file` alike, matching `FilesystemModule::removeTree`'s own.
    static constexpr size_t kPathCap = 192;

    /// One playlist row: its stable id and its full filesystem path, a heap string sized to the path (null while the row names no file).
    struct Track { uint32_t id = 0; char* file = nullptr; };
    Track    tracks_[kMaxTracks] = {};
    uint8_t  playlistCount_ = 0;
    uint32_t nextId_ = 1;
    uint8_t  cursor_ = 0;

    bool     shuffle_ = false;
    uint8_t  loopMode_ = kLoopAll;
    uint8_t  fpsSource_ = kFpsFromFile;   ///< where the rate comes from: the file's step time, or fps_
    uint8_t  fps_ = 30;   ///< the manual playback rate; 1000/fps milliseconds per frame

    PlayState       state_ = PlayState::Stopped;
    fseq::FseqHeader header_ = {};
    uint32_t        frameIndex_ = 0;
    uint32_t        frameCount_ = 0;
    uint32_t        lastFrameMs_ = 0;
    uint64_t        visited_ = 0;   ///< per-pass bitset over playlist rows, for shuffle+loopOff
    Random8         rng_;

    /// The one open track stream, read forward only, plus its buffered header; a track restart or change reopens it.
    platform::FsReadStream stream_{};
    bool     streamOpen_ = false;
    ScratchBuffer<uint8_t> headBuf_{*this};   ///< the header, block table and sparse table of virtually any real file: kHeadBytes, sized when a track opens
    size_t   headLen_ = 0;          ///< bytes actually filled in headBuf_
    size_t   headPos_ = 0;          ///< how far into headBuf_ sequential reads have already consumed

    ChannelDataSink sink_{*this};               ///< where the decoded frame lands, held and placed in wiring order
    ScratchBuffer<uint8_t> frameBuf_{*this};    ///< the one decoded frame, read straight off the file or a compression block
    ScratchBuffer<uint8_t> expandBuf_{*this};   ///< a sparse frame expanded to the sink's own size, before the one write into it
    ScratchBuffer<uint8_t> compBuf_{*this};     ///< one compression block's compressed bytes
    ScratchBuffer<uint8_t> blockBuf_{*this};    ///< that block, decompressed
    ScratchBuffer<uint8_t> zlibWork_{*this};    ///< the decoder's state, lent to platform::zlibInflateAll for a zlib track
    uint32_t currentBlockIndex_ = 0;           ///< which compression-block-table entry is loaded
    uint32_t currentBlockFirstFrame_ = 0;      ///< that block's first frame
    uint32_t currentBlockFrameCount_ = 0;      ///< how many frames blockBuf_ covers
    bool     aheadFrameReady_ = false;         ///< frameBuf_ holds a frame decoded one tick ahead, not yet handed to sink_

    ScratchBuffer<char> fileNames_{*this};   ///< kMaxFiles full filesystem paths of kPathCap bytes each, found by the scan; held while enabled
    uint8_t     fileCount_ = 0;
    bool        scanSkipped_ = false;   ///< a name or joined path did not fit kPathCap during the last scan

    /// Sized for "playing: " + a kPathCap-1-char path + " (999/999)", with headroom; setStatus holds the pointer, so this outlives the call.
    char statusBuf_[kPathCap + 32] = {};

    /// A plain bounded copy, not snprintf: GCC's -Wrestrict flags snprintf between two member arrays of the same `this`.
    static void copyBounded(char* dst, size_t dstCap, const char* src) {
        if (dstCap == 0) return;
        size_t n = std::strlen(src);
        if (n >= dstCap) n = dstCap - 1;
        std::memcpy(dst, src, n);
        dst[n] = '\0';
    }
    /// dir + "/" + name, built by hand for the same -Wrestrict reason as copyBounded; skips the slash when dir already ends in one. False when the joined path did not fit and was truncated.
    static bool joinPath(const char* dir, const char* name, char* out, size_t outCap) {
        const size_t dirLen = std::strlen(dir);
        const size_t needsSlash = dirLen == 0 || dir[dirLen - 1] != '/' ? 1 : 0;
        const size_t fits = dirLen + needsSlash + std::strlen(name) < outCap;
        size_t n = dirLen < outCap ? dirLen : (outCap > 0 ? outCap - 1 : 0);
        std::memcpy(out, dir, n);
        if (needsSlash && n < outCap) { out[n] = '/'; n++; }
        if (n < outCap) copyBounded(out + n, outCap - n, name);
        else if (outCap > 0) out[outCap - 1] = '\0';
        return fits;
    }

    /// Row `i`'s path, empty while it names no file.
    const char* trackFile(uint8_t i) const { return tracks_[i].file ? tracks_[i].file : ""; }
    /// Scanned file `i`'s full path.
    const char* fileName(uint8_t i) const { return fileNames_.data() + static_cast<size_t>(i) * kPathCap; }
    /// Give `t` its own copy of `path` (at most kPathCap - 1 bytes), keeping the old path when the allocation fails.
    static bool setTrackFile(Track& t, const char* path) {
        size_t n = std::strlen(path);
        if (n >= kPathCap) n = kPathCap - 1;
        char* copy = static_cast<char*>(platform::alloc(n + 1));
        if (!copy) return false;
        std::memcpy(copy, path, n);
        copy[n] = '\0';
        platform::free(t.file);
        t.file = copy;
        return true;
    }
    /// Free every row's path and empty the playlist.
    void clearTracks() {
        for (uint8_t i = 0; i < kMaxTracks; i++) { platform::free(tracks_[i].file); tracks_[i] = Track{}; }
        playlistCount_ = 0;
    }

    int indexOf(uint32_t id) const {
        for (uint8_t i = 0; i < playlistCount_; i++) if (tracks_[i].id == id) return i;
        return -1;
    }
    int fileIndexOf(const char* file) const {
        for (uint8_t i = 0; i < fileCount_; i++) if (std::strcmp(fileName(i), file) == 0) return i;
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
        aheadFrameReady_ = false;   // about to black out anyway: no point rendering it first
        writeBlackFrame();
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
        // tick20ms() already rendered any pending frame before calling here; this only guards a direct call (handleEndOfFileForTest).
        if (aheadFrameReady_) { writeFrameToSink(); aheadFrameReady_ = false; }
        if (loopMode_ == kLoopOne) {
            loadTrack(cursor_);   // re-opens from frame 0: a forward-only stream cannot rewind in place
            return;
        }
        if (loopMode_ == kLoopOff && allTracksVisited()) {
            closeTrackStream();
            state_ = PlayState::Stopped;
            frameIndex_ = 0;
            visited_ = 0;
            writeBlackFrame();
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
        if (!headBuf_.resize(kHeadBytes) || !platform::fsReadStreamOpen(stream_, path)) return false;
        streamOpen_ = true;
        size_t got = 0;
        while (got < kHeadBytes) {
            const int n = platform::fsReadStreamRead(stream_, reinterpret_cast<char*>(headBuf_.data()) + got, kHeadBytes - got);
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
            std::memcpy(dst, headBuf_.data() + headPos_, fromHead);
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

    /// Report why a track did not load or play, stop the transport and rewind; always false, so a failure path returns it.
    bool failLoad(const char* message) {
        setStatus(message, Severity::Error);
        state_ = PlayState::Stopped;
        frameIndex_ = 0;
        return false;
    }

    /// Open and parse one playlist row's header; forces Stopped and reports the reason on failure.
    bool loadTrack(uint8_t idx) {
        if (idx >= playlistCount_) { state_ = PlayState::Stopped; return false; }
        cursor_ = idx;
        if (!openTrackStream(trackFile(cursor_))) return failLoad("file read failed");
        fseq::FseqHeader hdr;
        const fseq::ParseError err = fseq::parseHeader(headBuf_.data(), headLen_, hdr);
        const char* errMsg = nullptr;
        switch (err) {
            case fseq::ParseError::BadMagic:               errMsg = "not an FSEQ file"; break;
            case fseq::ParseError::UnsupportedVersion:     errMsg = "unsupported FSEQ version"; break;
            case fseq::ParseError::UnsupportedCompression: errMsg = "compressed (zstd) FSEQ not supported yet"; break;
            case fseq::ParseError::Truncated:              errMsg = "FSEQ header truncated or unreadable"; break;
            case fseq::ParseError::Empty:                  errMsg = "empty show: 0 channels or frames"; break;
            default: break;   // None, or TooManySparseRanges (non-fatal, handled below)
        }
        if (!errMsg && hdr.compressionType == fseq::CompressionType::Zlib && hdr.compressionBlockCount == 0) {
            errMsg = "zlib file declares no compression blocks";
        }
        if (!errMsg && hdr.compressionType == fseq::CompressionType::Zlib && !platform::hasZlibInflate) {
            errMsg = "zlib plays on ESP32 only";
        }
        if (errMsg) return failLoad(errMsg);
        header_ = hdr;
        frameCount_ = header_.frameCount;
        frameBuf_.resize(header_.channelCount);
        if (!frameBuf_ || !sizeBlockBuffers() || !sizeExpandBuf()) return failLoad("out of memory for this track, or its block table is invalid");
        headPos_ = header_.channelDataOffset;   // skip the header, land exactly where frame 0 starts
        if (headPos_ > headLen_ && !skipHeaderGap(headPos_ - headLen_)) return failLoad("FSEQ header truncated or unreadable");
        currentBlockIndex_ = 0;
        currentBlockFirstFrame_ = 0;
        currentBlockFrameCount_ = 0;
        aheadFrameReady_ = false;
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

    /// Size the block buffers and decoder workspace once for the track's largest block, so the 50 Hz path never allocates.
    bool sizeBlockBuffers() {
        size_t maxCompressed = 0, maxDecompressed = 0;
        if (header_.compressionType == fseq::CompressionType::Zlib) {
            for (uint32_t i = 0; i < header_.compressionBlockCount; i++) {
                uint32_t firstFrame = 0, byteLen = 0, frames = 0;
                size_t decompLen = 0;
                if (!blockExtent(i, firstFrame, byteLen, frames, decompLen)) return false;
                if (byteLen > maxCompressed) maxCompressed = byteLen;
                if (decompLen > maxDecompressed) maxDecompressed = decompLen;
            }
        }
        const bool zlib = header_.compressionType == fseq::CompressionType::Zlib;
        return compBuf_.resize(maxCompressed) && blockBuf_.resize(maxDecompressed) && zlibWork_.resize(zlib ? platform::zlibWorkspaceBytes() : 0);
    }

    /// Size the sparse expansion buffer to the sink, once per track and per layer resize; a dense track frees it.
    bool sizeExpandBuf() {
        return expandBuf_.resize(header_.sparseRangeCount ? sink_.bytes() : 0);
    }

    /// One compression block's table entry, validated: where it starts, its compressed length, and how many frames and decompressed bytes it covers.
    bool blockExtent(uint32_t index, uint32_t& firstFrame, uint32_t& byteLen, uint32_t& frames, size_t& decompLen) const {
        if (index >= header_.compressionBlockCount || !fseq::blockEntry(headBuf_.data(), headLen_, index, firstFrame, byteLen)) return false;
        const bool hasNext = index + 1 < header_.compressionBlockCount;
        uint32_t nextFirstFrame = 0, nextByteLen = 0;
        if (hasNext && !fseq::blockEntry(headBuf_.data(), headLen_, index + 1, nextFirstFrame, nextByteLen)) return false;
        if (!fseq::validateBlockRange(firstFrame, header_.frameCount, hasNext, nextFirstFrame, header_.channelCount, frames, decompLen))
            return false;
        return byteLen != 0 && byteLen <= kMaxCompressedBlockBytes && frames != 0;
    }

    /// Decode frame `frameIndex_` into `frameBuf_`, one tick ahead of its render. The one blocking read `tick20ms` makes, bounded to one frame or block.
    bool decodeCurrentFrame() {
        if (header_.compressionType == fseq::CompressionType::Zlib) {
            const uint8_t* rawFrame = frameFromCompressedBlock(frameIndex_);
            if (!rawFrame) return failLoad("file read failed mid-file");
            std::memcpy(frameBuf_.data(), rawFrame, header_.channelCount);
        } else if (!readBytes(frameBuf_.data(), header_.channelCount)) {
            return failLoad("file read failed mid-file");
        }
        aheadFrameReady_ = true;
        return true;
    }

    /// Place the just-decoded frame into `sink_`, sparse or dense.
    void writeFrameToSink() {
        if (header_.sparseRangeCount == 0) {
            sink_.write(0, frameBuf_.data(), header_.channelCount);
            return;
        }
        // Unmapped channels go dark: the sparse table only addresses the fixtures a show touches.
        if (expandBuf_.bytes() != sink_.bytes()) return;   // sized by sizeExpandBuf(), never here on the render path
        std::memset(expandBuf_.data(), 0, expandBuf_.bytes());
        fseq::expandSparseFrame(header_, frameBuf_.data(), header_.channelCount, expandBuf_.data(), expandBuf_.bytes());
        sink_.write(0, expandBuf_.data(), expandBuf_.bytes());
    }

    /// Write one all-zero frame into the sink, through a small static chunk: the stop transition's one-time black-out.
    void writeBlackFrame() {
        static constexpr uint8_t kZero[256] = {};
        for (size_t off = 0; off < sink_.bytes(); off += sizeof(kZero)) {
            const size_t n = sink_.bytes() - off < sizeof(kZero) ? sink_.bytes() - off : sizeof(kZero);
            sink_.write(off, kZero, n);
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

    /// Read, decompress and install the next compression block into the buffers `loadTrack` sized; its byte count sets this call's real cost.
    bool loadNextBlock() {
        uint32_t firstFrame = 0, byteLen = 0, frames = 0;
        size_t decompLen = 0;
        if (!blockExtent(currentBlockIndex_, firstFrame, byteLen, frames, decompLen)) return false;
        if (byteLen > compBuf_.bytes() || decompLen > blockBuf_.bytes() || !readBytes(compBuf_.data(), byteLen)) return false;
        size_t outLen = 0;
        if (!platform::zlibInflateAll(zlibWork_.data(), compBuf_.data(), byteLen, blockBuf_.data(), decompLen, outLen) || outLen != decompLen) return false;

        currentBlockFirstFrame_ = firstFrame;
        currentBlockFrameCount_ = frames;
        currentBlockIndex_++;
        return true;
    }

    /// One directory level, collected: the same shape as FilesystemModule::removeTree's DirLevel.
    struct ScanLevel {
        static constexpr uint8_t kMax = 64;
        char names[kMax][kPathCap];
        bool isDir[kMax];
        uint8_t count = 0;
        bool skipped = false;   ///< an entry's own name alone did not fit kPathCap
    };

    static void collectScanEntry(const char* name, bool isDir, uint32_t, void* user) {
        auto* lvl = static_cast<ScanLevel*>(user);
        if (lvl->count >= ScanLevel::kMax) return;
        if (!name || name[0] == '.') return;
        if (std::strlen(name) >= sizeof(lvl->names[0])) { lvl->skipped = true; return; }
        std::snprintf(lvl->names[lvl->count], sizeof(lvl->names[0]), "%s", name);
        lvl->isDir[lvl->count] = isDir;
        lvl->count++;
    }

    /// Recurse into `path`, depth-bounded, one heap-allocated level at a time, the shape of `FilesystemModule::removeTree`.
    void scanDir(const char* path, uint8_t depth) {
        if (depth > kMaxScanDepth || fileCount_ >= kMaxFiles) return;
        auto* raw = platform::alloc(sizeof(ScanLevel));
        if (!raw) return;
        ScanLevel& lvl = *static_cast<ScanLevel*>(raw);   // plain data: the two counters are set here, the arrays are written before they are read
        lvl.count = 0;
        lvl.skipped = false;
        struct Freer { void* p; ~Freer() { platform::free(p); } } freer{raw};
        platform::fsList(path, &collectScanEntry, &lvl);
        if (lvl.skipped) scanSkipped_ = true;
        for (uint8_t i = 0; i < lvl.count && fileCount_ < kMaxFiles; i++) {
            char child[kPathCap];
            if (!joinPath(path, lvl.names[i], child, sizeof(child))) { scanSkipped_ = true; continue; }
            if (lvl.isDir[i]) {
                scanDir(child, static_cast<uint8_t>(depth + 1));
            } else if (hasExtension(lvl.names[i], ".fseq")) {
                copyBounded(fileNames_.data() + static_cast<size_t>(fileCount_++) * kPathCap, kPathCap, child);
            }
        }
    }

    /// Re-scan the filesystem root for `.fseq` files into the shared option set.
    void rescanFiles() {
        fileCount_ = 0;
        scanSkipped_ = false;
        if (fileNames_.resize(static_cast<size_t>(kMaxFiles) * kPathCap)) scanDir("/", 0);
        else setStatus("out of memory for the file list", Severity::Error);
        rebuildControls();
        // Fires the resync directly: rebuildControls()'s own change-detection does not hash a List's row/option-set content.
        notifySchemaChanged();
        if (scanSkipped_) setStatus("some .fseq names or paths too long, skipped", Severity::Warning);
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
        char fileCopy[kPathCap];
        copyBounded(fileCopy, sizeof(fileCopy), trackFile(cursor_));
        std::snprintf(statusBuf_, sizeof(statusBuf_), "%s: %s (%u/%u)", label, fileCopy,
                      static_cast<unsigned>(cursor_ + 1), static_cast<unsigned>(playlistCount_));
        setStatus(statusBuf_);
    }
};

} // namespace mm
