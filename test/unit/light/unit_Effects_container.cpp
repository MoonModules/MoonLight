/// @module Effects
/// @also Layer

#include "doctest.h"
#include "light/layouts/Layouts.h"
#include "light/layers/Effects.h"
#include "light/layouts/GridLayout.h"
#include "light/effects/RainbowEffect.h"
#include "light/effects/SpiralEffect.h"
#include "light/effects/DemoReelEffect.h"
#include "light/effects/FireEffect.h"   // heap-holding effect — the cascade-release probe
#include "light/modifiers/MultiplyModifier.h"
#include "light/modifiers/ModifierBase.h"
#include "light/drivers/Drivers.h"
#include "platform/platform.h"

#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

// RAII guard that restores the platform test clock on scope exit even if a REQUIRE/CHECK throws, without this, a mid-test failure would leave the global setTestNowMs override in place and pollute later test cases.
struct ClockGuard {
    ~ClockGuard() { mm::platform::setTestNowMs(0); }
};

// Effects renders its layers and Drivers composites them: one layer matches a bare Layer byte for byte; several each render their own buffer, on the render workers where a computer has them.

// A Effects container with one child Layer must produce the same output as that Layer used directly (no-op container).
TEST_CASE("Effects with one Layer produces the same output as a bare Layer") {
    // Pinned time, so both paths read the same millis() and Rainbow's hue sits still between the two ticks, which byte-exact comparison needs.
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

    // Each layer renders its own buffer (Spiral a spiral, Rainbow a hue gradient); the composite of the two is pinned by "Drivers composites two enabled Layers" below.
    auto& bufA = layerA.buffer();
    auto& bufB = layerB.buffer();
    REQUIRE(bufA.bytes() == static_cast<size_t>(8 * 8 * 3));
    REQUIRE(bufB.bytes() == static_cast<size_t>(8 * 8 * 3));

    bool aHasNonZero = false, bHasNonZero = false;
    for (size_t i = 0; i < bufA.bytes(); i++) if (bufA.data()[i] != 0) { aHasNonZero = true; break; }
    for (size_t i = 0; i < bufB.bytes(); i++) if (bufB.data()[i] != 0) { bHasNonZero = true; break; }
    CHECK_MESSAGE(aHasNonZero, "Layer A (Rainbow) wrote no pixels");
    CHECK_MESSAGE(bHasNonZero, "Layer B (Spiral) wrote no pixels");
}

// A minimal driver that just records the source buffer it's handed each tick, so a test can inspect the composited output without a real network/LED sink.
class CaptureDriver : public mm::DriverBase {
public:
    void setSourceBuffer(mm::Buffer* buf) override { src_ = buf; }
    mm::Buffer* src_ = nullptr;
};

// Two 3-channel layers over a 4×1 grid, blended by Drivers into a capturing driver. Drivers is the last member, so it destructs first and stops the core-1 encode worker before the capture goes, the order TSan needs.
struct TwoLayers {
    mm::Layouts layouts;
    mm::GridLayout grid;
    mm::Effects effects;
    mm::Layer bottom, top;
    CaptureDriver cap;
    mm::Drivers drivers;
    TwoLayers(mm::MoonModule& under, mm::MoonModule& over) {
        grid.width = 4; grid.height = 1; grid.depth = 1;
        layouts.addChild(&grid);
        bottom.setChannelsPerLight(3);
        bottom.addChild(&under);
        top.setChannelsPerLight(3);
        top.addChild(&over);
        effects.addChild(&bottom);
        effects.addChild(&top);
        effects.setLayouts(&layouts);
        drivers.addChild(&cap);
        drivers.setEffects(&effects);
    }
    void prepare() { effects.applyState(); drivers.applyState(); }
    void frame() { effects.tick(); drivers.tick(); }
};

// Drivers blends two or more enabled layers into its own output buffer and hands that to the drivers: the bottom overwrites, the top blends per its mode and opacity. The end-to-end pin for Drivers::tick's composite.
TEST_CASE("Drivers composites two enabled Layers into one output buffer") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 4; grid.height = 1; grid.depth = 1;   // 4 lights, dense (no LUT)
    layouts.addChild(&grid);

    mm::Effects effectsContainer;
    // Bottom layer: a spiral base.
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

    // Declared before its container on purpose: ~Drivers() stops the encode worker before ~CaptureDriver() runs, or the worker can be inside the driver's tick() as its vtable goes, a race TSan flags.
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
    mm::SpiralEffect base;
    mm::RainbowEffect over;
    TwoLayers rig(base, over);
    rig.top.setEnabled(false);         // only the bottom layer remains
    rig.prepare();
    rig.frame();

    CHECK(rig.effects.enabledLayerCount() == 1);
    REQUIRE(rig.cap.src_ != nullptr);  // driver still has a valid buffer, no crash
    REQUIRE(rig.cap.src_->bytes() == static_cast<size_t>(4 * 3));
}

