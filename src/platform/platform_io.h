#pragma once
/// The platform's general inputs and outputs: GPIO capability and live state, GPIO as a switch or a line, the ADC, infrared, the I2C bus and a USB MIDI desk.
///
/// One concern of the platform interface ([platform.h](platform.h) gathers them all), so a file includes the concerns it uses and an edit to one rebuilds its users alone.

#include "platform/platform_base.h"   // target flags, MM_NONBLOCKING and the compiler macros

namespace mm::platform {


// --- GPIO capability introspection (PinsModule) ---------------------------------------------

/// What one GPIO is, so the pin ownership map can flag a claim landing on an unsafe pin.
///
/// @technical The SDK answers the first three fields; `strap` and `reserved` come from a per-chip table, being datasheet knowledge.
/// Host reports every pin valid and free, a host build having no real pins to protect.
struct GpioCapability {
    bool validGpio = true;      ///< a real, usable GPIO on this chip
    bool outputCapable = true;  ///< has an output driver (classic ESP32 34-39 are input-only)
    bool rtc = false;           ///< an RTC pin, usable for deep-sleep wake and RTC I/O
    bool strap = false;         ///< a boot-strapping pin: driving it at reset can change boot mode
    bool reserved = false;      ///< wired to flash or PSRAM: routing I/O here corrupts the device
    bool usbSerial = false;     ///< a pad of the USB serial/JTAG port: a driver taking it ends serial over that port
};
/// Look up one pin's static capability; pure lookup, no state.
GpioCapability gpioCapability(uint8_t gpio);
/// Why a driver must refuse this pin, or null when it may use it.
const char* gpioRefusal(uint8_t gpio);
/// Hand `gpio` to a light driver before it configures the pad: a system function holding it lets go first, the USB serial/JTAG port's driver here, which storms with interrupts once another peripheral takes its pad.
void gpioTake(int gpio);
/// Make gpioCapability answer `cap` for one gpio, so pin severity is testable where every real pin is safe.
void setTestGpioCapability(uint8_t gpio, GpioCapability cap);
/// Drop every injected capability.
void clearTestGpioCapability();

/// What one GPIO is doing now, the pin map's second axis beside gpioCapability's static view.
///
/// @technical The pad reads on any pin, even one a peripheral drives.
/// So a driver's output must toggle while it renders, and a mic clock while the mic runs.
struct GpioLiveState {
    bool valid = false;    ///< pin is readable; false out of range, and on host, which omits the columns
    bool level = false;    ///< current pad level, true being HIGH
    bool output = false;   ///< the pad's output driver is enabled right now
    bool input = false;    ///< the pad's input buffer is enabled right now; a pin can be both
    uint8_t driveCap = 0;  ///< output drive strength 0..3 = WEAK / MEDIUM / STRONG / STRONGEST
};
/// Sample one pin's live state, on tick1s rather than per frame.
GpioLiveState gpioLiveState(uint8_t gpio);
/// Inject a live state for one gpio, so the level and drive columns are host-testable.
void setTestGpioLiveState(uint8_t gpio, GpioLiveState state);
/// Drop every injected live state.
void clearTestGpioLiveState();

// The board's I2C bus: one master bus, opened by its owner (I2cBusModule) and shared by every device on it, such as an audio codec.

/// Open the bus on these pins, closing one open on other pins first; false when the port cannot be opened.
bool i2cBusOpen(uint16_t sda, uint16_t scl);

/// Close the bus, releasing the devices on it first.
void i2cBusClose();

/// Whether the bus is open.
bool i2cBusReady() MM_NONBLOCKING;

/// A count that moves on every open and close, so a device on the bus notices that it has to attach again.
uint32_t i2cBusGeneration() MM_NONBLOCKING;

/// The bus is not open, which is distinct from a scan that found zero devices.
inline constexpr size_t kI2cBusUnavailable = static_cast<size_t>(-1);

/// Probe every address on the open bus, writing those that answer into `out` and returning the count: the standard i2cdetect.
size_t i2cScan(uint8_t* out, size_t maxOut);
/// Make a scan of the open bus find these addresses, as the chips on a board would answer.
void setTestI2cDevices(const uint8_t* addrs, size_t n);

// --- GPIO as a role: read a switch or drive a line, the module owning debouncing ---------------

/// The internal pull to enable on an input; a mechanical switch needs one, a driven pin does not.
enum class GpioPull : uint8_t { None = 0, Up, Down };

/// Configure one GPIO as an input; false where the chip has no usable input there.
bool gpioInputBegin(uint8_t gpio, GpioPull pull);

/// Read a configured pin, true being HIGH; an unconfigured pin reads false.
bool gpioRead(uint8_t gpio);

/// Drive one GPIO as a push-pull output, configuring it on first use; false where it has no driver.
bool gpioWrite(uint8_t gpio, bool high);

/// Make gpioRead answer `level` for one pin.
void setTestGpioLevel(uint8_t gpio, bool level);
/// Drop every injected level.
void clearTestGpioLevel();

/// Read one ADC pin as a raw count; false where the chip has no ADC there or the read failed.
bool adcRead(uint8_t gpio, uint16_t& raw);

/// The full-scale count `adcRead` reports here, so a caller scales the same on every chip.
uint16_t adcMaxCount();

/// Read one ADC pin as millivolts; false where `adcRead` would fail, or the chip carries no calibration.
bool adcReadMv(uint8_t gpio, uint16_t& mv);

/// Make adcReadMv answer `mv` for one pin.
void setTestAdcMv(uint8_t gpio, uint16_t mv);

/// Make adcRead answer `raw` for one pin, so a pedal's mapping is host-testable.
void setTestAdcValue(uint8_t gpio, uint16_t raw);
/// Drop every injected count.
void clearTestAdcValue();

/// Poll the IR receiver for a remote frame, answering true once per fresh code.
bool irRead(uint16_t pin, uint32_t& codeOut);

/// Release the IR channel, freeing its pin; irRead reopens lazily, so this is safe at any time.
void irStop();

/// Open or confirm the IR channel and report whether it is live, which irRead cannot distinguish.
bool irChannelReady(uint16_t pin);

// --- USB MIDI host, gated by `hasUsbMidiHost`: one class-compliant desk on the chip's own port ---

/// Take the chip's USB port as a host for a MIDI desk, ending its serial log and USB flashing; false on a chip with no USB host or while another owner has it.
bool usbMidiBegin();

/// Give the USB port back.
void usbMidiEnd();

/// Whether a MIDI desk is attached and its endpoints are open.
bool usbMidiConnected();

/// Take up to `max` received USB-MIDI event packets of 4 bytes each, returning at once; the count taken.
size_t usbMidiRead(uint8_t (*packets)[4], size_t max);

/// Queue USB-MIDI event packets for the desk, returning at once; false when no desk is attached or the queue is full.
bool usbMidiWrite(const uint8_t (*packets)[4], size_t count);

/// Make the host a USB MIDI host with a desk attached, so the MIDI service's USB path is host-testable; false detaches.
void setTestUsbMidiDesk(bool attached);
/// Deliver packets as if the attached test desk sent them.
void injectTestUsbMidi(const uint8_t (*packets)[4], size_t count);
/// Take up to `max` of the packets sent to the test desk, oldest first; the count taken.
size_t takeTestUsbMidiSent(uint8_t (*packets)[4], size_t max);

}  // namespace mm::platform
