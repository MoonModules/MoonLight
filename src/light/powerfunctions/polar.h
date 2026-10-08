#pragma once

#include "core/module/Control.h"         // ControlList: the controls an effect surfaces for the table
#include "core/util/ScratchBuffer.h"   // ScratchBuffer<T>: self-sizing, owner-tied scratch memory
#include "core/util/math16.h"          // atan16, dist16, angle16
#include "light/util/light_types.h"    // lengthType
#include "platform/platform.h"    // freeHeap, HEAP_RESERVE: the memory gate

#include <cstdint>

namespace mm {

/// The angle and radius of every pixel, computed once and read from a table.
///
/// A radial effect addresses the grid by angle and radius, which turns its motion around the center.
///
/// @moreinfo
///
/// ## The address is a constant
///
/// Computed per frame it measures 1.9 microseconds a pixel on an S3, 39 percent of a 64 by 64 frame.
/// It does not change between frames, so two tables replace that with two lookups a pixel.
///
/// ## The tables belong to a layer
///
/// Every effect on that layer shares one address, and prepare rebuilds only on a change.
///
/// ## The projection is a control
///
/// On a volumetric fixture angle and radius have no single meaning, and which is right is a property of the fixture.
/// Cylindrical carries depth separately and reduces to the flat behavior at one deep, spherical adds a second angle, and radial drops the angle entirely.
/// The choice is made once when the table is built, so it costs nothing a sample.
///
/// ## They decline rather than crowd the heap
///
/// A 32-edge grid asks 2 KB and a 128-edge wall 32 KB, a real fraction of a device without PSRAM.
/// So prepare declines rather than taking it, and the caller computes the address per pixel instead.
///
/// The polar address of every pixel on a grid, as tables.
///
/// Precision is chosen at prepare() time rather than by type, so an effect can offer it as a control and switch live, which is what the live-reconfiguration rule requires.
class PolarLut {
public:
    /// How a volumetric fixture's coordinates become an angle and a radius. Ignored at depth 1, where all three reduce to the same plane.
    enum class Mapping : uint8_t {
        Cylindrical = 0,   ///< angle and radius on xy, depth separate: the 2D behavior, extended
        Spherical   = 1,   ///< angle around and angle up, radius from the center
        Radial      = 2,   ///< distance from the center only
    };

    /// Owner is the module the tables belong to.
    explicit PolarLut(MoonModule& owner)
        : angle8_(owner), radius8_(owner), pitch8_(owner),
          angle16_(owner), pitch16_(owner), radius16_(owner) {}

    /// Build the tables for a `w` x `h` grid, if they are not already built for exactly that.
    /// `wide` selects the 16-bit tables, and the center is the grid's middle in whole pixels.
    ///
    /// Returns false when the tables are not available, and the caller then computes the address per pixel.
    /// It refuses when free heap minus the reserve cannot hold them, rather than starving something else later.
    /// If the allocation fails anyway it unwinds to nothing rather than holding a half-built table.
    /// The controls an effect surfaces to address a volumetric fixture, and the state behind them.
    /// Three effects had all four copied before this existed.
    struct Controls {
        bool    use = true;      ///< read the address from a table rather than computing it
        bool    wide = false;    ///< hold it at full 16-bit precision, at twice the memory
        uint8_t mapping = static_cast<uint8_t>(Mapping::Cylindrical);  ///< which projection, indexing kMappingOptions
    };

    static constexpr const char* kMappingOptions[] = {"cylindrical", "spherical", "radial"};
    /// How many mappings the select offers.
    static constexpr uint8_t kMappingCount = static_cast<uint8_t>(Mapping::Radial) + 1;

    /// Surface the three controls on `list`.
    static void addControls(ControlList& list, Controls& c) {
        list.addControl("polarTable", c.use);
        list.addControl("polarTable16", c.wide);
        list.addSelect("mapping", c.mapping, kMappingOptions, kMappingCount);
    }

    /// The polar address of one light, computed rather than read.
    /// What an effect uses when the table was declined (a device too tight for it) or switched off.
    ///
    /// Takes the mapping, so the fallback renders the SAME composition the table would have, whatever projection the fixture is set to.
    /// The effect kept working and quietly showed a different thing, which is worse than not working.
    struct Address {
        angle16  angle;   ///< around the axis
        uint32_t radius;  ///< in pixels from the center
        angle16  pitch;   ///< elevation, under Spherical; 0 otherwise
    };
    /// The mapping a controls block selects, clamped to one this class defines.
    static Mapping mappingOf(const Controls& c) {
        return static_cast<Mapping>(c.mapping >= kMappingCount ? static_cast<uint8_t>(Mapping::Cylindrical) : c.mapping);
    }

