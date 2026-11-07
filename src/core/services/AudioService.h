#pragma once

#include "core/util/format.h"   // formatTo: nonblocking formatting into a fixed buffer
#include "core/util/fnv.h"      // fnv1a: the sync hosts, compared as one number
#include "core/util/Addressing.h"   // sendAddressed: the multicast rule every sender shares
#include "core/util/HostList.h"     // the boards a unicast send goes to
#include "core/util/HostResolver.h" // a named board's address, looked up off the render thread, and formatPeer: the sender by its board's name
#include "core/module/MoonModule.h"
#include "core/util/ActiveInstance.h"   // the one-active-mic election (the seat + its RAII vacate)
#include "core/util/AudioFrame.h"
#include "core/util/SpscRing.h"     // the audio task's frames to the render tick
#include "core/services/AudioLevel.h"
#include "core/services/AudioBands.h"
#include "core/services/AudioDynamics.h"   // the level, floor, gate and window
#include "core/services/AudioHarmony.h"    // the twelve pitch classes from the notes
#include "core/services/AudioSimulation.h" // the synthesized signal `simulate` plays
#include "core/services/AudioAnalysis.h"   // the state an open input and `harmony` hold
#include "core/util/MoonLightAudioSyncPacket.h"   // the MoonLight audio-sync format (send/receive)
#include "core/util/WLEDAudioSyncPacket.h"   // WLED audio-sync wire format (send/receive)
#include "platform/platform.h"

#include <atomic>
#include <memory>   // unique_ptr: the analysis, held only while an input is open
#include <new>      // std::nothrow
#include <cstdint>
#include <cstdio>   // snprintf for the read-out strings
#include <cstring>

namespace mm {

/// The audio source the audio-reactive effects read: a level, sixteen bands, the 31 third octaves and the peak.
/// @card AudioService.png
///
/// Local analyzes a microphone, line-in or capture device here. Receive network follows a peer's audio, MoonLight's or WLED's, and Simulate synthesizes a song for demos. A local source can send its analysis on to other devices.
///
/// @attribution softhack007, troyhacks, NetMindz, wladi
///
/// @guide
///
/// ## Choosing a source
///
/// Simulate needs nothing. Receive network needs a MoonLight or WLED sender. Local idles on an ESP32 until real GPIOs are entered, and captures the picked device at once on desktop.
///
/// ## Wiring and capture
///
/// Microphone wiring, setting the levels and capturing what a machine plays are in [Audio input](https://moonmodules.org/MoonLight/reference/audio-input.html).
///
/// @technical
///
/// ## The modes run simple to advanced
///
/// Simulate synthesizes a signal for a demo or a test, Receive is a sink a peer drives, and Local runs the device's own input and analyzes it here. A device is exactly one of these, and changing it acquires or releases hardware live.
///
/// They are ordered by what each one needs: simulate nothing, receive a network, local a part wired to pins. So the default is the first entry, and a device demonstrates sound before anything is attached.
///
/// **The pipeline.** Each block: read, block the offset, measure the level, feed the third-octave filter bank, read the bands. The high-pass conditions the block once so both halves see the same signal, and a tick completing no block re-publishes the last frame.
///
/// Only the read is platform code; the rest is host-tested maths on a fixed resident scratch, so a block allocates nothing.
///
/// ## The audio task
///
/// The read and the analysis run on a task of their own, which blocks on the input until a block arrives, as Troy's AudioReactive does: the analysis keeps the input's pace whatever the frame rate, a slow frame loses no samples, and on a dual-core chip the bank leaves the render core its time.
///
/// Each finished frame goes through a single-producer single-consumer ring to the render tick, which keeps the newest, so effects read `latestFrame()` on the render thread as before. The settings reach the task as atomics the tick refreshes, and the second's tallies come back as atomics the readout takes and clears. A board without tasks reads and analyzes in the tick.
///
/// ## The stream's own clock
///
/// The analysis keeps time by the samples it has analyzed, a block at a time, not by the wall clock: the onsets' refractories then measure the music, whether a block is analyzed late on a busy core or a file is fed faster than real time on the host. A synthesized or received frame lives on the wall clock, which is its time.
///
/// ## Memory follows the mode
///
/// The analysis, the bank, the sample block, the dynamics and the frame ring, is allocated in internal RAM as an input opens and freed as it closes, so a board receiving audio sync or simulating holds none of it.
///
/// The harmony's state, the semitone bank, its trackers and the note onsets, about 8.5 KB, is a second allocation held only while `harmony` is on. Switching it pauses the audio task while the state changes hands, and leaves the input open; a prepare runs on the render loop between ticks, so the tick never meets a state changing.
///
/// ## Audio sync, one packet a frame
///
/// A sending board sends its frame in `syncFormat`: MoonLight's whole frame, WLED's packet, or both for an installation that mixes the two. Each format multicasts to its own group, so a WLED device never wakes for a packet it cannot read. The hits of the frames between two packets ride in the next, so the throttle drops none.
///
/// A receiving board joins both groups and tells the formats by their header. A board sending both sends each frame twice, so MoonLight's packet outranks WLED's while it arrives. A frame with the sender's smoothed bands keeps them, so every board shows the same frame, and one without, a WLED frame among them, gets the bands' ballistic here; a late or repeated MoonLight frame number is dropped.
class AudioService : public MoonModule {
public:
    /// `level` through the bands' peak-meter ballistic into `levelSmoothed`, for a frame this engine measured or synthesized; a synced frame keeps the sender's own.
    static void smoothLevel(AudioFrame& f) {
        const uint8_t level = static_cast<uint8_t>(f.level > 255 ? 255 : f.level);
        f.levelSmoothed = ballistic(static_cast<uint8_t>(f.levelSmoothed > 255 ? 255 : f.levelSmoothed), level, kBandRise, kBandFall);
    }

    /// Finish a block of raw bands in `f`: the bands' ballistic, then what derives from the bands.
    void finishBands(AudioFrame& f) {
        smoothBands(f.bands, f.bandsSmoothed);
        deriveFromBands(f);
    }
    /// The flux readout and the ranges, and count its onset and harmony change, which its source set, the analysis, the song or the sender.
    void deriveFromBands(AudioFrame& f) {
        f.flux = spectralFlux(prevBands_, f.bands);
        std::memcpy(prevBands_, f.bands, sizeof(prevBands_));
        bandsToRanges(f);
        if (f.onset) onsetCount_.fetch_add(1, std::memory_order_relaxed);
        if (f.harmonyChange) harmonyChanges_.fetch_add(1, std::memory_order_relaxed);
        if (f.flux > fluxPeak_.load(std::memory_order_relaxed)) fluxPeak_.store(f.flux, std::memory_order_relaxed);
    }
    uint8_t       prevBands_[16] = {};   ///< the previous block's bands, the flux's reference

    /// The block an input is read in, kAudioBlock.
    static constexpr size_t kBlock = kAudioBlock;

    /// A service, so the container accepts it as a child.
    ModuleRole role() const MM_NONBLOCKING override { return ModuleRole::Service; }

