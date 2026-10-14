#pragma once

#include "light/util/light_types.h"  // nrOfLightsType
#include "platform/platform.h"

#include <cstring>

namespace mm {

/// The table mapping each logical light to the physical lights it drives: one, none or many, with no table at all on the sequential identity fast path.
///
/// @moreinfo
///
/// ## Four mapping kinds
///
/// Identity (a plain grid) needs no table; a shuffled map (serpentine), an unmapped light (a sparse layout) and a multimap (mirroring) each need one.
///
/// ## Compressed sparse row
///
/// One array indexes each logical light into a run, the other holds the flat destinations, and the container supplies the total, so every destination is in bounds.
///
/// ## Paged destinations
///
/// A large map on a board without PSRAM can outgrow the largest free block, so the destinations split into power-of-two pages; the output is identical either way.
class MappingLUT {
public:
    /// An empty table, in identity mode until one is built.
    MappingLUT() = default;
    /// Release whatever the table allocated.
    ~MappingLUT() { free(); }

    /// Never copied: the table owns its allocations and two owners would double-free them.
    MappingLUT(const MappingLUT&) = delete;
    MappingLUT& operator=(const MappingLUT&) = delete;

    // A power of two, so the page split is a shift and a mask: Xtensa has no hardware divide.
    /// How many destinations one page holds, sized to fit a fragmented heap.
    static constexpr nrOfLightsType kPageEntries = 4096;
    /// The shift that turns a destination index into a page number.
    static constexpr nrOfLightsType kPageShift = 12;
    /// The mask that turns a destination index into a slot within its page.
    static constexpr nrOfLightsType kPageMask = kPageEntries - 1;
    /// How many pages the table can hold, capping it at 256K destinations.
    static constexpr int kMaxPages = 64;
    static_assert((kPageEntries & kPageMask) == 0, "kPageEntries must be a power of two");

    /// Fast path: logical == physical, no table needed. `hasLUT` returns false.
    void setIdentity(nrOfLightsType count) {
        free();
        identity_ = true;
        logicalCount_ = count;
    }

    // Three tiers: a single block, then pages, then refusal when the heap cannot hold it.
    /// Allocate the table, returning false only when memory genuinely cannot hold it.
    bool build(nrOfLightsType logicalCount, nrOfLightsType maxDestinations) {
        free();
        identity_ = false;
        logicalCount_ = logicalCount;
        destinationCapacity_ = maxDestinations;

        size_t offsetBytes = static_cast<size_t>(logicalCount + 1) * sizeof(nrOfLightsType);
        size_t destBytes = static_cast<size_t>(maxDestinations) * sizeof(nrOfLightsType);

        // Only the destinations array can hit the fragmentation cliff; offsets is small.
        offsets_ = static_cast<nrOfLightsType*>(platform::alloc(offsetBytes));
        if (!offsets_) { free(); return false; }
        std::memset(offsets_, 0, offsetBytes);

        if (!allocateDestinations(destBytes, maxDestinations)) {
            free();
            return false;
        }

        destinationCount_ = 0;
        return true;
    }

    /// Fill one logical entry's destinations (call sequentially, idx 0..logicalCount-1)
    void setMapping(nrOfLightsType logicalIdx, const nrOfLightsType* physicals, nrOfLightsType count) {
        if (!offsets_ || logicalIdx >= logicalCount_) return;
        offsets_[logicalIdx] = destinationCount_;
        for (nrOfLightsType i = 0; i < count && destinationCount_ < destinationCapacity_; i++) {
            writeDestination(destinationCount_, physicals[i]);
            destinationCount_++;
        }
    }

    /// Call after all setMapping calls to close the last offset
    void finalize() {
        if (offsets_) {
            offsets_[logicalCount_] = destinationCount_;
        }
    }

    /// Release the table and return to the identity fast path.
    void free() {
        if (offsets_) { platform::free(offsets_); offsets_ = nullptr; }
        if (destinations_) { platform::free(destinations_); destinations_ = nullptr; }
        // Reverse order, giving the allocator its best chance to coalesce the blocks.
        for (int i = pageCount_ - 1; i >= 0; i--) {
            platform::free(pages_[i]);
            pages_[i] = nullptr;
        }
        pageCount_ = 0;
        paged_ = false;
        logicalCount_ = 0;
        destinationCount_ = 0;
        destinationCapacity_ = 0;
        identity_ = true;
        overwrites_ = true;
    }

    /// Whether a table is allocated, rather than the identity fast path. @xref{four-mapping-kinds}
    bool hasLUT() const { return !identity_; }
    /// Whether the destinations are split into pages.
    bool isPaged() const { return paged_; }
    /// How many logical lights the table covers.
    nrOfLightsType logicalCount() const { return logicalCount_; }
    /// How many physical destinations the table holds in total.
    nrOfLightsType destinationCount() const { return destinationCount_; }

    // True lets blendMap plain-copy, around four times faster than the additive path.
    /// Whether each physical destination is written by at most one logical light.
    bool overwrites() const { return overwrites_; }
    /// Declare whether destinations are distinct, which chooses the copy or additive blend.
    void setOverwrites(bool v) { overwrites_ = v; }

    /// The bytes the table uses, which is zero in identity mode.
    size_t memoryUsed() const {
        if (identity_) return 0;
        return static_cast<size_t>(logicalCount_ + 1) * sizeof(nrOfLightsType)
             + static_cast<size_t>(destinationCount_) * sizeof(nrOfLightsType);
    }

    /// The bytes a prospective build would take, which paging does not change.
    static size_t estimateBytes(nrOfLightsType logicalCount, nrOfLightsType maxDest) {
        return static_cast<size_t>(logicalCount + 1) * sizeof(nrOfLightsType)
             + static_cast<size_t>(maxDest) * sizeof(nrOfLightsType);
    }

