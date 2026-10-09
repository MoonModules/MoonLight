/// @module Effects
/// @also Layer

#include "doctest.h"
#include "light/layouts/Layouts.h"
#include "light/layers/Effects.h"
#include "light/layouts/GridLayout.h"
#include "light/effects/RainbowEffect.h"
#include "light/effects/SpiralEffect.h"
#include "light/effects/FireEffect.h"   // heap-holding effect — the cascade-release probe
#include "light/modifiers/MultiplyModifier.h"
#include "light/drivers/Drivers.h"
#include "platform/platform.h"

#include <cstring>

// RAII guard that restores the platform test clock on scope exit even if a REQUIRE/CHECK throws, without this, a mid-test failure would leave the global setTestNowMs override in place and pollute later test cases.
struct ClockGuard {
    ~ClockGuard() { mm::platform::setTestNowMs(0); }
};

// The Effects container is a thin pass-through with one child Layer: behavior must match what a bare Layer produced before the shape change. These tests pin that, anyone changing Effects::tick() will know immediately if the single-child path stops being a no-op.
//
// Composition (alpha-blend across multiple Effects) is not yet wired, the second test exercises the multi-Layer path enough to confirm each child Layer's loop runs and writes a populated buffer. Once composition lands, add a third test asserting the composed output blends as documented.

// A Effects container with one child Layer must produce the same output as that Layer used directly (no-op container).
TEST_CASE("Effects with one Layer produces the same output as a bare Layer") {
    // Pin virtual time so both Layer paths read the same elapsed value from RainbowEffect's platform::millis() phase. Without this, the two tick() calls land microseconds apart on the real clock and Rainbow's hue rotates between them, making byte-exact comparison impossible (the structural compare this test used to do hid the actual contract).
    mm::platform::setTestNowMs(1000);
    ClockGuard clockGuard;  // restores setTestNowMs(0) even if a REQUIRE below fails

    // --- Reference: bare Layer (no Effects container) ---
    mm::Layouts layoutsA;
    mm::GridLayout gridA;
    gridA.width = 16;
    gridA.height = 16;
    gridA.depth = 1;
    layoutsA.addChild(&gridA);

    mm::Layer bareLayer;
    bareLayer.setLayouts(&layoutsA);
    bareLayer.setChannelsPerLight(3);
    mm::RainbowEffect bareEffect;
    bareLayer.addChild(&bareEffect);
    bareLayer.applyState();
    bareLayer.tick();

    // --- New shape: Effects container wrapping one Layer ---
    mm::Layouts layoutsB;
    mm::GridLayout gridB;
    gridB.width = 16;
    gridB.height = 16;
    gridB.depth = 1;
    layoutsB.addChild(&gridB);

    mm::Effects effectsContainer;
    mm::Layer childLayer;
    childLayer.setChannelsPerLight(3);
    effectsContainer.addChild(&childLayer);
    effectsContainer.setLayouts(&layoutsB);  // propagates to childLayer
    mm::RainbowEffect childEffect;
    childLayer.addChild(&childEffect);

    effectsContainer.applyState();
    // Effects::tick runs each child Layer in order; for the single-child case that's exactly one bareLayer.tick() equivalent.
    effectsContainer.tick();

    // --- Both buffers must be byte-identical at the same elapsed time ---
    auto& bufA = bareLayer.buffer();
    auto& bufB = childLayer.buffer();
    REQUIRE(bufA.bytes() == bufB.bytes());
    REQUIRE(bufA.bytes() == static_cast<size_t>(16 * 16 * 3));
    CHECK(std::memcmp(bufA.data(), bufB.data(), bufA.bytes()) == 0);
    // clockGuard restores setTestNowMs(0) on scope exit
}

