#pragma once

#include "light/drivers/DriverBase.h"

#include "light/drivers/LedDriverConfig.h"
#include "light/drivers/PinList.h"         // parsePinList / assignCounts (shared with I80Peripheral)
#include "light/drivers/RmtSymbol.h"       // makeRmtSymbol (the bit shapes the peripheral expands with)
#include "platform/platform.h"

namespace mm {

/// Output driver: WS2812B-class addressable LEDs over the ESP32 RMT peripheral, one GPIO and one RMT TX channel per strand fed consecutive slices of the source buffer. The default on classic-ESP32 and S3 boards, and the example future drivers copy. It fuses the correction and the wire-byte encode into one pass, then hands per-pin slices to the platform.
///
/// Prior art: WS2812B on FastLED and WLED, and the clockless RMT techniques of hpwit (Yves Bazin).
///
/// Flicker on LEDs that should be off is signal integrity rather than firmware. The playbook is on the drivers page.
///
/// @moreinfo
///
/// ## The wire contract
///
/// One-wire NRZ at 800 kHz. Each bit is a 1.25 µs cell starting high then dropping low, the high duration encoding the bit, MSB-first per byte. `Correction` applies channel order before the encode, and a frame latches on 300 µs of idle-low. Timings convert to RMT ticks from the granted resolution, never hard-coded.
///
/// ## Which RMT API
///
/// The peripheral half uses the modern RMT driver, not the legacy channel-numbered one. That is not a preference: the legacy driver was removed entirely in ESP-IDF v6, which is the build IDF.
/// On chips whose RMT has a DMA backend, the whole-frame loopback capture uses it.
///
/// @card RmtLedDriver.png
class RmtLedDriver : public DriverBase {
public:
    /// This driver prices its own frame, so the shared path leaves the limiter to it.
    bool limitsCurrent() const override { return true; }

    /// Default to the GRB preset, which is how WS2812 and SK6812 strips are physically wired.
    RmtLedDriver() { setDefaultPresetName("GRB"); }

    /// Hard cap on the pin arrays: the largest RMT TX group of any supported chip.
    static constexpr uint8_t kMaxPins = 8;

    // UNSET by default: the strand is user-soldered, so a hard-coded pin would be a guess.
    /// Comma-separated GPIO list, one RMT TX channel per pin.
    char pins[24] = "";

    // An unassigned remainder splits evenly, and each pin is capped at a WS2812 ceiling.
    /// Comma-separated lights-per-pin, matched to `pins` by position.
    char ledsPerPin[48] = "";

    // Named by SPEED, not chipset: chipset names do not partition the timings.
    /// Wire timing; the default satisfies WS2812, WS2812B and SK6812 at once.
    uint8_t timing = 0;

    /// The three numbers behind `timing`, shown only when it is set to custom.
    uint16_t t0hNs = 350;
    /// Custom timing: how long a 1 bit stays high, in nanoseconds.
    uint16_t t1hNs = 700;
    /// Custom timing: the whole bit cell, in nanoseconds.
    uint16_t periodNs = 1250;

    /// The index of `custom` in the table below: the one option that reads the three fields.
    static constexpr uint8_t kTimingCustom = 3;

    /// The presets, in the order the select lists them.
    static constexpr const char* kTimingOptions[] = {
        "800kHz WS2812B/SK6812",   // 350 / 700 / 1250: the default, and most strips
        "400kHz WS2811",           // 500 / 1200 / 2500: 12V WS2811 in low-speed mode
        "800kHz WS2811 fast",      // 250 / 600 / 1250: WS2811 strapped to high speed
        "custom",                  // the three fields below
    };

