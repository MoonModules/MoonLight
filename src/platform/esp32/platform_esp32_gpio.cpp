/// What the pin ownership map above is built from, combining two sources.
///
/// No chip type escapes this file: the module gets a plain capability.
///
/// @technical
///
/// ## Two sources, because one of the questions has no query
///
/// The SDK's own checks answer whether a pin is valid, output-capable and in the low-power domain, and those are always correct.
/// Whether a pin is a boot strap or belongs to the flash or external memory has no query at all, being board and datasheet knowledge.
/// So a small per-chip table carries it.
/// That table mirrors the hardware reference page, which is its one documented source, and the two are kept in step.
///
/// ## Two lists per chip, one of them conditional
///
/// Reserved pins corrupt the device if used, and straps change the boot mode if driven at reset.
/// The set is keyed on the build's target, so the build IS the chip variant, and a pin listed in neither is neither.
///
/// The second list is the pins the memory bus takes only when the module HAS external memory, which is a runtime fact rather than a build one.
/// One image runs on a bare module and on one with it fitted, so those are added only when it is actually present.
///
/// ## The package is read at runtime, not from the build
///
/// The system-in-package parts wire their in-package memory differently while the same firmware runs on all of them, so the package comes from the chip's own fuses.
/// On one of them four pads are not bonded at all, which in practice means the in-package parts use them.
/// Muxing a peripheral onto one wedges the flash cache, and the board resets with no panic and no dump.
/// Two neighboring pins ARE free on that package, which one bench board's microphone uses.
///
/// ## Pins are configured for input and output together
///
/// A plain output leaves the input buffer disabled, so a read returns nothing on a pin that is really driving high, and the map's live column then lies about it.
/// That cost a long debugging round on a relay that was working the whole time.
/// The input buffer costs nothing here and makes a driven pin readable, which is what every other peripheral on this chip already does.
///
/// ## Only the first converter, deliberately
///
/// One unit is opened on first use and kept, since the handle owns the peripheral and opening one per read would reconfigure it every tick.
/// The second converter is shared with the radio on every chip here and times out whenever the radio holds it.
/// A pin there would read fine on the bench and fail once the device joined a network, so refusing is the honest answer until something needs it.
///
/// A raw count is not a fixed fraction of full scale, every part's converter being nonlinear in its own way.
/// And the correction for its own silicon lives in the chip's fuses.
/// The scheme is chosen by which capability the target declares, curve fitting where it is supported and line fitting otherwise.

#include "platform/platform.h"

#include "sdkconfig.h"        // CONFIG_IDF_TARGET_*: selects the per-chip strap/reserved table
#include "soc/gpio_num.h"     // GPIO_IS_VALID_GPIO / GPIO_IS_VALID_OUTPUT_GPIO
#include "driver/gpio.h"      // gpio_get_level / gpio_get_drive_capability: the live-state reads
#include "driver/rtc_io.h"    // rtc_gpio_is_valid_gpio
#include "esp_adc/adc_oneshot.h"  // adcRead: the ADC1 oneshot unit
#include "esp_adc/adc_cali.h"      // adcReadMv: per-chip eFuse correction
#include "esp_adc/adc_cali_scheme.h"
#include "esp_efuse.h"        // esp_efuse_get_pkg_ver: the classic ESP32's PACKAGE decides its pin table
#include "soc/efuse_defs.h"   // EFUSE_RD_CHIP_VER_PKG_*: the package ids
#include "esp_heap_caps.h"    // heap_caps_get_total_size(MALLOC_CAP_SPIRAM): detect PSRAM without a new
                              // component dep (the heap component is always linked; esp_psram is not, and adding it to REQUIRES would switch main to strict mode, hiding the implicitly-available components the other platform files rely on)

#include <cstddef>
#include <cstdint>