    /// The capture input, `default` following the system setting.
    uint8_t device = 0;
    /// The microphone kinds `micMode` picks from, in its option order.
    enum Mic : uint8_t { kMicI2s, kMicPdm, kMicCount };
    /// Which kind of microphone is wired, I2S or two-wire PDM.
    /// @guide PDM uses `wsPin` as its clock and `sdPin` as its data, as the QuinLED Dig-Next-2's onboard mic does.
    uint8_t micMode = kMicI2s;
    /// The bit clock GPIO, unset until entered.
    int8_t sckPin = -1;
    /// The word select GPIO, or the clock of a PDM mic.
    int8_t wsPin = -1;
    /// The data GPIO, unset until entered.
    int8_t sdPin = -1;
    /// The master clock an ADC or codec needs, unset for a plain mic.
    int8_t mclkPin = -1;
    /// The codecs `codec` picks from, in its option order.
    enum Codec : uint8_t { kCodecNone, kCodecEs8311, kCodecEs7210, kCodecCount };
    /// The codec in front of the microphone, `none` for a part that speaks I2S itself.
    uint8_t codec = kCodecNone;
    /// The codec's 7-bit address on the board's I2C bus.
    /// @guide 0x18 for an ES8311, 0x40 to 0x43 for an ES7210 by its strapping. The bus is @ref I2cBusModule.
    uint8_t codecAddr = 0x18;
    /// The codec's analog input gain in dB, applied before its converter.
    /// @guide 30 for a microphone, 0 for line-in.
    uint8_t codecGain = 30;

    /// The codec the settings ask for.
    platform::CodecType codecType() const MM_NONBLOCKING {
        return codec == kCodecEs8311 ? platform::CodecType::Es8311
             : codec == kCodecEs7210 ? platform::CodecType::Es7210 : platform::CodecType::None;
    }
    /// The codecs the selector offers, each by the name the bus scan shows it under.
    static constexpr const char* kCodecOptions[] = {"none", "ES8311", "ES7210"};

    /// The codec, while the wired microphone uses one, so the bus scan names its address.
    uint8_t i2cDevices(I2cDevice* out, uint8_t max) const override {
        if (mode != kLocalMode || micMode == kMicPdm || codec == kCodecNone || codec >= kCodecCount || max == 0) return 0;
        out[0] = {codecAddr, kCodecOptions[codec]};
        return 1;
    }
    /// The rates the selector offers, in its option order.
    enum SampleRate : uint8_t { kRate8000, kRate16000, kRate22050, kRate24000, kRate44100, kRate48000, kSampleRateCount };
    /// The mic or ADC sample rate.
    /// @guide 24000 carries the 28 bands to 10 kHz, the 16 WLED bars among them; 48000 adds the three to 20 kHz.
    uint8_t  sampleRateSel = kRate24000;
    /// How far over the room's learned noise sound must rise before the lights answer, in dB.
    /// @guide Raise it until a quiet room reads dark.
    uint8_t  squelch = 10;
    /// Measure the notes, for the pitch classes, the harmony's changes and the notes bars.
    /// @guide Off saves the semitone bank's share of a core where nothing reads the harmony.
    bool     harmony = true;
    /// The synthesized patterns `simulate` picks from, in its option order.
    enum Simulate : uint8_t { kSimMusic, kSimSweep, kSimulateCount };
    /// The pattern: a plausible song, or a test march.
    uint8_t  simulate = kSimMusic;
    /// The source: a synthesized signal, the network, or its own input.
    uint8_t  mode = 0;
    /// A synthesized signal, so a device demonstrates sound before anything is wired to it.
    static constexpr uint8_t kSimMode = 0;
    /// Another device's analysis, over the network that has to exist to carry it.
    static constexpr uint8_t kReceiveMode = 1;
    /// This device's own microphone, which needs pins and a part to be connected to them.
    static constexpr uint8_t kLocalMode = platform::hasNetwork ? 2 : 1;
    /// Send the local analysis as audio-sync packets.
    bool     send = false;
    /// What the sync machinery does: nothing, broadcast the local analysis, or sink a peer's.
    enum Sync : uint8_t { kSyncNone, kSyncSend, kSyncReceive };
    /// The role the current mode and `send` give the sync socket.
    Sync     sync() const { return !platform::hasNetwork ? kSyncNone : mode == kReceiveMode ? kSyncReceive
                                 : (mode == kLocalMode && send ? kSyncSend : kSyncNone); }
    /// The UDP port both directions use, matching on both ends.
    /// @guide WLED listens on 11988, so another port is not WLED-compatible.
    uint16_t syncPort = WLED_SYNC_PORT;
    /// The addressings `addressing` picks from, in its option order.
    enum SyncAddressing : uint8_t { kAddressMulticast, kAddressUnicast, kAddressCount };
    /// How the analysis travels: multicast, which WLED hears, or unicast.
    /// @guide Unicast to `hosts` reaches a few MoonLight boards faster on WiFi, which retries it.
    uint8_t  addressing = kAddressMulticast;
    /// The boards a unicast send reaches, addresses or names.
    char     hosts[64] = {};
    /// The packet formats `syncFormat` picks from, in its option order.
    enum SyncFormat : uint8_t { kFormatMoonLight, kFormatWled, kFormatBoth, kFormatCount };
    /// What a sending board sends: MoonLight's whole frame, WLED's packet, or both.
    /// @guide Receivers read either. WLED feeds only WLED devices; both feeds MoonLight boards too.
    uint8_t  syncFormat = kFormatMoonLight;

    /// Each option's rate in hertz.
    static constexpr uint16_t kSampleRates[kSampleRateCount] = {8000, 16000, 22050, 24000, 44100, 48000};
    /// The selected rate in hertz.
    uint32_t sampleRate() const { return kSampleRates[sampleRateSel < kSampleRateCount
                                                       ? sampleRateSel : kSampleRateCount - 1]; }

    /// The wired microphone's controls: its kind, its bus, and the codec in front of it, each shown only where it applies.
    void defineMicControls(bool localMode) {
        // A two-wire part has neither clock, so showing them would invite dead settings.
        static constexpr const char* kMicModeOptions[] = {"I2S", "PDM"};
        controls_.addSelect("micMode", micMode, kMicModeOptions, kMicCount);
        controls_.setHidden(controls_.count() - 1, !localMode);
        const bool pdm = micMode == kMicPdm;
        controls_.addPin("sckPin", sckPin);        controls_.setHidden(controls_.count() - 1, !localMode || pdm);
        controls_.addPin("wsPin", wsPin);          controls_.setHidden(controls_.count() - 1, !localMode);
        controls_.addPin("sdPin", sdPin);          controls_.setHidden(controls_.count() - 1, !localMode);
        controls_.addPin("mclkPin", mclkPin);      controls_.setHidden(controls_.count() - 1, !localMode || pdm);
        // A codec sits on an I2S line only, so a two-wire part never shows it; its address shows once one is chosen, the bus being the board's.
        controls_.addSelect("codec", codec, kCodecOptions, kCodecCount);
        controls_.setPersistLabel(controls_.count() - 1);   // by name, as the device model writes it
        controls_.setHidden(controls_.count() - 1, !localMode || pdm);
        controls_.addControl("codecAddr", codecAddr, 0, 127);
        controls_.setHexField(controls_.count() - 1);   // an address, written in hex as the datasheet and the bus scan give it
        controls_.setHidden(controls_.count() - 1, !localMode || pdm || codec == kCodecNone);
        controls_.addControl("codecGain", codecGain, 0, 30);
        controls_.setHidden(controls_.count() - 1, !localMode || pdm || codec == kCodecNone);
    }