// The output buffer exists only to composite, to map through a LUT, or for the multicore split (the frame core 1 reads); a lone identity layer hands its own buffer to the drivers. dynamicBytes() is 0 exactly when there is none.
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
    // A frame published, then the layer disabled and rebuilt: activeLayer() still answers for geometry, but the driver's source goes null, so it emits no stale frame of the disabled layer.
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
    // activeLayer() and setLayouts() skip children that are not layers rather than miscast them, since the engine does not enforce the UI's child roles: a container of such children answers nullptr.
    struct GenericChild : public mm::MoonModule {};

    mm::Effects effects;
    GenericChild stranger;
    effects.addChild(&stranger);
    CHECK(stranger.role() == mm::ModuleRole::Generic);  // sanity check the stub
    CHECK(effects.activeLayer() == nullptr);             // skipped, not miscast
}

// Disabling a parent releases every descendant's resources, since applyState() routes each node by its effectivelyEnabled(); FireEffect's heat buffer, visible as dynamicBytes(), stands in for any module's resource.
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

    // The parent disabled and swept again, as the Scheduler does: the effect is effectively disabled, so its applyState releases and frees the heap while fire.enabled() stays true.
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

    // The layer's prepare() rebuilds the LUT before the effect builds on it, so the re-enabled effect renders: a tick writes lit pixels, where a stale buffer would leave the frame black.
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

namespace {
// Counts its renders.
class CountingRender : public mm::EffectBase {
public:
    void tick() MM_NONBLOCKING override { renders++; }
    int renders = 0;
};
}  // namespace

// A show alternates expensive effects by opacity, so an invisible layer of a composite costs nothing, the bottom one too. A lone layer is read as it is, so it always renders.
TEST_CASE("a composite's layer at opacity 0 is not rendered, the bottom one included; a lone layer always is") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 8; grid.height = 8; grid.depth = 1;
    layouts.addChild(&grid);
    mm::Effects effects;
    mm::Layer bottom, top;
    CountingRender fxBottom, fxTop;
    bottom.addChild(&fxBottom);
    top.addChild(&fxTop);
    effects.addChild(&bottom);
    effects.addChild(&top);
    effects.setLayouts(&layouts);
    effects.applyState();
    bottom.opacity = 0;
    top.opacity = 0;
    effects.tick();
    CHECK(fxBottom.renders == 0);
    CHECK(fxTop.renders == 0);
    top.opacity = 1;
    effects.tick();
    CHECK(fxTop.renders == 1);
    top.setEnabled(false);   // the bottom alone: no composite, so it renders whatever its opacity
    effects.tick();
    CHECK(fxBottom.renders == 1);
}

// On the StadBeest the walk look, the bottom of three layers, stayed lit at opacity 0 while the autopilot faded the others in and out.
TEST_CASE("the bottom layer of a composite fades by its own opacity, over black") {
    mm::platform::setTestNowMs(5000);   // frozen, so every frame draws the same rainbow and each compares with the first
    mm::RainbowEffect base;
    mm::SpiralEffect over;
    TwoLayers rig(base, over);
    rig.top.opacity = 0;   // only the bottom shows, so the output is the bottom scaled by its opacity
    rig.prepare();
    rig.frame();
    REQUIRE(rig.cap.src_ != nullptr);
    const mm::Buffer& bottom = rig.bottom.buffer();
    const std::vector<uint8_t> full(bottom.data(), bottom.data() + bottom.bytes());
    bool lit = false;
    for (size_t i = 0; i < full.size(); i++) { CHECK(rig.cap.src_->data()[i] == full[i]); lit = lit || full[i]; }
    REQUIRE(lit);   // the rainbow lights something, so a fade is visible

    rig.bottom.opacity = 0;
    rig.frame();
    for (size_t i = 0; i < full.size(); i++) CHECK(rig.cap.src_->data()[i] == 0);   // faded out entirely

    rig.bottom.opacity = 128;
    rig.frame();
    for (size_t i = 0; i < full.size(); i++) CHECK(rig.cap.src_->data()[i] <= full[i] / 2 + 1);   // about half, never more
}

