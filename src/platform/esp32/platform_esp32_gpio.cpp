/// What the pin ownership map above is built from, combining two sources.
///
/// No chip type escapes this file: the module gets a plain capability.
///
/// @technical
///
/// ## Two sources, because one of the questions has no query
///
/// The SDK's own checks answer whether a pin is valid, output-capable and in the low-power domain, and those are always correct.
///
/// Whether a pin is a boot strap or belongs to the flash or external memory has no query, being board and datasheet knowledge, so a small per-chip table carries it.
///
/// That table mirrors the hardware reference page, which is its one documented source, and the two are kept in step.
///
/// ## Two lists per chip, one of them conditional
///
/// Reserved pins corrupt the device if used, and straps change the boot mode if driven at reset; the set is keyed on the build's target, and every pin outside both lists is free.
///
/// The second list is the pins the memory bus takes only on a module WITH external memory, a runtime fact: one image runs on both, so those are added when it is present.
///
/// ## The package is read at runtime, not from the build
///
/// The system-in-package parts wire their in-package memory differently while the same firmware runs on all of them, so the package comes from the chip's own fuses.
///
/// On one of them four pads are unbonded, used by the in-package parts: muxing a peripheral onto one wedges the flash cache, and the board resets with no panic and no dump.
///
/// Two neighboring pins ARE free on that package, which one bench board's microphone uses.
///
/// ## Pins are configured for input and output together
///
/// A plain output leaves the input buffer disabled, so a read of a pin driving high returns low and the map's live column lies; that cost a long round on a working relay.
///
/// The input buffer is free here and makes a driven pin readable, which is what every other peripheral on this chip already does.
///
/// ## Only the first converter, deliberately
///
/// One unit is opened on first use and kept, since the handle owns the peripheral and opening one per read would reconfigure it every tick.
///
/// The second converter is shared with the radio on every chip here and times out whenever the radio holds it: a pin there reads fine on the bench and fails on a network.
///
/// A raw count is no fixed fraction of full scale, each converter being nonlinear in its own way, and the correction for its silicon lives in the chip's fuses.
///
/// The scheme follows the capability the target declares: curve fitting where it is supported, line fitting otherwise.

#include "platform/platform.h"

#include "sdkconfig.h"        // CONFIG_IDF_TARGET_*: selects the per-chip strap/reserved table
#include "soc/gpio_num.h"     // GPIO_IS_VALID_GPIO / GPIO_IS_VALID_OUTPUT_GPIO
#include "soc/io_mux_reg.h"   // USB_INT_PHY0_D[MP]_GPIO_NUM: the pads the USB serial/JTAG port owns
#include "soc/soc_caps.h"     // SOC_USB_SERIAL_JTAG_SUPPORTED
#include "driver/gpio.h"      // gpio_get_level / gpio_get_drive_capability: the live-state reads
#include "driver/rtc_io.h"    // rtc_gpio_is_valid_gpio
#include "esp_adc/adc_oneshot.h"  // adcRead: the ADC1 oneshot unit
#include "esp_adc/adc_cali.h"      // adcReadMv: per-chip eFuse correction
#include "esp_adc/adc_cali_scheme.h"
#include "esp_efuse.h"        // esp_efuse_get_pkg_ver: the classic ESP32's PACKAGE decides its pin table
#include "soc/efuse_defs.h"   // EFUSE_RD_CHIP_VER_PKG_*: the package ids
#include "hal/efuse_hal.h"    // efuse_hal_chip_revision: the PSRAM rule's revision and a package's -V3
#include "esp_heap_caps.h"    // heap_caps_get_total_size(MALLOC_CAP_SPIRAM): PSRAM read from the always-linked heap, since esp_psram in REQUIRES would put main in strict mode

#include <cstddef>
#include <cstdint>

#if defined(CONFIG_IDF_TARGET_ESP32) && defined(CONFIG_SPIRAM) && !defined(CONFIG_SPIRAM_BOOT_INIT)
#include "esp_psram.h"                       // esp_psram_init / esp_psram_is_initialized
#include "esp_private/esp_psram_extram.h"    // esp_psram_extram_add_to_heap_allocator: ESP-IDF's own boot heap step, which has no public twin
#include "esp_private/startup_internal.h"    // ESP_SYSTEM_INIT_FN: the startup stage ESP-IDF's own PSRAM heap step runs in
#include "esp_private/esp_gpio_reserve.h"    // esp_gpio_revoke: the pads a failed PSRAM probe keeps reserved
#include "platform/esp32/psram_rule.h"

