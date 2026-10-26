#pragma once

#include "core/util/ScratchBuffer.h"
#include "core/util/TryLock.h"     // staging, shared by a writer task and the render copy
#include "light/effects/EffectBase.h"
#include "light/layers/Layer.h"    // the mapping a sender-mapped frame is placed through
#include "platform/platform.h"     // yield, while a writer waits out the render copy

#include <atomic>
#include <cstring>

namespace mm {

/// Where channel data lands, whatever carried it: Art-Net, E1.31 and DDP over a network, or an FSEQ file from flash or SD.
///
/// A member of the effect that owns the transport, so Network Receive and the FSEQ player are thin effects on one copy of this logic.
///
/// It holds the last frame, so the lights keep it between frames, and copies a frame only when it changed.
///
/// A receiver-mapped frame is copied into the layer, which applies this board's wiring. A sender-mapped one alone on its layer goes to the output as it is.
///
/// Beside another writer or under a modifier a sender-mapped frame is placed by the layout's positions, so a modifier transforms it.
class ChannelDataSink {
public:
    /// What a frame costs on its way to the drivers, for a status to name.
    enum Path : uint8_t {
        kPathNone,      ///< receiver mapping, which maps once as every layer does
        kPathOneCopy,   ///< sender mapping straight to the output, or through an identity mapping
        kPathTwice,     ///< sender mapping placed on the grid and mapped forward again
    };

    /// Declared as a member of the effect it serves, which owns the memory.
    explicit ChannelDataSink(EffectBase& owner) : owner_(owner) {}

    /// Size staging by logical cell, or by physical light for a sender-mapped frame. One the layer maps gets a placement by layout position, unless it goes straight to the output.
    void prepare(bool senderMapped) {
        const Layer* layer = owner_.layer();
        senderMapped_ = senderMapped && layer;
        const size_t lights = senderMapped_ ? layer->physicalLightCount() : owner_.nrOfLights();
        staging_.resize(lights * owner_.channelsPerLight());
        const bool placed = senderMapped_ && layer->lut().hasLUT() && layer->wiringSource() != &owner_;
        placement_.resize(placed ? owner_.nrOfLights() : 0);
        if (placement_) layer->placeByLayout(placement_.data(), static_cast<nrOfLightsType>(placement_.count()));
        dirty_.store(true, std::memory_order_release);   // a resize, or a re-enable, paints the layer from staging once
    }

    /// Whether a frame is in wiring order, which a layer showing only it hands to the output unmapped.
    bool wiringOrder() const MM_NONBLOCKING { return senderMapped_; }

    /// The render tick: copy a changed frame into the layer, unless the layer hands it to the output as it is; returns the path it took.
    Path tick() {
        const Layer* layer = owner_.layer();
        if (!senderMapped_ || !layer) {
            copyIntoLayer(layer);
            return kPathNone;
        }
        if (layer->wiringSource() == &owner_) return kPathOneCopy;   // Drivers copies it to the output
        copyIntoLayer(layer);
        return layer->lut().hasLUT() ? kPathTwice : kPathOneCopy;
    }

    /// Copy staging to the output when a write changed it, or whole when asked; what a writer holds waits for the next frame.
    void copyWiringFrame(uint8_t* out, size_t bytes, bool force) {
        if (!force && !dirty_.load(std::memory_order_acquire)) return;
        const size_t n = staging_.bytes() < bytes ? staging_.bytes() : bytes;
        copyInChunks(n, [&](size_t at, size_t len) { std::memcpy(out + at, staging_.data() + at, len); });
    }

    /// The one clamped write into staging, bound-checked before any addition. `kWait` is for a writer task, which waits out the render copy; without it a busy lock drops the write.
    template <bool kWait = true>
    void write(size_t offset, const uint8_t* data, size_t len, bool frameDone = true) {
        if (!staging_ || offset >= staging_.bytes()) {
            if (frameDone && staging_) dirty_.store(true, std::memory_order_release);   // a frame's last packet past staging still ends the frame
            return;
        }
        const size_t n = offset + len > staging_.bytes() ? staging_.bytes() - offset : len;
        if (!acquire<kWait>()) return;
        std::memcpy(staging_.data() + offset, data, n);
        // Inside the lock, so a copy that takes it next sees this write; a frame's earlier packets wait for its last, so the render copies it once.
        if (frameDone) dirty_.store(true, std::memory_order_release);
        lock_.release();
    }

