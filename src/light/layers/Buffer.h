#pragma once

#include "light/util/light_types.h"  // nrOfLightsType
#include "platform/platform.h"

#include <cstdint>
#include <cstring>
#include <span>

namespace mm {

/// The contiguous light data every effect writes and every driver reads.
///
/// Allocated once outside the hot path and reused each frame, and kept by a resize to the same size.
/// A layer and a driver group each own one when memory allows, and share one when it is tight.
/// @technical
///
/// ## Any channel layout fits
///
/// The storage is a raw byte array rather than a pixel type, addressed by channel count and offset.
/// That is what lets RGB, RGBW and multi-channel DMX fixtures share one buffer.
/// A `std::span` view is the zero-cost safe accessor over it.
///
/// ## Locking is avoided, not optimized
///
/// A semaphore costs around 150 bytes on an ESP32, so the patterns here stay lock-free.
/// An atomic pointer swap for double buffering, or one shared semaphore across layers.
class Buffer {
public:
    /// An empty buffer, holding nothing until it is allocated.
    Buffer() = default;
    /// Release the allocation this buffer owns.
    ~Buffer() { free(); }

    /// Never copied: the buffer owns its allocation and two owners would double-free it.
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    /// Take over another buffer's allocation, leaving it empty.
    Buffer(Buffer&& other) noexcept
        : data_(other.data_), count_(other.count_), channelsPerLight_(other.channelsPerLight_) {
        other.data_ = nullptr;
        other.count_ = 0;
        other.channelsPerLight_ = 0;
    }

    Buffer& operator=(Buffer&& other) noexcept {
        if (this != &other) {
            free();
            data_ = other.data_;
            count_ = other.count_;
            channelsPerLight_ = other.channelsPerLight_;
            other.data_ = nullptr;
            other.count_ = 0;
            other.channelsPerLight_ = 0;
        }
        return *this;
    }

    /// Size the buffer for `nrOfLights` at `cpl` channels each, cleared, returning false when memory refuses.
    bool allocate(nrOfLightsType nrOfLights, uint8_t cpl) {
        size_t totalBytes = static_cast<size_t>(nrOfLights) * cpl;
        // The same size keeps the block, so a layer rebuilt every frame at its size takes nothing from the heap.
        if (!holds(nrOfLights, cpl)) {
            free();
            if (totalBytes == 0) return false;
            data_ = static_cast<uint8_t*>(platform::alloc(totalBytes));
            if (!data_) return false;
        }
        count_ = nrOfLights;
        channelsPerLight_ = cpl;
        clear();
        return true;
    }

    /// Release the allocation and report the buffer as empty.
    void free() {
        if (data_) {
            platform::free(data_);
            data_ = nullptr;
        }
        count_ = 0;
        channelsPerLight_ = 0;
    }

    /// Set every channel to zero, leaving the allocation in place.
    void clear() {
        if (data_) std::memset(data_, 0, bytes());
    }

    /// The raw bytes, for a writer.
    uint8_t* data() { return data_; }
    /// The raw bytes, for a reader.
    const uint8_t* data() const { return data_; }

    /// A bounds-carrying view of the bytes, for a writer.
    std::span<uint8_t> span() { return {data_, bytes()}; }
    /// A bounds-carrying view of the bytes, for a reader.
    std::span<const uint8_t> span() const { return {data_, bytes()}; }

    /// How many lights the buffer holds.
    nrOfLightsType count() const { return count_; }
    /// How many bytes each light occupies.
    uint8_t channelsPerLight() const { return channelsPerLight_; }
    /// How many bytes the lights occupy.
    size_t bytes() const { return static_cast<size_t>(count_) * channelsPerLight_; }
    /// Whether `allocate` keeps the current block for this size: it holds exactly that many bytes.
    bool holds(nrOfLightsType nrOfLights, uint8_t cpl) const {
        return data_ && static_cast<size_t>(nrOfLights) * cpl == bytes();
    }

private:
    uint8_t* data_ = nullptr;
    nrOfLightsType count_ = 0;
    uint8_t channelsPerLight_ = 0;
};

} // namespace mm
