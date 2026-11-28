/// The peripheral half of the SPI backend of the parallel LED driver: a general-purpose SPI host whose data lines are the lanes.
///
/// @technical
///
/// ## A whole frame in one transfer
///
/// The driver encodes byte slots, bit L for lane L, three slots a WS2812 bit at the slot rate. The host shifts one, two, four or eight data lines a clock, so the slots pack into what that width shifts out and the DMA streams the frame with no refill to keep up with: a radio, a critical section or a flash write cannot corrupt it, where an RMT refilled by interrupt has a deadline every half-block.
///
/// Packing runs in place, then the buffer's tail is zeroed again, since the driver zeroes the latch pad once and rewrites only the rows each frame.
///
/// ## Which host
///
/// The first free general-purpose host from SPI2 up: two on the classic, S3 and P4. A host the Ethernet or the SD card already holds refuses, and the next is tried.

#include "platform/platform.h"
#include "sdkconfig.h"

#if CONFIG_SOC_GPSPI_SUPPORTED
#include "driver/spi_master.h"
#include "hal/spi_ll.h"   // SPI_LL_DMA_MAX_BIT_LEN: the transfer ceiling, per chip
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "platform/esp32/platform_esp32_heap.h"   // allocCaps, freeCaps: MoonLight's blocks, counted

#include <cstring>
#include <new>

