/// @defgroup platform_esp32_i2c The board's I2C bus
/// One master bus per board, opened by its owner and shared by every device on it, with the standard i2cdetect scan.
///
/// Domain-neutral: an audio codec, a sensor or a display attaches to the same bus rather than opening its own.
///
/// @moreinfo
///
/// ## One owner, many devices
///
/// Two drivers each opening a bus on the same port collide the moment both are active, so the bus is opened once and the devices attach to it.
/// Moving the bus to other pins first releases what is attached, and the generation count tells each device to attach again.
/// An inert stub elsewhere, so a target without the peripheral still links and reports as much.

#include "platform/platform.h"

#include "soc/soc_caps.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

#if SOC_I2C_SUPPORTED

#include "driver/i2c_master.h"
#include "esp_log.h"

namespace mm::platform {

namespace {
const char* I2C_TAG = "mm_i2c";
i2c_master_bus_handle_t g_bus = nullptr;
uint16_t g_sda = 0, g_scl = 0;
std::atomic<uint32_t> g_generation{0};
}  // namespace

// For the devices on the bus in this layer, such as the codec; the domain never sees a handle.
i2c_master_bus_handle_t i2cBusHandle() { return g_bus; }

bool i2cBusOpen(uint16_t sda, uint16_t scl) {
    if (g_bus && sda == g_sda && scl == g_scl) return true;
    i2cBusClose();
    i2c_master_bus_config_t busCfg = {};
    busCfg.i2c_port = I2C_NUM_0;
    busCfg.sda_io_num = static_cast<gpio_num_t>(sda);
    busCfg.scl_io_num = static_cast<gpio_num_t>(scl);
    busCfg.clk_source = I2C_CLK_SRC_DEFAULT;
    busCfg.glitch_ignore_cnt = 7;
    busCfg.flags.enable_internal_pullup = true;
    if (i2c_new_master_bus(&busCfg, &g_bus) != ESP_OK) {
        ESP_LOGW(I2C_TAG, "i2c bus open failed (sda %u scl %u)", sda, scl);
        g_bus = nullptr;
        return false;
    }
    g_sda = sda;
    g_scl = scl;
    g_generation.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void i2cBusClose() {
    if (!g_bus) return;
    audioCodecDeinit();   // a device still attached would keep the bus from closing
    i2c_del_master_bus(g_bus);
    g_bus = nullptr;
    g_generation.fetch_add(1, std::memory_order_relaxed);
}

bool i2cBusReady() MM_NONBLOCKING { return g_bus != nullptr; }

uint32_t i2cBusGeneration() MM_NONBLOCKING { return g_generation.load(std::memory_order_relaxed); }

size_t i2cScan(uint8_t* out, size_t maxOut) {
    if (!g_bus) return kI2cBusUnavailable;
    if (!out || maxOut == 0) return 0;
    // The 7-bit range, 0x00 and 0x78 and up being reserved; 50 ms each keeps a full scan under a second, run from a button off the render path.
    size_t found = 0;
    for (uint8_t addr = 0x01; addr < 0x78 && found < maxOut; addr++) {
        if (i2c_master_probe(g_bus, addr, 50) == ESP_OK) out[found++] = addr;
    }
    return found;
}

void setTestI2cDevices(const uint8_t*, size_t) {}

}  // namespace mm::platform

#else  // !SOC_I2C_SUPPORTED: inert stubs so an I2C-less target links

namespace mm::platform {

bool i2cBusOpen(uint16_t, uint16_t) { return false; }
void i2cBusClose() {}
bool i2cBusReady() MM_NONBLOCKING { return false; }
uint32_t i2cBusGeneration() MM_NONBLOCKING { return 0; }
size_t i2cScan(uint8_t*, size_t) { return kI2cBusUnavailable; }
void setTestI2cDevices(const uint8_t*, size_t) {}

}  // namespace mm::platform

#endif  // SOC_I2C_SUPPORTED
