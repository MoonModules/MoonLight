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
/// Beside another writer a sender-mapped frame is mapped back to the grid, and under a static modifier it is placed by the layout's positions.
class ChannelDataSink {
public:
    /// What a frame costs on its way to the drivers, for a status to name.
    enum Path : uint8_t {
        kPathNone,      ///< receiver mapping, which maps once as every layer does
        kPathOneCopy,   ///< sender mapping straight to the output, or through an identity mapping
        kPathTwice,     ///< sender mapping mapped back to the grid and forward again
    };

    /// Declared as a member of the effect it serves, which owns the memory.
    explicit ChannelDataSink(EffectBase& owner) : owner_(owner) {}

    /// Size staging to the layer, by logical cell or, for a sender-mapped frame, by physical light; static modifiers add the layout placement.
    void prepare(bool senderMapped) {
        const Layer* layer = owner_.layer();
        senderMapped_ = senderMapped && layer;
        const size_t lights = senderMapped_ ? layer->physicalLightCount() : owner_.nrOfLights();
        staging_.resize(lights * owner_.channelsPerLight());
        placement_.resize(senderMapped_ && layer->foldsStatically() ? owner_.nrOfLights() : 0);
        if (placement_) layer->placeByLayout(placement_.data(), static_cast<nrOfLightsType>(placement_.count()));
        dirty_.store(true, std::memory_order_release);   // a resize, or a re-enable, paints the layer from staging once
    }

    /// Whether a frame is in wiring order, which a layer showing only it hands to the output unmapped.
    bool wiringOrder() const MM_NONBLOCKING { return senderMapped_; }

    /// The render tick: copy a changed frame into the layer, unless the layer hands it to the output as it is; returns the path it took.
    Path tick() {
        const Layer* layer = owner_.layer();
        if (!senderMapped_ || !layer) {
            copyIntoLayer(layer, false);
            return kPathNone;
        }
        if (layer->wiringSource() == &owner_) return kPathOneCopy;   // Drivers copies it to the output
        copyIntoLayer(layer, true);
        return layer->lut().hasLUT() ? kPathTwice : kPathOneCopy;
    }

    /// Copy staging to the output when a write changed it, or whole when asked; a write in progress waits for the next frame.
    void copyWiringFrame(uint8_t* out, size_t bytes, bool force) {
        if (!force && !dirty_.load(std::memory_order_acquire)) return;
        LockGuard guard(lock_);
        if (!guard) return;
        dirty_.store(false, std::memory_order_release);
        std::memcpy(out, staging_.data(), staging_.bytes() < bytes ? staging_.bytes() : bytes);
    }

    /// The one clamped write into staging, bound-checked before any addition; `kWait` for a writer task, which waits out one render copy at most, else a busy lock drops the write.
    template <bool kWait = true>
    void write(size_t offset, const uint8_t* data, size_t len) {
        if (!staging_ || offset >= staging_.bytes()) return;
        const size_t n = offset + len > staging_.bytes() ? staging_.bytes() - offset : len;
        if constexpr (kWait) {
            while (!lock_.tryAcquire()) platform::yield();
        } else if (!lock_.tryAcquire()) {
            return;
        }
        std::memcpy(staging_.data() + offset, data, n);
        lock_.release();
        dirty_.store(true, std::memory_order_release);
    }

    /// The held frame's bytes, for a test pinning the lifecycle.
    const uint8_t* data() const { return staging_.data(); }
    /// How many bytes staging holds.
    size_t bytes() const { return staging_.bytes(); }

private:
    /// Copy staging into the layer when it changed or the layer was overwritten, through whichever placement the frame needs.
    void copyIntoLayer(const Layer* layer, bool sender) {
        uint8_t* buf = owner_.buffer();
        if (!buf || !staging_) return;
        const uint32_t gen = layer ? layer->bufferGen() : 0;
        if (!dirty_.load(std::memory_order_acquire) && layer && gen == lastGen_) return;
        LockGuard guard(lock_);
        if (!guard) return;   // a write holds staging: copy on the next tick
        dirty_.store(false, std::memory_order_release);
        place(buf, layer, sender);
        lastGen_ = gen + 1;   // the Layer bumps the generation after this tick, for our own copy
    }

    /// Staging into the layer buffer: by the layout's positions under static modifiers, through the mapping's inverse when sender-mapped, else as it is.
    void place(uint8_t* buf, const Layer* layer, bool sender) {
        const size_t bufBytes = static_cast<size_t>(owner_.nrOfLights()) * owner_.channelsPerLight();
        if (placement_) gatherPlaced(buf, bufBytes);
        else if (sender) layer->lut().gatherFromPhysical(buf, bufBytes, staging_.data(), staging_.bytes(), owner_.channelsPerLight());
        else std::memcpy(buf, staging_.data(), staging_.bytes() < bufBytes ? staging_.bytes() : bufBytes);
    }

    /// Each cell takes the light the layout places at its position; a cell with none keeps its value.
    void gatherPlaced(uint8_t* buf, size_t bufBytes) {
        const size_t cpl = owner_.channelsPerLight();
        const size_t physLights = staging_.bytes() / cpl;
        const size_t cells = bufBytes / cpl < placement_.count() ? bufBytes / cpl : placement_.count();
        for (size_t cell = 0; cell < cells; cell++) {
            const size_t src = placement_[cell];
            if (src >= physLights) continue;   // kNoDriver, or past a shorter frame
            MappingLUT::copyLight(buf + cell * cpl, staging_.data() + src * cpl, cpl);
        }
    }

    EffectBase& owner_;
    ScratchBuffer<uint8_t> staging_{owner_};             ///< the held frame
    ScratchBuffer<nrOfLightsType> placement_{owner_};    ///< each cell's physical light by layout position, only under static modifiers
    TryLock lock_;                                       ///< staging, between a writer's writes and the render copy
    std::atomic<bool> dirty_{true};                      ///< staging changed since the last copy
    uint32_t lastGen_ = 0;                               ///< the layer's write generation after our last copy
    bool senderMapped_ = false;                          ///< the frame is in wiring order, as prepare was told
};

}  // namespace mm
