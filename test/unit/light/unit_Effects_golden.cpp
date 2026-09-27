/// @module EffectBase
///
/// @moreinfo
///
/// ## What carries no golden, and why
///
/// DemoReelEffect is absent because it hosts whichever effects the ModuleFactory registry happens to contain, and that registry is global and populated by whatever tests ran before, so its frame depends on test ORDER, not on its own code. A hash there would flap. Its behavior test (unit_DemoReelEffect) covers it with an explicit registry.
///
/// Audio-driven effects (Blurz, GEQ, GEQ3D, FreqMatrix, FreqSaws, NoiseMeter, PaintBrush, AudioSpectrum) are deliberately ABSENT: their output depends on whatever the audio service holds, so a hash over their frames would pin the test rig's audio state rather than the effect. Their migrations rely on their behavior tests plus the Canvas equivalence test in unit_Canvas.
/// @also AuroraEffect, BallpitEffect, BouncingBallsEffect, DissolveEffect, DistortionWavesEffect, EchoEffect,
/// @also FireEffect, FireworksEffect, FishTankEffect, FixedRectangleEffect, FluidEffect,
/// @also FlyingToastersEffect, GameOfLifeEffect, LavaLampEffect, LissajousEffect, MetaballsEffect,
/// @also NebulaEffect, NoiseEffect, PacmanEffect, PlasmaEffect, PolarNoiseEffect, PraxisEffect,
/// @also RainbowEffect, RingsEffect, RubiksCubeEffect, SdfShapesEffect, SineEffect, SolidEffect,
/// @also SphereMoveEffect, SpiralEffect, StarFieldEffect, StarSkyEffect, TetrixEffect, TextEffect,
/// @also RadialSpectrumEffect, VuMetersEffect, TrailsEffect, TruchetEffect, TunnelEffect, WaterRippleEffect, WaveEffect

/// Pins the EXACT rendered output of the time-driven effects, so the power-function migration's "renders exactly the same" claim is proved rather than asserted.
///
/// These ten effects each hand-roll the same BPM phase accumulator, and step 1 of the migration replaces that hand-rolled arithmetic with the shared BeatPhase. A behavior test still passes if the replacement is off by one LSB or drifts over frames; only a hash over the frame catches it.
///
/// Five hashes were UPDATED on 2026-08-06 with the BeatPhase migration, Sine, Plasma, DistortionWaves, Spiral and Metaballs, all for the SAME reason, deliberately: the hand-rolled accumulator started from `lastElapsed_ = 0`, so on the very first tick it added `now * bpm` and the startup phase depended on how long the device had been running, the same pattern would make an effect start at a different point in its animation on every boot. BeatPhase uses the first call as the time base only. Verified as the SOLE cause by reproducing the old first-tick behavior on top of BeatPhase and watching the original hash return.
///
/// The control case proves it: WaveEffect ALREADY carried that guard, and its hash did NOT move across the same migration. So a moved hash here means "this effect gained the guard", not "the migration drifted". (NoiseEffect was a second control case until the gradient-noise swap and the two-noise merge moved its hash for reasons of their own, recorded below.)
///
/// Every other hash below was captured from the code BEFORE the migration and must not move. If one does, either the migration changed the arithmetic (a bug, the accumulators are meant to be identical) or the change was intentional and reviewed, in which case the golden is updated in the same commit with the reason in the message.
///
/// Every FrameTime user carries hashes captured AFTER two changes: their motion moved onto elapsed time (architecture.md, tick-rate rule), and FrameTime's reference period became exact, deriving it as `1000 / 60` truncated to 16 ms, a 62.5 Hz reference that ran every 60-fps-calibrated setting about 4% fast. What these effects draw on a given frame changed; the motion per SECOND is now correct rather than merely consistent. unit_Effects_framerate.cpp pins the behavior behind them.

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
#include "light/effects/PacmanEffect.h"
#include "light/effects/FlyingToastersEffect.h"
#include "light/effects/TruchetEffect.h"
#include "light/effects/SineEffect.h"
#include "light/effects/StarSkyEffect.h"
#include "light/effects/SpiralEffect.h"
#include "light/effects/WaveEffect.h"

using namespace mm;

