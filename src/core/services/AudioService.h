#pragma once

#include "core/module/MoonModule.h"
#include "core/util/ActiveInstance.h"   // the one-active-mic election (the seat + its RAII vacate)
#include "core/util/AudioFrame.h"
#include "core/services/AudioLevel.h"
#include "core/services/AudioBands.h"
#include "core/util/math8.h"      // beatsin8 / sin8, the simulated-audio oscillators
#include "light/util/WLEDAudioSyncPacket.h"   // WLED audio-sync wire format (send/receive)
#include "platform/platform.h"

#include <cstdint>
#include <cstdio>   // snprintf for the read-out strings
#include <cstring>

namespace mm {

/// Acquires an audio source and publishes a frame: a level, sixteen bands, and the peak.
///
/// The producer half of the audio-reactive pipeline, which every audio effect reads.
/// Named for what it does, acquisition plus analysis, rather than for one source.
/// @card AudioService.png
///
/// @moreinfo
///
/// ## The modes run simple to advanced
///
/// Simulate synthesizes a signal for a demo or a test. Receive is a sink a peer drives.
/// Local runs the device's own input and analyzes it here.
/// A device is exactly one of these, and changing it acquires or releases hardware live.
///
/// They are ordered by what each one needs: simulate needs nothing, receive needs a network, local needs a part wired to pins.
/// So the default is the first entry rather than an index that depended on the platform, and a device demonstrates sound before anything is attached to it.
///
/// ## The pipeline
///
/// Each block: read, block the offset, measure the level, window, transform, map to bands.
/// The high-pass conditions the block once, so both halves see the same cleaned signal.
/// A block outlasts a tick, so a tick completing none re-publishes the last frame.
/// The level is measured independently of the bands.
///
/// Only the read and the transform are platform code; the rest is host-tested maths.
/// The scratch is fixed and resident, so a block allocates nothing.
class AudioService : public MoonModule {
public:
    /// Finish a block of raw bands:
    void finishBands() {
        smoothBands(frame_.bands, frame_.bandsSmoothed);
        frame_.flux = spectralFlux(prevBands_, frame_.bands);
        std::memcpy(prevBands_, frame_.bands, sizeof(prevBands_));
        frame_.onset = onset_.feed(frame_.flux, platform::millis()) ? frame_.flux : 0;
        if (frame_.onset) onsetCount_++;
        if (frame_.flux > fluxPeak_) fluxPeak_ = frame_.flux;
    }
    uint8_t       prevBands_[16] = {};   ///< the previous block's bands, the flux's reference
    BandConditioner cond_;               ///< the per-band floor and peak tables, learned live
    LevelConditioner levelCond_;         ///< the same, for the overall level (VU) in automatic mode
    OnsetDetector onset_;                ///< the hit decision, with its running mean and refractory

    /// The block and transform size, a power of two, around twenty milliseconds of audio.
    static constexpr size_t kBlock = 512;
    /// How many magnitude bins that produces.
    static constexpr size_t kMag = kBlock / 2;

    /// A service, so the container accepts it as a child.
    ModuleRole role() const MM_NONBLOCKING override { return ModuleRole::Service; }

    // It respects the enabled flag, unlike a free diagnostic:

    // The pins default to unset, so adding the module claims no GPIO until a user wires one.
    /// Which capture device to open, where the host offers a choice.
    uint8_t device = 0;
    /// Which kind of microphone is wired, the two-wire kind having no clocks to set.
    uint8_t micMode = 0;
    int8_t sckPin = -1;          ///< the bit clock, or -1 while unset
    int8_t wsPin = -1;           ///< the word select, or -1 while unset
    int8_t sdPin = -1;           ///< the data line, or -1 while unset
    /// The master clock a converter may need, a self-clocked part leaving it unset.
    int8_t mclkPin = -1;
    /// Which of the standard rates to run at, a choice rather than a free number.
    uint8_t  sampleRateSel = 2;
    /// The silence threshold: below it a band reads as nothing.
    uint8_t  floor = 100;
    /// The window's width, a higher value running the display hotter.
    uint8_t  gain = 128;
    /// Who sets the display window: the two sliders, or the learner.
    uint8_t  levels = 1;
    // Both act on the learned per-band range, already normalized per rig, so one value serves.
    /// How much of a band's deviation to correct, short of the ratios that pump.
    static constexpr uint8_t kRatio = 4;
    /// How far a band may be lifted, so a silent one is never amplified into its own noise.
    static constexpr uint8_t kMaxGainDb = 24;
    /// Which synthesized pattern to produce: a plausible song, or a deterministic march.
    uint8_t  simulate = 0;
    /// The source: a synthesized signal, the network, or its own input.
    uint8_t  mode = 0;
    /// A synthesized signal, so a device demonstrates sound before anything is wired to it.
    static constexpr uint8_t kSimMode = 0;
    /// Another device's analysis, over the network that has to exist to carry it.
    static constexpr uint8_t kReceiveMode = 1;
    /// This device's own microphone, which needs pins and a part to be connected to them.
    static constexpr uint8_t kLocalMode = platform::hasNetwork ? 2 : 1;
    /// Whether to broadcast the local analysis, which only the local mode can do.
    bool     send = false;
    /// What the sync machinery does:
    uint8_t  sync() const { return (platform::hasNetwork && mode == kReceiveMode) ? 2
                                 : (mode == kLocalMode && send ? 1 : 0); }
    /// The port both directions use, which must match on both ends.
    uint16_t syncPort = WLED_SYNC_PORT;

    /// The rates the selector offers.
    static constexpr uint16_t kSampleRates[] = {8000, 16000, 22050, 44100};
    /// How many there are.
    static constexpr uint8_t kSampleRateCount = 4;
    /// The selected rate in hertz.
    uint32_t sampleRate() const { return kSampleRates[sampleRateSel < kSampleRateCount
                                                       ? sampleRateSel : 2]; }

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
        if constexpr (platform::hasI2sMic) {
            // A two-wire part has neither clock, so showing them would invite dead settings.
            static constexpr const char* kMicModeOptions[] = {"I2S", "PDM"};
            controls_.addSelect("micMode", micMode, kMicModeOptions, 2);
            controls_.setHidden(controls_.count() - 1, !localMode);
            const bool pdm = micMode == 1;
            controls_.addPin("sckPin", sckPin);        controls_.setHidden(controls_.count() - 1, !localMode || pdm);
            controls_.addPin("wsPin", wsPin);          controls_.setHidden(controls_.count() - 1, !localMode);
            controls_.addPin("sdPin", sdPin);          controls_.setHidden(controls_.count() - 1, !localMode);
            controls_.addPin("mclkPin", mclkPin);      controls_.setHidden(controls_.count() - 1, !localMode || pdm);
        }
        if constexpr (platform::hasAudioCapture) {
            // Re-enumerated per rebuild, so a hot-plugged device appears on the next change.
            const char* const* deviceOptions = nullptr;
            const uint8_t deviceCount = static_cast<uint8_t>(platform::audioCaptureDevices(&deviceOptions));
            controls_.addSelect("device", device, deviceOptions, deviceCount);
            controls_.setPersistLabel(controls_.count() - 1);
            controls_.setHidden(controls_.count() - 1, !localMode);
        }
        static constexpr const char* kRateOptions[] = {"8000", "16000", "22050", "44100"};
        controls_.addSelect("sampleRate", sampleRateSel, kRateOptions, kSampleRateCount);
        controls_.setHidden(controls_.count() - 1, !localMode);
        // One decision, then the controls it needs:
        static constexpr const char* kLevelsOptions[] = {"manual", "automatic"};
        controls_.addSelect("levels", levels, kLevelsOptions, 2);
        controls_.setHidden(controls_.count() - 1, !localMode);
        const bool manual = levels == 0;
        // The threshold means one thing in both modes, so it shows in both:
        controls_.addControl("floor", floor, 0, 255); controls_.setHidden(controls_.count() - 1, !localMode);
        controls_.addControl("gain", gain, 1, 255);   controls_.setHidden(controls_.count() - 1, !localMode || !manual);
        // Automatic exposes no knobs: they changed the numbers and nothing a viewer could see.
        if constexpr (platform::hasNetwork) {
            controls_.addControl("send audio", send);
            controls_.setHidden(controls_.count() - 1, !localMode);
        }
        // The pattern picker, shown only in the synthesized mode.
        static constexpr const char* kSimulateOptions[] = {"music", "sweep"};
        controls_.addSelect("simulate", simulate, kSimulateOptions, 2);
        controls_.setHidden(controls_.count() - 1, !simMode);
        // Only where a socket is bound, the rows toggling live with the mode.
        if constexpr (platform::hasNetwork) {
            const bool hasSocket = (sync() != 0);
            controls_.addControl("syncPort", syncPort, 1, 65535);
            controls_.setHidden(controls_.count() - 1, !hasSocket);
        }
        // The audio driving the effects, so these stay visible in every mode.
        controls_.addReadOnly("level RMS", levelStr_, sizeof(levelStr_));
        // The one row that answers whether the detector is hearing the beat.
        controls_.addReadOnly("onsets", onsetStr_, sizeof(onsetStr_));
        controls_.addReadOnly("peakHz", peakStr_, sizeof(peakStr_));
        MoonModule::defineControls();
    }