namespace mm::platform {

namespace {

// The per-chip strap and reserved pins, from the hardware reference page: @xref{two-lists-per-chip-one-of-them-conditional|what each list means}. Both helpers are plain linear scans over tiny fixed arrays.
bool inList(uint8_t gpio, const uint8_t* list, size_t n) {
    for (size_t i = 0; i < n; i++) if (list[i] == gpio) return true;
    return false;
}

#if defined(CONFIG_IDF_TARGET_ESP32)
// Classic ESP32 in a plain module (WROOM / WROVER, a D0WD die): flash 6-11 always; 16/17 are the extra flash/PSRAM bus on WROVER modules only.
constexpr uint8_t kReserved[]        = {6, 7, 8, 9, 10, 11};
constexpr uint8_t kReservedIfPsram[] = {16, 17};
constexpr uint8_t kStrap[]           = {0, 2, 5, 12, 15};
// The package is read from the chip's own fuses at runtime: @xref{the-package-is-read-at-runtime-not-from-the-build|why, and what an unbonded pad costs}.
constexpr uint8_t kReservedPicoV302[] = {6, 9, 10, 11};
constexpr uint8_t kAbsentPicoV302[]   = {16, 17, 18, 23};
constexpr uint8_t kReservedPicoD4[]   = {6, 7, 8, 9, 10, 11, 16, 17};

/// The eFuse package id, read once (it cannot change after boot).
uint32_t packageId() {
    static const uint32_t pkg = esp_efuse_get_pkg_ver();
    return pkg;
}
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
// The wider memory bus reserves a second range, but only on a module that has it, and the mode is the image's rather than the part's.
// So a narrower build leaves those free, which one board drives panel lines on, and the wider image cannot run on that part anyway.
// The debug and serial roles are conflicts rather than reservations: they stay usable as ordinary pins, so they are not flagged here.
constexpr uint8_t kReserved[]        = {26, 27, 28, 29, 30, 31, 32};
#if CONFIG_SPIRAM_MODE_OCT
constexpr uint8_t kReservedIfPsram[] = {33, 34, 35, 36, 37};
#else
constexpr uint8_t kReservedIfPsram[] = {};
#endif
constexpr uint8_t kStrap[]           = {0, 3, 45, 46};
#elif defined(CONFIG_IDF_TARGET_ESP32P4)
// ESP32-P4: flash/PSRAM are module-internal (the SDK's valid-GPIO query already excludes the bonded ones on a given package); straps 34-38.
constexpr uint8_t kReserved[]        = {};
constexpr uint8_t kReservedIfPsram[] = {};
constexpr uint8_t kStrap[]           = {34, 35, 36, 37, 38};
#elif defined(CONFIG_IDF_TARGET_ESP32S31)
// ESP32-S31: flash/PSRAM module-internal (SDK query excludes them). Board-wired peripheral pins (RGMII/codec/SD) are a per-board concern the catalog owns, not a chip strap/reserved fact.
constexpr uint8_t kReserved[]        = {};
constexpr uint8_t kReservedIfPsram[] = {};
constexpr uint8_t kStrap[]           = {};
#else
constexpr uint8_t kReserved[]        = {};
constexpr uint8_t kReservedIfPsram[] = {};
constexpr uint8_t kStrap[]           = {};
#endif

// Queried once and cached: PSRAM presence cannot change after boot. The heap-caps total for the SPIRAM region is the cheap read, 0 meaning none (the heap component is always linked, unlike esp_psram, see the include note).
bool psramPresent() {
    static const bool present = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
    return present;
}

}  // namespace

GpioCapability gpioCapability(uint8_t gpio) {
    GpioCapability c;
    c.validGpio     = GPIO_IS_VALID_GPIO(gpio);
    c.outputCapable = GPIO_IS_VALID_OUTPUT_GPIO(gpio);
    c.rtc           = rtc_gpio_is_valid_gpio(static_cast<gpio_num_t>(gpio));
    c.strap         = inList(gpio, kStrap, sizeof(kStrap));
    // kReservedIfPsram is gated on PSRAM being PRESENT, except where the image itself fixes the pins: an octal-PSRAM S3 build wires 33-37 to the PSRAM bus whatever the runtime finds, and a board whose PSRAM failed to init would otherwise hand those pads out as free GPIO.
#if defined(CONFIG_IDF_TARGET_ESP32S3) && CONFIG_SPIRAM_MODE_OCT
    constexpr bool kPsramPinsFixedByImage = true;
#else
    constexpr bool kPsramPinsFixedByImage = false;
#endif
    c.reserved      = inList(gpio, kReserved, sizeof(kReserved)) ||
                      ((kPsramPinsFixedByImage || psramPresent()) &&
                       inList(gpio, kReservedIfPsram, sizeof(kReservedIfPsram)));
#if defined(CONFIG_IDF_TARGET_ESP32)
    // The die's valid-GPIO mask does not know the package; the tables above do (see their note).
    switch (packageId()) {
    case EFUSE_RD_CHIP_VER_PKG_ESP32PICOV302:
        c.reserved  = inList(gpio, kReservedPicoV302, sizeof(kReservedPicoV302));
        c.validGpio = c.validGpio && !inList(gpio, kAbsentPicoV302, sizeof(kAbsentPicoV302));
        break;
    case EFUSE_RD_CHIP_VER_PKG_ESP32PICOD4:
        c.reserved  = inList(gpio, kReservedPicoD4, sizeof(kReservedPicoD4));
        break;
    default:
        break;
    }
#endif
    return c;
}

const char* gpioRefusal(uint8_t gpio) {
    const GpioCapability c = gpioCapability(gpio);
    if (!c.validGpio) return "does not exist on this chip package";
    if (c.reserved)   return "is wired to flash/PSRAM on this chip";
    return nullptr;
}

GpioLiveState gpioLiveState(uint8_t gpio) {
    GpioLiveState s;
    if (!GPIO_IS_VALID_GPIO(gpio)) return s;   // valid=false → the map omits the live columns
    s.valid = true;
    s.level = gpio_get_level(static_cast<gpio_num_t>(gpio)) != 0;   // reads the pad, see the wire
    gpio_drive_cap_t cap = GPIO_DRIVE_CAP_DEFAULT;
    gpio_get_drive_capability(static_cast<gpio_num_t>(gpio), &cap);
    s.driveCap = static_cast<uint8_t>(cap);    // 0..3 = WEAK / MEDIUM / STRONG / STRONGEST
    // Live pin DIRECTION straight off the pad config (not the role's intent): is the output driver / input buffer enabled right now. A role that should drive but reads back !output = the pin isn't being driven (a dead driver or wire fault), the mismatch the map flags.
    gpio_io_config_t io = {};
    if (gpio_get_io_config(static_cast<gpio_num_t>(gpio), &io) == ESP_OK) {
        s.output = io.oe;
        s.input  = io.ie;
    }
    return s;
}

// --- GPIO as a working input/output -----------------------------------------------------------
// The two above are the pin map's diagnostics. These are the role: a module that owns a pin reads a switch or drives a line through them. Thin by design - gpio_config once, then the register-level get/set - so a per-tick poll costs a read and the module keeps the policy (debounce, edges).

bool gpioInputBegin(uint8_t gpio, GpioPull pull) {
    if (!GPIO_IS_VALID_GPIO(gpio)) return false;
    // A RESERVED pin is wired to flash, PSRAM or native USB, and routing I/O there corrupts the device (gpioCapability's own words). Refused rather than configured: the caller reports a pin it could not open, where a corrupted flash reports nothing at all.
    if (gpioCapability(gpio).reserved) return false;
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << gpio;
    cfg.mode         = GPIO_MODE_INPUT;
    cfg.pull_up_en   = pull == GpioPull::Up   ? GPIO_PULLUP_ENABLE   : GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = pull == GpioPull::Down ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE;
    cfg.intr_type    = GPIO_INTR_DISABLE;      // polled, not interrupt-driven: see the seam's docs
    return gpio_config(&cfg) == ESP_OK;
}

bool gpioRead(uint8_t gpio) {
    if (!GPIO_IS_VALID_GPIO(gpio)) return false;
    return gpio_get_level(static_cast<gpio_num_t>(gpio)) != 0;
}

bool gpioWrite(uint8_t gpio, bool high) {
    // Output-capable, not merely valid: the classic ESP32's 34-39 are input-only, and driving one silently does nothing. Refusing here is what lets a caller report the pin rather than wonder.
    if (!GPIO_IS_VALID_OUTPUT_GPIO(gpio)) return false;
    // And not RESERVED, the same policy gpioInputBegin applies: a pin wired to flash, PSRAM or native USB corrupts the device when driven, and a relay list is exactly where a wrong number gets typed.
    if (gpioCapability(gpio).reserved) return false;
    // Configured on first use so a caller that owns the pin just writes it. gpio_config is idempotent, and this runs on a control change, never per frame.
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << gpio;
    // Input and output together: @xref{pins-are-configured-for-input-and-output-together|why a plain output makes the map lie}.
    cfg.mode         = GPIO_MODE_INPUT_OUTPUT;
    cfg.intr_type    = GPIO_INTR_DISABLE;
    if (gpio_config(&cfg) != ESP_OK) return false;
    return gpio_set_level(static_cast<gpio_num_t>(gpio), high ? 1 : 0) == ESP_OK;
}

// The analogue converter: @xref{only-the-first-converter-deliberately|why one unit, and why not the second}.
namespace {
adc_oneshot_unit_handle_t g_adc1 = nullptr;
bool g_adcChanReady[ADC_CHANNEL_9 + 1] = {};

/// The ADC1 channel this GPIO is, or -1. Per chip, because the mapping is fixed in silicon.
int adc1ChannelFor(uint8_t gpio) {
    adc_unit_t unit;
    adc_channel_t chan;
    if (adc_oneshot_io_to_channel(static_cast<int>(gpio), &unit, &chan) != ESP_OK) return -1;
    if (unit != ADC_UNIT_1) return -1;      // ADC2 races the WiFi radio: see above
    return static_cast<int>(chan);
}
}  // namespace

bool adcRead(uint8_t gpio, uint16_t& raw) {
    const int chan = adc1ChannelFor(gpio);
    if (chan < 0) return false;
    if (!g_adc1) {
        adc_oneshot_unit_init_cfg_t init = {};
        init.unit_id = ADC_UNIT_1;
        if (adc_oneshot_new_unit(&init, &g_adc1) != ESP_OK) { g_adc1 = nullptr; return false; }
    }
    if (!g_adcChanReady[chan]) {
        adc_oneshot_chan_cfg_t cfg = {};
        // 12 dB (the full ~0..3.1 V span) and the chip's widest resolution: a sense divider and a pedal both swing across the whole range, and a narrower attenuation would clip them silently. adcMaxCount() reports the matching full scale so a caller scales correctly.
        cfg.atten    = ADC_ATTEN_DB_12;
        cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
        if (adc_oneshot_config_channel(g_adc1, static_cast<adc_channel_t>(chan), &cfg) != ESP_OK)
            return false;
        g_adcChanReady[chan] = true;
    }
    int value = 0;
    if (adc_oneshot_read(g_adc1, static_cast<adc_channel_t>(chan), &value) != ESP_OK) return false;
    raw = static_cast<uint16_t>(value < 0 ? 0 : value);
    return true;
}

uint16_t adcMaxCount() { return 4095; }    // 12 bits, matching ADC_BITWIDTH_DEFAULT on these chips

// Calibrated voltages, applied through a handle created once per unit and kept, exactly as the unit's own handle is.
namespace {
adc_cali_handle_t g_adcCali = nullptr;
bool g_adcCaliTried = false;

/// The calibration handle at our attenuation, or nothing where the chip carries no factory data; attempted once, since a part without it would otherwise retry on every read.
adc_cali_handle_t adcCali() {
    if (g_adcCaliTried) return g_adcCali;
    g_adcCaliTried = true;
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cfg = {};
    cfg.unit_id  = ADC_UNIT_1;
    cfg.atten    = ADC_ATTEN_DB_12;
    cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_cali_create_scheme_curve_fitting(&cfg, &g_adcCali) != ESP_OK) g_adcCali = nullptr;
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cfg = {};
    cfg.unit_id  = ADC_UNIT_1;
    cfg.atten    = ADC_ATTEN_DB_12;
    cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_cali_create_scheme_line_fitting(&cfg, &g_adcCali) != ESP_OK) g_adcCali = nullptr;
#endif
    return g_adcCali;
}
}  // namespace

bool adcReadMv(uint8_t gpio, uint16_t& mv) {
    uint16_t raw = 0;
    if (!adcRead(gpio, raw)) return false;
    adc_cali_handle_t cali = adcCali();
    if (!cali) return false;                    // no eFuse calibration: refuse rather than guess
    int out = 0;
    if (adc_cali_raw_to_voltage(cali, static_cast<int>(raw), &out) != ESP_OK) return false;
    mv = static_cast<uint16_t>(out < 0 ? 0 : out);
    return true;
}

// The test seams are desktop-only: on a board the pins are real, and a test that wants to inject a level has the hardware to do it.
void setTestGpioLevel(uint8_t, bool) {}
void clearTestGpioLevel() {}
void setTestAdcValue(uint8_t, uint16_t) {}
void setTestAdcMv(uint8_t, uint16_t) {}
void clearTestAdcValue() {}

}  // namespace mm::platform