    /// Declare the mode, then only the controls that mode needs.
    void defineControls() override {
        // The mode is the identity, so it comes first and everything else is its detail.
        const bool localMode = (mode == kLocalMode);
        const bool simMode = (mode == kSimMode);
        // Ordered simple to advanced, so the default is the first entry rather than an index: @xref{the-modes-run-simple-to-advanced}.
        if constexpr (platform::hasNetwork) {
            static constexpr const char* kModeOptions[] = {"simulate", "receive network", "local audio"};
            controls_.addSelect("mode", mode, kModeOptions, 3);
        } else {
            static constexpr const char* kModeOptions[] = {"simulate", "local audio"};
            controls_.addSelect("mode", mode, kModeOptions, 2);
        }
        // The input and its analysis, shown only in the local mode.
        if constexpr (platform::hasI2sMic) defineMicControls(localMode);
        if constexpr (platform::hasAudioCapture) {
            // Re-enumerated per rebuild, so a hot-plugged device appears on the next change.
            const char* const* deviceOptions = nullptr;
            const uint8_t deviceCount = static_cast<uint8_t>(platform::audioCaptureDevices(&deviceOptions));
            controls_.addSelect("device", device, deviceOptions, deviceCount);
            controls_.setPersistLabel(controls_.count() - 1);
            controls_.setHidden(controls_.count() - 1, !localMode);
        }
        static constexpr const char* kRateOptions[] = {"8000", "16000", "22050", "24000", "44100", "48000"};
        controls_.addSelect("sampleRate", sampleRateSel, kRateOptions, kSampleRateCount);
        controls_.setPersistLabel(controls_.count() - 1);   // by name, so a rate added to the list keeps a stored one
        controls_.setHidden(controls_.count() - 1, !localMode);
        controls_.addControl("squelch", squelch, 0, 40);
        controls_.setHidden(controls_.count() - 1, !localMode);
        controls_.addControl("harmony", harmony);
        controls_.setHidden(controls_.count() - 1, !localMode);
        if constexpr (platform::hasNetwork) {
            controls_.addControl("send audio", send);
            controls_.setHidden(controls_.count() - 1, !localMode);
            static constexpr const char* kFormatNames[] = {"MoonLight", "WLED", "both"};
            controls_.addSelect("syncFormat", syncFormat, kFormatNames, kFormatCount);
            controls_.setHidden(controls_.count() - 1, !localMode || !send);
            // Multicast first, the WLED-compatible default; unicast reaches a few MoonLight boards faster on WiFi.
            static constexpr const char* kSyncModeNames[] = {"multicast", "unicast"};
            controls_.addSelect("addressing", addressing, kSyncModeNames, kAddressCount);
            controls_.setHidden(controls_.count() - 1, !localMode || !send);
            controls_.addText("hosts", hosts, sizeof(hosts));
            controls_.setHostsField(controls_.count() - 1);
            controls_.setHidden(controls_.count() - 1, !localMode || !send || addressing != kAddressUnicast);
        }
        // The pattern picker, shown only in the synthesized mode.
        static constexpr const char* kSimulateOptions[] = {"music", "sweep"};
        controls_.addSelect("simulate", simulate, kSimulateOptions, kSimulateCount);
        controls_.setHidden(controls_.count() - 1, !simMode);
        // Only where a socket is bound, the rows toggling live with the mode.
        if constexpr (platform::hasNetwork) {
            const bool hasSocket = (sync() != kSyncNone);
            controls_.addControl("syncPort", syncPort, 1, 65535);
            controls_.setHidden(controls_.count() - 1, !hasSocket);
        }
        // These stay visible in every mode.
        /// The level of the audio driving the effects, from any source.
        controls_.addReadOnly("level RMS", levelStr_, sizeof(levelStr_));
        /// The drum hits heard per second, and the spectral flux.
        controls_.addReadOnly("onsets", onsetStr_, sizeof(onsetStr_));
        /// The dominant frequency of the audio driving the effects.
        controls_.addReadOnly("peakHz", peakStr_, sizeof(peakStr_));
        /// The share of one core the bands took last second, and the input's real rate.
        controls_.addReadOnly("cpu", cpuStr_, sizeof(cpuStr_));
        /// The level, the room's floor, the window's top in dBFS, and the gate.
        controls_.addReadOnly("dynamics", dynStr_, sizeof(dynStr_));
        /// The three strongest pitch classes and the harmony's changes per second.
        controls_.addReadOnly("notes", notesStr_, sizeof(notesStr_));
        /// The key over the last 15 s, and how far the notes are tuned from A440.
        controls_.addReadOnly("key", keyStr_, sizeof(keyStr_));
        /// The bass, mids and treble on the bands' scale, as the effects read them.
        controls_.addReadOnly("ranges", rangesStr_, sizeof(rangesStr_));
        MoonModule::defineControls();
    }

    /// Which changes rebuild the channel, rebind the socket, or re-toggle the rows.
    bool affectsPrepare(const char* name) const override {
        // The pins, the kind of microphone and the device re-create the channel; the rate, the mode and the sync settings rebind; `harmony` allocates or frees its state.
        static constexpr const char* kPrepares[] = {"wsPin", "sdPin", "sckPin", "mclkPin", "micMode", "device", "sampleRate",
                                                    "mode", "send audio", "syncPort", "addressing", "hosts", "harmony"};
        if (std::strncmp(name, "codec", 5) == 0) return true;   // the codec and its bus, which re-init it
        for (const char* p : kPrepares) if (std::strcmp(name, p) == 0) return true;
        return false;
    }

    /// Claim the frame seat, then acquire only the hardware the current mode needs.
    void prepare() override {
        micSeat_.claim();       // the first live instance wins the seat, in any mode
        if (mode == kLocalMode) {
            // Only this mode runs a peripheral, reopened only when a setting it was opened with changed: a rebuild for another module must not cut the audio.
            if (!inited_ || reinitPending_ || micConfig() != openedWith_) reinit();
            else if (harmony != harmonyApplied_) switchHarmony();
        } else {
            if (deinit()) clearStatus();   // the others free the channel and its pins; a mic diagnosis would otherwise linger
        }
        // The socket likewise, rebound only when what it binds to changed.
        if (!syncSet_ || syncConfig() != syncedWith_) {
            syncReinit();
            syncedWith_ = syncConfig();
            syncSet_ = true;
        }
        parseSyncHosts();
    }
    /// One-time wiring only: the acquire and the election live in the build.
    void setup() override {}
    /// Free the peripheral and the socket, and vacate the seat for any survivor.
    void release() override {
        deinit();
        if constexpr (platform::hasNetwork) { syncSock_.close(); syncOpen_ = false; }
        micSeat_.vacate();
        MoonModule::release();
    }

    /// The latest analyzed frame, always valid, so a consumer without a mic reads silence.
    const AudioFrame* audioFrame() const { return &frame_; }
    /// The frame effects read, writable so a test draws a state no source produces on the host.
    AudioFrame& frameForTest() { return frame_; }
    /// The analysis for the selected rate, from silence, as opening the input starts it.
    void resetAnalysisForTest() { if (openAnalysis()) resetAnalysis(); }
    /// The semitone bank's tuning from A440 in cents, which the retune sets.
    float tuningForTest() const { return harmonyState_ ? harmonyState_->tuningCents : 0.0f; }
    /// Whether the analysis and the harmony's state are held, which only an open input and `harmony` justify.
    bool analysisHeldForTest() const { return analysis_ != nullptr; }
    /// Whether the harmony's state is held.
    bool harmonyHeldForTest() const { return harmonyState_ != nullptr; }
    /// One block of input samples, left-justified 24-bit as the input delivers them, through the analysis the input runs: the host's seam for the analysis path, after resetAnalysisForTest.
    const AudioFrame& analyzeForTest(const int32_t (&block)[kBlock]) {
        if (harmony != (harmonyState_ != nullptr)) applyHarmony();   // the switch, as prepare applies it
        std::memcpy(analysis_->samples, block, sizeof(block));
        squelchNow_.store(squelch, std::memory_order_relaxed);
        analyzeBlock();
        return analysis_->work;
    }