// With two child Layers, each one's tick() runs and writes its own buffer (the container iterates all enabled children).
TEST_CASE("Effects with two Effects: each child Layer's tick runs and writes its buffer") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 8;
    grid.height = 8;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Effects effectsContainer;

    mm::Layer layerA;
    layerA.setChannelsPerLight(3);
    mm::RainbowEffect effectA;
    layerA.addChild(&effectA);

    mm::Layer layerB;
    layerB.setChannelsPerLight(3);
    mm::SpiralEffect effectB;
    layerB.addChild(&effectB);

    effectsContainer.addChild(&layerA);
    effectsContainer.addChild(&layerB);
    effectsContainer.setLayouts(&layouts);
    effectsContainer.applyState();
    effectsContainer.tick();

    // Both child Layer buffers must be populated, each Layer renders its own buffer here; the Drivers composite of those buffers is pinned by the "Drivers composites two enabled Layers" case below. (Checkerboard with default controls writes a checker pattern; Rainbow writes a hue gradient.)
    auto& bufA = layerA.buffer();
    auto& bufB = layerB.buffer();
    REQUIRE(bufA.bytes() == static_cast<size_t>(8 * 8 * 3));
    REQUIRE(bufB.bytes() == static_cast<size_t>(8 * 8 * 3));

    bool aHasNonZero = false, bHasNonZero = false;
    for (size_t i = 0; i < bufA.bytes(); i++) if (bufA.data()[i] != 0) { aHasNonZero = true; break; }
    for (size_t i = 0; i < bufB.bytes(); i++) if (bufB.data()[i] != 0) { bHasNonZero = true; break; }
    CHECK_MESSAGE(aHasNonZero, "Layer A (Rainbow) wrote no pixels");
    CHECK_MESSAGE(bHasNonZero, "Layer B (Checkerboard) wrote no pixels");
}

// A minimal driver that just records the source buffer it's handed each tick, so a test can inspect the composited output without a real network/LED sink.
class CaptureDriver : public mm::DriverBase {
public:
    void setSourceBuffer(mm::Buffer* buf) override { src_ = buf; }
    mm::Buffer* src_ = nullptr;
};

// Multi-layer composition: Drivers blends ≥2 enabled Layers into its own output buffer and hands THAT to drivers (not a single Layer's buffer). Bottom layer overwrites; top layer blends per its blendMode/opacity. This is the end-to-end pin for the composite loop in Drivers::tick.
TEST_CASE("Drivers composites two enabled Layers into one output buffer") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 4; grid.height = 1; grid.depth = 1;   // 4 lights, dense (no LUT)
    layouts.addChild(&grid);

    mm::Effects effectsContainer;
    // Bottom layer: a checkerboard base.
    mm::Layer bottom; bottom.setChannelsPerLight(3);
    mm::SpiralEffect base; bottom.addChild(&base);
    // Top layer: rainbow, additive at full opacity → bottom + top, clamped.
    mm::Layer top; top.setChannelsPerLight(3);
    mm::RainbowEffect over; top.addChild(&over);
    top.blendMode = mm::Layer::kBlendAdditive;
    top.opacity = 255;

    effectsContainer.addChild(&bottom);
    effectsContainer.addChild(&top);
    effectsContainer.setLayouts(&layouts);

    // The driver is declared BEFORE its container ON PURPOSE: stack objects destruct in reverse declaration order, so this puts ~Drivers() (which stops the core-1 encode worker) ahead of ~CaptureDriver(). The other way round, the worker can still be inside the driver's tick() when its vtable is torn down, a vptr race TSan flags. Production is safe by a different route (release()/removeChild() quiesce before any delete); a stack test has only declaration order.
    CaptureDriver cap;
    mm::Drivers drivers;
    drivers.addChild(&cap);
    drivers.setEffects(&effectsContainer);

    effectsContainer.applyState();
    drivers.applyState();      // sizes + allocates the composite output buffer
    effectsContainer.tick();      // both Layers render their own buffers
    drivers.tick();              // composite into outputBuffer_, hand it to cap

    REQUIRE(effectsContainer.enabledLayerCount() == 2);
    // The driver was handed the composite buffer (4 lights × 3ch), not a raw layer.
    REQUIRE(cap.src_ != nullptr);
    REQUIRE(cap.src_->bytes() == static_cast<size_t>(4 * 3));

    // The composite must equal additive(bottom, top) per channel, clamped, i.e. for every byte, output >= bottom (top only adds) and output >= top's contribution.
    auto& outBuf = *cap.src_;
    auto& botBuf = bottom.buffer();
    auto& topBuf = top.buffer();
    REQUIRE(botBuf.bytes() == outBuf.bytes());
    REQUIRE(topBuf.bytes() == outBuf.bytes());
    bool sawSum = false;
    for (size_t i = 0; i < outBuf.bytes(); i++) {
        uint16_t expect = static_cast<uint16_t>(botBuf.data()[i]) + topBuf.data()[i];
        if (expect > 255) expect = 255;
        CHECK(outBuf.data()[i] == static_cast<uint8_t>(expect));
        if (botBuf.data()[i] && topBuf.data()[i]) sawSum = true;
    }
    CHECK_MESSAGE(sawSum, "expected at least one light where both Layers contribute (proves real compositing)");
}