// A faded bottom layer keeps its moving heads' aim, while a faded layer above it blends its aim into theirs by its mode: opacity fades a position only where something lies under it.
TEST_CASE("a faded bottom layer keeps its motion bytes, and a faded top layer blends its own into them") {
    mm::RainbowEffect base;
    mm::SpiralEffect over;
    TwoLayers rig(base, over);
    rig.bottom.setChannelsPerLight(6);   // RGBW, then pan and tilt
    rig.top.setChannelsPerLight(6);
    rig.prepare();
    // The pan byte of every light, each layer aiming its own way, as the composite sees it.
    const auto panAfter = [&](uint8_t bottomOpacity, uint8_t topOpacity) {
        rig.bottom.opacity = bottomOpacity;
        rig.top.opacity = topOpacity;
        rig.effects.tick();
        for (size_t light = 0; light < 4; light++) {
            rig.bottom.buffer().data()[light * 6 + mm::FixtureChannels::kMotionBase] = 200;
            rig.top.buffer().data()[light * 6 + mm::FixtureChannels::kMotionBase] = 100;
        }
        rig.drivers.tick();
        REQUIRE(rig.cap.src_ != nullptr);
        return rig.cap.src_->data()[mm::FixtureChannels::kMotionBase];
    };
    CHECK(panAfter(128, 0) == 200);   // the bottom alone, faded: its aim, not half of it
    CHECK(panAfter(255, 0) == 200);   // at full opacity it overwrites, aim included
    const uint8_t mixed = panAfter(128, 128);
    CHECK(mixed > 200);                // the top, half faded, adds half its aim, additive being its default mode
    CHECK(mixed < 255);
}

// Four layers of time-driven effects over a 16x16 grid, as a frame's work for the render workers.
struct FourLayers {
    mm::Layouts layouts;
    mm::GridLayout grid;
    mm::Effects effects;
    mm::Layer layers[4];
    mm::RainbowEffect rainbowA, rainbowB;
    mm::SpiralEffect spiralA, spiralB;
    FourLayers() {
        grid.width = 16; grid.height = 16; grid.depth = 1;
        layouts.addChild(&grid);
        mm::MoonModule* fx[4] = {&rainbowA, &spiralA, &rainbowB, &spiralB};
        for (int i = 0; i < 4; i++) {
            layers[i].setChannelsPerLight(3);
            layers[i].addChild(fx[i]);
            effects.addChild(&layers[i]);
        }
        effects.setLayouts(&layouts);
    }
};

TEST_CASE("render workers render a frame's layers byte for byte as the render loop alone does") {
    ClockGuard clockGuard;
    FourLayers alone, pooled;
    mm::platform::setTestSpawnFails(true);
    alone.effects.setup();   // no worker starts: the render loop renders every layer
    mm::platform::setTestSpawnFails(false);
    pooled.effects.setup();
    const uint8_t want = static_cast<uint8_t>(mm::platform::cpuCores() - 1);
    CHECK(alone.effects.renderWorkers() == 0);
    CHECK(pooled.effects.renderWorkers() == (want < mm::WorkerPool::kMaxWorkers ? want : mm::WorkerPool::kMaxWorkers));
    alone.effects.applyState();
    pooled.effects.applyState();
    for (uint32_t frame = 0; frame < 20; frame++) {
        mm::platform::setTestNowMs(1000 + frame * 16);
        alone.effects.tick();
        pooled.effects.tick();
        for (int i = 0; i < 4; i++) {
            const mm::Buffer& a = alone.layers[i].buffer();
            const mm::Buffer& p = pooled.layers[i].buffer();
            REQUIRE(a.bytes() == p.bytes());
            CHECK(std::memcmp(a.data(), p.data(), a.bytes()) == 0);
        }
    }
}

// A modifier that asks for a rebuild every frame and counts the folds of each rebuild, and those off the render loop's thread.
class RebuildProbe : public mm::ModifierBase {
public:
    std::thread::id renderThread = std::this_thread::get_id();
    mutable std::atomic<int> folds{0}, elsewhere{0};
    bool modifyLogical(mm::Coord3D&) const override {
        folds.fetch_add(1);
        if (std::this_thread::get_id() != renderThread) elsewhere.fetch_add(1);
        return true;
    }
    bool consumeNeedsRebuild() override { return true; }
};