    /// Whether the sync socket is open, a read-only view of the socket lifecycle so a test can assert it through the public tick.
    bool syncOpenForTest() const { return syncOpen_; }

    /// Whether a mic diagnosis is outstanding, which suppresses the sync line while it is.
    bool micStatusStaleForTest() const { return micStatusStale_; }
    /// Set the flag a local-mode diagnosis would have set, so leaving that mode can be tested anywhere.
    void setMicStatusStaleForTest(bool stale) { micStatusStale_ = stale; }
    /// The sync state as the card shows it, which is the reported state itself.
    const char* syncStatusForTest() const { return status() ? status() : ""; }
    /// How often a send may go out.
    static constexpr uint32_t syncSendIntervalMsForTest() { return kSyncSendIntervalMs; }
    /// How many sends have gone out.
    uint32_t syncSendCountForTest() const { return syncSendCount_; }
    /// How long a peer may be quiet before it is stale.
    static constexpr uint32_t syncFallbackMsForTest() { return kSyncFallbackMs; }
    /// How long a failed open waits.
    static constexpr uint32_t syncOpenRetryMsForTest() { return kSyncOpenRetryMs; }
    /// How many times the receive socket joined the group, for the tests.
    uint32_t syncJoinCountForTest() const { return syncSock_.joinCountForTest(); }

    /// What a consumer reads with no source: a class constant, since a function-local static needs a guard on first use.
    static constexpr AudioFrame kSilence{};
    /// The live frame every consumer reads, or silence where there is no source.
    static const AudioFrame* latestFrame() MM_NONBLOCKING {
        AudioService* a = ActiveInstance<AudioService>::active();
        return a ? &a->frame_ : &kSilence;
    }

    /// Produce this tick's frame, from the network, a synthesizer, or the local input.
    void tick() MM_NONBLOCKING override {
        // Re-claim an empty seat, so removing the active module lets a survivor take over.
        micSeat_.claim();

        // Receiving is a pure sink; sending produces the frame first, then sends it with the hits since the last packet.
        const Sync s = sync();
        const bool open = s != kSyncNone && syncEnsureSocket();   // lazy-open once the network is up
        if (s == kSyncReceive) {   // with no socket yet, still never runs the local mic
            if (open) syncReceive();
            return;
        }
        const bool fresh = produceFrame();
        if (open) {
            if (fresh) syncHits_.add(frame_);
            syncSend();
        }
    }

    /// This tick's frame from the synthesizer or the local input, true when one is new.
    bool produceFrame() MM_NONBLOCKING {
        // A whole mode rather than a fill-in, so it always runs.
        if (mode == kSimMode) { if (!simFrameDue(platform::millis(), simPrevMs_)) return false; synthesizeFrame(simulate == kSimSweep); return true; }

        // From here it is the local mode, which holds the last frame until an input is up.
        if (!platform::hasAudioInput || !inited_) return false;

        // The audio task reads and analyzes; a board without tasks does it here. Either way the tick takes the frames through the ring.
        if (!analysis_) return false;
        squelchNow_.store(squelch, std::memory_order_relaxed);   // the setting, handed to whichever thread analyzes
        // Only once no task loop runs: a stop that timed out leaves the loop reading with the handle already gone.
        if (!audioTask_.impl && audioLoopExited_.load(std::memory_order_acquire)) readAndAnalyze(0);
        return takeNewest(analysis_->frames, frame_);
    }

    /// Drain what the input holds, a block analyzed each time one fills, so no sample overflows the driver between reads: `waitMs` 0 in the tick, a wait in the audio task.
    void readAndAnalyze(uint32_t waitMs) {
        Analysis& a = *analysis_;
        for (uint8_t blocks = 0; blocks < kMaxBlocksPerTick;) {
            const size_t want = kBlock - a.filled;
            const size_t n = platform::audioMicRead(mic_, a.samples + a.filled, want, waitMs);
            // Read before the filter rewrites the samples.
            micSamples1s_.fetch_add(static_cast<uint32_t>(n), std::memory_order_relaxed);
            for (size_t i = 0; i < n; i++) if (a.samples[a.filled + i] != 0) { micNonzero1s_.fetch_add(1, std::memory_order_relaxed); break; }
            a.filled += n;
            if (a.filled == kBlock) {
                a.filled = 0;
                analyzeBlock();
                a.frames.push(&a.work, 1);   // a full ring drops the newest, the tick reading the ones before
                blocks++;
            }
            if (n < want) return;   // the input holds no more
            waitMs = 0;             // only the first read waits
        }
    }

    /// The block's harmony from the semitone bank into the frame, or every harmony field 0 with `harmony` off.
    void analyzeHarmony() {
        AudioFrame& w = analysis_->work;
        if (!harmonyState_) {
            std::memset(w.chroma, 0, sizeof(w.chroma));
            std::memset(w.notesHits, 0, sizeof(w.notesHits));
            w.harmonyChange = false;
            w.notesCount = w.key = w.bassNote = 0;
            w.tuning = 0;
            return;
        }
        Harmony& h = *harmonyState_;
        const AudioDynamics& dyn = analysis_->dyn;
        const float dtMs = analysis_->blockMs;
        w.harmonyChange = h.tracker.update(h.semitones, dyn.gateOpen, dtMs, w.chroma);
        w.notesCount = semitonesToNotes(h.semitones, dyn, w.notes);
        h.noteOnsets.take(w.notesHits, dyn.gateOpen);
        w.key = h.key.update(w.chroma, dyn.gateOpen, dtMs);
        w.bassNote = semitonesToBassNote(h.semitones, dyn.gateOpen);
        w.tuning = static_cast<int8_t>(std::lround(h.tuningCents));
        h.retune(h.tuning.update(h.semitones, dyn.gateOpen, dtMs), analysis_->openedRate, dtMs);
    }

    /// The most blocks one tick analyzes: 48 kHz at 24 frames a second, bounding a slow tick's work.
    static constexpr uint8_t kMaxBlocksPerTick = 4;

    /// One full block through the high-pass, the level and the bank into the analyzed frame, on whichever thread reads.
    void analyzeBlock() {
        Analysis& a = *analysis_;
        AudioFrame& w = a.work;
        a.dyn.squelchDb = static_cast<float>(squelchNow_.load(std::memory_order_relaxed));
        // The block's time is the stream's, its samples counted, so a block analyzed late or faster than real time keeps its own: @xref{the-streams-own-clock}.
        const uint64_t blockStart = a.streamSamples;
        a.streamSamples += kBlock;
        // Removes the offset before any analysis, continuous across blocks.
        a.dc.process(a.samples, kBlock, a.dcPole);
        const float levelDb = blockLevelDbFs(a.samples, kBlock);

        // The bands, through the window the level's dynamics set.
        const uint32_t bankStart = platform::micros();
        for (size_t at = 0; at < kBlock; at += ThirdOctaveBank::kChunk) {   // the bank's own chunk at a time, so the floats stay off the stack's budget
            float part[ThirdOctaveBank::kChunk];
            for (size_t i = 0; i < ThirdOctaveBank::kChunk; i++) part[i] = static_cast<float>(a.samples[at + i] >> 8);
            a.bank.process(part);
            if (harmonyState_) harmonyState_->semitones.process(part);
            const uint32_t chunkEndMs = static_cast<uint32_t>((blockStart + at + ThirdOctaveBank::kChunk) * 1000u / a.openedRate);
            // The onsets read the bank between chunks, every 5 ms of the stream's time, as AudioOnsets' notes say.
            a.onsets.update(a.bank, chunkEndMs);
            if (harmonyState_) harmonyState_->noteOnsets.update(harmonyState_->semitones, chunkEndMs);
        }
        bankUs1s_.fetch_add(platform::micros() - bankStart, std::memory_order_relaxed);
        uint16_t peakHz = 0, peakMag = 0;
        w.thirdsCount = bankToBands(a.bank, levelDb, a.blockMs, a.dyn, w.thirds, w.bands, peakHz, peakMag);
        w.level = a.dyn.levelToByte(levelDb);
        w.gate = a.dyn.gateToByte();
        w.gateOpen = a.dyn.gateOpen;
        a.onsets.take(w, a.dyn.gateOpen);   // the block's own gate, decided by its own level
        analyzeHarmony();
        w.seq++;
        smoothLevel(w);
        finishBands(w);
        levelDbX10_.store(static_cast<int16_t>(a.dyn.levelDb * 10.0f), std::memory_order_relaxed);
        floorDbX10_.store(static_cast<int16_t>(a.dyn.floorDb * 10.0f), std::memory_order_relaxed);
        topDbX10_.store(static_cast<int16_t>(a.dyn.topDb * 10.0f), std::memory_order_relaxed);
        gateOpen_.store(a.dyn.gateOpen, std::memory_order_relaxed);

        // Held when there is no real signal, so it does not wander in silence.
        if (peakMag > 8) { w.peakHz = peakHz; w.peakMag = peakMag; }

        // The window's peak, since sampling the instantaneous value once a second lands in the gaps.
        if (w.level > levelPeak_.load(std::memory_order_relaxed)) levelPeak_.store(static_cast<uint8_t>(w.level > 255 ? 255 : w.level), std::memory_order_relaxed);
    }