// Disabling the top layer drops cleanly to the single (bottom) layer, no crash, the driver now sees the bottom layer's content. Pins the robustness path.
TEST_CASE("Drivers composition drops to single layer when one is disabled") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 4; grid.height = 1; grid.depth = 1;
    layouts.addChild(&grid);

    mm::Effects effectsContainer;
    mm::Layer bottom; bottom.setChannelsPerLight(3);
    mm::SpiralEffect base; bottom.addChild(&base);
    mm::Layer top; top.setChannelsPerLight(3);
    mm::RainbowEffect over; top.addChild(&over);
    effectsContainer.addChild(&bottom);
    effectsContainer.addChild(&top);
    effectsContainer.setLayouts(&layouts);

    // The driver is declared BEFORE its container ON PURPOSE: stack objects destruct in reverse declaration order, so this puts ~Drivers() (which stops the core-1 encode worker) ahead of ~CaptureDriver(). The other way round, the worker can still be inside the driver's tick() when its vtable is torn down, a vptr race TSan flags. Production is safe by a different route (release()/removeChild() quiesce before any delete); a stack test has only declaration order.
    CaptureDriver cap;
    mm::Drivers drivers;
    drivers.addChild(&cap);
    drivers.setEffects(&effectsContainer);

    top.setEnabled(false);             // only the bottom layer remains
    effectsContainer.applyState();
    drivers.applyState();
    effectsContainer.tick();
    drivers.tick();

    CHECK(effectsContainer.enabledLayerCount() == 1);
    REQUIRE(cap.src_ != nullptr);      // driver still has a valid buffer, no crash
    REQUIRE(cap.src_->bytes() == static_cast<size_t>(4 * 3));
}

