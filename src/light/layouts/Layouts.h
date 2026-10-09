#pragma once

#include "light/layouts/LayoutBase.h"  // LayoutBase and CoordCallback, which children are cast to
#include "core/module/MoonModule.h"
#include "light/util/light_types.h" // lengthType, nrOfLightsType

#include <cstdio> // std::snprintf for the status line
#include <limits> // the most lights a boot outside safe mode places

namespace mm {


/// The container defining the installation's physical light topology.
///
/// One `Layouts` describes the setup, and every layer in `Effects` renders into it. Children stitch into one flat physical address space, in registration order.
/// @card Layouts.png
///
/// @moreinfo
///
/// ## The container owns coordinate iteration
///
/// `placeLights` walks each enabled child's coordinates, offsetting the physical indices, so sixteen strips making one panel address as a single run and a layer builds its mapping from them.
///
/// ## Disabling and reordering shift indices
///
/// Disabling a layout removes its lights and later layouts shift down; reordering by drag and drop sets each layout's physical range. Both move ArtNet universe assignments, so disable the driver to keep a mapping stable.
///
/// ## The status line
///
/// It reports the total light count and the physical bounding box: a dense grid's count equals its box volume, so a smaller count signals a sparse layout.
class Layouts : public MoonModule {
public:
    /// The tag the UI shows for this container.
    const char* tags() const override { return "💫"; }
    /// The child role this container accepts, which is layouts alone.
    const char* acceptsChildRoles() const override { return "layout"; }

    /// The lights safe mode places, since a layout too large for memory crashes the device at every boot.
    static constexpr nrOfLightsType kSafeModeLights = 1024;
    /// The most lights this boot places.
    static nrOfLightsType maxLights() {
        return MoonModule::safeMode() ? kSafeModeLights : std::numeric_limits<nrOfLightsType>::max();
    }

    // Disabling the container reports zero, the same as disabling every child.
    /// The lights across every enabled child, which sizes the layer and output buffers: @xref{disabling-and-reordering-shift-indices}.
    nrOfLightsType totalLightCount() const {
        if (!enabled()) return 0;
        nrOfLightsType total = 0;
        for (uint8_t i = 0; i < childCount(); i++) {
            if (!child(i)->enabled()) continue;
            total += static_cast<LayoutBase*>(child(i))->lightCount();
        }
        return total < maxLights() ? total : maxLights();
    }

    /// Emit every enabled child's positions into the sink, offset into one address space: @xref{the-container-owns-coordinate-iteration}.
    void placeLights(const CoordSink& sink) const {
        if (!enabled()) return;
        nrOfLightsType offset = 0;
        for (uint8_t i = 0; i < childCount(); i++) {
            if (!child(i)->enabled()) continue;
            auto* layout = static_cast<LayoutBase*>(child(i));
            // Both kinds relay through their own offset, so a child's gap stays a gap here.
            struct WrapCtx {
                const CoordSink* sink;
                nrOfLightsType offset;
                uint32_t limit;
            };
            WrapCtx wctx{&sink, offset, maxLights()};
            layout->placeLights(CoordSink{
                [](void* wc, nrOfLightsType idx, lengthType x, lengthType y, lengthType z) {
                    auto* w = static_cast<WrapCtx*>(wc);
                    if (uint32_t(idx) + w->offset < w->limit) w->sink->pixel(idx + w->offset, x, y, z);
                },
                [](void* wc, nrOfLightsType idx, lengthType x, lengthType y, lengthType z) {
                    auto* w = static_cast<WrapCtx*>(wc);
                    if (uint32_t(idx) + w->offset < w->limit) w->sink->blackPixel(idx + w->offset, x, y, z);
                },
                &wctx});
            offset += layout->lightCount();
        }
    }

    // Gates the dense-identity fast path off, since an identity map would light a gap.
    /// Whether any enabled child holds physical slots dark.
    bool hasBlackPixels() const {
        if (!enabled()) return false;
        for (uint8_t i = 0; i < childCount(); i++) {
            if (child(i)->enabled() && static_cast<LayoutBase*>(child(i))->hasBlackPixels()) return true;
        }
        return false;
    }

    // Recomputed on a rebuild rather than per tick, and an empty setup flags a warning.
    /// Report the light count and the physical bounding box on the status line: @xref{the-status-line}.
    void prepare() override {
        const nrOfLightsType lights = totalLightCount();
        // One placeLights pass for the bounding box: max coordinate + 1 per axis.
        struct Extent { lengthType x, y, z; bool any; } e{0, 0, 0, false};
        // A gap counts toward the box, occupying a real position, so one callback handles both.
        placeLights(CoordSink{[](void* ctx, nrOfLightsType, lengthType x, lengthType y, lengthType z) {
            auto* ex = static_cast<Extent*>(ctx);
            if (x > ex->x) ex->x = x;
            if (y > ex->y) ex->y = y;
            if (z > ex->z) ex->z = z;
            ex->any = true;
        }, nullptr, &e});
        const lengthType w = e.any ? e.x + 1 : 0;
        const lengthType h = e.any ? e.y + 1 : 0;
        const lengthType d = e.any ? e.z + 1 : 0;
        std::snprintf(statusBuf_, sizeof(statusBuf_), "%u lights · %u×%u×%u%s",
                      static_cast<unsigned>(lights),
                      static_cast<unsigned>(w), static_cast<unsigned>(h), static_cast<unsigned>(d),
                      MoonModule::safeMode() ? " · safe mode" : "");
        setStatus(statusBuf_, lights == 0 ? Severity::Warning : Severity::Status);
    }

private:
    /// Backing store for the status line, which `setStatus` borrows rather than copies.
    char statusBuf_[64] = {};   // the widest line, a 10-digit count, three 5-digit sides and the safe-mode mark, is 59
};

} // namespace mm
