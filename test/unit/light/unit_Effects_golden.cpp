/// @module EffectBase
///
/// @technical
///
/// ## What carries no golden, and why
///
/// DemoReelEffect is absent because it hosts whichever effects the ModuleFactory registry happens to contain, and that registry is global and populated by whatever tests ran before, so its frame depends on test ORDER, not on its own code. A hash there would flap. Its behavior test (unit_DemoReelEffect) covers it with an explicit registry.
///
/// Audio-driven effects (Blurz, GEQ, GEQ3D, FreqMatrix, FreqSaws, NoiseMeter, PaintBrush, AudioSpectrum) are deliberately ABSENT: their output depends on whatever the audio service holds, so a hash over their frames would pin the test rig's audio state rather than the effect. Their migrations rely on their behavior tests plus the Canvas equivalence test in unit_Canvas.
/// @also AuroraEffect, BallpitEffect, BouncingBallsEffect, DissolveEffect, DistortionWavesEffect, EchoEffect,
/// @also FireEffect, FireworksEffect, FishTankEffect, FixedRectangleEffect, FluidEffect,
/// @also FlyingToastersEffect, GameOfLifeEffect, LavaLampEffect, LissajousEffect, MetaballsEffect,
/// @also NebulaEffect, NoiseEffect, MoonManEffect, PlasmaEffect, PolarNoiseEffect, PraxisEffect,
/// @also RainbowEffect, RingsEffect, RubiksCubeEffect, SdfShapesEffect, SineEffect, SolidEffect,
/// @also SphereMoveEffect, SpiralEffect, StarFieldEffect, StarSkyEffect, TetrixEffect, TextEffect,
/// @also RadialSpectrumEffect, VuMetersEffect, TrailsEffect, TruchetEffect, TunnelEffect, WaterRippleEffect, WaveEffect

/// Pins the exact frames of the time-driven effects, a hash catching the one-step phase error a behavior test passes; a moved hash is re-blessed in its commit, naming which moved and why.

#include "golden_frame.h"

#include "light/effects/BouncingBallsEffect.h"
#include "light/effects/FixedRectangleEffect.h"
#include "light/effects/LissajousEffect.h"
#include "light/effects/PraxisEffect.h"
#include "light/effects/SolidEffect.h"
#include "light/effects/SphereMoveEffect.h"
#include "light/effects/TetrixEffect.h"
#include "light/effects/TextEffect.h"
#include "light/effects/GameOfLifeEffect.h"
#include "light/effects/RubiksCubeEffect.h"
#include "light/effects/StarFieldEffect.h"
#include "light/effects/DistortionWavesEffect.h"
#include "light/effects/LavaLampEffect.h"
#include "light/effects/MetaballsEffect.h"
#include "light/effects/NoiseEffect.h"
#include "light/effects/PlasmaEffect.h"
#include "light/effects/FireEffect.h"
#include "light/effects/RainbowEffect.h"
#include "light/effects/AuroraEffect.h"
#include "light/effects/RingsEffect.h"
#include "light/effects/SdfShapesEffect.h"
#include "light/effects/PolarNoiseEffect.h"
#include "light/effects/WaterRippleEffect.h"
#include "light/effects/FluidEffect.h"
#include "light/effects/NebulaEffect.h"
#include "light/effects/RadialSpectrumEffect.h"
#include "light/effects/VuMetersEffect.h"
#include "light/effects/TrailsEffect.h"
#include "light/effects/TunnelEffect.h"
#include "light/effects/EchoEffect.h"
#include "light/effects/DissolveEffect.h"
#include "light/effects/FireworksEffect.h"
#include "light/effects/BallpitEffect.h"
#include "light/effects/FishTankEffect.h"
#include "light/effects/MoonManEffect.h"
#include "light/effects/FlyingToastersEffect.h"
#include "light/effects/TruchetEffect.h"
#include "light/effects/SineEffect.h"
#include "light/effects/StarSkyEffect.h"
#include "light/effects/SpiralEffect.h"
#include "light/effects/WaveEffect.h"

using namespace mm;