// Drivers' composition/output-buffer allocation contract (architecture.md § Adaptive allocation). The driver output buffer exists ONLY when the pipeline must blend into physical space; otherwise the lone layer's buffer is handed to drivers directly (zero-copy). dynamicBytes() reflects outputBuffer_.bytes(), so it's 0 ⇔ no buffer. Pins all three cases in one place: 1. one identity (no-LUT) layer  → NO output buffer (zero-copy), WITH multicore off 2. two enabled Layers           → output buffer (must composite) 3. one layer WITH a LUT         → output buffer (must map logical→physical) The multicore render↔encode split adds a fourth reason to own a buffer: it is the frame core 1 reads while core 0 renders the next one, so with the split ON the identity case DOES allocate one (case 1b). That is the documented cost of multicore; turning it off (or failing to allocate) restores the zero-copy profile exactly.
TEST_CASE("Drivers allocates the output buffer only when compositing or mapping is needed") {
    // --- Case 1: a single identity (dense-grid, no-LUT) layer, multicore OFF → no output buffer ---
    {
        mm::Layouts layouts; mm::GridLayout grid;
        grid.width = 8; grid.height = 8; grid.depth = 1;
        layouts.addChild(&grid);
        mm::Effects effects;
        mm::Layer only; only.setChannelsPerLight(3);
        mm::SpiralEffect eff; only.addChild(&eff);
        effects.addChild(&only);
        effects.setLayouts(&layouts);
        CaptureDriver cap; mm::Drivers drivers; drivers.addChild(&cap);   // driver first: ~Drivers stops the worker before ~CaptureDriver
        drivers.multicore = false;                   // single-core: the memory-lean zero-copy profile
        drivers.setEffects(&effects);
        effects.applyState(); drivers.applyState();

        CHECK_FALSE(only.lut().hasLUT());            // dense grid → identity, no LUT
        CHECK(effects.enabledLayerCount() == 1);
        CHECK(drivers.dynamicBytes() == 0);          // NO output buffer allocated
        REQUIRE(cap.src_ != nullptr);                // driver reads the layer buffer directly
        CHECK(cap.src_ == &only.buffer());           // zero-copy: it's the layer's own buffer
    }

    // --- Case 1b: the same identity layer, multicore ON → the split owns a buffer (the handoff) ---
    {
        mm::Layouts layouts; mm::GridLayout grid;
        grid.width = 8; grid.height = 8; grid.depth = 1;
        layouts.addChild(&grid);
        mm::Effects effects;
        mm::Layer only; only.setChannelsPerLight(3);
        mm::SpiralEffect eff; only.addChild(&eff);
        effects.addChild(&only);
        effects.setLayouts(&layouts);
        CaptureDriver cap; mm::Drivers drivers; drivers.addChild(&cap);   // driver first: ~Drivers stops the worker before ~CaptureDriver
        drivers.multicore = true;                    // the split needs a stable frame for core 1
        drivers.setEffects(&effects);
        effects.applyState(); drivers.applyState();

        CHECK_FALSE(only.lut().hasLUT());            // still identity — the buffer is NOT for mapping
        CHECK(drivers.dynamicBytes() == 8 * 8 * 3);  // one frame: the cross-core handoff buffer
        REQUIRE(cap.src_ != nullptr);
        CHECK(cap.src_ != &only.buffer());           // core 1 reads the stable handoff, not the live layer
        drivers.release();                           // stop the worker before the stack objects die
    }

    // --- Case 2: two enabled Layers → output buffer (must composite) ---
    {
        mm::Layouts layouts; mm::GridLayout grid;
        grid.width = 8; grid.height = 8; grid.depth = 1;
        layouts.addChild(&grid);
        mm::Effects effects;
        mm::Layer a; a.setChannelsPerLight(3); mm::SpiralEffect ea; a.addChild(&ea);
        mm::Layer b; b.setChannelsPerLight(3); mm::RainbowEffect eb; b.addChild(&eb);
        effects.addChild(&a); effects.addChild(&b);
        effects.setLayouts(&layouts);
        CaptureDriver cap; mm::Drivers drivers; drivers.addChild(&cap);   // driver first: ~Drivers stops the worker before ~CaptureDriver
        drivers.setEffects(&effects);
        effects.applyState(); drivers.applyState();

        CHECK(effects.enabledLayerCount() == 2);
        CHECK(drivers.dynamicBytes() == static_cast<size_t>(8 * 8 * 3));  // output buffer allocated
        REQUIRE(cap.src_ != nullptr);
        CHECK(cap.src_ != &a.buffer());              // driver reads the composite, not a raw layer
    }

    // --- Case 3: a single layer WITH a LUT (a mirror modifier) → output buffer ---
    {
        mm::Layouts layouts; mm::GridLayout grid;
        grid.width = 8; grid.height = 8; grid.depth = 1;
        layouts.addChild(&grid);
        mm::Effects effects;
        mm::Layer only; only.setChannelsPerLight(3);
        mm::SpiralEffect eff; only.addChild(&eff);
        mm::MultiplyModifier mirror; mirror.mirrorX = true; only.addChild(&mirror);
        effects.addChild(&only);
        effects.setLayouts(&layouts);
        CaptureDriver cap; mm::Drivers drivers; drivers.addChild(&cap);   // driver first: ~Drivers stops the worker before ~CaptureDriver
        drivers.setEffects(&effects);
        effects.applyState(); drivers.applyState();

        CHECK(only.lut().hasLUT());                  // mirror modifier → a real LUT
        CHECK(effects.enabledLayerCount() == 1);
        CHECK(drivers.dynamicBytes() > 0);           // output buffer allocated (map target)
        REQUIRE(cap.src_ != nullptr);
        CHECK(cap.src_ != &only.buffer());           // driver reads the mapped output, not the logical buffer
    }

    // --- Case 4: a live layer is DISABLED → drivers transition to idle, no stale buffer ---
    // The real-world sequence: a frame is published with the layer enabled, then the user disables it and the pipeline rebuilds. activeLayer() still surfaces the (now disabled) layer so geometry stays queryable, but output selection must use the *enabled* source, with none, the driver's source buffer goes null so it emits nothing instead of re-sending the last frame off the disabled layer.
    {
        mm::Layouts layouts; mm::GridLayout grid;
        grid.width = 8; grid.height = 8; grid.depth = 1;
        layouts.addChild(&grid);
        mm::Effects effects;
        mm::Layer only; only.setChannelsPerLight(3);
        mm::SpiralEffect eff; only.addChild(&eff);
        // A LUT modifier so the pre-fix bug would route through the output path, proves the disabled gate, not just the no-LUT zero-copy branch.
        mm::MultiplyModifier mirror; mirror.mirrorX = true; only.addChild(&mirror);
        effects.addChild(&only);
        effects.setLayouts(&layouts);
        CaptureDriver cap; mm::Drivers drivers; drivers.addChild(&cap);   // driver first: ~Drivers stops the worker before ~CaptureDriver
        drivers.setEffects(&effects);

        // Enabled first: the driver has a valid source buffer (a real frame).
        effects.applyState(); drivers.applyState();
        CHECK(effects.firstEnabledLayer() == &only);
        CHECK(effects.enabledLayerCount() == 1);
        REQUIRE(cap.src_ != nullptr);                // a frame is being published

        // Now disable the only layer and rebuild, the driver must drop to idle.
        only.setEnabled(false);
        effects.applyState(); drivers.applyState();
        CHECK(effects.activeLayer() == &only);        // fallback for geometry
        CHECK(effects.firstEnabledLayer() == nullptr);// no enabled source
        CHECK(effects.enabledLayerCount() == 0);
        CHECK(drivers.dynamicBytes() == 0);          // no output buffer allocated
        CHECK(cap.src_ == nullptr);                  // driver idle — the prior frame is NOT re-sent
    }
}