namespace mm::platform {

namespace {

// One host with its device, its buffers and the transfer each buffer has in flight.
struct SpiBus {
    spi_host_device_t host = SPI2_HOST;
    spi_device_handle_t dev = nullptr;
    uint8_t* buf[2] = {};
    size_t capacity = 0;
    uint8_t lines = 1;              // the data lines a transfer drives
    uint32_t clockHz = 0;
    spi_transaction_t trans[2] = {};
    bool queued[2] = {};
    uint32_t lastUs = 0;
};

// The SDK's mode flag for a transfer over `lines` data lines.
uint32_t modeFlag(uint8_t lines) {
    switch (lines) {
        case 2: return SPI_TRANS_MODE_DIO;
        case 4: return SPI_TRANS_MODE_QIO;
#if CONFIG_SOC_SPI_SUPPORT_OCT
        case 8: return SPI_TRANS_MODE_OCT;
#endif
        default: return 0;
    }
}

// The bus on `dataPins`, lane L on data line L, with no clock pin: a self-clocked strand needs none.
spi_bus_config_t busConfig(const uint16_t* dataPins, uint8_t laneCount, size_t bufferBytes, uint8_t lines) {
    spi_bus_config_t bus = {};
    int* data[] = {&bus.data0_io_num, &bus.data1_io_num, &bus.data2_io_num, &bus.data3_io_num,
#if CONFIG_SOC_SPI_SUPPORT_OCT
                   &bus.data4_io_num, &bus.data5_io_num, &bus.data6_io_num, &bus.data7_io_num
#endif
    };
    constexpr uint8_t kLines = sizeof(data) / sizeof(data[0]);
    for (uint8_t i = 0; i < kLines; i++) *data[i] = i < laneCount ? dataPins[i] : -1;
    bus.sclk_io_num = -1;
    bus.max_transfer_sz = static_cast<int>(bufferBytes);
    bus.flags = SPICOMMON_BUSFLAG_MASTER | (lines == 8 ? SPICOMMON_BUSFLAG_OCTAL : 0);
    return bus;
}

// Open the first free host from SPI2 up: @xref{which-host}.
bool openHost(SpiBus* b, const spi_bus_config_t& bus) {
    for (int host = SPI2_HOST; host < SOC_SPI_PERIPH_NUM; host++) {
        b->host = static_cast<spi_host_device_t>(host);
        if (spi_bus_initialize(b->host, &bus, SPI_DMA_CH_AUTO) == ESP_OK) return true;
    }
    return false;
}

// Pins named, a lane count the hosts' data lines carry, and a frame to send; an octal bus needs all eight data pins, so five to seven strands refuse.
bool argsFit(const uint16_t* dataPins, uint8_t laneCount, size_t bufferBytes) {
    return dataPins && laneCount > 0 && laneCount <= spiLanes && (laneCount <= 4 || laneCount == 8) && bufferBytes > 0;
}

// The one or two DMA buffers, zeroed and word-aligned as the DMA reads them; a second that does not fit leaves the driver single-buffered.
bool allocBuffers(SpiBus* b, size_t bytes, bool wantSecond) {
    const uint32_t caps = MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL;
    b->buf[0] = static_cast<uint8_t*>(callocCaps(bytes, caps, 4));
    if (!b->buf[0]) return false;
    if (wantSecond) b->buf[1] = static_cast<uint8_t*>(callocCaps(bytes, caps, 4));
    b->capacity = bytes;
    return true;
}

void release(SpiBus* b) {
    if (b->dev) {
        for (uint8_t i = 0; i < 2; i++) {
            spi_transaction_t* done = nullptr;
            if (b->queued[i]) spi_device_get_trans_result(b->dev, &done, pdMS_TO_TICKS(100));
        }
        spi_bus_remove_device(b->dev);
        spi_bus_free(b->host);
    }
    for (uint8_t* p : b->buf) if (p) freeCaps(p);
    delete b;
}

}  // namespace

bool spiWs2812Init(SpiWs2812Handle& h, const uint16_t* dataPins, uint8_t laneCount, uint32_t clockHz,
                   size_t bufferBytes, bool wantSecondBuffer) {
    if (h.impl || !argsFit(dataPins, laneCount, bufferBytes)) return false;
    auto* b = new (std::nothrow) SpiBus();
    if (!b) return false;
    b->lines = spiLinesFor(laneCount);
    b->clockHz = clockHz;

    if (!openHost(b, busConfig(dataPins, laneCount, bufferBytes, b->lines))) { delete b; return false; }

    spi_device_interface_config_t devCfg = {};
    devCfg.clock_speed_hz = static_cast<int>(clockHz);
    devCfg.mode = 0;
    devCfg.spics_io_num = -1;
    devCfg.queue_size = 2;
    devCfg.flags = SPI_DEVICE_HALFDUPLEX;   // a write-only bus, which a multi-line transfer requires
    if (spi_bus_add_device(b->host, &devCfg, &b->dev) != ESP_OK) { spi_bus_free(b->host); delete b; return false; }

    if (!allocBuffers(b, bufferBytes, wantSecondBuffer)) { release(b); return false; }
    for (uint8_t i = 0; i < laneCount; i++) gpioTake(dataPins[i]);
    h.impl = b;
    return true;
}

uint8_t* spiWs2812Buffer(const SpiWs2812Handle& h, uint8_t buffer) {
    return h.impl && buffer < 2 ? static_cast<SpiBus*>(h.impl)->buf[buffer] : nullptr;
}

size_t spiWs2812BufferCapacity(const SpiWs2812Handle& h) { return h.impl ? static_cast<SpiBus*>(h.impl)->capacity : 0; }

// The DMA length register's width, whole words: 2^24 bits on the classic chip, 2^18 on the S3, about that on the P4.
size_t spiMaxTransferBytes() { return (SPI_LL_DMA_MAX_BIT_LEN / 8) & ~static_cast<size_t>(3); }

bool spiWs2812Transmit(SpiWs2812Handle& h, uint8_t buffer, size_t bytes) {
    auto* b = static_cast<SpiBus*>(h.impl);
    if (!b || buffer > 1 || !b->buf[buffer] || bytes == 0 || bytes > b->capacity || b->queued[buffer]) return false;
    uint8_t* p = b->buf[buffer];
    size_t packed = spiPackSlots(p, bytes, b->lines);
    // @xref{a-whole-frame-in-one-transfer|why the tail is zeroed}; the length rounds to whole words, as the DMA reads them.
    const size_t words = (packed + 3) & ~static_cast<size_t>(3);
    std::memset(p + packed, 0, b->capacity - packed);
    packed = words <= b->capacity ? words : packed;
    spi_transaction_t& t = b->trans[buffer];
    t = {};
    t.flags = modeFlag(b->lines);
    t.length = packed * 8;
    t.tx_buffer = p;
    if (spi_device_queue_trans(b->dev, &t, 0) != ESP_OK) return false;
    b->queued[buffer] = true;
    // The pure wire time: the bits over the lines at the clock.
    b->lastUs = static_cast<uint32_t>(static_cast<uint64_t>(packed) * 8 / b->lines * 1000000u / b->clockHz);
    return true;
}

bool spiWs2812Wait(SpiWs2812Handle& h, uint8_t buffer, uint32_t timeoutMs) {
    auto* b = static_cast<SpiBus*>(h.impl);
    if (!b || buffer > 1 || !b->queued[buffer]) return true;
    // Transfers finish in the order they were queued, so the oldest result is this buffer's or the other's before it.
    for (uint8_t tries = 0; tries < 2 && b->queued[buffer]; tries++) {
        spi_transaction_t* done = nullptr;
        if (spi_device_get_trans_result(b->dev, &done, pdMS_TO_TICKS(timeoutMs)) != ESP_OK) return false;
        for (uint8_t i = 0; i < 2; i++) if (done == &b->trans[i]) b->queued[i] = false;
    }
    return !b->queued[buffer];
}

uint32_t spiWs2812LastTransmitUs(const SpiWs2812Handle& h) { return h.impl ? static_cast<SpiBus*>(h.impl)->lastUs : 0; }

void spiWs2812Deinit(SpiWs2812Handle& h) {
    if (!h.impl) return;
    release(static_cast<SpiBus*>(h.impl));
    h.impl = nullptr;
}

}  // namespace mm::platform
#endif  // CONFIG_SOC_GPSPI_SUPPORTED