    // RMT is a transceiver, so the driver can verify its own output on real silicon.
    /// On-device loopback self-test: transmit a known pattern over a jumper and verify it.
    bool     loopbackTest = false;  // checkbox: on = run + keep re-running on change
    // -1 as unset, rather than 0, so GPIO 0 stays a valid loopback pin.
    /// Optional TX override for the test, so it runs on a dedicated jumper.
    int8_t   loopbackTxPin = -1;
    /// Jumper this to the TX pin for the test (unset = -1 by default; bench used pin 5).
    int8_t   loopbackRxPin = -1;

    // A 24-bit burst passes through a wire that mangles a sustained frame; this catches that.
    /// Whole-frame stress variant: transmit real frames back to back and verify every bit.
    bool     loopbackFrame = false;

    // The requested clock, not a tick count: the encoder converts via the granted resolution.
    /// The RMT tick clock this driver requests, 25 ns per tick.
    static constexpr uint32_t kResolutionHz = 40'000'000;

    // The pin and count parsing lives in PinList.h, shared with I80Peripheral.

    /// Bind the window, the two pin text lists, and the loopback self-test controls.
    void defineDriverControls() override {
        addWindowControls();   // start / count: the slice of the shared buffer this driver outputs
        controls_.addText("pins", pins, sizeof(pins));
        controls_.addText("ledsPerPin", ledsPerPin, sizeof(ledsPerPin));
        controls_.addSelect("timing", timing, kTimingOptions,
                            static_cast<uint8_t>(sizeof(kTimingOptions) / sizeof(kTimingOptions[0])));
        // Ranges are the encodable window, not datasheet limits: a user types what theirs says.
        const bool custom = timing == kTimingCustom;
        controls_.addControl("t0hNs", t0hNs, 50, 4000);
        controls_.setHidden(controls_.count() - 1, !custom);
        controls_.addControl("t1hNs", t1hNs, 50, 4000);
        controls_.setHidden(controls_.count() - 1, !custom);
        controls_.addControl("periodNs", periodNs, 200, 8000);
        controls_.setHidden(controls_.count() - 1, !custom);
        controls_.addControl("loopbackTest", loopbackTest);
        controls_.setAdvanced(controls_.count() - 1);   // expert-mode: a bench self-test, not a normal-use control
        // Always bound so persistence can load them, but shown only while test mode is on.
        controls_.addPin("loopbackTxPin", loopbackTxPin);
        controls_.setHidden(controls_.count() - 1, !loopbackTest);
        controls_.addPin("loopbackRxPin", loopbackRxPin);
        controls_.setHidden(controls_.count() - 1, !loopbackTest);
        controls_.addControl("loopbackFrame", loopbackFrame);
        controls_.setHidden(controls_.count() - 1, !loopbackTest);
    }

    /// Which controls re-parse and re-init the RMT channels live, through the prepare sweep.
    bool affectsPrepare(const char* name) const override {
        return std::strcmp(name, "pins") == 0 || std::strcmp(name, "ledsPerPin") == 0
            || std::strcmp(name, "timing") == 0 || std::strcmp(name, "t0hNs") == 0
            || std::strcmp(name, "t1hNs") == 0 || std::strcmp(name, "periodNs") == 0
            // Not to rebuild: only a prepare sweep re-runs defineControls, which shows the pins.
            || std::strcmp(name, "loopbackTest") == 0
            || isWindowControl(name);
    }

    // Runs off the render loop, in the API handler context, so a blocking self-test is fine.
    /// React to a control change, re-running the loopback while its mode is on.
    void onControlChanged(const char* name) override {
        const bool isTestControl = std::strcmp(name, "loopbackTest") == 0;
        const bool isPinControl  = std::strcmp(name, "pins") == 0
                                || std::strcmp(name, "loopbackTxPin") == 0
                                || std::strcmp(name, "loopbackRxPin") == 0
                                || std::strcmp(name, "loopbackFrame") == 0;
        if (isTestControl && !loopbackTest) {
            // Re-derive the real status afterwards: a config or init error must survive.
            clearFailBuf();
            clearStatus();
            parseConfig();
            reinit();
        } else if (loopbackTest && (isTestControl || isPinControl)) {
            // This runs BEFORE the prepare sweep re-parses, so refresh or the test uses stale pins.
            if (std::strcmp(name, "pins") == 0) { parseConfig(); reinit(); }
            runLoopbackSelfTest();
        }
        // Chain to the base, or this driver's own brightness and preset controls are dead.
        DriverBase::onControlChanged(name);
    }