// activeLayer() returns the first enabled child, or the only child if all are disabled (so dimensions stay queryable during boot/toggle-off).
TEST_CASE("Effects::activeLayer returns first enabled child, or nullptr when empty") {
    mm::Effects empty;
    CHECK(empty.activeLayer() == nullptr);

    mm::Effects oneChild;
    mm::Layer onlyLayer;
    oneChild.addChild(&onlyLayer);
    CHECK(oneChild.activeLayer() == &onlyLayer);

    // Disabling the only child still surfaces it as the fallback (so dimensions can still be queried for buffer allocation, important during boot or a toggle-everything-off state).
    onlyLayer.setEnabled(false);
    CHECK(oneChild.activeLayer() == &onlyLayer);

    // With two children, a disabled first one yields the second as active.
    mm::Effects twoChildren;
    mm::Layer first, second;
    twoChildren.addChild(&first);
    twoChildren.addChild(&second);
    first.setEnabled(false);
    CHECK(twoChildren.activeLayer() == &second);
}

// firstEnabledLayer() is the output-selection counterpart to activeLayer(): it never falls back to a disabled layer, so it returns nullptr exactly when nothing renders.
TEST_CASE("Effects::firstEnabledLayer returns first enabled child, nullptr when all disabled") {
    mm::Effects empty;
    CHECK(empty.firstEnabledLayer() == nullptr);

    mm::Effects effects;
    mm::Layer first, second;
    effects.addChild(&first);
    effects.addChild(&second);
    CHECK(effects.firstEnabledLayer() == &first);     // both enabled → first

    first.setEnabled(false);
    CHECK(effects.firstEnabledLayer() == &second);    // skips the disabled first
    CHECK(effects.activeLayer() == &second);          // agrees while one stays enabled

    second.setEnabled(false);
    CHECK(effects.firstEnabledLayer() == nullptr);    // none enabled → nothing renders
    CHECK(effects.activeLayer() == &first);           // but geometry fallback still resolves
}

