#pragma once

#include "light/drivers/DriverBase.h"
#include "light/drivers/PinList.h"   // parsePinList: the GPIO list every pin driver reads
#include "core/util/format.h"          // formatTo: the pin a refused channel names

#include <cstdint>
#include <cstring>

namespace mm {

/// Lights whose channels are each a pin, pulsed by the chip's PWM: a bulb, an analog strip, a constant-current controller.
///
/// `pins` lists one GPIO per output channel and the fixture profile names each channel, so the light count is the pins divided by the profile's channels.
/// `frequency` sets the pulse rate, and the resolution follows from it: the most duty bits one period holds.
class PwmLightDriver : public DriverBase {
public:
    /// One pin per channel of a plain RGB light, until the profile says otherwise; the curve reaches the timer in 16 bits, so a dim fade keeps its steps.
    PwmLightDriver() {
        setDefaultFixtureName("RGB");
        correction_.wide = true;
    }

    /// The most pins one driver drives, the classic ESP32's channel count.
    static constexpr uint8_t kMaxPins = 16;

    char pins[64] = "";              ///< one GPIO per output channel, in the profile's channel order
    uint16_t frequency = 19531;      ///< the pulse rate in Hz, QuinLED's recommended rate

    /// The window, the pins and the frequency.
    void defineDriverControls() override {
        addWindowControls();
        controls_.addText("pins", pins, sizeof(pins));
        controls_.addControl("frequency", frequency, 100, 40000);
        controls_.setNumberField(controls_.count() - 1);   // typed, since a fixture's datasheet names an exact rate
    }

    /// The pins, the frequency and the window re-lay the outputs.
    bool affectsPrepare(const char* name) const override {
        return std::strcmp(name, "pins") == 0 || std::strcmp(name, "frequency") == 0 || isWindowControl(name);
    }

    /// Read the window from `buf`.
    void setSourceBuffer(Buffer* buf) override { sourceBuffer_ = buf; }

    /// Start one timer, attach a channel per pin with evenly spaced phases, and say how many lights that is.
    void prepare() override {
        stop();
        uint8_t n = 0;
        const uint8_t limit = platform::pwmChannelCount() < kMaxPins ? platform::pwmChannelCount() : kMaxPins;
        if (!pins[0]) { setConfigWarn("set the pins"); return; }
        if (const char* err = parsePinList(pins, pinList_, limit, n)) { setConfigErr(err); return; }
        clearConfigErr();
        layOut(n);
    }

    /// A profile with another channel count regroups the pins into lights, the timer left running.
    void onCorrectionChanged() override {
        if (outChannelsChanged() && timer_ >= 0) group();
    }

    /// Correct each light of the window and write the duties that changed.
    void tick() MM_NONBLOCKING override {
        if (timer_ < 0 || lights_ == 0 || !sourceBuffer_ || !sourceBuffer_->data()) return;
        const uint8_t outCh = correction_.outChannels;
        const uint8_t srcCh = sourceBuffer_->channelsPerLight();
        nrOfLightsType winStart = 0, winLen = 0;
        windowSlice(sourceBuffer_->count(), winStart, winLen);
        const uint8_t lights = winLen < lights_ ? static_cast<uint8_t>(winLen) : lights_;
        uint16_t out[kMaxPins];
        uint8_t narrow[kMaxPins];
        for (uint8_t l = 0; l < lights; l++) {
            const uint8_t* src = sourceBuffer_->data() + static_cast<size_t>(winStart + l) * srcCh;
            // Without its table (an allocation that failed) the light stays 8-bit, widened.
            if (correction_.lut16) correction_.applyWide(src, out, srcCh);
            else {
                correction_.apply(src, narrow, srcCh);
                for (uint8_t c = 0; c < outCh; c++) out[c] = static_cast<uint16_t>(narrow[c] * 257);
            }
            for (uint8_t c = 0; c < outCh; c++) write(l * outCh + c, out[c]);
        }
    }

    /// Release the timer and its channels, the pins driven low.
    void release() override {
        stop();
        DriverBase::release();
    }

    /// The duty steps of a full channel, for the tests.
    uint32_t maxDutyForTest() const { return maxDuty_; }
    /// The platform channel behind output `i`, for the tests.
    int channelForTest(uint8_t i) const { return i < pinCount_ ? channels_[i] : -1; }
    /// How many whole lights the pins make, for the tests.
    uint8_t lightsForTest() const { return lights_; }

private:
    Buffer* sourceBuffer_ = nullptr;
    uint16_t pinList_[kMaxPins] = {};
    uint8_t pinCount_ = 0;
    int timer_ = -1;
    int channels_[kMaxPins] = {};
    uint32_t duty_[kMaxPins] = {};
    uint32_t maxDuty_ = 0;
    uint8_t lights_ = 0;
    char attachErr_[64] = "";

    // Scaled from 16 bits to the resolution and rounded, so 65535 is fully on and a lit value never rounds to dark.
    void write(uint8_t output, uint16_t value) MM_NONBLOCKING {
        uint32_t duty = static_cast<uint32_t>((uint64_t{value} * maxDuty_ + 32767) / 65535);
        if (duty == 0 && value) duty = 1;
        if (duty == duty_[output]) return;
        duty_[output] = duty;
        platform::pwmWrite(channels_[output], duty);
    }

    // Each channel's pulse starts a share of the period later than the one before, so full white does not switch every channel at once.
    void layOut(uint8_t n) {
        stop();
        pinCount_ = n;
        uint8_t bits = 0;
        timer_ = platform::pwmStart(frequency, bits);
        if (timer_ < 0) { setConfigErr("no PWM timer free at this frequency"); return; }
        maxDuty_ = (uint32_t{1} << bits) - 1;
        for (uint8_t i = 0; i < n; i++) {
            channels_[i] = platform::pwmAttach(timer_, static_cast<uint8_t>(pinList_[i]), (maxDuty_ + 1) / n * i);
            duty_[i] = 0;
            if (channels_[i] < 0) {
                stop();
                formatTo(attachErr_, sizeof(attachErr_), "GPIO %u takes no PWM channel: not an output, or none free", static_cast<unsigned>(pinList_[i]));
                setConfigErr(attachErr_);
                return;
            }
        }
        group();
    }

    // The pins as whole lights of the profile's channels; the ones past the last whole light go dark.
    void group() {
        const uint8_t outCh = correction_.outChannels;
        lights_ = outCh ? static_cast<uint8_t>(pinCount_ / outCh) : 0;
        for (uint8_t i = lights_ * outCh; i < pinCount_; i++) write(i, 0);
        const bool partial = lights_ * outCh < pinCount_;
        setConfigWarn(partial ? "pins past the last whole light idle" : nullptr);
        if (!partial && lights_ > 0) setDrivingInfo(lights_, lights_, outCh);
    }

    void stop() {
        if (timer_ >= 0) platform::pwmStop(timer_);
        timer_ = -1;
        lights_ = 0;
    }
};

} // namespace mm