    /// Fill frame_ with a synthesized signal, on the wall clock that is its time.
    void synthesizeFrame(bool sweep) {
        const uint32_t now = platform::millis();
        simulateAudio(frame_, now, sweep, simPrevMs_);
        simPrevMs_ = simPrevMs_ != 0 && now - simPrevMs_ < 4 * kSimFrameMs ? simPrevMs_ + kSimFrameMs : now;   // on the block grid, so a slow render still gets a frame a block; a long pause resumes
        frame_.seq++;
        smoothLevel(frame_);
        finishBands(frame_);
        // Feed the same 1 s peak-hold the mic path uses, so the "level RMS" readout tracks the simulation too.
        if (frame_.level > levelPeak_.load(std::memory_order_relaxed)) levelPeak_.store(static_cast<uint8_t>(frame_.level > 255 ? 255 : frame_.level), std::memory_order_relaxed);
    }

    /// Reopen the mic when its bus frees or moves, publish the second's level, onset and peak readouts, and diagnose a silent mic.
    void tick1s() MM_NONBLOCKING override {
        // Re-init when the shared I2S bus came free, as the LED driver retries, or when the I2C bus moved under the codec.
        const bool micFreed = !inited_ && platform::audioMicSharedBusFree(micMode == kMicPdm ? platform::MicMode::Pdm
                                                                                       : platform::MicMode::I2sStd);
        const bool busMoved = codec != kCodecNone && platform::i2cBusGeneration() != codecBusGen_;
        if (mode == kLocalMode && (micFreed || busMoved || reinitPending_)) reinit();
        // The second's tallies, each taken and cleared at once, since the audio task adds to them meanwhile.
        const uint32_t micSamples = micSamples1s_.exchange(0, std::memory_order_relaxed);
        const uint32_t micNonzero = micNonzero1s_.exchange(0, std::memory_order_relaxed);
        const uint32_t bankUs = bankUs1s_.exchange(0, std::memory_order_relaxed);
        mm::formatTo(levelStr_, sizeof(levelStr_), "%u", static_cast<unsigned>(levelPeak_.exchange(0, std::memory_order_relaxed)));
        mm::formatTo(onsetStr_, sizeof(onsetStr_), "%u/s, flux %u",
                      static_cast<unsigned>(onsetCount_.exchange(0, std::memory_order_relaxed)),
                      static_cast<unsigned>(fluxPeak_.exchange(0, std::memory_order_relaxed)));
        mm::formatTo(peakStr_, sizeof(peakStr_), "%u Hz", static_cast<unsigned>(frame_.peakHz));
        char names[12];
        formatNotes(frame_.chroma, names, sizeof(names));
        mm::formatTo(notesStr_, sizeof(notesStr_), "%s, changes %u/s", names,
                     static_cast<unsigned>(harmonyChanges_.exchange(0, std::memory_order_relaxed)));
        char keyName[12];
        formatKey(frame_.key, keyName, sizeof(keyName));
        mm::formatTo(keyStr_, sizeof(keyStr_), "%s, tuned %+d cents", keyName, static_cast<int>(frame_.tuning));   // the frame's, so a receiver shows the sender's
        mm::formatTo(rangesStr_, sizeof(rangesStr_), "bass %u, mid %u, treble %u", static_cast<unsigned>(frame_.bass),
                     static_cast<unsigned>(frame_.mid), static_cast<unsigned>(frame_.treble));
        mm::formatTo(dynStr_, sizeof(dynStr_), "level %d, floor %d, top %d dB, %s",
                     levelDbX10_.load(std::memory_order_relaxed) / 10, floorDbX10_.load(std::memory_order_relaxed) / 10,
                     topDbX10_.load(std::memory_order_relaxed) / 10, gateOpen_.load(std::memory_order_relaxed) ? "open" : "gated");
        // The bands' microseconds this second, ten thousand of them a percent of one core.
        mm::formatTo(cpuStr_, sizeof(cpuStr_), "bands %u.%u%%, %u Hz",
                     static_cast<unsigned>(bankUs / 10000), static_cast<unsigned>(bankUs / 1000 % 10),
                     static_cast<unsigned>(micSamples));   // the samples the input delivered this second, its real rate
        refreshNamedHosts(hosts, syncHosts_, syncHostCount_);

        // Mic-health diagnosis from the 1 s tallies (see the read path).
        const bool directMicLive = platform::hasI2sMic && inited_ && mode == kLocalMode
                                   && codecType() == platform::CodecType::None;
        if (directMicLive) {
            if (micSamples == 0)
                setStatus(micMode == kMicPdm ? "mic: no samples, check wsPin (PDM clock)"
                                       : "mic: no samples, check sckPin / wsPin (I2S clocks)",
                          Severity::Warning);
            else if (micNonzero == 0)
                setStatus("mic: data line silent, check sdPin (SD/DOUT) + mic power", Severity::Warning);
            else if (micStatusStale_)
                setStatus("", Severity::Status);   // data flowing again, clear a prior diagnosis
            micStatusStale_ = (micSamples == 0 || micNonzero == 0);
        } else if (mode != kLocalMode) {
            // No mic to diagnose on this path (Receive or Simulate), so no diagnosis may be OUTSTANDING.
            micStatusStale_ = false;
        }
        // Live sync state on the module's OWN status line:
        if constexpr (platform::hasNetwork) {
            const Sync s = sync();
            if (s != kSyncNone && syncOpen_ && !micStatusStale_) {
                if (s == kSyncSend) setStatus("sending");
                else if (lastSyncRecv_ != 0
                         && platform::millis() - lastSyncRecv_ < kSyncFallbackMs) {
                    // Named, because "receiving" alone cannot tell a rig taking the right source from one locked.
                    char peer[HostResolver::kPeerChars];
                    HostResolver::formatPeer(peer, sizeof(peer), syncPeer_);
                    mm::formatTo(syncStr_, sizeof(syncStr_), "receiving %s from %s", syncRxWled_ ? "WLED" : "MoonLight", peer);
                    setStatus(syncStr_);
                } else {
                    reportListening();
                }
            }
        }
        MoonModule::tick1s();
    }

private:
    /// The one-active-mic election.
    ActiveInstance<AudioService> micSeat_{*this};