// A 16x16 grid at the real 20 ms cadence, where a phase error shows as a column shift; the polar effects are also pinned by unit_PolarLut_equivalence.
TEST_CASE("time-driven effects render byte-identical frames (migration guard)") {
    SUBCASE("two SDF shapes orbit and melt together, with a soft edge")       { SdfShapesEffect e;       golden::checkGolden("SdfShapesEffect", golden::renderHash(e, 16, 16, 1), 0x48dfac88d9b68e63ull); }
    SUBCASE("a warped noise field folded into a kaleidoscope")      { PolarNoiseEffect e;      golden::checkGolden("PolarNoiseEffect", golden::renderHash(e, 16, 16, 1), 0x0c458394ac1ddfb9ull); }
    SUBCASE("heat rises, cools and colors through the palette")      { FireEffect e;            golden::checkGolden("FireEffect", golden::renderHash(e, 16, 16, 1), 0x1cadbabb59bc489bull); }
    SUBCASE("dye poured into a simulated medium, carried by the flow it works out") { FluidEffect e; golden::checkGolden("FluidEffect", golden::renderHash(e, 16, 16, 1), 0xa800da826044d579ull); }
    SUBCASE("a field births light and a curl flow carries it into a cloud")          { NebulaEffect e; golden::checkGolden("NebulaEffect", golden::renderHash(e, 16, 16, 1), 0x8d881df4db5e2966ull); }
    SUBCASE("sixteen VU needles with mass, one per band") { VuMetersEffect e; golden::checkGolden("VuMetersEffect", golden::renderHash(e, 16, 16, 1), 0x4c9ddf61e3f3bf78ull); }
    SUBCASE("the spectrum as ripples, one sector per band, radius as time") { RadialSpectrumEffect e; golden::checkGolden("RadialSpectrumEffect", golden::renderHash(e, 16, 16, 1), 0xf76a40a372582783ull); }
    SUBCASE("dots thrown into a flow, leaving tails it carries and bends") { TrailsEffect e; golden::checkGolden("TrailsEffect", golden::renderHash(e, 16, 16, 1), 0x0aeb0a1bb0a56d14ull); }
    SUBCASE("layered noise curtains, each drifting on its own clock")      { AuroraEffect e;          golden::checkGolden("AuroraEffect", golden::renderHash(e, 16, 16, 1), 0xbdb7ab706c5d26beull); }
    SUBCASE("drops ripple, reflect off the edges and interfere")     { WaterRippleEffect e;     golden::checkGolden("WaterRippleEffect", golden::renderHash(e, 16, 16, 1), 0xa11f9c4f27cba8d5ull); }
    SUBCASE("a texture-mapped tunnel flying toward a vanishing point")          { TunnelEffect e;          golden::checkGolden("TunnelEffect", golden::renderHash(e, 16, 16, 1), 0x2ec1e1faaed6f867ull); }
    SUBCASE("the previous frame fed back zoomed and rotated, leaving trails")            { EchoEffect e;            golden::checkGolden("EchoEffect", golden::renderHash(e, 16, 16, 1), 0xffb9406f5147804bull); }
    SUBCASE("two color fields trade places pixel by pixel")        { DissolveEffect e;        golden::checkGolden("DissolveEffect", golden::renderHash(e, 16, 16, 1), 0x2ad6b1d52b19d4dbull); }
    SUBCASE("shells rise, stall at their apex and burst into falling sparks")       { FireworksEffect e;       golden::checkGolden("FireworksEffect", golden::renderHash(e, 16, 16, 1), 0x7636d32bfb3f4b2bull); }
    SUBCASE("balls fall, pile up and shove each other aside")         { BallpitEffect e;         golden::checkGolden("BallpitEffect", golden::renderHash(e, 16, 16, 1), 0xdd4efe1ccba2a4b2ull); }
    SUBCASE("fish swim across the tank, each its own color from the palette")  { FishTankEffect e;  golden::checkGolden("FishTankEffect", golden::renderHash(e, 16, 16, 1), 0xb6f0d0b269ff7004ull); }
    SUBCASE("MoonMan and the invaders cross the wall, each invader its own color")  { MoonManEffect e;  golden::checkGolden("MoonManEffect", golden::renderHash(e, 16, 16, 1), 0x61a8334144208811ull); }
    // The hash moved 2026-08-28: launch() draws its speed span with next16() instead of an 8-bit value, which the old draw clamped at large sprite scales. Per-toaster velocities change, so the frame does; the effect is correct, the golden was re-recorded.
    SUBCASE("toasters flap and drift diagonally, toast trails along")  { FlyingToastersEffect e;  golden::checkGolden("FlyingToastersEffect", golden::renderHash(e, 16, 16, 1), 0x8958d378f86b9e3bull); }
    SUBCASE("arc tiles join into endless winding paths")         { TruchetEffect e;         golden::checkGolden("TruchetEffect", golden::renderHash(e, 16, 16, 1), 0xda7982c3c584d9adull); }
    SUBCASE("SineEffect")            { SineEffect e;            golden::checkGolden("SineEffect",            golden::renderHash(e, 16, 16, 1), 0x22463ad81c1eb4d3ull); }
    SUBCASE("PlasmaEffect")          { PlasmaEffect e;          golden::checkGolden("PlasmaEffect",          golden::renderHash(e, 16, 16, 1), 0x4770f59a1fb09993ull); }
    SUBCASE("NoiseEffect")           { NoiseEffect e;           golden::checkGolden("NoiseEffect",           golden::renderHash(e, 16, 16, 1), 0x5f73da8d9b5769efull); }
    SUBCASE("DistortionWavesEffect") { DistortionWavesEffect e; golden::checkGolden("DistortionWavesEffect", golden::renderHash(e, 16, 16, 1), 0x5e1d9348525db90eull); }
    SUBCASE("LavaLampEffect")        { LavaLampEffect e;        golden::checkGolden("LavaLampEffect",        golden::renderHash(e, 16, 16, 1), 0x3c312e8a75b9ac83ull); }
    SUBCASE("MetaballsEffect")       { MetaballsEffect e;       golden::checkGolden("MetaballsEffect",       golden::renderHash(e, 16, 16, 1), 0xc338977fbb8da2d3ull); }
    SUBCASE("SpiralEffect")          { SpiralEffect e;          golden::checkGolden("SpiralEffect",          golden::renderHash(e, 16, 16, 1), 0xa62a2abaa3c5a609ull); }
    SUBCASE("RingsEffect")           { RingsEffect e;           golden::checkGolden("RingsEffect",           golden::renderHash(e, 16, 16, 1), 0xf3b1ea7162afcf6bull); }
    SUBCASE("WaveEffect")            { WaveEffect e;            golden::checkGolden("WaveEffect",            golden::renderHash(e, 16, 16, 1), 0x53fbd86e8b6961ccull); }
    SUBCASE("StarSkyEffect")         { StarSkyEffect e;         golden::checkGolden("StarSkyEffect",         golden::renderHash(e, 16, 16, 1), 0xa7ff8aab806be9ffull); }
    SUBCASE("RainbowEffect")         { RainbowEffect e;         golden::checkGolden("RainbowEffect",         golden::renderHash(e, 16, 16, 1), 0x75a2b1be1db07979ull); }
    SUBCASE("BouncingBallsEffect")    { BouncingBallsEffect e;       golden::checkGolden("BouncingBallsEffect", golden::renderHash(e, 16, 16, 1), 0xfdf2b36940ae4e75ull); }   // its balls in closed form, phases hashed by column
    SUBCASE("FixedRectangleEffect")   { FixedRectangleEffect e;      golden::checkGolden("FixedRectangleEffect", golden::renderHash(e, 16, 16, 1), 0x22b828f908e9ce1cull); }
    SUBCASE("LissajousEffect")        { LissajousEffect e;           golden::checkGolden("LissajousEffect", golden::renderHash(e, 16, 16, 1), 0x7a5f13102f039d12ull); }
    SUBCASE("PraxisEffect")           { PraxisEffect e;              golden::checkGolden("PraxisEffect", golden::renderHash(e, 16, 16, 1), 0x0420f0404b3f12c5ull); }
    SUBCASE("SolidEffect")            { SolidEffect e;               golden::checkGolden("SolidEffect", golden::renderHash(e, 16, 16, 1), 0x56711c1cf0c8ae83ull); }
    SUBCASE("SphereMoveEffect")       { SphereMoveEffect e;          golden::checkGolden("SphereMoveEffect", golden::renderHash(e, 16, 16, 1), 0x9d30711413e470a5ull); }   // its jitter hashed by place and tick
    SUBCASE("TetrixEffect")           { TetrixEffect e;              golden::checkGolden("TetrixEffect", golden::renderHash(e, 16, 16, 1), 0xb2b13038316b916dull); }
    SUBCASE("TextEffect")             { TextEffect e;                golden::checkGolden("TextEffect", golden::renderHash(e, 16, 16, 1), 0xf2344942db9d42ffull); }
    SUBCASE("GameOfLifeEffect")       { GameOfLifeEffect e;          golden::checkGolden("GameOfLifeEffect", golden::renderHash(e, 16, 16, 1), 0xb2fb46cdf32ddd8bull); }
    SUBCASE("RubiksCubeEffect")       { RubiksCubeEffect e;          golden::checkGolden("RubiksCubeEffect", golden::renderHash(e, 16, 16, 1), 0xecd4da66adc09f5dull); }
    SUBCASE("StarFieldEffect")        { StarFieldEffect e;           golden::checkGolden("StarFieldEffect", golden::renderHash(e, 16, 16, 1), 0x332be82582722b41ull); }
}