// If the container holds only non-Layer children, activeLayer() returns nullptr (the role-guard skips, never miscasts).
TEST_CASE("Effects::activeLayer returns nullptr when no child has role Layer") {
    // The role-guard in activeLayer (and setLayouts) skips non-Layer children rather than miscasting. Today the UI's acceptsChildren mapping keeps non-Layer children out, but the engine doesn't enforce it, so the engine must degrade gracefully. Pin the contract: a Effects container populated only with non-Layer children returns nullptr from activeLayer(), not a miscast pointer.
    struct GenericChild : public mm::MoonModule {};

    mm::Effects effects;
    GenericChild stranger;
    effects.addChild(&stranger);
    CHECK(stranger.role() == mm::ModuleRole::Generic);  // sanity check the stub
    CHECK(effects.activeLayer() == nullptr);             // skipped, not miscast
}

// The disable cascade: disabling a PARENT releases every descendant's resources, because applyState() routes each node by its own effectivelyEnabled(), which is false for a child whose ancestor is disabled. This is the core guarantee of the unified lifecycle: a disabled subtree holds nothing (memory or hardware). FireEffect is the probe, its heat buffer's dynamicBytes() is host-observable, standing in for any per-module resource.
TEST_CASE("Disabling a parent Layer cascades release to its effects (effectivelyEnabled)") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 16; grid.height = 16; grid.depth = 1;
    layouts.addChild(&grid);

    mm::Effects effects;
    mm::Layer layer;
    layer.setChannelsPerLight(3);
    effects.addChild(&layer);
    effects.setLayouts(&layouts);
    mm::FireEffect fire;                    // holds a heap heat buffer sized to the grid
    layer.addChild(&fire);

    effects.applyState();                    // build the whole tree
    REQUIRE(fire.enabled());                // itself enabled
    REQUIRE(fire.effectivelyEnabled());     // and no ancestor disabled
    CHECK(fire.dynamicBytes() > 0);         // heat buffer allocated

    // Disable the PARENT layer (the effect's own flag stays true) and re-sweep, as the Scheduler does after an enabled-toggle. The effect is now effectively-disabled (ancestor off) → its applyState routes to release → heap freed, even though fire.enabled() is still true.
    layer.setEnabled(false);
    effects.applyState();
    CHECK(fire.enabled());                  // the effect's OWN flag is untouched
    CHECK_FALSE(fire.effectivelyEnabled()); // but an ancestor is disabled
    CHECK(fire.dynamicBytes() == 0);        // cascade released the child's memory

    // Re-enable the parent → the effect (still self-enabled) re-acquires on the next sweep.
    layer.setEnabled(true);
    effects.applyState();
    CHECK(fire.effectivelyEnabled());
    CHECK(fire.dynamicBytes() > 0);         // re-acquired

    // Effects-specific: an effect builds against the LAYER's LUT/buffer, so the router must run the Layer's prepare() (rebuild the LUT) BEFORE recursing into the effect (applyState visits the parent, then children). Prove the re-enabled effect actually RENDERS, a stale/zero-size buffer would leave the frame black. A tick after re-enable must write non-zero pixels.
    layer.tick();                           // ticks the effect through the Layer
    const uint8_t* buf = layer.buffer().data();
    bool anyLit = false;
    for (size_t i = 0; i < layer.buffer().bytes() && !anyLit; i++) anyLit = buf[i] != 0;
    CHECK(anyLit);                          // Fire renders into the freshly-rebuilt buffer

    // A child individually disabled under an enabled parent also releases.
    fire.setEnabled(false);
    effects.applyState();
    CHECK(fire.dynamicBytes() == 0);
}