// A 2D grid wide enough that a phase error shows as a visible column shift, small enough to stay a fast unit test.
// Eight frames at the real 20 ms cadence exercise the accumulator's carry.
//
// A hash moves only when the rendered output does, so a move is a decision rather than a failure.
// Re-bless it once the change is understood, and name in the commit which effects moved and why.
// Spiral, Tunnel and PolarNoise are pinned by unit_PolarLut_equivalence instead, which requires the 16-bit table to render bit-identically to the computed address.
TEST_CASE("time-driven effects render byte-identical frames (migration guard)") {
    SUBCASE("two SDF shapes orbit and melt together, with a soft edge")       { SdfShapesEffect e;       golden::checkGolden("SdfShapesEffect", golden::renderHash(e, 16, 16, 1), 0xbcfb74b4836606a3ull); }
    SUBCASE("a warped noise field folded into a kaleidoscope")      { PolarNoiseEffect e;      golden::checkGolden("PolarNoiseEffect", golden::renderHash(e, 16, 16, 1), 0x8d48e0d1e0180610ull); }
    SUBCASE("heat rises, cools and colors through the palette")      { FireEffect e;            golden::checkGolden("FireEffect", golden::renderHash(e, 16, 16, 1), 0x1cadbabb59bc489bull); }
    SUBCASE("dye poured into a simulated medium, carried by the flow it works out") { FluidEffect e; golden::checkGolden("FluidEffect", golden::renderHash(e, 16, 16, 1), 0x46f503452c0435efull); }
    SUBCASE("a field births light and a curl flow carries it into a cloud")          { NebulaEffect e; golden::checkGolden("NebulaEffect", golden::renderHash(e, 16, 16, 1), 0x6cefd6a2f242aeafull); }
    SUBCASE("sixteen VU needles with mass, one per band") { VuMetersEffect e; golden::checkGolden("VuMetersEffect", golden::renderHash(e, 16, 16, 1), 0x4c9ddf61e3f3bf78ull); }
    SUBCASE("the spectrum as ripples, one sector per band, radius as time") { RadialSpectrumEffect e; golden::checkGolden("RadialSpectrumEffect", golden::renderHash(e, 16, 16, 1), 0xf76a40a372582783ull); }
    SUBCASE("dots thrown into a flow, leaving tails it carries and bends") { TrailsEffect e; golden::checkGolden("TrailsEffect", golden::renderHash(e, 16, 16, 1), 0x2f9b1de2ed8382beull); }
    SUBCASE("layered noise curtains, each drifting on its own clock")      { AuroraEffect e;          golden::checkGolden("AuroraEffect", golden::renderHash(e, 16, 16, 1), 0xfb4329a20959443dull); }
    SUBCASE("drops ripple, reflect off the edges and interfere")     { WaterRippleEffect e;     golden::checkGolden("WaterRippleEffect", golden::renderHash(e, 16, 16, 1), 0xa11f9c4f27cba8d5ull); }
    SUBCASE("a texture-mapped tunnel flying toward a vanishing point")          { TunnelEffect e;          golden::checkGolden("TunnelEffect", golden::renderHash(e, 16, 16, 1), 0x7b4d4451a3de3887ull); }
    SUBCASE("the previous frame fed back zoomed and rotated, leaving trails")            { EchoEffect e;            golden::checkGolden("EchoEffect", golden::renderHash(e, 16, 16, 1), 0x53d2ba4d4fdf9499ull); }
    SUBCASE("two color fields trade places pixel by pixel")        { DissolveEffect e;        golden::checkGolden("DissolveEffect", golden::renderHash(e, 16, 16, 1), 0xeb7810ca874152bcull); }
    SUBCASE("shells rise, stall at their apex and burst into falling sparks")       { FireworksEffect e;       golden::checkGolden("FireworksEffect", golden::renderHash(e, 16, 16, 1), 0x5ffbfcab94c90a94ull); }
    SUBCASE("balls fall, pile up and shove each other aside")         { BallpitEffect e;         golden::checkGolden("BallpitEffect", golden::renderHash(e, 16, 16, 1), 0xdd4efe1ccba2a4b2ull); }
    SUBCASE("fish swim across the tank, each its own color from the palette")  { FishTankEffect e;  golden::checkGolden("FishTankEffect", golden::renderHash(e, 16, 16, 1), 0x3564663bebacac3aull); }
    SUBCASE("Pacman and the ghosts cross the wall, each ghost its own color")  { PacmanEffect e;  golden::checkGolden("PacmanEffect", golden::renderHash(e, 16, 16, 1), 0x776b749b4d02eb9bull); }
    // The hash moved 2026-08-28: launch() draws its speed span with next16() instead of an 8-bit value, which the old draw clamped at large sprite scales. Per-toaster velocities change, so the frame does; the effect is correct, the golden was re-recorded.
    SUBCASE("toasters flap and drift diagonally, toast trails along")  { FlyingToastersEffect e;  golden::checkGolden("FlyingToastersEffect", golden::renderHash(e, 16, 16, 1), 0x8958d378f86b9e3bull); }
    SUBCASE("arc tiles join into endless winding paths")         { TruchetEffect e;         golden::checkGolden("TruchetEffect", golden::renderHash(e, 16, 16, 1), 0xdcb9b41536eff043ull); }
    SUBCASE("SineEffect")            { SineEffect e;            golden::checkGolden("SineEffect",            golden::renderHash(e, 16, 16, 1), 0xe96c6fd2da1b264bull); }
    SUBCASE("PlasmaEffect")          { PlasmaEffect e;          golden::checkGolden("PlasmaEffect",          golden::renderHash(e, 16, 16, 1), 0xfe821e9102099b93ull); }
    SUBCASE("NoiseEffect")           { NoiseEffect e;           golden::checkGolden("NoiseEffect",           golden::renderHash(e, 16, 16, 1), 0xf3c36c9cccfdd04full); }
    SUBCASE("DistortionWavesEffect") { DistortionWavesEffect e; golden::checkGolden("DistortionWavesEffect", golden::renderHash(e, 16, 16, 1), 0xe4cd8111e8159133ull); }
    SUBCASE("LavaLampEffect")        { LavaLampEffect e;        golden::checkGolden("LavaLampEffect",        golden::renderHash(e, 16, 16, 1), 0x3c312e8a75b9ac83ull); }
    SUBCASE("MetaballsEffect")       { MetaballsEffect e;       golden::checkGolden("MetaballsEffect",       golden::renderHash(e, 16, 16, 1), 0x96a26bf931ad8341ull); }
    SUBCASE("SpiralEffect")          { SpiralEffect e;          golden::checkGolden("SpiralEffect",          golden::renderHash(e, 16, 16, 1), 0xfb0fb3b138dde70full); }
    SUBCASE("RingsEffect")           { RingsEffect e;           golden::checkGolden("RingsEffect",           golden::renderHash(e, 16, 16, 1), 0xf3b1ea7162afcf6bull); }
    SUBCASE("WaveEffect")            { WaveEffect e;            golden::checkGolden("WaveEffect",            golden::renderHash(e, 16, 16, 1), 0xa1150376dd23bea1ull); }
    SUBCASE("StarSkyEffect")         { StarSkyEffect e;         golden::checkGolden("StarSkyEffect",         golden::renderHash(e, 16, 16, 1), 0xa7ff8aab806be9ffull); }
    SUBCASE("RainbowEffect")         { RainbowEffect e;         golden::checkGolden("RainbowEffect",         golden::renderHash(e, 16, 16, 1), 0x75a2b1be1db07979ull); }
    SUBCASE("BouncingBallsEffect")    { BouncingBallsEffect e;       golden::checkGolden("BouncingBallsEffect", golden::renderHash(e, 16, 16, 1), 0x8b89c982e566f5b4ull); }
    SUBCASE("FixedRectangleEffect")   { FixedRectangleEffect e;      golden::checkGolden("FixedRectangleEffect", golden::renderHash(e, 16, 16, 1), 0x22b828f908e9ce1cull); }
    SUBCASE("LissajousEffect")        { LissajousEffect e;           golden::checkGolden("LissajousEffect", golden::renderHash(e, 16, 16, 1), 0x7a5f13102f039d12ull); }
    SUBCASE("PraxisEffect")           { PraxisEffect e;              golden::checkGolden("PraxisEffect", golden::renderHash(e, 16, 16, 1), 0x0420f0404b3f12c5ull); }
    SUBCASE("SolidEffect")            { SolidEffect e;               golden::checkGolden("SolidEffect", golden::renderHash(e, 16, 16, 1), 0x56711c1cf0c8ae83ull); }
    SUBCASE("SphereMoveEffect")       { SphereMoveEffect e;          golden::checkGolden("SphereMoveEffect", golden::renderHash(e, 16, 16, 1), 0xb3f3d7c75fe49fdbull); }
    SUBCASE("TetrixEffect")           { TetrixEffect e;              golden::checkGolden("TetrixEffect", golden::renderHash(e, 16, 16, 1), 0xcb0541c3597a6709ull); }
    SUBCASE("TextEffect")             { TextEffect e;                golden::checkGolden("TextEffect", golden::renderHash(e, 16, 16, 1), 0xf2344942db9d42ffull); }
    SUBCASE("GameOfLifeEffect")       { GameOfLifeEffect e;          golden::checkGolden("GameOfLifeEffect", golden::renderHash(e, 16, 16, 1), 0xb2fb46cdf32ddd8bull); }
    SUBCASE("RubiksCubeEffect")       { RubiksCubeEffect e;          golden::checkGolden("RubiksCubeEffect", golden::renderHash(e, 16, 16, 1), 0xecd4da66adc09f5dull); }
    SUBCASE("StarFieldEffect")        { StarFieldEffect e;           golden::checkGolden("StarFieldEffect", golden::renderHash(e, 16, 16, 1), 0x332be82582722b41ull); }
}
