/// @defgroup platform_esp32_pwm PWM output on LEDC
/// The LEDC peripheral behind the PWM functions: timers set the frequency, channels drive the pins.

#include "platform/platform.h"

#include "driver/ledc.h"
#include "soc/soc_caps.h"

#include <cstdint>

namespace mm::platform {

namespace {

#if SOC_LEDC_SUPPORT_HS_MODE
constexpr int kModes = 2;
#else
constexpr int kModes = 1;
#endif
// One row of channels across the classic ESP32's two speed banks, so a driver never sees how the chip splits them.
constexpr int kChannels = SOC_LEDC_CHANNEL_NUM * kModes;

struct PwmChannel { int timer = -1; };
bool g_timerUsed[SOC_LEDC_TIMER_NUM] = {};
uint8_t g_timerBits[SOC_LEDC_TIMER_NUM] = {};
PwmChannel g_channels[kChannels];

ledc_mode_t modeOf(int channel) {
#if SOC_LEDC_SUPPORT_HS_MODE
    if (channel >= SOC_LEDC_CHANNEL_NUM) return LEDC_HIGH_SPEED_MODE;
#endif
    return LEDC_LOW_SPEED_MODE;
}

ledc_channel_t indexOf(int channel) { return static_cast<ledc_channel_t>(channel % SOC_LEDC_CHANNEL_NUM); }

// The timer in one bank at these bits; true when the hardware accepts the pair.
bool configure(ledc_mode_t mode, int timer, uint32_t frequency, uint8_t bits) {
    ledc_timer_config_t t = {};
    t.speed_mode = mode;
    t.duty_resolution = static_cast<ledc_timer_bit_t>(bits);
    t.timer_num = static_cast<ledc_timer_t>(timer);
    t.freq_hz = frequency;
    t.clk_cfg = LEDC_AUTO_CLK;
    return ledc_timer_config(&t) == ESP_OK;
}

// The timer in every bank, so any channel can follow it, at the most bits the hardware accepts; 0 when none.
uint8_t configureAll(int timer, uint32_t frequency) {
    // Down from what an 80 MHz clock allows, since the automatic clock choice differs per chip.
    uint8_t bits = 0;
    while (bits < SOC_LEDC_TIMER_BIT_WIDTH && (uint64_t{80'000'000} >> (bits + 1)) >= frequency) bits++;
    for (; bits > 0; bits--) {
        bool ok = true;
        for (int m = 0; m < kModes && ok; m++) ok = configure(modeOf(m * SOC_LEDC_CHANNEL_NUM), timer, frequency, bits);
        if (ok) return bits;
    }
    return 0;
}

}  // namespace

uint8_t pwmChannelCount() MM_NONBLOCKING { return kChannels; }

int pwmStart(uint32_t frequency, uint8_t& bits) {
    if (frequency == 0) return -1;
    for (int t = 0; t < SOC_LEDC_TIMER_NUM; t++) {
        if (g_timerUsed[t]) continue;
        bits = configureAll(t, frequency);
        if (bits == 0) return -1;
        g_timerUsed[t] = true;
        g_timerBits[t] = bits;
        return t;
    }
    return -1;
}

int pwmAttach(int timer, uint8_t pin, uint32_t phase) {
    if (timer < 0 || timer >= SOC_LEDC_TIMER_NUM || !g_timerUsed[timer]) return -1;
    for (int c = 0; c < kChannels; c++) {
        if (g_channels[c].timer >= 0) continue;
        ledc_channel_config_t cfg = {};
        cfg.gpio_num = pin;
        cfg.speed_mode = modeOf(c);
        cfg.channel = indexOf(c);
        cfg.timer_sel = static_cast<ledc_timer_t>(timer);
        cfg.duty = 0;
        cfg.hpoint = static_cast<int>(phase);
        if (ledc_channel_config(&cfg) != ESP_OK) return -1;
        g_channels[c].timer = timer;
        return c;
    }
    return -1;
}

// The two calls are register writes under a spinlock, which never waits on another task.
void pwmWrite(int channel, uint32_t duty) MM_NONBLOCKING {
    if (channel < 0 || channel >= kChannels || g_channels[channel].timer < 0) return;
    ledc_set_duty(modeOf(channel), indexOf(channel), duty);
    ledc_update_duty(modeOf(channel), indexOf(channel));
}

void pwmStop(int timer) {
    if (timer < 0 || timer >= SOC_LEDC_TIMER_NUM || !g_timerUsed[timer]) return;
    // Stopped low, then deconfigured, which hands each pin back so the next layout can claim it again.
    for (int c = 0; c < kChannels; c++) {
        if (g_channels[c].timer != timer) continue;
        ledc_stop(modeOf(c), indexOf(c), 0);
        ledc_channel_config_t cfg = {};
        cfg.speed_mode = modeOf(c);
        cfg.channel = indexOf(c);
        cfg.deconfigure = true;
        ledc_channel_config(&cfg);
        g_channels[c].timer = -1;
    }
    for (int m = 0; m < kModes; m++) {
        const ledc_mode_t mode = modeOf(m * SOC_LEDC_CHANNEL_NUM);
        ledc_timer_pause(mode, static_cast<ledc_timer_t>(timer));
        ledc_timer_config_t t = {};
        t.speed_mode = mode;
        t.timer_num = static_cast<ledc_timer_t>(timer);
        t.deconfigure = true;
        ledc_timer_config(&t);
    }
    g_timerUsed[timer] = false;
}

uint32_t pwmDutyForTest(int /*channel*/) { return 0; }

int pwmPinForTest(int /*channel*/) { return -1; }

uint32_t pwmPhaseForTest(int /*channel*/) { return 0; }

}  // namespace mm::platform