// The heap step ESP-IDF's boot init takes at priority 103, taken here at 104 (before constructors and app_main) only where psramHeapSafe allows, since one classic image runs on every revision.
ESP_SYSTEM_INIT_FN(mm_psram_heap, CORE, BIT(0), 104) {
#ifdef CONFIG_SPIRAM_CACHE_WORKAROUND
    constexpr bool kCacheWorkaround = true;
#else
    constexpr bool kCacheWorkaround = false;
#endif
    if (esp_psram_init() != ESP_OK) {   // no PSRAM: the probe reserved its pins before the ID read and kept them
        esp_gpio_revoke(mm::platform::psramPadsFreedWhenAbsent(esp_efuse_get_pkg_ver()));
        return ESP_OK;
    }
    if (!mm::platform::psramHeapSafe(efuse_hal_chip_revision(), kCacheWorkaround)) return ESP_OK;   // found, left unused
    if (esp_psram_extram_add_to_heap_allocator() != ESP_OK) return ESP_OK;
#ifdef CONFIG_SPIRAM_USE_MALLOC
    heap_caps_malloc_extmem_enable(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL);
#endif
    return ESP_OK;
}
#endif

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
// The octal bus reserves 33-37 in an octal image only (a quad build keeps them for one board's panel lines); debug and serial pins stay ordinary pins, so they are left out.
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
#if defined(CONFIG_IDF_TARGET_ESP32) && defined(CONFIG_SPIRAM) && !defined(CONFIG_SPIRAM_BOOT_INIT)
    static const bool present = esp_psram_is_initialized();   // found, in the heap or left unused: either way its chip owns the bus pins
#else
    static const bool present = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
#endif
    return present;
}
// The two pads the USB serial/JTAG port owns, where the chip has one.
bool usbSerialPad(uint8_t gpio) {
#if SOC_USB_SERIAL_JTAG_SUPPORTED && defined(USB_INT_PHY0_DP_GPIO_NUM)
    return gpio == USB_INT_PHY0_DM_GPIO_NUM || gpio == USB_INT_PHY0_DP_GPIO_NUM;
#else
    (void)gpio;
    return false;
#endif
}

}  // namespace

size_t psramFoundBytes() {
#if defined(CONFIG_IDF_TARGET_ESP32) && defined(CONFIG_SPIRAM) && !defined(CONFIG_SPIRAM_BOOT_INIT)
    static const size_t bytes = esp_psram_is_initialized() ? esp_psram_get_size() : 0;   // found, in the heap or left unused (psramHeapSafe)
#else
    static const size_t bytes = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
#endif
    return bytes;
}

const char* chipPackage() {
#if defined(CONFIG_IDF_TARGET_ESP32)
    // efuse_defs.h's package codes under the part names esptool prints, the datasheet's; a revision 3 die adds -V3 where esptool does.
    struct Package { uint32_t code; const char* name; const char* v3; };
    static constexpr Package kPackages[] = {
        {EFUSE_RD_CHIP_VER_PKG_ESP32D0WDQ6,   "D0WDQ6",     "D0WDQ6-V3"},
        {EFUSE_RD_CHIP_VER_PKG_ESP32D0WDQ5,   "D0WD",       "D0WD-V3"},
        {EFUSE_RD_CHIP_VER_PKG_ESP32D2WDQ5,   "D2WD",       "D2WD"},
        {EFUSE_RD_CHIP_VER_PKG_ESP32U4WDH,    "U4WDH",      "U4WDH"},      // code 4, which ESP-IDF also names PICO-D2, a part that stayed a sample
        {EFUSE_RD_CHIP_VER_PKG_ESP32PICOD4,   "PICO-D4",    "PICO-D4"},
        {EFUSE_RD_CHIP_VER_PKG_ESP32PICOV302, "PICO-V3-02", "PICO-V3-02"},
        {EFUSE_RD_CHIP_VER_PKG_ESP32D0WDR2V3, "D0WDR2-V3",  "D0WDR2-V3"},
    };
    const uint32_t code = esp_efuse_get_pkg_ver();
    for (const Package& p : kPackages)
        if (p.code == code) return efuse_hal_chip_revision() >= 300 ? p.v3 : p.name;
    return "unknown";
#else
    return "";
#endif
}