    /// One pixel's polar address under `mapping`, from its offset to the center.
    static Address addressOf(Mapping mapping, int32_t dx, int32_t dy, int32_t dz) {
        switch (mapping) {
            case Mapping::Radial:
                return {0, dist16(dist16(dx, dy), dz), 0};
            case Mapping::Spherical:
                return {atan16(dy, dx), dist16(dist16(dx, dy), dz),
                        atan16(dz, static_cast<int32_t>(dist16(dx, dy)))};
            case Mapping::Cylindrical:
            default:
                return {atan16(dy, dx), dist16(dx, dy), 0};
        }
    }

    /// Build (or release) the table for a fixture of this size, per the controls. The one call an effect makes from prepare().
    bool prepareFor(const Controls& c, lengthType w, lengthType h, lengthType d) {
        if (!c.use) { release(); return false; }
        return prepare(static_cast<uint16_t>(w), static_cast<uint16_t>(h), static_cast<uint16_t>(d),
                       c.wide, mappingOf(c));
    }

    /// The flat form, a separate overload so a flag and a depth cannot be confused.
    bool prepare(uint16_t w, uint16_t h, bool wide = false) {
        return prepare(w, h, 1, wide, Mapping::Cylindrical);
    }

    /// The volumetric form.
    bool prepare(uint16_t w, uint16_t h, uint16_t d, bool wide,
                 Mapping mapping = Mapping::Cylindrical) {
        if (w == w_ && h == h_ && d == d_ && wide == wide_ && mapping == mapping_ && ready_) return true;
        w_ = w; h_ = h; d_ = d; wide_ = wide; mapping_ = mapping; ready_ = false;
        const std::size_t n = static_cast<std::size_t>(w) * h * (d ? d : 1);
        if (n == 0) { release(); return false; }      // an empty grid holds nothing, not the old table

        // Only spherical needs a second angle, so only it costs a third table.
        const bool needPitch = mapping == Mapping::Spherical && d > 1;

        // Free the width we are not using, so the budget below sees the old tables back.
        if (wide) { angle8_.resize(0); radius8_.resize(0); pitch8_.resize(0); }
        else      { angle16_.resize(0); radius16_.resize(0); pitch16_.resize(0); }
        if (!needPitch) { pitch8_.resize(0); pitch16_.resize(0); }

        // The memory gate: worth having, not worth the last of the heap. A host reports 0, meaning unlimited.
        const std::size_t want = n * (wide ? 4u : 2u) + (needPitch ? n * (wide ? 2u : 1u) : 0u);
        const std::size_t freeHeap = platform::freeHeap();
        if (freeHeap != 0) {
            const std::size_t budget = freeHeap > platform::HEAP_RESERVE ? freeHeap - platform::HEAP_RESERVE : 0;
            if (budget < want) { release(); return false; }
        }

        if (wide) {
            if (!angle16_.resize(n) || !radius16_.resize(n)) { release(); return false; }
            if (needPitch && !pitch16_.resize(n)) { release(); return false; }
        } else {
            if (!angle8_.resize(n) || !radius8_.resize(n)) { release(); return false; }
            if (needPitch && !pitch8_.resize(n)) { release(); return false; }
        }

        // The furthest corner: radii scale against it, so the edge reaches full scale at any aspect ratio.
        const int32_t cx = w / 2, cy = h / 2, cz = d / 2;
        const int32_t fx = (cx > w - 1 - cx ? cx : w - 1 - cx);
        const int32_t fy = (cy > h - 1 - cy ? cy : h - 1 - cy);
        const int32_t fz = (cz > d - 1 - cz ? cz : d - 1 - cz);
        maxRadius_ = mapping == Mapping::Cylindrical ? dist16(fx, fy)
                                                     : dist16(dist16(fx, fy), fz);
        if (maxRadius_ == 0) maxRadius_ = 1;               // a 1x1 grid: avoid a divide by zero

        for (uint16_t z = 0; z < (d ? d : 1); z++)
        for (uint16_t y = 0; y < h; y++) {
            for (uint16_t x = 0; x < w; x++) {
                const int32_t dx = static_cast<int32_t>(x) - cx;
                const int32_t dy = static_cast<int32_t>(y) - cy;
                const int32_t dz = static_cast<int32_t>(z) - cz;

                // Through the same call the computed fallback makes, the table being a cache of it.
                const Address ad = addressOf(mapping, dx, dy, dz);
                angle16 a = ad.angle;
                uint32_t r = ad.radius;
                angle16 pitch = ad.pitch;
                // Scaled to full range and rounded: truncating twice lands a pixel short and displaces the center.
                const uint32_t rs = r >= maxRadius_ ? 65535u : (r * 65535u + maxRadius_ / 2) / maxRadius_;
                const std::size_t i = (static_cast<std::size_t>(z) * h + y) * w + x;
                if (wide) {
                    angle16_[i]  = a;
                    radius16_[i] = static_cast<uint16_t>(rs);
                    if (needPitch) pitch16_[i] = pitch;
                } else {
                    angle8_[i]  = static_cast<uint8_t>(a >> 8);
                    radius8_[i] = static_cast<uint8_t>((rs + 128) >> 8 > 255 ? 255 : (rs + 128) >> 8);
                    if (needPitch) pitch8_[i] = static_cast<uint8_t>(pitch >> 8);
                }
            }
        }
        ready_ = true;
        return true;
    }

