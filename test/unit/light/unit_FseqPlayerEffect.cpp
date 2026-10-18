/// @module FseqPlayerEffect
/// @also FseqFile

/// Pins the FSEQ player: the transport state machine against injected state, and the scan/decode/blit path against real `.fseq` bytes under a test filesystem root, mounted or plain flash alike.

#include "doctest.h"

#include "light/effects/FseqPlayerEffect.h"
#include "light/layouts/GridLayout.h"
#include "core/util/JsonSink.h"

#include <filesystem>
#include <string>
#include <vector>

using namespace mm;

namespace {

fseq::FseqHeader makeHeader(uint32_t channelCount, uint32_t frameCount, uint8_t stepTimeMs = 25) {
    fseq::FseqHeader h;
    h.channelDataOffset = 32;
    h.channelCount = channelCount;
    h.frameCount = frameCount;
    h.stepTimeMs = stepTimeMs;
    return h;
}

// A real v1 FSEQ file's bytes: frame i's channels all hold the byte value i, so a test can tell which frame decoded.
std::vector<uint8_t> makeFseqV1Bytes(uint32_t channelCount, uint32_t frameCount, uint8_t stepTimeMs = 25) {
    std::vector<uint8_t> b(32 + static_cast<size_t>(channelCount) * frameCount, 0);
    std::memcpy(b.data(), "PSEQ", 4);
    b[4] = 32; b[5] = 0;       // channelDataOffset = 32
    b[6] = 0; b[7] = 1;        // minor=0, major=1
    for (int i = 0; i < 4; i++) b[10 + i] = static_cast<uint8_t>(channelCount >> (8 * i));
    for (int i = 0; i < 4; i++) b[14 + i] = static_cast<uint8_t>(frameCount >> (8 * i));
    b[18] = stepTimeMs;
    for (uint32_t f = 0; f < frameCount; f++)
        std::memset(b.data() + 32 + static_cast<size_t>(f) * channelCount, static_cast<int>(f), channelCount);
    return b;
}

// A fresh temp dir as the flash root for one test (the unit_HttpServerModule_sdcard.cpp shape), mounting a card by default; pass false for flash-root-only.
struct SdFixture {
    char root[256];
    bool mounted;
    explicit SdFixture(bool mountSd = true) : mounted(mountSd) {
        static unsigned counter = 0;
        std::snprintf(root, sizeof(root), "%s/mm_fseq_test_%u",
                      std::filesystem::temp_directory_path().string().c_str(), counter++);
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        platform::fsSetRoot(root);
        if (mounted) {
            platform::SdPinConfig pins{};
            pins.clk = 43; pins.cmd = 44; pins.d0 = 39;
            REQUIRE(platform::sdMount(pins));
        }
    }
    // NOLINTNEXTLINE(bugprone-exception-escape)
    ~SdFixture() noexcept {
        if (mounted) platform::sdUnmount();
        platform::fsSetRoot("");
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    void writeFseq(const char* path, const std::vector<uint8_t>& bytes) {
        REQUIRE(platform::fsWriteAtomic(path, reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    }
};

// A real 4x4 Layer the effect is parented to, the same fixture shape unit_DemoReelEffect.cpp uses so elapsed()/buffer() resolve through the parent.
struct Scene {
    Layouts layouts;
    GridLayout grid;
    Layer layer;
    FseqPlayerEffect effect;
    Scene() {
        grid.width = 4; grid.height = 4; grid.depth = 1;
        layouts.addChild(&grid);
        layer.setLayouts(&layouts);
        layer.setChannelsPerLight(3);
        layer.addChild(&effect);
    }
};

// A v1 file whose channelDataOffset is pushed past the header's own 32 fixed bytes, the way a real export's variable-length metadata block would; the padding is arbitrary (zero).
std::vector<uint8_t> makeFseqV1BytesWithOffset(uint16_t channelDataOffset, uint32_t channelCount, uint32_t frameCount) {
    std::vector<uint8_t> b(static_cast<size_t>(channelDataOffset) + static_cast<size_t>(channelCount) * frameCount, 0);
    std::memcpy(b.data(), "PSEQ", 4);
    b[4] = static_cast<uint8_t>(channelDataOffset & 0xFF);
    b[5] = static_cast<uint8_t>(channelDataOffset >> 8);
    b[6] = 0; b[7] = 1;
    for (int i = 0; i < 4; i++) b[10 + i] = static_cast<uint8_t>(channelCount >> (8 * i));
    for (int i = 0; i < 4; i++) b[14 + i] = static_cast<uint8_t>(frameCount >> (8 * i));
    b[18] = 25;
    for (uint32_t f = 0; f < frameCount; f++)
        std::memset(b.data() + channelDataOffset + static_cast<size_t>(f) * channelCount, static_cast<int>(f), channelCount);
    return b;
}

// Two tracks written, scanned and added as playlist rows, for the two tests exercising a second row.
void twoTrackPlaylist(SdFixture& sd, FseqPlayerEffect& e, uint32_t& id1, uint32_t& id2) {
    sd.writeFseq("/sd/a.fseq", makeFseqV1Bytes(10, 2));
    sd.writeFseq("/sd/b.fseq", makeFseqV1Bytes(10, 2));
    e.setup();
    e.addListRow(id1);
    e.addListRow(id2);
}

} // namespace

// Pins the independence the product owner asked for directly: no SD-specific check or status, ever.
TEST_CASE("setup scans silently, with no SD-specific check or status") {
    Scene s;
    auto& e = s.effect;
    e.setup();
    CHECK(e.status() == nullptr);
}

TEST_CASE("rescan finds a real .fseq file at the flash root, with no SD mount at all") {
    SdFixture flash(/*mountSd=*/false);
    flash.writeFseq("/show.fseq", makeFseqV1Bytes(9, 3));
    Scene s;
    auto& e = s.effect;
    e.setup();
    uint32_t id = 0;
    REQUIRE(e.addListRow(id));
    // addListRow defaults to the first scanned file, so a non-empty default proves the scan found it at the flash root.
    CHECK(e.listRowCount() == 1);
}

TEST_CASE("playing a flash-found file decodes and blits its frames, same as an SD one") {
    SdFixture flash(/*mountSd=*/false);
    flash.writeFseq("/show.fseq", makeFseqV1Bytes(/*channelCount=*/48, /*frameCount=*/3));   // 48 = 4x4x1x3
    Scene s;
    auto& e = s.effect;
    e.setup();
    uint32_t id = 0;
    REQUIRE(e.addListRow(id));

    e.onControlChanged("play");
    REQUIRE(e.stateForTest() == FseqPlayerEffect::PlayState::Playing);
    REQUIRE(e.status() != nullptr);
    CHECK(std::string(e.status()).rfind("playing", 0) == 0);

    const uint8_t* buf = s.layer.buffer().data();
    const size_t bytes = s.layer.buffer().count() * 3;

    e.decodeOneFrameForTest();   // frame 0: every byte is 0
    e.tick();
    for (size_t i = 0; i < bytes; i++) CHECK(buf[i] == 0);

    e.decodeOneFrameForTest();   // frame 1: every byte is 1
    e.tick();
    for (size_t i = 0; i < bytes; i++) CHECK(buf[i] == 1);
}

TEST_CASE("setup with a mounted but empty card scans cleanly to an empty playlist") {
    SdFixture sd;
    Scene s;
    auto& e = s.effect;
    e.setup();
    CHECK(e.listRowCount() == 0);
}

TEST_CASE("rescan finds a real .fseq file, case-insensitively, in a nested folder, skipping dotfiles") {
    SdFixture sd;
    sd.writeFseq("/sd/show.FSEQ", makeFseqV1Bytes(9, 3));
    sd.writeFseq("/sd/shows/nested.fseq", makeFseqV1Bytes(9, 3));
    sd.writeFseq("/sd/.hidden.fseq", makeFseqV1Bytes(9, 3));
    sd.writeFseq("/sd/notes.txt", {1, 2, 3});
    Scene s;
    auto& e = s.effect;
    e.setup();   // rescans once on its own

    uint32_t id = 0;
    REQUIRE(e.addListRow(id));
    // addListRow defaults to the first scanned file, so a non-empty default proves the scan found something real.
    CHECK(e.listRowCount() == 1);
}

TEST_CASE("onFileChanged rescans on any path, so an upload anywhere reaches the playlist unasked") {
    SdFixture sd;
    Scene s;
    auto& e = s.effect;
    e.setup();
    uint32_t idBefore = 0;
    REQUIRE(e.addListRow(idBefore));   // nothing scanned yet: defaults to an empty file
    e.deleteListRow(idBefore);

    sd.writeFseq("/sd/new.fseq", makeFseqV1Bytes(9, 3));
    e.onFileChanged("/sd/new.fseq");

    uint32_t idAfter = 0;
    REQUIRE(e.addListRow(idAfter));
    CHECK(e.listRowCount() == 1);
    // This effect has no path it specifically cares about: any changed path triggers the same rescan, with no crash from a path that names no real file.
    e.onFileChanged("/.config/SomeModule.json");
}

TEST_CASE("prepare() starts playing on its own once there is a playlist to play") {
    Scene s;
    auto& e = s.effect;
    // Empty playlist: nothing to start, and no crash from trying anyway.
    e.prepare();
    CHECK(e.stateForTest() == FseqPlayerEffect::PlayState::Stopped);

    uint32_t id = 0;
    e.addListRow(id);
    e.prepare();
    // The row's file is empty: nothing was ever scanned. An error status (not silence) proves prepare() tried to auto-start.
    CHECK(e.stateForTest() == FseqPlayerEffect::PlayState::Stopped);
    REQUIRE(e.status() != nullptr);
    CHECK(e.severity() == MoonModule::Severity::Error);
}

TEST_CASE("playing a real file decodes and blits its frames in order") {
    SdFixture sd;
    sd.writeFseq("/sd/show.fseq", makeFseqV1Bytes(/*channelCount=*/48, /*frameCount=*/3));   // 48 = 4x4x1x3
    Scene s;
    auto& e = s.effect;
    e.setup();
    uint32_t id = 0;
    REQUIRE(e.addListRow(id));

    e.onControlChanged("play");
    REQUIRE(e.stateForTest() == FseqPlayerEffect::PlayState::Playing);
    // Pins a real bug: loadTrack's own refreshStatus() must see the state this call transitions to, not the one it leaves, or a successful play reports "stopped".
    REQUIRE(e.status() != nullptr);
    CHECK(std::string(e.status()).rfind("playing", 0) == 0);

    const uint8_t* buf = s.layer.buffer().data();
    const size_t bytes = s.layer.buffer().count() * 3;

    e.decodeOneFrameForTest();   // frame 0: every byte is 0
    e.tick();
    for (size_t i = 0; i < bytes; i++) CHECK(buf[i] == 0);

    e.decodeOneFrameForTest();   // frame 1: every byte is 1
    e.tick();
    for (size_t i = 0; i < bytes; i++) CHECK(buf[i] == 1);

    e.decodeOneFrameForTest();   // frame 2, the last one
    e.tick();
    for (size_t i = 0; i < bytes; i++) CHECK(buf[i] == 2);

    // Frame 3 does not exist: handleEndOfFile already ran inside the decode above (loopAll default: reloads frame 0).
    CHECK(e.frameIndexForTest() == 0);

    // Pause updates status too, not just state: a pre-existing gap found and fixed alongside the play-status bug above.
    e.onControlChanged("pause");
    REQUIRE(e.status() != nullptr);
    CHECK(std::string(e.status()).rfind("paused", 0) == 0);
}

TEST_CASE("release() forces a clean stop, so a later re-enable never resumes into freed buffers") {
    Scene s;
    auto& e = s.effect;
    e.setStateForTest(FseqPlayerEffect::PlayState::Playing);
    e.release();
    CHECK(e.stateForTest() == FseqPlayerEffect::PlayState::Stopped);
}

TEST_CASE("a stopped player owns its background (clears to black), but a paused one re-blits the same frame") {
    Scene s;
    auto& e = s.effect;
    uint8_t* buf = const_cast<uint8_t*>(s.layer.buffer().data());
    const size_t bytes = s.layer.buffer().count() * 3;
    constexpr uint8_t kStale = 0xA7;   // a value no real frame would coincidentally produce

    std::memset(buf, kStale, bytes);
    REQUIRE(e.stateForTest() == FseqPlayerEffect::PlayState::Stopped);   // the default, no playlist
    e.tick();
    for (size_t i = 0; i < bytes; i++) CHECK(buf[i] == 0);   // owns its background: goes black

    std::memset(buf, kStale, bytes);
    e.setStateForTest(FseqPlayerEffect::PlayState::Paused);
    e.tick();
    // No frame has ever decoded (frameBuf_ was never allocated): blitFrameBuf's own guard makes this a no-op, not a crash.
    for (size_t i = 0; i < bytes; i++) CHECK(buf[i] == kStale);
}

TEST_CASE("the playlist is an editable list: add, delete, move, edit") {
    Scene s;
    auto& e = s.effect;
    uint32_t id1 = 0, id2 = 0, id3 = 0;
    CHECK(e.isEditableList());
    CHECK(e.addListRow(id1));
    CHECK(e.addListRow(id2));
    CHECK(e.listRowCount() == 2);
    CHECK(id1 != id2);

    CHECK(e.deleteListRow(id1));
    CHECK(e.listRowCount() == 1);
    CHECK_FALSE(e.deleteListRow(id1));   // already gone

    CHECK(e.addListRow(id3));
    CHECK(e.listRowCount() == 2);
    CHECK(e.moveListRow(id3, 0));        // id3 now row 0, id2 row 1

    // No SD files have been scanned, so the file picker's option index is out of range.
    CHECK_FALSE(e.setListRowField(id2, "file", "{\"value\":0}"));
    CHECK_FALSE(e.setListRowField(999999, "file", "{\"value\":0}"));   // an unknown id
    CHECK_FALSE(e.setListRowField(id2, "nonsense", "{\"value\":0}"));  // an unknown field
}

TEST_CASE("play with an empty playlist does nothing") {
    Scene s;
    auto& e = s.effect;
    e.onControlChanged("play");
    CHECK(e.stateForTest() == FseqPlayerEffect::PlayState::Stopped);
}

TEST_CASE("play with no real file to open fails gracefully rather than pretending to start") {
    Scene s;
    auto& e = s.effect;
    uint32_t id = 0;
    e.addListRow(id);
    e.onControlChanged("play");
    // The row's file is empty: loadTrack's open fails, so playback never actually starts.
    CHECK(e.stateForTest() == FseqPlayerEffect::PlayState::Stopped);
    REQUIRE(e.status() != nullptr);
    CHECK(e.severity() == MoonModule::Severity::Error);
}

TEST_CASE("pause only takes effect while playing") {
    Scene s;
    auto& e = s.effect;
    e.setStateForTest(FseqPlayerEffect::PlayState::Stopped);
    e.onControlChanged("pause");
    CHECK(e.stateForTest() == FseqPlayerEffect::PlayState::Stopped);

    e.setStateForTest(FseqPlayerEffect::PlayState::Playing);
    e.onControlChanged("pause");
    CHECK(e.stateForTest() == FseqPlayerEffect::PlayState::Paused);

    e.onControlChanged("pause");   // already paused: no further change
    CHECK(e.stateForTest() == FseqPlayerEffect::PlayState::Paused);
}

TEST_CASE("stop resets the frame position and only applies while playing or paused") {
    Scene s;
    auto& e = s.effect;
    e.onControlChanged("stop");   // Stopped -> Stopped: a no-op, not a crash
    CHECK(e.stateForTest() == FseqPlayerEffect::PlayState::Stopped);

    e.setStateForTest(FseqPlayerEffect::PlayState::Playing);
    e.setHeaderForTest(makeHeader(10, 100));
    e.onControlChanged("stop");
    CHECK(e.stateForTest() == FseqPlayerEffect::PlayState::Stopped);
    CHECK(e.frameIndexForTest() == 0);
}

TEST_CASE("previous and next move the cursor only, while the transport is stopped") {
    Scene s;
    auto& e = s.effect;
    uint32_t id1 = 0, id2 = 0, id3 = 0;
    e.addListRow(id1);
    e.addListRow(id2);
    e.addListRow(id3);
    REQUIRE(e.cursorForTest() == 0);

    e.onControlChanged("next");
    CHECK(e.cursorForTest() == 1);
    e.onControlChanged("next");
    CHECK(e.cursorForTest() == 2);
    e.onControlChanged("next");       // wraps past the last row
    CHECK(e.cursorForTest() == 0);
    e.onControlChanged("previous");   // wraps back
    CHECK(e.cursorForTest() == 2);

    // Stopped, so moving the cursor never touches playback state.
    CHECK(e.stateForTest() == FseqPlayerEffect::PlayState::Stopped);
}

TEST_CASE("shuffle's cursor never repeats the row it just left") {
    Scene s;
    auto& e = s.effect;
    uint32_t ids[5] = {};
    for (uint32_t& id : ids) e.addListRow(id);
    e.setShuffleForTest(true);

    uint8_t prev = e.cursorForTest();
    for (int i = 0; i < 200; i++) {
        e.onControlChanged("next");
        const uint8_t now = e.cursorForTest();
        CHECK(now < 5);
        CHECK(now != prev);   // the do-while non-repeat guard in advanceCursor
        prev = now;
    }
}

TEST_CASE("the recursive scan's path join never doubles the slash at the SD root, and truncates safely") {
    char out[32];
    FseqPlayerEffect::joinPathForTest("/sd", "show.fseq", out, sizeof(out));
    CHECK(std::string(out) == "/sd/show.fseq");

    FseqPlayerEffect::joinPathForTest("/sd/shows", "intro.fseq", out, sizeof(out));
    CHECK(std::string(out) == "/sd/shows/intro.fseq");

    FseqPlayerEffect::joinPathForTest("/sd/shows/", "intro.fseq", out, sizeof(out));
    CHECK(std::string(out) == "/sd/shows/intro.fseq");   // trailing slash on dir: no double "//"

    char small[6];
    FseqPlayerEffect::joinPathForTest("/sd/shows", "averylongname.fseq", small, sizeof(small));
    CHECK(std::strlen(small) < sizeof(small));   // truncated, still NUL-terminated, never overrun
}

TEST_CASE("fpsSource=manual: the fps slider sets the playback rate directly, ignoring the file's own step time") {
    Scene s;
    auto& e = s.effect;
    e.setFpsSourceForTest(FseqPlayerEffect::kFpsManual);
    // The file's stepTimeMs (25ms, i.e. 40fps) deliberately differs from every fps set below, to prove the slider wins.
    e.setHeaderForTest(makeHeader(10, 50, /*stepTimeMs=*/25));
    e.setFpsForTest(30);
    CHECK(e.effectiveStepMsForTest() == 33);   // 1000/30, rounded down

    e.setFpsForTest(20);
    CHECK(e.effectiveStepMsForTest() == 50);   // 1000/20
    e.setFpsForTest(1);
    CHECK(e.effectiveStepMsForTest() == 1000);
    e.setFpsForTest(50);
    CHECK(e.effectiveStepMsForTest() == 20);   // the ceiling the `fps` control allows: tick20ms's own cadence
}

TEST_CASE("fpsSource=file is the default, and reads the track's own stepTimeMs directly") {
    Scene s;
    auto& e = s.effect;
    // Reading the file's rate without ever touching setFpsSourceForTest proves kFpsFromFile is the real default.
    e.setHeaderForTest(makeHeader(10, 50, /*stepTimeMs=*/25));
    CHECK(e.effectiveStepMsForTest() == 25);   // the file's own rate, not the fps_ default of 30 (33ms)

    e.setHeaderForTest(makeHeader(10, 50, /*stepTimeMs=*/100));
    CHECK(e.effectiveStepMsForTest() == 100);   // 10fps, well under the cap
}

TEST_CASE("fpsSource=file caps a rate faster than 50fps rather than honoring it") {
    Scene s;
    auto& e = s.effect;
    e.setHeaderForTest(makeHeader(10, 50, /*stepTimeMs=*/10));   // 100fps authored, double the cap
    CHECK(e.effectiveStepMsForTest() == 20);   // clamped to 1000/50, tick20ms's own cadence

    e.setHeaderForTest(makeHeader(10, 50, /*stepTimeMs=*/0));   // an unauthored rate: default to the cap, not a /0
    CHECK(e.effectiveStepMsForTest() == 20);
}

TEST_CASE("loading a file-sourced track faster than 50fps warns, naming the rate it asked for") {
    SdFixture sd;
    // stepTimeMs=10 is 100fps, double the cap this rate source can honor.
    sd.writeFseq("/sd/fast.fseq", makeFseqV1Bytes(10, 3, /*stepTimeMs=*/10));
    Scene s;
    auto& e = s.effect;
    e.setup();
    uint32_t id = 0;
    REQUIRE(e.addListRow(id));

    e.onControlChanged("play");
    REQUIRE(e.stateForTest() == FseqPlayerEffect::PlayState::Playing);
    REQUIRE(e.status() != nullptr);
    CHECK(e.severity() == MoonModule::Severity::Warning);
    CHECK(std::string(e.status()).find("100") != std::string::npos);
    CHECK(std::string(e.status()).find("50") != std::string::npos);
}

TEST_CASE("a manual-mode load never warns about the file's own rate, however fast it is authored") {
    SdFixture sd;
    sd.writeFseq("/sd/fast.fseq", makeFseqV1Bytes(10, 3, /*stepTimeMs=*/10));
    Scene s;
    auto& e = s.effect;
    e.setup();
    e.setFpsSourceForTest(FseqPlayerEffect::kFpsManual);
    uint32_t id = 0;
    REQUIRE(e.addListRow(id));

    e.onControlChanged("play");
    REQUIRE(e.stateForTest() == FseqPlayerEffect::PlayState::Playing);
    REQUIRE(e.status() != nullptr);
    CHECK(e.severity() != MoonModule::Severity::Warning);
}

TEST_CASE("loopOne restarts the same track at frame 0 without advancing the cursor") {
    SdFixture sd;
    sd.writeFseq("/sd/show.fseq", makeFseqV1Bytes(10, 3));
    Scene s;
    auto& e = s.effect;
    e.setup();
    uint32_t id = 0;
    e.addListRow(id);
    e.setLoopModeForTest(FseqPlayerEffect::kLoopOne);
    e.onControlChanged("play");
    REQUIRE(e.stateForTest() == FseqPlayerEffect::PlayState::Playing);

    e.handleEndOfFileForTest();
    CHECK(e.frameIndexForTest() == 0);
    CHECK(e.stateForTest() == FseqPlayerEffect::PlayState::Playing);   // stays playing: it is the same track
    CHECK(e.cursorForTest() == 0);                                     // never advanced
}

TEST_CASE("loopOff stops once every row has already played this pass, with no further read needed") {
    Scene s;
    auto& e = s.effect;
    uint32_t ids[3] = {};
    for (uint32_t& id : ids) e.addListRow(id);
    e.setStateForTest(FseqPlayerEffect::PlayState::Playing);
    e.setLoopModeForTest(FseqPlayerEffect::kLoopOff);
    e.setHeaderForTest(makeHeader(10, 50));
    // Simulate a pass that already touched every row (as loadTrack would have, had the reads succeeded).
    e.markVisitedForTest(0);
    e.markVisitedForTest(1);
    e.markVisitedForTest(2);
    REQUIRE(e.allTracksVisitedForTest());

    e.handleEndOfFileForTest();   // this branch returns before ever touching loadTrack/the card
    CHECK(e.stateForTest() == FseqPlayerEffect::PlayState::Stopped);
    CHECK(e.frameIndexForTest() == 0);
    CHECK_FALSE(e.allTracksVisitedForTest());   // the bitset is cleared for the next pass
}

TEST_CASE("loopAll advances to the next real track and keeps playing") {
    SdFixture sd;
    Scene s;
    auto& e = s.effect;
    uint32_t id1 = 0, id2 = 0;
    twoTrackPlaylist(sd, e, id1, id2);
    e.setLoopModeForTest(FseqPlayerEffect::kLoopAll);
    e.onControlChanged("play");
    REQUIRE(e.stateForTest() == FseqPlayerEffect::PlayState::Playing);
    REQUIRE(e.cursorForTest() == 0);

    e.handleEndOfFileForTest();
    CHECK(e.stateForTest() == FseqPlayerEffect::PlayState::Playing);   // the next row's open succeeded
    CHECK(e.cursorForTest() == 1);
    CHECK(e.frameIndexForTest() == 0);
}

TEST_CASE("a channelDataOffset past the buffered header region is skipped, landing frame data correctly") {
    SdFixture sd;
    // channelDataOffset (5000) exceeds the 4096-byte buffered header region: skipHeaderGap must consume the gap from the live stream so frame 0 still starts at the right byte.
    sd.writeFseq("/sd/gap.fseq", makeFseqV1BytesWithOffset(5000, /*channelCount=*/48, /*frameCount=*/3));
    Scene s;
    auto& e = s.effect;
    e.setup();
    uint32_t id = 0;
    REQUIRE(e.addListRow(id));

    e.onControlChanged("play");
    REQUIRE(e.stateForTest() == FseqPlayerEffect::PlayState::Playing);

    const uint8_t* buf = s.layer.buffer().data();
    const size_t bytes = s.layer.buffer().count() * 3;
    e.decodeOneFrameForTest();   // frame 0: every byte is 0, same as the no-gap decode test
    e.tick();
    for (size_t i = 0; i < bytes; i++) CHECK(buf[i] == 0);
    e.decodeOneFrameForTest();   // frame 1: every byte is 1
    e.tick();
    for (size_t i = 0; i < bytes; i++) CHECK(buf[i] == 1);
}

TEST_CASE("a playlist row whose file no longer scans is rendered with no selection, not the first file") {
    SdFixture sd;
    sd.writeFseq("/sd/a.fseq", makeFseqV1Bytes(10, 2));
    Scene s;
    auto& e = s.effect;
    e.setup();
    uint32_t id = 0;
    REQUIRE(e.addListRow(id));   // defaults to the one scanned file, "/sd/a.fseq"

    std::filesystem::remove(std::string(sd.root) + "/sd/a.fseq");   // gone from disk; a rescan drops it from the option set
    e.onFileChanged("/sd/a.fseq");
    CHECK(e.listRowCount() == 1);   // the row survives rescans; only the file options shrink

    JsonSink sink;
    e.writeListRowDetail(sink, 0);
    CHECK(std::string(sink.data()).find("\"value\":-1,\"optionsRef\":\"files\"") != std::string::npos);
}

TEST_CASE("a playlist row's own play field jumps to that row and starts a fresh pass") {
    SdFixture sd;
    Scene s;
    auto& e = s.effect;
    uint32_t id1 = 0, id2 = 0;
    twoTrackPlaylist(sd, e, id1, id2);
    e.markVisitedForTest(0);   // pretend row 0 already played this pass

    e.setListRowField(id2, "play", "{}");
    CHECK(e.cursorForTest() == 1);
    CHECK(e.stateForTest() == FseqPlayerEffect::PlayState::Playing);   // the row's own file opened fine
}