    // The buffer and the channels are separate concerns: a rebuild must not free the buffer.
    /// One-time wiring: parse the pin lists; the acquire lives in prepare().
    void setup() override { parseConfig(); }
    /// Release the RMT channels, free the frame buffer, and clear the shared error state.
    void release() override {
        deinitAll();
        freeFrame();
        DriverBase::release();   // frees the correction scratch, clears failBuf_ + configErr_
    }

    /// Re-parse, resize the frame buffer and re-init the channels, off the hot path.
    void prepare() override {
        // Drain first: a prepare can land mid-frame while the peripheral still reads the buffer.
        if (txInFlight_) {
            for (uint8_t attempt = 0; attempt < 4 && txInFlight_; attempt++)
                txInFlight_ = !waitForPins();
            // Still busy, so defer: rebuilding now would free the buffer the peripheral reads.
            if (txInFlight_) return;
        }
        parseConfig();
        resizeFrame();
        reinit();
        // Gated on both flags: a false "driving N lights" over a real error hides a dark strip.
        if (inited_ && !frameUnusable_ && !configErr_ && !configWarn_ && txLightCount_ > 0)
            setDrivingInfo(txLightCount_, winLen_, correction_.outChannels);
    }

    /// Re-derive the per-pin offsets when the preset changes the output channel count.
    void onCorrectionChanged() override { if (!effectivelyEnabled()) return; parseConfig(); resizeFrame(); }

    /// Point the driver at the source frame buffer, re-parsing and resizing to match.
    void setSourceBuffer(Buffer* buf) override {
        sourceBuffer_ = buf;
        parseConfig();      // counts derive from the buffer's light count
        if (effectivelyEnabled()) resizeFrame();
    }

    /// Fuse the correction and the encode in one pass, then start every pin before waiting.
    void tick() MM_NONBLOCKING override {
        if constexpr (platform::rmtTxChannels == 0) return;  // inert off RMT chips
        if (!inited_ || !sourceBuffer_ || !sourceBuffer_->data()) return;

        // A frame still on the wire OWNS the buffer: re-encoding now corrupts what it clocks out.
        if (txInFlight_) {
            txInFlight_ = !waitForPins();     // still busy: leave frame_ alone for another tick
            if (txInFlight_) return;
        }

        const nrOfLightsType n = txLightCount_ < winLen_ ? txLightCount_ : winLen_;
        const uint8_t outCh = correction_.outChannels;
        // Skip rather than overrun when the buffer is stale, the guard ArtNet uses.
        if (n == 0 || outCh == 0 || pinCount_ == 0
            || !frame_ || frameCap_ < frameBytesFor(n, outCh)) return;   // buffer not ready

        // One pass: correct a light into wire bytes and encode them straight into the buffer.
        const uint8_t* src = sourceBuffer_->data();
        const uint8_t srcCh = sourceBuffer_->channelsPerLight();
        // Price the window first, so the whole frame scales together.
        correction_.measure(src + winStart_ * srcCh, srcCh, n);
        // The bit expansion happens on the way out, so this buffer is outCh bytes per light.
        for (nrOfLightsType i = 0; i < n; i++) {
            correction_.apply(src + (winStart_ + i) * srcCh, frame_ + static_cast<size_t>(i) * outCh, srcCh);
        }
        // Start every pin before waiting on any, so the tick costs the longest strand not the sum.
        const size_t bytesPerLight = static_cast<size_t>(outCh);
        bool started[kMaxPins] = {};
        for (uint8_t i = 0; i < pinCount_; i++) {
            const nrOfLightsType pinStart = static_cast<nrOfLightsType>(pinOffsets_[i] / bytesPerLight);
            if (pinStart >= n) break;  // contiguous: this pin and all later ones are past the encoded lights
            const nrOfLightsType pinLights =
                (pinStart + pinCounts_[i] > n) ? static_cast<nrOfLightsType>(n - pinStart) : pinCounts_[i];
            if (pinLights == 0) continue;
            started[i] = platform::rmtWs2812Transmit(rmt_[i], frame_ + pinOffsets_[i],
                                        static_cast<size_t>(pinLights) * bytesPerLight);
        }
        for (uint8_t i = 0; i < pinCount_; i++) started_[i] = started[i];
        txInFlight_ = !waitForPins();
        if (cfg_.reset_us) platform::delayUs(cfg_.reset_us);
    }