    /// Free the tables, for a caller that has switched the address back to per-pixel computation.
    void release() {
        if (!ready_ && bytes() == 0) return;
        angle8_.resize(0); radius8_.resize(0); pitch8_.resize(0);
        angle16_.resize(0); radius16_.resize(0); pitch16_.resize(0);
        w_ = h_ = d_ = 0;
        ready_ = false;
    }

    /// True once the tables hold a grid; false if allocation failed and the caller must compute.
    bool ready() const { return ready_; }
    /// True when the 16-bit tables are the ones built.
    bool wide() const { return wide_; }
    /// The distance from center to the furthest corner, in pixels: what `radius` is scaled against.
    uint32_t maxRadius() const { return maxRadius_; }
    /// What the tables currently cost.
    std::size_t bytes() const {
        return angle8_.bytes() + radius8_.bytes() + pitch8_.bytes()
             + angle16_.bytes() + radius16_.bytes() + pitch16_.bytes();
    }
    /// Which projection the built tables use.
    Mapping mapping() const { return mapping_; }

    /// The elevation of pixel `i` above the xy plane, under the spherical projection. Zero under the other two, which have no second angle.
    angle16 pitch(std::size_t i) const {
        if (!ready_) return 0;
        if (wide_) return pitch16_.count() ? pitch16_.data()[i] : 0;
        if (!pitch8_.count()) return 0;
        const uint32_t v = pitch8_.data()[i];
        return static_cast<angle16>((v << 8) | v);
    }

    /// The angle of pixel `i` (row-major, y * width + x), as a full angle16 whichever width is built.
    angle16 angle(std::size_t i) const {
        if (!ready_) return 0;
        if (wide_) return angle16_.data()[i];
        const uint32_t v = angle8_.data()[i];
        return static_cast<angle16>((v << 8) | v);
    }

    /// The radius of pixel `i`, 0 at the center to 65535 at the furthest corner.
    uint16_t radius(std::size_t i) const {
        if (!ready_) return 0;
        if (wide_) return radius16_.data()[i];
        // Widened so the top of the narrow range reaches full scale, or the outer edge never arrives.
        const uint32_t v = radius8_.data()[i];
        return static_cast<uint16_t>((v << 8) | v);
    }

    /// The radius of pixel `i` in PIXELS from the center, which is the form a field scaled in grid units wants. Rounded, so it round-trips the stored value rather than landing short.
    uint32_t radiusPixels(std::size_t i) const {
        if (!ready_) return 0;
        return (static_cast<uint32_t>(radius(i)) * maxRadius_ + 32768u) >> 16;
    }

    /// The angle at (x, y). The index form above is what a pixel loop should use.
    angle16 angleAt(uint16_t x, uint16_t y, uint16_t z = 0) const { return angle(index(x, y, z)); }
    /// The radius at (x, y, z).
    uint16_t radiusAt(uint16_t x, uint16_t y, uint16_t z = 0) const { return radius(index(x, y, z)); }
    /// The elevation at (x, y, z), under the spherical projection.
    angle16 pitchAt(uint16_t x, uint16_t y, uint16_t z = 0) const { return pitch(index(x, y, z)); }
    /// The running index of (x, y, z), the same order the buffer uses.
    std::size_t index(uint16_t x, uint16_t y, uint16_t z = 0) const {
        return (static_cast<std::size_t>(z) * h_ + y) * w_ + x;
    }

private:
    ScratchBuffer<uint8_t>  angle8_, radius8_, pitch8_;
    ScratchBuffer<angle16>  angle16_, pitch16_;
    ScratchBuffer<uint16_t> radius16_;
    uint16_t w_ = 0, h_ = 0, d_ = 0;
    Mapping  mapping_ = Mapping::Cylindrical;
    uint32_t maxRadius_ = 1;
    bool     wide_ = false;
    bool     ready_ = false;
};

}  // namespace mm