    /// Walk the physical destinations of one logical light, on the hot path. @xref{compressed-sparse-row}
    template<typename F>
    void forEachDestination(nrOfLightsType logicalIdx, F&& callback) const {
        if (identity_) {
            callback(logicalIdx);
            return;
        }
        if (!offsets_ || logicalIdx >= logicalCount_) return;
        nrOfLightsType start = offsets_[logicalIdx];
        nrOfLightsType end = offsets_[logicalIdx + 1];
        if (!paged_) {
            // The common case, where the branch predicts not-taken and this stays flat.
            for (nrOfLightsType i = start; i < end; i++) {
                callback(destinations_[i]);
            }
            return;
        }
        // One run can straddle a page boundary, so recompute the page when the slot wraps.
        for (nrOfLightsType i = start; i < end; i++) {
            const nrOfLightsType* page = pages_[i >> kPageShift];
            callback(page[i & kPageMask]);
        }
    }

    /// Write a wiring-order frame into the logical buffer, inverting this mapping: each cell takes its first physical light's value; identity is one copy.
    void gatherFromPhysical(uint8_t* logical, size_t logicalBytes, const uint8_t* physical, size_t physicalBytes,
                            uint8_t channelsPerLight) const {
        if (identity_) {
            std::memcpy(logical, physical, logicalBytes < physicalBytes ? logicalBytes : physicalBytes);
            return;
        }
        if (!offsets_ || channelsPerLight == 0) return;
        if (paged_) gatherWalk(logical, logicalBytes, physical, physicalBytes, channelsPerLight,
                               [this](nrOfLightsType s) { return pages_[s >> kPageShift][s & kPageMask]; });
        else gatherWalk(logical, logicalBytes, physical, physicalBytes, channelsPerLight,
                        [this](nrOfLightsType s) { return destinations_[s]; });
    }

private:
    /// The gather loop, one instance per storage tier so the page test leaves the loop; each offset is read once.
    template <class Dest>
    void gatherWalk(uint8_t* logical, size_t logicalBytes, const uint8_t* physical, size_t physicalBytes,
                    uint8_t channelsPerLight, Dest dest) const {
        const size_t cpl = channelsPerLight;
        const size_t physLights = physicalBytes / cpl;
        const size_t cells = logicalBytes / cpl < logicalCount_ ? logicalBytes / cpl : logicalCount_;
        nrOfLightsType start = offsets_[0];
        for (size_t li = 0; li < cells; li++) {
            const nrOfLightsType here = start;
            start = offsets_[li + 1];
            if (here == start) continue;   // a cell no light shows keeps its value
            const size_t first = dest(here);
            if (first >= physLights) continue;
            copyLight(logical + li * cpl, physical + first * cpl, cpl);
        }
    }

public:
    /// One light's channels, the common three unrolled since it runs per light on a gather.
    static void copyLight(uint8_t* d, const uint8_t* s, size_t cpl) MM_NONBLOCKING {
        if (cpl == 3) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; }
        else std::memcpy(d, s, cpl);
    }

private:

    // Tier the destinations allocation, false only on tier 3. @xref{paged-destinations}
    bool allocateDestinations(size_t destBytes, nrOfLightsType maxDestinations) {
        if (destBytes == 0) { paged_ = false; return true; }  // nothing to map

        // A zero maxAllocBlock means unlimited, so the desktop exercises the flat path too.
        size_t maxBlock = platform::maxAllocBlock();
        if (maxBlock == 0 || maxBlock >= destBytes) {
            destinations_ = static_cast<nrOfLightsType*>(platform::alloc(destBytes));
            if (destinations_) { paged_ = false; return true; }
            // Fall through to paging if the single alloc lost a race.
        }

        // Refuse rather than cram the heap: starving the stacks fails somewhere else instead.
        size_t freeHeap = platform::freeHeap();
        if (freeHeap != 0) {  // 0 == desktop (unlimited) → always page-able
            size_t budget = freeHeap > platform::HEAP_RESERVE
                                ? freeHeap - platform::HEAP_RESERVE : 0;
            if (budget < destBytes) return false;
        }

        // Tier 2: page it.
        int needed = static_cast<int>((maxDestinations + kPageEntries - 1) / kPageEntries);
        if (needed > kMaxPages) return false;
        for (int p = 0; p < needed; p++) {
            pages_[p] = static_cast<nrOfLightsType*>(
                platform::alloc(static_cast<size_t>(kPageEntries) * sizeof(nrOfLightsType)));
            if (!pages_[p]) {
                // Partial failure: unwind the pages we got (reverse order).
                for (int q = p - 1; q >= 0; q--) { platform::free(pages_[q]); pages_[q] = nullptr; }
                pageCount_ = 0;
                return false;
            }
        }
        pageCount_ = needed;
        paged_ = true;
        return true;
    }

    void writeDestination(nrOfLightsType slot, nrOfLightsType value) {
        if (paged_) pages_[slot >> kPageShift][slot & kPageMask] = value;
        else        destinations_[slot] = value;
    }

    bool identity_ = true;
    bool paged_ = false;
    nrOfLightsType logicalCount_ = 0;
    nrOfLightsType* offsets_ = nullptr;
    nrOfLightsType* destinations_ = nullptr;   // tier-1 single block (null when paged)
    nrOfLightsType* pages_[kMaxPages] = {};     // tier-2 pages (empty when single)
    int pageCount_ = 0;
    nrOfLightsType destinationCount_ = 0;
    nrOfLightsType destinationCapacity_ = 0;
    bool overwrites_ = true;   // single-write destinations → blendMap may copy
};

} // namespace mm