    platform::AudioMicHandle mic_;
    bool inited_ = false;
    /// What the microphone is opened with, so a prepare that changed none of it keeps it open.
    struct MicConfig {
        uint8_t device, micMode, sampleRateSel, codec, codecAddr, codecGain;
        int8_t sck, ws, sd, mclk;
        bool operator==(const MicConfig&) const = default;
    };
    MicConfig micConfig() const { return {device, micMode, sampleRateSel, codec, codecAddr, codecGain, sckPin, wsPin, sdPin, mclkPin}; }
    MicConfig openedWith_{};
    /// What the sync socket is set up for, so a prepare that changed none of it keeps it bound.
    struct SyncConfig {
        uint8_t mode, addressing;
        bool send;
        uint16_t port;
        uint32_t hosts;
        bool operator==(const SyncConfig&) const = default;
    };
    SyncConfig syncConfig() const { return {mode, addressing, send, syncPort, fnv1a(hosts, std::strlen(hosts))}; }
    SyncConfig syncedWith_{};
    bool syncSet_ = false;
    uint32_t codecBusGen_ = 0;   ///< the bus generation the codec last attached to, so a moved bus re-attaches it
    bool reinitPending_ = false;   ///< a reinit that a still-running task stopped, retried each second and on the next prepare
    using Analysis = AudioAnalysis;
    using Harmony = AudioHarmonyState;
    std::unique_ptr<Analysis> analysis_;        // held only while an input is open: @xref{memory-follows-the-mode}
    std::unique_ptr<Harmony> harmonyState_;     // and this only while `harmony` is on
    bool harmonyApplied_ = false;               // the `harmony` the state was last applied for, held or not for want of RAM
    platform::WorkerTask audioTask_;     // reads and analyzes, where the platform runs tasks
    std::atomic<bool> audioStop_{false};
    std::atomic<bool> audioLoopExited_{true};   // the loop's last act, so a stop that timed out never frees the input under it
    std::atomic<uint8_t> squelchNow_{10};   // the setting as the tick last saw it, for the task
    std::atomic<uint8_t> harmonyChanges_{0};   // the harmony's changes in the current 1 s window, for the readout
    std::atomic<int16_t> levelDbX10_{-1200}, floorDbX10_{-1200}, topDbX10_{-1200};   // the dynamics in tenths of a dB, for the readout
    std::atomic<bool> gateOpen_{false};

    AudioFrame frame_;

    char levelStr_[12] = {};
    char cpuStr_[40] = {};
    char dynStr_[48] = {};
    std::atomic<uint32_t> bankUs1s_{0};   ///< microseconds the bands took in the current second, added by the audio task (UI only)
    char onsetStr_[20] = {};
    std::atomic<uint8_t> onsetCount_{0};  ///< onsets in the current 1 s display window (UI only)
    std::atomic<uint8_t> fluxPeak_{0};    ///< peak flux in that window (UI only)
    char peakStr_[12] = {};
    char notesStr_[32] = {};   // all twelve names fit
    char keyStr_[32] = {};
    char rangesStr_[32] = {};
    std::atomic<uint8_t> levelPeak_{0};   // peak level across the current 1 s display window (UI only)

    /// Mic-health tallies over the 1 s window (see the read path and the tick1s diagnosis).
    std::atomic<uint32_t> micSamples1s_{0};
    std::atomic<uint32_t> micNonzero1s_{0};
    bool     micStatusStale_ = false;

    /// The audio sync socket, either format: one, bound only in Send/Receive.
    platform::UdpSocket syncSock_;
    uint32_t lastSyncSend_ = 0;      // millis of the last send (send throttle)
    uint32_t syncSendCount_ = 0;     // sends made; test-visible so the throttle can be observed
    AudioHits syncHits_;             // the hits of the frames since the last packet, which a throttled send would drop
    AudioFrame syncFrame_;           // the frame a packet is parsed into or built from, a member since it and the datagram together would pass the stack's budget
    AudioHits syncRxHits_;           // the hits of the packets since the last frame taken, late ones among them
    uint16_t syncRxSeq_ = 0;         // the last MoonLight frame number taken, so a late or repeated one is dropped
    bool     syncRxSeqValid_ = false;   // whether syncRxSeq_ holds one, false until the first and after a silence
    bool     syncRxWled_ = false;    // whether the last frame taken came in WLED's format, for the status line
    uint32_t simPrevMs_ = 0;             ///< the last simulated frame's time, so the song's kick reads once
    uint32_t lastSyncRecv_ = 0;      // millis of the last received packet (receive auto-blend)
    uint32_t syncMlHeardMs_ = 0;     // millis of the last MoonLight packet, which outranks WLED's while it arrives
    uint8_t  syncPeer_[4] = {};      // source address of that packet, for the status line
    bool     syncOpen_ = false;      // socket opened for the current mode (lazy-open latch)

    /// Where a send goes, or why a unicast send reaches nobody.
    void reportSending() {
        if (addressing == kAddressUnicast && syncHostsError_) setStatus(syncHostsError_, Severity::Error);
        else if (addressing == kAddressUnicast && syncHostCount_ == 0) setStatus("set hosts to send to", Severity::Warning);
        else setStatus("sending");
    }

    /// Report an open receive socket, saying so when only unicast can reach it.
    void reportListening() {
        const uint8_t joined = syncSock_.multicastJoined();
        if (joined == kSyncGroupCount) setStatus("listening");
        else setStatus(joined ? "listening, one format's multicast group refused" : "listening, unicast only: multicast unavailable", Severity::Warning);
    }
    uint32_t lastSyncOpenFailMs_ = 0;  // millis of the last failed open (0 = none); bring-up backoff
    char     syncStr_[sizeof("receiving MoonLight from ") + HostResolver::kPeerChars] = {};   // "receiving <format> from <board> (<ip>)"
    static constexpr uint32_t kSyncSendIntervalMs = 25;   // ~40/s, WLED-friendly, well under a flood
    static constexpr uint32_t kSyncFallbackMs = 1000;     // no packet this long → resume local mic
    static constexpr uint32_t kSyncOpenRetryMs = 1000;    // pause between socket bring-up retries after a failure
    static constexpr int kSyncMaxRecvPerTick = 8;         // bounded non-blocking drain (sync is low-rate)
    static constexpr uint16_t kSyncSeqWindow = 64;        // how far behind the last MoonLight frame number is late, not a restarted sender

    static constexpr const char* kInitFailMsg = "mic init failed, check pins / rate";

    /// (Re)create the I2S channel for the current pins + rate.
    void reinit() {
        reinitPending_ = false;
        codecBusGen_ = platform::i2cBusGeneration();   // every path through here has seen this bus, so tick1s waits for the next change
        openedWith_ = micConfig();                     // the settings this attempt opens with
        if constexpr (platform::hasAudioCapture) {
            // Desktop:
            if (!deinit()) { reinitPending_ = true; return; }
            inited_ = platform::audioCaptureInit(mic_, device, sampleRate());
            if (!inited_) {
                setStatus("capture init failed, pick another device", Severity::Error);
                return;
            }
            openStream();
            return;
        }
        if constexpr (!platform::hasI2sMic) {
            setStatus("mic: no audio input on this platform", Severity::Warning);
            return;
        }
        if (!deinit()) { reinitPending_ = true; return; }
        if (const char* missing = micNotReady()) { setStatus(missing, Severity::Status); return; }
        // Bring up the I2S channel FIRST.
        inited_ = platform::audioMicInit(mic_, static_cast<uint16_t>(wsPin),
                                         static_cast<uint16_t>(sdPin),
                                         static_cast<uint16_t>(sckPin), mclkPin, sampleRate(),
                                         static_cast<platform::MicMode>(micMode));
        if (!inited_) {
            setStatus(kInitFailMsg, Severity::Error);
            return;
        }
        // Now configure the codec over I2C (MCLK is running).
        if (!platform::audioCodecInit(codecType(), codecAddr, sampleRate(), codecGain)) {
            deinit();   // tear the I2S channel back down, we couldn't bring the codec up
            setStatus("mic: codec init failed, check I2C wiring", Severity::Error);
            return;
        }
        openStream();   // the INMP441 emits ~250 ms of power-on settling garbage after the clock starts
    }