    /// Which changes rebuild the channel, rebind the socket, or re-toggle the rows.
    bool affectsPrepare(const char* name) const override {
        return std::strcmp(name, "wsPin") == 0 || std::strcmp(name, "sdPin") == 0
            || std::strcmp(name, "sckPin") == 0 || std::strcmp(name, "mclkPin") == 0
            // Re-creates the channel, and hides the clocks the other kind does not have.
            || std::strcmp(name, "micMode") == 0
            || std::strcmp(name, "device") == 0
            || std::strcmp(name, "sampleRate") == 0 || std::strcmp(name, "mode") == 0
            || std::strcmp(name, "send audio") == 0 || std::strcmp(name, "syncPort") == 0
            // This swaps which sliders are shown, so it toggles rows as the mode does.
            || std::strcmp(name, "levels") == 0;
    }

    /// Claim the frame seat, then acquire only the hardware the current mode needs.
    void prepare() override {
        micSeat_.claim();       // the first live instance wins the seat, in any mode
        if (mode == kLocalMode) {
            reinit();           // only this mode runs a peripheral
        } else {
            deinit();           // the others free the channel and its pins
            clearStatus();      // a mic diagnosis would otherwise linger in a mode with no mic
        }
        syncReinit();
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

    // Read-only views of the socket lifecycle, so a test can assert it through the public tick.
    /// Whether the sync socket is open.
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

    /// The live frame every consumer reads, or silence where there is no source.
    static const AudioFrame* latestFrame() MM_NONBLOCKING {
        // Not a function-local static, which would need a guard and a lock on first use.
        static constexpr AudioFrame kSilence{};
        AudioService* a = ActiveInstance<AudioService>::active();
        return a ? &a->frame_ : &kSilence;
    }

    /// Produce this tick's frame, from the network, a synthesizer, or the local input.
    void tick() MM_NONBLOCKING override {
        // Re-claim an empty seat, so removing the active module lets a survivor take over.
        micSeat_.claim();

        // Sending broadcasts the frame and falls through to produce it; receiving is a pure sink.
        if constexpr (platform::hasNetwork) {
            const uint8_t s = sync();
            if (s != 0 && syncEnsureSocket()) {   // lazy-open once the network is up
                if (s == 1) syncSend();
                else if (s == 2) { syncReceive(); return; }
            }
            if (s == 2) return;   // sink with no socket yet: still never runs the local mic
        }

        // A whole mode rather than a fill-in, so it always runs and returns.
        if (mode == kSimMode) { synthesizeFrame(simulate == 1); return; }

        // From here it is the local mode, which holds the last frame until an input is up.
        if constexpr (!platform::hasAudioInput) {
            return;
        } else {
            if (!inited_) return;
        }

        // Each tick contributes a partial block, and the analysis runs once one is full.
        const size_t n = platform::audioMicRead(mic_, samples_ + filled_, kBlock - filled_);
        // Read before the filter rewrites the samples.
        micSamples1s_ += n;
        for (size_t i = 0; i < n; i++) if (samples_[filled_ + i] != 0) { micNonzero1s_++; break; }
        if (n == 0) return;                            // nothing ready this tick
        filled_ += n;
        if (filled_ < kBlock) return;                  // wait for a whole block
        filled_ = 0;                                   // consumed below; refill next

        // Removes the offset and the rumble before any analysis, continuous across blocks.
        dc_.process(samples_, kBlock);

        // Measured independently of the transform, on a gentler floor so it keeps moving.
        computeLevel(samples_, kBlock, static_cast<uint8_t>(floor / 2), gain, frame_,
                     levels == 1 ? &levelCond_ : nullptr,
                     static_cast<uint32_t>(kBlock * 1000u / sampleRate()));

        // The textbook light smoothing:
        frame_.levelSmoothed = static_cast<uint16_t>((frame_.levelSmoothed * 3 + frame_.level) / 4);

        // The spectrum, through the same mapping the level uses.
        uint16_t peakHz = 0, peakMag = 0;
        applyWindow(samples_, kBlock, windowed_);
        platform::audioFft(windowed_, kBlock, mag_);
        magnitudesToBands(mag_, kMag, sampleRate(), floor, gain,
                          frame_.bands, peakHz, peakMag,
                          levels == 1 ? &cond_ : nullptr,
                          static_cast<uint32_t>(kBlock * 1000u / sampleRate()),
                          kRatio, static_cast<float>(kMaxGainDb), true);
        finishBands();

        // Held when there is no real signal, so it does not wander in silence.
        if (peakMag > 8) { frame_.peakHz = peakHz; frame_.peakMag = peakMag; }

        // Analysis runs faster than the send, so latching here reports each beat exactly once.
        const uint32_t nowMs = platform::millis();
        if (frame_.level > frame_.levelSmoothed + kSyncPeakMargin &&
            nowMs - lastPeakMs_ >= kSyncPeakRefractoryMs) {
            syncPeakLatched_ = true;
            lastPeakMs_ = nowMs;
        }

        // The window's peak, since sampling the instantaneous value once a second lands in the gaps.
        if (frame_.level > levelPeak_) levelPeak_ = frame_.level;
    }

    /// Fill frame_ with a synthesized signal.
    void synthesizeFrame(bool sweep) {
        const uint32_t t = platform::millis();
        if (sweep) {
            // One band lit at a time, stepping bass→treble every ~250 ms and wrapping.
            const uint8_t pos = static_cast<uint8_t>((t / 250u) % 16u);
            const uint8_t env = triwave8(static_cast<uint8_t>((t % 250u) * 255u / 250u));  // 0..255 within a step
            for (uint8_t b = 0; b < 16; b++) frame_.bands[b] = (b == pos) ? env : 0;
            finishBands();
            frame_.level = env;
            frame_.peakHz = static_cast<uint16_t>(80 + pos * 700);   // bass→~10.6 kHz across the 16 steps
            frame_.peakMag = env;
        } else {
            // Musical "song":
            const uint8_t beat = (t % 600u < 90u) ? static_cast<uint8_t>(triwave8(static_cast<uint8_t>((t % 600u) * 255u / 90u))) : 0;
            uint16_t sum = 0;
            for (uint8_t b = 0; b < 16; b++) {
                // Per-band oscillator: rate rises with b (treble flickers faster), phase spread by b.
                const uint8_t rate = static_cast<uint8_t>(1 + b);                   // BPM-ish multiplier
                const uint8_t osc = sin8(static_cast<uint8_t>(t * rate / 8u + b * 24u));
                uint16_t v = static_cast<uint16_t>((osc * 3u) / 4u);               // 0..191 base
                if (b < 4) v = static_cast<uint16_t>(v + beat / 2u);               // beat lifts the bass
                frame_.bands[b] = static_cast<uint8_t>(v > 255 ? 255 : v);
                sum = static_cast<uint16_t>(sum + frame_.bands[b]);
            }
            const uint8_t swell = sin8(static_cast<uint8_t>(t / 24u));             // slow volume breath
            uint16_t lvl = static_cast<uint16_t>(swell / 2u + sum / 32u + beat / 2u);
            frame_.level = lvl > 255 ? 255 : lvl;
            finishBands();
            // Peak drifts across the spectrum so freq-mapped effects move.
            frame_.peakHz = static_cast<uint16_t>(80 + sin8(static_cast<uint8_t>(t / 40u)) * 40u);
            frame_.peakMag = frame_.level;
        }
        frame_.levelSmoothed = static_cast<uint16_t>((frame_.levelSmoothed * 3 + frame_.level) / 4);
        // Feed the same 1 s peak-hold the mic path uses, so the "level RMS" display tracks the.
        if (frame_.level > levelPeak_) levelPeak_ = static_cast<uint8_t>(frame_.level);
    }

    void tick1s() MM_NONBLOCKING override {
        // The mirror of the LED driver's retry:
        if (mode == kLocalMode && !inited_
            && platform::audioMicSharedBusFree(micMode == 1 ? platform::MicMode::Pdm
                                                            : platform::MicMode::I2sStd)) reinit();
        std::snprintf(levelStr_, sizeof(levelStr_), "%u", static_cast<unsigned>(levelPeak_));
        std::snprintf(onsetStr_, sizeof(onsetStr_), "%u/s, flux %u",
                      static_cast<unsigned>(onsetCount_), static_cast<unsigned>(fluxPeak_));
        onsetCount_ = 0; fluxPeak_ = 0;
        std::snprintf(peakStr_, sizeof(peakStr_), "%u Hz", static_cast<unsigned>(frame_.peakHz));
        levelPeak_ = 0;   // reset for the next window

        // Mic-health diagnosis from the 1 s tallies (see the read path).
        const bool directMicLive = platform::hasI2sMic && inited_ && mode == kLocalMode
                                   && platform::audioCodecType == platform::CodecType::None;
        if (directMicLive) {
            if (micSamples1s_ == 0)
                setStatus(micMode == 1 ? "mic: no samples, check wsPin (PDM clock)"
                                       : "mic: no samples, check sckPin / wsPin (I2S clocks)",
                          Severity::Warning);
            else if (micNonzero1s_ == 0)
                setStatus("mic: data line silent, check sdPin (SD/DOUT) + mic power", Severity::Warning);
            else if (micStatusStale_)
                setStatus("", Severity::Status);   // data flowing again, clear a prior diagnosis
            micStatusStale_ = (micSamples1s_ == 0 || micNonzero1s_ == 0);
        } else if (mode != kLocalMode) {
            // No mic to diagnose on this path (Receive or Simulate), so no diagnosis may be OUTSTANDING.
            micStatusStale_ = false;
        }
        micSamples1s_ = 0;
        micNonzero1s_ = 0;
        // Live sync state on the module's OWN status line:
        if constexpr (platform::hasNetwork) {
            const uint8_t s = sync();
            if (s != 0 && syncOpen_ && !micStatusStale_) {
                if (s == 1) setStatus("sending");
                else if (lastSyncRecv_ != 0
                         && platform::millis() - lastSyncRecv_ < kSyncFallbackMs) {
                    // Named, because "receiving" alone cannot tell a rig taking the right source from one locked.
                    std::snprintf(syncStr_, sizeof(syncStr_), "receiving from %u.%u.%u.%u",
                                  syncPeer_[0], syncPeer_[1], syncPeer_[2], syncPeer_[3]);
                    setStatus(syncStr_);
                } else {
                    setStatus("listening");
                }
            }
        }
        MoonModule::tick1s();
    }

private:
    // The one-active-mic election:
    ActiveInstance<AudioService> micSeat_{*this};

    platform::AudioMicHandle mic_;
    bool inited_ = false;
    size_t filled_ = 0;         // samples accumulated toward the next full block
    DcBlocker dc_;              // ~40 Hz high-pass, continuous across blocks

    // Fixed hot-path scratch, sized once, never reallocated.
    int32_t samples_[kBlock] = {};
    float windowed_[kBlock] = {};
    float mag_[kMag] = {};

    AudioFrame frame_;

    char levelStr_[12] = {};
    char onsetStr_[20] = {};
    uint8_t onsetCount_ = 0;  ///< onsets in the current 1 s display window (UI only)
    uint8_t fluxPeak_ = 0;    ///< peak flux in that window (UI only)
    char peakStr_[12] = {};
    uint8_t levelPeak_ = 0;   // peak frame_.level across the current 1 s display window (UI only)

    // Mic-health tallies over the 1 s window (see the read path + tick1s diagnosis).
    uint32_t micSamples1s_ = 0;
    uint32_t micNonzero1s_ = 0;
    bool     micStatusStale_ = false;

    // WLED audio sync (light/WLEDAudioSyncPacket.h). One socket, bound only in Send/Receive.
    platform::UdpSocket syncSock_;
    uint32_t lastSyncSend_ = 0;      // millis of the last send (send throttle)
    uint32_t syncSendCount_ = 0;         // sends made; test-visible so the throttle can be observed
                                         // (NOT a wire field: byte 17 is WLED's reserved2)
    bool     syncPeakLatched_ = false;   // a beat seen since the last transmit (WLED's udpSamplePeak)
    uint32_t lastPeakMs_ = 0;            // when that beat was, for the refractory window
    uint32_t lastSyncRecv_ = 0;      // millis of the last received packet (receive auto-blend)
    uint8_t  syncPeer_[4] = {};      // source address of that packet, for the status line
    bool     syncOpen_ = false;      // socket opened for the current mode (lazy-open latch)
    uint32_t lastSyncOpenFailMs_ = 0;  // millis of the last failed open (0 = none); bring-up backoff
    char     syncStr_[40] = {};      // scratch for the "receiving from <ip>" status line
    static constexpr uint32_t kSyncSendIntervalMs = 25;   // ~40/s, WLED-friendly, well under a flood
    static constexpr uint32_t kSyncFallbackMs = 1000;     // no packet this long → resume local mic
    static constexpr uint32_t kSyncOpenRetryMs = 1000;    // pause between socket bring-up retries after a failure
    static constexpr int kSyncMaxRecvPerTick = 8;         // bounded non-blocking drain (sync is low-rate)

    static constexpr const char* kInitFailMsg = "mic init failed, check pins / rate";

    /// (Re)create the I2S channel for the current pins + rate.
    void reinit() {
        if constexpr (platform::hasAudioCapture) {
            // Desktop:
            deinit();
            inited_ = platform::audioCaptureInit(mic_, device, sampleRate());
            if (!inited_) {
                setStatus("capture init failed, pick another device", Severity::Error);
                return;
            }
            dc_.reset();
            clearStatus();
            return;
        }
        if constexpr (!platform::hasI2sMic) {
            setStatus("mic: no audio input on this platform", Severity::Warning);
            return;
        }
        deinit();
        // Any pin unset (-1, the default until the user wires a mic):
        const bool pdm = micMode == 1;
        if (wsPin < 0 || sdPin < 0 || (!pdm && sckPin < 0)) {
            setStatus(pdm ? "mic: set wsPin (clock) / sdPin (data)"
                          : "mic: set sckPin / wsPin / sdPin", Severity::Status);
            return;
        }
        // Bring up the I2S channel FIRST.
        const int16_t mclk = platform::audioCodecType == platform::CodecType::None
                           ? mclkPin : static_cast<int16_t>(platform::audioCodecPins.mclk);
        inited_ = platform::audioMicInit(mic_, static_cast<uint16_t>(wsPin),
                                         static_cast<uint16_t>(sdPin),
                                         static_cast<uint16_t>(sckPin), mclk, sampleRate(),
                                         static_cast<platform::MicMode>(micMode));
        if (!inited_) {
            setStatus(kInitFailMsg, Severity::Error);
            return;
        }
        // Now configure the codec over I2C (MCLK is running).
        if (!platform::audioCodecInit(platform::audioCodecType, platform::audioCodecPins,
                                      sampleRate())) {
            deinit();   // tear the I2S channel back down, we couldn't bring the codec up
            setStatus("mic: codec init failed, check I2C wiring", Severity::Error);
            return;
        }
        dc_.reset();   // start the high-pass clean for the new stream
        // The INMP441 emits ~250 ms of power-on settling garbage after the clock starts.
        clearStatus();
    }

    void deinit() {
        if constexpr (!platform::hasAudioInput) return;
        if (inited_) platform::audioMicDeinit(mic_);
        platform::audioCodecDeinit();   // releases the codec + its I2C bus (no-op if none)
        inited_ = false;
        filled_ = 0;
        // Publish silence:
        frame_ = AudioFrame{};
        // The ANALYSIS history goes with it, so a restarted source begins from a DEFINED state rather.
        std::memset(prevBands_, 0, sizeof(prevBands_));
        onset_ = OnsetDetector{};
    }

    // --- WLED audio sync (guarded: only compiled where platform::hasNetwork) ---

    /// Reset the sync socket to the current mode.
    void syncReinit() {
        if constexpr (!platform::hasNetwork) return;
        syncSock_.close();                 // syncEnsureSocket() re-opens per mode when the net is up
        syncOpen_ = false;
        lastSyncOpenFailMs_ = 0;           // a mode change retries bring-up immediately (no stale backoff)
        lastSyncRecv_ = 0;
        std::memset(syncPeer_, 0, sizeof(syncPeer_));
        const uint8_t s = sync();
        // Only when there IS a socket to wait for.
        if (s == 1)      setStatus("send: waiting for network");
        else if (s == 2) setStatus("receive: waiting for network");
        else if (const char* cur = status();
                 cur && (std::strstr(cur, "waiting for network") || std::strstr(cur, "socket failed")
                         || std::strstr(cur, "bind failed") || std::strstr(cur, "from ")
                         || std::strstr(cur, "listening on")))
            setStatus("");
    }

    /// Lazily open the sync socket for the current mode, once the network stack is up.
    bool syncEnsureSocket() {
        if constexpr (!platform::hasNetwork) return false;
        const uint8_t s = sync();
        if (s == 0) return false;
        if (syncOpen_) return true;
        if (!platform::networkReady()) return false;   // interface not up yet, try again next tick
        // Back off between failed bring-ups:
        const uint32_t now = platform::millis();
        if (lastSyncOpenFailMs_ != 0 && now - lastSyncOpenFailMs_ < kSyncOpenRetryMs) return false;
        if (s == 1) {                      // send → the WLED multicast group (configurable port)
            char grp[16]; formatDottedQuad(grp, kSyncMulticastAddr_);
            if (syncSock_.open() && syncSock_.connect(grp, syncPort)) {
                syncOpen_ = true;
                setStatus("sending");
            } else {
                syncSock_.close();
                setStatus("send: socket failed", Severity::Error);
            }
        } else {                           // receive → bind the port, then JOIN the group
            // The join is what makes a multicast datagram reach this socket at all:
            char grp[16]; formatDottedQuad(grp, kSyncMulticastAddr_);
            if (syncSock_.open() && syncSock_.bind(syncPort) && syncSock_.joinMulticast(grp)) {
                syncOpen_ = true;
                setStatus("listening");
            } else {
                syncSock_.close();
                setStatus("receive: bind failed", Severity::Error);
            }
        }
        // Stamp a failure (or clear the timer on success).
        lastSyncOpenFailMs_ = syncOpen_ ? 0 : (now == 0 ? 1 : now);
        return syncOpen_;
    }

    /// Broadcast the current frame_ as a WLED v2 packet, throttled to ~40/s.
    void syncSend() {
        if constexpr (!platform::hasNetwork) return;
        const uint32_t now = platform::millis();
        if (now - lastSyncSend_ < kSyncSendIntervalMs) return;
        lastSyncSend_ = now;
        // samplePeak is a LATCHED beat flag, the way WLED sends it:
        const bool peak = syncPeakLatched_;
        syncPeakLatched_ = false;
        uint8_t pkt[WLED_SYNC_PACKET_SIZE];
        buildWledAudioSync(pkt, frame_, peak);
        syncSock_.sendTo(pkt, WLED_SYNC_PACKET_SIZE);
        syncSendCount_++;
    }

    /// Drain the sync socket (bounded, non-blocking) in Receive mode.
    bool syncReceive() {
        if constexpr (!platform::hasNetwork) return false;
        uint8_t pkt[WLED_SYNC_PACKET_SIZE + 8];   // a little slack over the 44-byte v2
        uint8_t srcIp[4] = {};
        for (int i = 0; i < kSyncMaxRecvPerTick; i++) {
            const int n = syncSock_.recvFrom(pkt, sizeof(pkt), srcIp);
            if (n <= 0) break;                     // -1 = nothing pending
            AudioFrame rf;
            if (parseWledAudioSync(pkt, static_cast<size_t>(n), rf)) {
                // The packet carries RAW bands; the ballistic is ours and lives across packets.
                uint8_t keep[16];
                std::memcpy(keep, frame_.bandsSmoothed, sizeof(keep));
                frame_ = rf;                       // received audio drives the effects
                std::memcpy(frame_.bandsSmoothed, keep, sizeof(keep));
                finishBands();
                lastSyncRecv_ = platform::millis();
                // Whose audio this is.
                std::memcpy(syncPeer_, srcIp, sizeof(syncPeer_));
                // Feed the peer level into the same 1 s peak window the local mic uses, so the "level RMS".
                if (frame_.level > levelPeak_)
                    levelPeak_ = static_cast<uint8_t>(frame_.level > 255 ? 255 : frame_.level);
            }
            // else: a v1 / foreign packet, ignore, keep draining.
        }
        // Fresh received audio → skip local mic. Stale (peer quiet) → fall through.
        return lastSyncRecv_ != 0
            && (platform::millis() - lastSyncRecv_) < kSyncFallbackMs;
    }

    static constexpr uint16_t kSyncPeakMargin = 8;   // level over smoothed = a beat (samplePeak hint)
    // WLED gates its own peak detection on `millis() - timeOfPeak > 80`, so one loud passage.
    static constexpr uint32_t kSyncPeakRefractoryMs = 80;
    // The IP MULTICAST ADDRESS WLED audio sync uses:
    static constexpr uint8_t kSyncMulticastAddr_[4] = {239, 0, 0, 1};
};

} // namespace mm