GpioCapability gpioCapability(uint8_t gpio) {
    GpioCapability c;
    c.validGpio     = GPIO_IS_VALID_GPIO(gpio);
    c.outputCapable = GPIO_IS_VALID_OUTPUT_GPIO(gpio);
    c.rtc           = rtc_gpio_is_valid_gpio(static_cast<gpio_num_t>(gpio));
    c.strap         = inList(gpio, kStrap, sizeof(kStrap));
    // kReservedIfPsram follows PSRAM PRESENT, except where the image fixes the pins: an octal S3 build wires 33-37 to the bus whatever it finds, so a failed init frees no pads.
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
    c.usbSerial     = usbSerialPad(gpio);
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
    // Live pin DIRECTION off the pad config, the role's intent aside: a role that should drive but reads back as input is a dead driver or wire fault, the mismatch the map flags.
    gpio_io_config_t io = {};
    if (gpio_get_io_config(static_cast<gpio_num_t>(gpio), &io) == ESP_OK) {
        s.output = io.oe;
        s.input  = io.ie;
    }
    return s;
}

// --- GPIO as a working input/output -----------------------------------------------------------
// The pin map's diagnostics are above; these are the role, thin by design: gpio_config once, then register-level get/set, so a poll costs one read and the module keeps the policy.

bool gpioInputBegin(uint8_t gpio, GpioPull pull) {
    if (!GPIO_IS_VALID_GPIO(gpio)) return false;
    // A RESERVED pin is wired to flash or PSRAM, and routing I/O there corrupts the device (gpioCapability's own words). Refused rather than configured: the caller reports a pin it could not open, where a corrupted flash is silent.
    if (gpioCapability(gpio).reserved) return false;
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << gpio;
    cfg.mode         = GPIO_MODE_INPUT;
    // An input-only pad (the classic ESP32's 34-39) has no internal pull, so the board's own resistor serves and the pull is left out.
    const bool hasPulls = GPIO_IS_VALID_OUTPUT_GPIO(gpio);
    cfg.pull_up_en   = hasPulls && pull == GpioPull::Up   ? GPIO_PULLUP_ENABLE   : GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = hasPulls && pull == GpioPull::Down ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE;
    cfg.intr_type    = GPIO_INTR_DISABLE;      // polled, not interrupt-driven: see the seam's docs
    return gpio_config(&cfg) == ESP_OK;
}

bool gpioRead(uint8_t gpio) {
    if (!GPIO_IS_VALID_GPIO(gpio)) return false;
    return gpio_get_level(static_cast<gpio_num_t>(gpio)) != 0;
}

bool gpioWrite(uint8_t gpio, bool high) {
    // Output-capable, beyond valid: the classic ESP32's 34-39 are input-only and ignore a drive, so refusing here lets a caller report the pin.
    if (!GPIO_IS_VALID_OUTPUT_GPIO(gpio)) return false;
    // And not RESERVED, the same policy gpioInputBegin applies: a pin wired to flash or PSRAM corrupts the device when driven, and a relay list is exactly where a wrong number gets typed.
    if (gpioCapability(gpio).reserved) return false;
    // Configured on first use so a caller that owns the pin writes it directly; gpio_config is idempotent, and this runs on a control change, once, outside the frame.
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
        // 12 dB (the full ~0..3.1 V span) at the widest resolution, since a sense divider and a pedal swing across the whole range; adcMaxCount() reports the matching full scale.
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

/// The calibration handle at our attenuation, or null where the chip lacks factory data; attempted once, so a part that lacks it is asked a single time.
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

// The test seams are host-only: on a board the pins are real, and a test that wants to inject a level has the hardware to do it.
void setTestGpioLevel(uint8_t, bool) {}
void clearTestGpioLevel() {}
void setTestAdcValue(uint8_t, uint16_t) {}
void setTestAdcMv(uint8_t, uint16_t) {}
void clearTestAdcValue() {}

}  // namespace mm::platform