    /// A newly opened input: its analysis allocated and reset for the stream, and the task started.
    void openStream() {
        if (!openAnalysis()) return;
        resetAnalysis();   // start the high-pass and the bank clean for the new stream
        startAudioTask();
        clearStatus();
        settleHarmony();
    }

    /// Remember the `harmony` the state was applied for, so a board with no room for it tries once per switch rather than on every prepare, and say so.
    void settleHarmony() {
        harmonyApplied_ = harmony;
        if (harmony && !harmonyState_) setStatus("audio: no internal RAM for the harmony, running without it", Severity::Warning);
    }

    /// What the wiring still lacks before the mic can open, or nullptr when nothing does.
    const char* micNotReady() const {
        // Any pin unset (-1, the default until the user wires a mic):
        const bool pdm = micMode == kMicPdm;
        if (wsPin < 0 || sdPin < 0 || (!pdm && sckPin < 0))
            return pdm ? "mic: set wsPin (clock) / sdPin (data)" : "mic: set sckPin / wsPin / sdPin";
        // A codec without the board's bus would probe nothing and fail the whole microphone, so it says so first.
        if (codec != kCodecNone && !platform::i2cBusReady()) return "mic: the codec needs the I2C bus: set its pins under System, I2cBus";
        return nullptr;
    }

    /// Read and analyze on a task of its own, a blocking read of the input, so the analysis keeps the input's pace whatever the frame rate and leaves the render core its time: @xref{the-audio-task}.
    void startAudioTask() {
        squelchNow_.store(squelch, std::memory_order_relaxed);
        audioStop_.store(false, std::memory_order_release);
        audioLoopExited_.store(false, std::memory_order_release);
        // Priority 6, over the frame encoder's 5 and under the network's 7: a lost sample is lost for good, a late frame only late. Under the encoder a busy core starved it to a third of its samples.
        if (!platform::spawnPinnedTask(audioTask_, "mmAudio", &audioTrampoline, this, 4096, 6, 1)) {
            audioTask_.impl = nullptr;
            audioLoopExited_.store(true, std::memory_order_release);
        }
    }

    /// Stop the task, true once its loop is gone: a platform that detaches a task after a timeout leaves it owning the input and the analysis.
    bool stopAudioTask() {
        if (audioTask_.impl) {
            audioStop_.store(true, std::memory_order_release);
            platform::stopPinnedTask(audioTask_);
            audioTask_.impl = nullptr;
        }
        if (!audioLoopExited_.load(std::memory_order_acquire)) return false;
        AudioFrame drop;
        if (analysis_) while (analysis_->frames.pop(&drop, 1) == 1) {}   // a frame the tick never took belongs to the stopped stream
        return true;
    }

    static void audioTrampoline(void* self) { static_cast<AudioService*>(self)->runAudioLoop(); }

    /// Block on the input and analyze each block as it fills, until stopAudioTask signals.
    void runAudioLoop() {
        while (!audioStop_.load(std::memory_order_acquire)) readAndAnalyze(kReadWaitMs);
        audioLoopExited_.store(true, std::memory_order_release);
    }
    static constexpr uint32_t kReadWaitMs = 20;   // a stop is seen within this, and a block at 22,050 Hz arrives within it

    /// The analysis and the harmony for the selected rate, from silence.
    void resetAnalysis() {
        analysis_->reset(sampleRate());
        if (harmonyState_) resetHarmony();
    }

    /// Allocate the analysis for an opening input, and the harmony's state if `harmony` is on; false, with the card saying so, when internal RAM has no room for the analysis: @xref{memory-follows-the-mode}.
    bool openAnalysis() {
        if (!analysis_) analysis_.reset(new (std::nothrow) Analysis());
        if (!analysis_) {
            setStatus("audio: no internal RAM for the analysis", Severity::Error);
            return false;
        }
        // Allocated only: resetAnalysis designs it at the opened rate. Without room it runs as harmony off, and openStream says so.
        if (!harmony) harmonyState_.reset();
        else if (!harmonyState_) harmonyState_.reset(new (std::nothrow) Harmony());
        return true;
    }

    /// The `harmony` switch alone: the task pauses while the state changes hands, and a task that will not stop leaves it for the retry each second.
    void switchHarmony() {
        if (!stopAudioTask()) {
            setStatus("audio task did not stop: the input stays open until it does", Severity::Error);
            reinitPending_ = true;
            return;
        }
        applyHarmony();
        startAudioTask();
        settleHarmony();
    }

    /// Hold the harmony's state exactly while `harmony` is on, with the analysis's task stopped or absent.
    void applyHarmony() {
        if (!harmony) { harmonyState_.reset(); return; }
        if (harmonyState_) return;
        harmonyState_.reset(new (std::nothrow) Harmony());
        if (harmonyState_ && analysis_) resetHarmony();
    }

    /// The harmony from silence, at the opened rate and on A440.
    void resetHarmony() {
        harmonyState_->reset(analysis_->openedRate);
    }

    /// Free the input and clear the analysis, false while a task that would not stop still reads them.
    bool deinit() {
        if constexpr (!platform::hasAudioInput) return true;
        if (!stopAudioTask()) {   // before the input it reads goes
            setStatus("audio task did not stop: the input stays open until it does", Severity::Error);
            return false;
        }
        // The codec attaches only once this instance's channel is up, so an instance that never started one, such as a type probe, leaves the live codec alone.
        if (inited_) {
            platform::audioMicDeinit(mic_);
            platform::audioCodecDeinit();
        }
        inited_ = false;
        analysis_.reset();      // the stopped task held the last reference: @xref{memory-follows-the-mode}
        harmonyState_.reset();
        // Publish silence:
        frame_ = AudioFrame{};
        // The ANALYSIS history goes with it, so a restarted source begins from a DEFINED state rather.
        std::memset(prevBands_, 0, sizeof(prevBands_));
        return true;
    }

    // --- WLED audio sync (guarded: only compiled where platform::hasNetwork) ---

    /// Reset the sync socket to the current mode.
    void syncReinit() {
        if constexpr (!platform::hasNetwork) return;
        syncSock_.close();                 // syncEnsureSocket() re-opens per mode when the net is up
        syncOpen_ = false;
        lastSyncOpenFailMs_ = 0;           // a mode change retries bring-up immediately (no stale backoff)
        lastSyncRecv_ = syncMlHeardMs_ = 0;
        std::memset(syncPeer_, 0, sizeof(syncPeer_));
        syncHits_ = {};
        syncRxHits_ = {};
        syncRxSeqValid_ = false;
        const Sync s = sync();
        // Only when there IS a socket to wait for.
        if (s == kSyncSend)         setStatus("send: waiting for network");
        else if (s == kSyncReceive) setStatus("receive: waiting for network");
        else if (isSyncStatus(status())) setStatus("");
    }

