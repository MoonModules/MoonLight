#pragma once
/// MoonLight's own blocks with a memory type of their own, DMA-capable, internal or aligned, counted on MoonLight's memory line as `alloc` and `new` are.
///
/// An output engine's frame buffer, a probe's capture buffer or the OTA chunk is MoonLight's, so it counts as MoonLight's in MoonShine's memory sheet rather than as unaccounted. Every heap_caps block the platform layer takes goes through these, and is given back through `freeCaps`.

#include <cstddef>
#include <cstdint>

namespace mm::platform {

/// A block of `caps` memory (ESP-IDF's MALLOC_CAP_*), aligned to `align` when it is non-zero, counted; null when refused.
void* allocCaps(size_t bytes, uint32_t caps, size_t align = 0);
/// As allocCaps, the block zeroed.
void* callocCaps(size_t bytes, uint32_t caps, size_t align = 0);
/// Count a block an ESP-IDF helper allocated for MoonLight, such as an LCD draw buffer, so its freeCaps balances; returns it.
void* adoptCaps(void* p);
/// Give back a block from allocCaps, callocCaps or adoptCaps; null is ignored.
void freeCaps(void* p);

}  // namespace mm::platform