TEST_CASE("render workers leave a modifier's rebuild to the render loop, one layer at a time") {
    // Layers share the layout a rebuild places lights through, so a rebuild on a worker would run beside another layer's.
    ClockGuard clockGuard;
    RebuildProbe probes[4];
    FourLayers rig;
    for (int i = 0; i < 4; i++) rig.layers[i].addChild(&probes[i]);
    rig.effects.setup();
    rig.effects.applyState();
    int built[4];
    for (int i = 0; i < 4; i++) built[i] = probes[i].folds.load();   // the folds applyState made
    for (uint32_t frame = 0; frame < 2; frame++) {   // frame 0 asks, frame 1 applies
        mm::platform::setTestNowMs(1000 + frame * 16);
        rig.effects.tick();
    }
    for (int i = 0; i < 4; i++) {
        CHECK(probes[i].folds.load() > built[i]);   // a rebuild the frames asked for, beyond applyState's
        CHECK(probes[i].elsewhere.load() == 0);
    }
}

// A hosted effect that records the thread it is created on, as a stand-in the reel can pick.
struct HostedProbe : mm::RainbowEffect {
    static inline std::thread::id renderThread = std::this_thread::get_id();
    static inline std::atomic<bool> armed{false};
    static inline std::atomic<int> created{0}, createdElsewhere{0};
    HostedProbe() {
        if (!armed.load()) return;   // registerType builds a probe instance too
        created.fetch_add(1);
        if (std::this_thread::get_id() != renderThread) createdElsewhere.fetch_add(1);
    }
};

TEST_CASE("render workers leave a demo reel's effect swap to the render loop") {
    // Create and setParent write state every layer shares (the factory's name buffer, the last-enabled effect), so two reels advancing side by side swap on the render loop.
    ClockGuard clockGuard;
    mm::ModuleFactory::registerType<mm::RainbowEffect>("RainbowEffect");
    mm::ModuleFactory::registerType<mm::SpiralEffect>("SpiralEffect");
    mm::ModuleFactory::registerType<HostedProbe>("HostedProbeEffect");
    mm::ModuleFactory::registerType<mm::DemoReelEffect>("DemoReelEffect");

    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 8; grid.height = 8; grid.depth = 1;
    layouts.addChild(&grid);
    mm::Effects effects;
    mm::Layer layers[2];
    mm::DemoReelEffect reels[2];
    for (int i = 0; i < 2; i++) {
        layers[i].setChannelsPerLight(3);
        reels[i].interval = 1;
        reels[i].randomPalette = false;
        layers[i].addChild(&reels[i]);
        effects.addChild(&layers[i]);
    }
    effects.setLayouts(&layouts);
    effects.setup();
    if (effects.renderWorkers() == 0) return;   // a one-core host renders on the render loop alone, where the swap already is
    effects.applyState();
    const int n = reels[0].eligibleCountForTest();
    REQUIRE(n >= 2);
    HostedProbe::created = 0;
    HostedProbe::createdElsewhere = 0;
    HostedProbe::armed = true;
    // Each frame is past the interval: the tick decides a swap and the next frame's serial phase applies it, so a lap of n effects fits in n + 2 frames.
    const int frames = 2 * (n + 2);
    const char* previous[2] = {reels[0].currentTypeForTest(), reels[1].currentTypeForTest()};
    int moved[2] = {0, 0};
    for (int frame = 0; frame < frames; frame++) {
        mm::platform::setTestNowMs(1000 + static_cast<uint32_t>(frame) * 1001);
        effects.tick();
        for (int i = 0; i < 2; i++) {
            const char* now = reels[i].currentTypeForTest();
            REQUIRE(now != nullptr);
            if (std::strcmp(now, previous[i]) != 0) moved[i]++;
            previous[i] = now;
        }
    }
    HostedProbe::armed = false;
    for (int i = 0; i < 2; i++) CHECK(moved[i] > 0);   // each reel moved on
    CHECK(HostedProbe::created.load() > 0);            // the probe was picked and created
    CHECK(HostedProbe::createdElsewhere.load() == 0);  // on the render loop, as the swap runs there
}