    /// Wait on every pin that started, and report whether they all finished.
    bool waitForPins() MM_NONBLOCKING {
        bool allDone = true;
        for (uint8_t i = 0; i < pinCount_; i++) {
            if (!started_[i]) continue;
            if (platform::rmtWs2812Wait(rmt_[i], 1000 /* ms */)) started_[i] = false;
            else allDone = false;
        }
        return allDone;
    }

    /// Test-only accessors, which pin the buffer lifecycle and the multi-pin slice arithmetic.
    const uint8_t* frameBuffer() const { return frame_; }
    /// Bytes allocated in the symbol buffer. Test-only.
    size_t frameCapacity() const { return frameCap_; }
    /// Number of parsed output pins (0 = idle). Test-only.
    uint8_t pinCount() const { return pinCount_; }
    /// Lights on pin `i` (0 if out of range). Test-only.
    nrOfLightsType pinLightCount(uint8_t i) const { return i < pinCount_ ? pinCounts_[i] : 0; }
    /// Byte offset of pin `i`'s slice in the symbol buffer (0 if out of range). Test-only.
    size_t pinFrameOffsetBytes(uint8_t i) const { return i < pinCount_ ? pinOffsets_[i] : 0; }

private:
    // The correction lives on DriverBase, applied per light, as in NetworkSendDriver.
    Buffer* sourceBuffer_ = nullptr;

    LedDriverConfig cfg_;

public:
    /// The wire timing the `timing` control resolved to, in the nanoseconds a datasheet states.
    const LedDriverConfig& wireTimingForTest() const { return cfg_; }

private:
    platform::RmtWs2812Handle rmt_[kMaxPins];
    uint16_t       pinList_[kMaxPins] = {};    // parsed pins, list order
    nrOfLightsType pinCounts_[kMaxPins] = {};  // lights per pin (slice lengths)
    size_t         pinOffsets_[kMaxPins] = {}; // slice start in frame_, bytes
    nrOfLightsType txLightCount_ = 0;          // Σ pinCounts_: the lights encoded and transmitted
    nrOfLightsType winStart_ = 0;              // first source-buffer light this driver reads (the window)
    nrOfLightsType winLen_ = 0;                // window length (lights), clamped to the buffer
    uint8_t pinCount_ = 0;                     // 0 = idle (parse error / no pins)
    bool inited_ = false;                      // all-or-nothing across the pins
    bool started_[kMaxPins] = {};              // which pins have a transmit still to be waited on
    bool frameUnusable_ = false;  // frame buffer missing, or in PSRAM the refill cannot read
    bool txInFlight_ = false;                  // a frame is still clocking out of frame_
    uint8_t* frame_ = nullptr;      // owned; the wire bytes for the whole frame (outChannels per light)
    size_t frameCap_ = 0;          // bytes allocated

    // The status helpers live on DriverBase, shared verbatim with the other parallel drivers.

    // Falls back to kMaxPins off RMT chips, so the slicing stays host-testable.
    static constexpr uint8_t maxPinsForTarget() {
        return (platform::rmtTxChannels > 0 && platform::rmtTxChannels < kMaxPins)
                   ? platform::rmtTxChannels
                   : kMaxPins;
    }