    /// Whether the status line holds one of the sync's own messages, which a mode without a socket clears.
    static bool isSyncStatus(const char* s) {
        static constexpr const char* kOwn[] = {"waiting for network", "socket failed", "bind failed", "from ", "listening", "sending", "set hosts"};
        if (!s) return false;
        for (const char* k : kOwn) if (std::strstr(s, k)) return true;
        return false;
    }

    /// Lazily open the sync socket for the current mode, once the network stack is up.
    bool syncEnsureSocket() {
        if constexpr (!platform::hasNetwork) return false;
        const Sync s = sync();
        if (s == kSyncNone) return false;
        if (syncOpen_) return true;
        if (!platform::networkReady()) return false;   // interface not up yet, try again next tick
        // Back off between failed bring-ups:
        const uint32_t now = platform::millis();
        if (lastSyncOpenFailMs_ != 0 && now - lastSyncOpenFailMs_ < kSyncOpenRetryMs) return false;
        // Send → each format's group, or the hosts; receive → bind, then join both groups. A failed join degrades rather than refuses, since unicast still arrives.
        syncOpen_ = syncSock_.open() && (s == kSyncSend || syncSock_.bind(syncPort));
        if (!syncOpen_) {
            syncSock_.close();
            setStatus(s == kSyncSend ? "send: socket failed" : "receive: bind failed", Severity::Error);
        } else if (s == kSyncSend) {
            reportSending();
        } else {
            syncSock_.setMulticastGroups(kSyncGroups_, kSyncGroupCount);
            reportListening();
        }
        // Stamp a failure (or clear the timer on success).
        lastSyncOpenFailMs_ = syncOpen_ ? 0 : (now == 0 ? 1 : now);
        return syncOpen_;
    }

    /// Send the current frame in `syncFormat`, with the hits since the last packet, throttled to ~40/s: @xref{audio-sync-one-packet-a-frame}.
    void syncSend() {
        if constexpr (!platform::hasNetwork) return;
        const uint32_t now = platform::millis();
        if (now - lastSyncSend_ < kSyncSendIntervalMs) return;
        lastSyncSend_ = now;
        syncFrame_ = frame_;
        syncHits_.applyTo(syncFrame_);
        syncHits_ = {};
        uint8_t pkt[kMlSyncMaxPacket];
        if (syncFormat != kFormatWled) sendSyncPacket(pkt, buildMoonLightAudioSync(pkt, syncFrame_), kSyncGroups_[kFormatMoonLight].ip);
        if (syncFormat != kFormatMoonLight) sendSyncPacket(pkt, buildWledAudioSync(pkt, syncFrame_), kSyncGroups_[kFormatWled].ip);
        syncSendCount_++;
    }

    /// One packet to its format's group, or to the hosts. A steady 40 frames a second is a stream, so no broadcast copy on any network.
    void sendSyncPacket(const uint8_t* pkt, size_t n, const uint8_t group[4]) {
        sendAddressed(addressing == kAddressUnicast ? Addressing::Unicast : Addressing::Multicast, Traffic::FrameRate,
                      syncHosts_, syncHostCount_, group, [&](const uint8_t ip[4]) { syncSock_.sendToAddr(ip, syncPort, pkt, n); });
    }

    /// Drain the sync socket (bounded, non-blocking) in Receive mode, either format, into one new frame with the hits of every packet.
    void syncReceive() {
        if constexpr (!platform::hasNetwork) return;
        if (lastSyncRecv_ != 0 && platform::millis() - lastSyncRecv_ >= kSyncFallbackMs) syncRxSeqValid_ = false;   // a silent sender may come back restarted
        uint8_t pkt[kMlSyncMaxDatagram];     // what the format allows, so a later version's sections arrive whole
        uint8_t srcIp[4] = {};
        const uint16_t seq = static_cast<uint16_t>(frame_.seq + 1);   // the receiver numbers its frames, so a consumer reading one on two ticks takes its hits once
        bool fresh = false, smoothHere = false;
        for (int i = 0; i < kSyncMaxRecvPerTick; i++) {
            const int n = syncSock_.recvFrom(pkt, sizeof(pkt), srcIp);
            if (n <= 0) break;                     // -1 = nothing pending
            const uint16_t sections = parseMoonLightAudioSync(pkt, static_cast<size_t>(n), syncFrame_);
            const bool ml = sections != 0;
            if (!ml && (syncMlHeardMs_ != 0 && platform::millis() - syncMlHeardMs_ < kSyncFallbackMs)) continue;   // the same frame again, from a board sending both
            if (!ml && !parseWledAudioSync(pkt, static_cast<size_t>(n), syncFrame_)) continue;   // a v1 / foreign packet
            lastSyncRecv_ = platform::millis();
            if (ml) syncMlHeardMs_ = lastSyncRecv_;
            std::memcpy(syncPeer_, srcIp, sizeof(syncPeer_));   // whose audio this is
            const uint16_t behind = static_cast<uint16_t>(syncRxSeq_ - syncFrame_.seq);
            const bool late = ml && syncRxSeqValid_ && behind < kSyncSeqWindow;   // repeated, or late
            if (!late || behind != 0) syncRxHits_.add(syncFrame_);   // a late frame's hits went in no other packet, so the next frame carries them
            if (late) continue;
            if (ml) { syncRxSeq_ = syncFrame_.seq; syncRxSeqValid_ = true; }
            smoothHere = !(sections & (1u << kMlSectionSmoothed));   // WLED's packet, or a sender that does not smooth
            if (smoothHere) std::memcpy(syncFrame_.bandsSmoothed, frame_.bandsSmoothed, sizeof(syncFrame_.bandsSmoothed));   // the ballistic run here lives across packets
            frame_ = syncFrame_;
            syncRxWled_ = !ml;
            fresh = true;
        }
        if (!fresh) return;
        syncRxHits_.applyTo(frame_);
        syncRxHits_ = {};
        if (smoothHere) finishBands(frame_);
        else deriveFromBands(frame_);              // the sender's smoothed bands kept, so every board shows the same frame
        frame_.seq = seq;
        // Feed the peer level into the same 1 s peak window the local mic uses, so the "level RMS".
        if (frame_.level > levelPeak_.load(std::memory_order_relaxed))
            levelPeak_.store(static_cast<uint8_t>(frame_.level > 255 ? 255 : frame_.level), std::memory_order_relaxed);
    }

    /// Each format's multicast group, in `SyncFormat` order: MoonLight's own, and WLED's, which every WLED device joins.
    static constexpr platform::UdpSocket::MulticastGroup kSyncGroups_[] = {{{239, 0, 0, 2}}, {{239, 0, 0, 1}}};
    static constexpr uint8_t kSyncGroupCount = sizeof(kSyncGroups_) / sizeof(kSyncGroups_[0]);

    /// How many boards a unicast audio send reaches.
    static constexpr uint8_t kMaxSyncHosts = 8;
    Host     syncHosts_[kMaxSyncHosts] = {};
    uint8_t  syncHostCount_ = 0;
    const char* syncHostsError_ = nullptr;   ///< why the host list was refused, shown on the status

    /// Parse the hosts, starting the name lookups here where allocating is allowed.
    void parseSyncHosts() {
        syncHostsError_ = parseHostList(hosts, syncHosts_, kMaxSyncHosts, syncHostCount_);
        if (syncHostsError_) { syncHostCount_ = 0; return; }
        startResolverForNames(syncHosts_, syncHostCount_);
        refreshNamedHosts(hosts, syncHosts_, syncHostCount_);
    }
};

} // namespace mm