    /// Take the lock for a write: a writer task spins out one chunk of the render copy rather than a frame while the socket's queue fills, and the tick gives up at once.
    template <bool kWait>
    bool acquire() {
        if constexpr (!kWait) return lock_.tryAcquire();
        for (uint32_t spins = 0; !lock_.tryAcquire(); spins++) if (spins >= kSpins) platform::yield();
        return true;
    }

    /// The held frame's bytes, for a test pinning the lifecycle.
    const uint8_t* data() const { return staging_.data(); }
    /// How many bytes staging holds.
    size_t bytes() const { return staging_.bytes(); }

private:
    /// Copy staging into the layer when it changed or the layer was overwritten, through whichever placement the frame needs.
    void copyIntoLayer(const Layer* layer) {
        uint8_t* buf = owner_.buffer();
        if (!buf || !staging_) return;
        const uint32_t gen = layer ? layer->bufferGen() : 0;
        // Nothing new and nobody else wrote the layer: our own tick's bump is the next generation, not someone else's write.
        if (!dirty_.load(std::memory_order_acquire) && layer && gen == lastGen_) { lastGen_ = gen + 1; return; }
        // By the layout's positions for a sender-mapped frame the layer maps, else as it is: row by row, or wiring order on an identity mapping.
        const size_t cpl = owner_.channelsPerLight();
        const size_t bufBytes = static_cast<size_t>(owner_.nrOfLights()) * cpl;
        if (placement_) {
            const size_t cells = bufBytes / cpl < placement_.count() ? bufBytes / cpl : placement_.count();
            copyInChunks(cells * cpl, [&](size_t at, size_t len) { gatherPlaced(buf, at / cpl, (at + len) / cpl); });
        } else {
            const size_t n = staging_.bytes() < bufBytes ? staging_.bytes() : bufBytes;
            copyInChunks(n, [&](size_t at, size_t len) { std::memcpy(buf + at, staging_.data() + at, len); });
        }
        lastGen_ = gen + 1;   // the Layer bumps the generation after this tick, for our own copy
    }

    /// Run `copy` over `total` bytes a chunk at a time under the lock, so a writer waits one chunk; a lock held past the spins leaves the rest dirty for the next frame.
    template <typename Copy>
    void copyInChunks(size_t total, Copy&& copy) {
        dirty_.store(false, std::memory_order_release);   // before the first chunk, so a write landing behind the copy marks it again
        for (size_t at = 0; at < total; at += kChunkBytes) {
            uint32_t spins = 0;
            while (!lock_.tryAcquire()) {
                if (++spins >= kSpins) { dirty_.store(true, std::memory_order_release); return; }
            }
            copy(at, total - at < kChunkBytes ? total - at : kChunkBytes);
            lock_.release();
        }
    }

    /// Cells `from` up to `to` take the light the layout places at their position; a cell with none keeps its value.
    void gatherPlaced(uint8_t* buf, size_t from, size_t to) {
        const size_t cpl = owner_.channelsPerLight();
        const size_t physLights = staging_.bytes() / cpl;
        for (size_t cell = from; cell < to; cell++) {
            const size_t src = placement_[cell];
            if (src >= physLights) continue;   // kNoDriver, or past a shorter frame
            MappingLUT::copyLight(buf + cell * cpl, staging_.data() + src * cpl, cpl);
        }
    }

    static constexpr uint32_t kSpins = 256;   ///< tries before a waiter gives up, a few microseconds of spinning
    static constexpr size_t kChunkBytes = 4096;   ///< one lock hold, tens of microseconds
    EffectBase& owner_;
    ScratchBuffer<uint8_t> staging_{owner_};             ///< the held frame
    ScratchBuffer<nrOfLightsType> placement_{owner_};    ///< each cell's physical light by layout position, only under static modifiers
    TryLock lock_;                                       ///< staging, between a writer's writes and the render copy
    std::atomic<bool> dirty_{true};                      ///< staging changed since the last copy
    uint32_t lastGen_ = 0;                               ///< the layer's write generation after our last copy
    bool senderMapped_ = false;                          ///< the frame is in wiring order, as prepare was told
};

}  // namespace mm