    static size_t frameBytesFor(nrOfLightsType lights, uint8_t channels) {
        return static_cast<size_t>(lights) * channels;
    }

    /// Hand the peripheral the two symbols a data bit expands to, changed live between frames.
    bool pushBitTiming(uint8_t i) {
        const uint16_t t0h = nsToTicks(cfg_.t0h_ns, i);
        const uint16_t t1h = nsToTicks(cfg_.t1h_ns, i);
        const uint16_t period = nsToTicks(cfg_.period_ns, i);
        return platform::rmtWs2812SetBitTiming(rmt_[i],
            makeRmtSymbol(t0h, 1, static_cast<uint16_t>(period - t0h), 0),
            makeRmtSymbol(t1h, 1, static_cast<uint16_t>(period - t1h), 0));
    }

    /// Ticks for `ns` on channel `i`, read from that channel's own granted resolution.
    uint16_t nsToTicks(uint32_t ns, uint8_t i = 0) const MM_NONBLOCKING {
        uint32_t hz = platform::rmtWs2812Resolution(rmt_[i]);   // 0 before init: fall through
        if (hz == 0) hz = kResolutionHz;
        return static_cast<uint16_t>((static_cast<uint64_t>(ns) * hz) / 1'000'000'000ull);
    }

    // --- pin/count config (plain parsing; runs on every platform) ---

    /// Turn the `timing` selection into the wire timing the encoder reads.
    void applyTiming() {
        switch (timing) {
            // The 400 kHz WS2811 mode: twice the bit period, so a frame takes twice as long.
            case 1: cfg_.t0h_ns = 500; cfg_.t1h_ns = 1200; cfg_.period_ns = 2500; break;
            // WS2811 in high-speed mode: within the WS2812 decode window, so a refinement.
            case 2: cfg_.t0h_ns = 250; cfg_.t1h_ns = 600;  cfg_.period_ns = 1250; break;
            case kTimingCustom:
                // Trusted as typed, but ordered: no chip decodes a 1 shorter than a 0.
                cfg_.t0h_ns = t0hNs;
                cfg_.t1h_ns = t1hNs > t0hNs ? t1hNs : static_cast<uint16_t>(t0hNs + 50);
                cfg_.period_ns = periodNs > cfg_.t1h_ns ? periodNs
                                                        : static_cast<uint16_t>(cfg_.t1h_ns + 50);
                break;
            // The default: satisfies WS2812, WS2812B and SK6812 at once.
            default: cfg_.t0h_ns = 350; cfg_.t1h_ns = 700; cfg_.period_ns = 1250; break;
        }
    }

    bool parseConfig() {
        applyTiming();
        pinCount_ = 0;
        uint8_t n = 0;
        const char* warn = nullptr;
        const char* err = parsePinList(pins, pinList_, maxPinsForTarget(), n);
        if (!err) {
            // Over the driver's window, not the whole buffer, so the remainder stays in slice.
            const nrOfLightsType bufN = sourceBuffer_ ? sourceBuffer_->count() : 0;
            windowSlice(bufN, winStart_, winLen_);
            err = assignCounts(ledsPerPin, n, winLen_, pinCounts_, kMaxWs2812LedsPerPin, &warn);
        }
        if (err) {
            setConfigErr(err);
            return false;
        }
        pinCount_ = n;
        const uint8_t outCh = correction_.outChannels;
        size_t off = 0;
        txLightCount_ = 0;
        for (uint8_t i = 0; i < pinCount_; i++) {
            pinOffsets_[i] = off;
            off += static_cast<size_t>(pinCounts_[i]) * outCh;   // BYTES: frame_ holds wire bytes
            txLightCount_ = static_cast<nrOfLightsType>(txLightCount_ + pinCounts_[i]);
        }
        clearConfigErr();
        // A clamp is a Warning, not an idling error: the output still runs, so it says so.
        setConfigWarn(warn);
        // Real consumption, not a grid times pins guess; an idle driver stays statusless.
        if (!warn && txLightCount_ > 0) setDrivingInfo(txLightCount_, winLen_, outCh);
        return true;
    }

    // --- symbol buffer (plain heap; runs on every platform) ---

    // Grows only, keeping a big-enough existing allocation. Off the hot path.
    void resizeFrame() {
        if (!sourceBuffer_) return;
        // Sized to what the pins CLOCK OUT, not the window, which can be far larger.
        nrOfLightsType winStart, win;
        windowSlice(sourceBuffer_->count(), winStart, win);
        nrOfLightsType n = txLightCount_ > 0 ? txLightCount_ : win;
        if (n > win) n = win;               // never exceed the window's own light count
        const uint8_t ch = correction_.outChannels;
        if (n == 0 || ch == 0) { frameUnusable_ = false; return; }
        // Sized to outChannels, so a correction wider than RGBW cannot overflow it.
        const size_t need = frameBytesFor(n, ch);
        if (frame_ && frameCap_ >= need) { frameUnusable_ = false; return; }
        freeFrame();
        // INTERNAL RAM deliberately: the refill reads these with the flash cache possibly off.
        frame_ = static_cast<uint8_t*>(platform::allocInternal(need));
        if (!frame_) frame_ = static_cast<uint8_t*>(platform::alloc(need));
        frameCap_ = frame_ ? need : 0;
        // A failed or PSRAM allocation must SAY so, or the strip freezes under a healthy status.
        frameUnusable_ = !frame_ || platform::ptrIsPsram(frame_);
        if (frameUnusable_) {
            if (failBufEnsure()) {
                std::snprintf(failBuf_, kFailBufLen, frame_
                                  ? "%u lights need %u KB of internal RAM"
                                  : "out of memory for %u lights (%u KB)",
                              static_cast<unsigned>(n),
                              static_cast<unsigned>((need + 1023) / 1024));
                setStatus(failBuf_, Severity::Error);
            } else {
                setStatus(kNoFrameMemMsg, Severity::Error);
            }
            if (frame_) freeFrame();   // hand back memory the transmit will never read
        } else if (status() == failBuf_ || status() == kNoFrameMemMsg) {
            clearStatus();   // the count came back down (or the heap freed up): retract our error
        }
        publishHeapBytes();   // the frame buffer grew: refresh the memory readout
    }

    void freeFrame() {
        if (frame_) { platform::free(frame_); frame_ = nullptr; frameCap_ = 0; publishHeapBytes(); }
    }

protected:
    /// This driver's heap: the base scratch plus the frame buffer, for the memory readout.
    size_t driverHeapBytes() const override {
        return DriverBase::driverHeapBytes() + frameCap_;
    }

private:

    // --- loopback self-test (control-driven) ---

    // The status slot holds a pointer, not a copy, so only the FAIL case needs a buffer.
    void runLoopbackSelfTest() {
        if constexpr (platform::rmtTxChannels == 0) {
            clearFailBuf();
            setStatus("loopback: not supported on this platform", Severity::Warning);
            return;
        }
        if (pinCount_ == 0) {
            clearFailBuf();
            setStatus("loopback: no valid pins", Severity::Warning);
            return;
        }
        // Guard before the cast below, which would turn an unset -1 into GPIO 255.
        if (loopbackRxPin < 0) {
            clearFailBuf();
            setStatus("loopback: set loopbackRxPin (jumper it to the TX pin)", Severity::Status);
            return;
        }
        // Release every TX channel first, so the test's RX channel can always allocate.
        deinitAll();
        // Falls back to the first data pin when the override is unset.
        const uint8_t txPin = loopbackTxPin >= 0
            ? static_cast<uint8_t>(loopbackTxPin)
            : static_cast<uint8_t>(pinList_[0]);
        const uint8_t rxPin = static_cast<uint8_t>(loopbackRxPin);
        platform::RmtLoopbackResult r;
        if (loopbackFrame) {
            // The first pin's slice, or 64 lights: the size that exposes frame-rate corruption.
            const uint16_t lights = pinCounts_[0] > 0
                ? static_cast<uint16_t>(pinCounts_[0]) : 64;
            const uint8_t ch = correction_.outChannels ? correction_.outChannels : 3;
            r = platform::rmtWs2812LoopbackFrame(txPin, rxPin, lights, ch);
        } else {
            r = platform::rmtWs2812Loopback(txPin, rxPin);
        }
        reinit();
        if (!r.jumperDetected) {
            clearFailBuf();
            setStatus("loopback: jumper not detected", Severity::Warning);
        } else if (r.pass) {
            // A static literal: setStatus holds the pointer, so the string must outlive the call.
            clearFailBuf();
            setStatus("loopback PASS", Severity::Status);
        } else {
            failBufEnsure();
            if (failBuf_ && loopbackFrame) {
                // From the channel count the frame was built with, so RGBW indexes right too.
                const unsigned bitsPerLight =
                    (correction_.outChannels ? correction_.outChannels : 3u) * 8u;
                std::snprintf(failBuf_, kFailBufLen,
                              "loopback FAIL: bad bit %u/%u (light %u)",
                              static_cast<unsigned>(r.firstBadBit),
                              static_cast<unsigned>(r.bitsChecked),
                              static_cast<unsigned>(r.firstBadBit / bitsPerLight));
                setStatus(failBuf_, Severity::Error);
            } else if (failBuf_) {
                std::snprintf(failBuf_, kFailBufLen, "loopback FAIL: sent %02X%02X%02X got %02X%02X%02X",
                              r.sent[0], r.sent[1], r.sent[2], r.got[0], r.got[1], r.got[2]);
                setStatus(failBuf_, Severity::Error);
            } else {
                setStatus("loopback FAIL", Severity::Error);
            }
        }
    }

    // --- RMT channels (hardware; RMT targets only) ---

    static constexpr const char* kInitFailMsg = "RMT init failed, check the pins";
    static constexpr const char* kNoFrameMemMsg = "out of memory for this many lights";

    // All-or-nothing: one clear error beats some strands dark and some lit.
    void reinit() {
        if constexpr (platform::rmtTxChannels == 0) return;
        deinitAll();
        if (pinCount_ == 0) return;   // parse error: already in the status slot
        for (uint8_t i = 0; i < pinCount_; i++) {
            if (platform::rmtWs2812Init(rmt_[i], static_cast<uint8_t>(pinList_[i]),
                                        kResolutionHz, cfg_.invert)
                && pushBitTiming(i)) {   // the expander needs the bit shapes before the first frame
                continue;
            }
            // Name the failing pin, rather than going dark with no explanation.
            deinitAll();
            clearFailBuf();
            if (failBufEnsure()) {
                std::snprintf(failBuf_, kFailBufLen, "RMT init failed on pin %u",
                              static_cast<unsigned>(pinList_[i]));
                setStatus(failBuf_, Severity::Error);
            } else {
                setStatus(kInitFailMsg, Severity::Error);
            }
            return;
        }
        inited_ = true;
        // Guarded on the buffer error: an unguarded clear retracts it microseconds after it is set.
        if (failBuf_ && status() == failBuf_ && !frameUnusable_) clearFailBuf();
        if (status() == kInitFailMsg) clearStatus();
    }

    // Channels only, NOT the buffer: reinit calls this, so freeing here strands tick().
    void deinitAll() {
        if constexpr (platform::rmtTxChannels == 0) return;
        for (uint8_t i = 0; i < kMaxPins; i++) {
            if (rmt_[i].impl) platform::rmtWs2812Deinit(rmt_[i]);
        }
        inited_ = false;
    }
};

} // namespace mm
