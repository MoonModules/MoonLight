/// Registers every module type the firmware can build, so a preset, a scenario or the type picker can name any of them.
///
/// @moreinfo
///
/// ## Why the registry is its own translation unit
///
/// The composition root and the in-process scenario runner both need every type registered, and two hand-kept lists drift.
/// A list that missed the default effect is what this file replaces, so the runner could not construct the module every device boots with.
/// One home, included by both, and a type added here reaches both at once.
///
/// ## Why LED drivers are gated by the preprocessor
///
/// The preprocessor rather than `if constexpr`, because the goal is excluding the code and a constexpr branch still compiles every arm.
/// These are capability macros, not the platform ones the boundary rule forbids.
/// Each parallel-WS2812 backend header self-registers its factory into ParallelLedDriver's peripheral registry, gated by the chip's `CONFIG_SOC_*`.
/// So including the ones this silicon supports is what populates the `peripheral` control's options.
/// Registering only what the silicon can run keeps the type picker honest, offering no I80Peripheral on a chip without an i80 bus, and keeps the binary lean.
/// NDI, HLS and RTSP are gated by CAPABILITY instead: their headers compile everywhere, since the platform calls are declared on every target.
/// An `if constexpr` discarded branch must still PARSE, so those includes cannot be gated.
///
/// ## Why panel cards are per firmware and HUB75 is per chip
///
/// Panel receiver cards need a gigabit link, and no capability macro separates the boards that have one.
/// So the firmware catalogue names the variants that get it, and everything else saves the flash.
/// HUB75 is a GPIO panel rather than a receiver card, needing LCD_CAM or Parlio silicon and nothing else, so it gates on the chip like every other LED driver.
/// Tying it to `MM_PANEL_CARDS` hid it from every S3 that is not a panel-card firmware, which is most of them.
///
/// ## Why the quiesce-render hook is a function pointer
///
/// Before core mutates the tree, adding, removing or replacing a child, core 1 stops so it cannot dereference a node being freed.
/// Core cannot name Drivers, a light module, so it calls through this function-pointer seam: see MoonModule quiesceForMutation.
/// Wired here, the one place that legitimately depends on both sides.
///
/// ## What registerType captures
///
/// The second argument is the module's spec page, which the UI turns into a help link.
/// Effects, modifiers and leaf layouts share one page per type; the rest keep their own.
/// `registerType<T>` also captures the type's `dimensions()` via if-constexpr when present.
/// EffectBase and ModifierBase both expose one, so the UI's chip lights up without any per-domain wrapper.
/// Layouts, effects and modifiers are registered alphabetically by display name, matching the picker and the docs so the three orders agree at a glance.
///
#include "module_types.h"

#include "core/module/Scheduler.h"
#include "light/layers/Effects.h"
#include "light/layouts/GridLayout.h"
#include "light/layouts/GridBlacksLayout.h"
#include "light/layouts/SphereLayout.h"
#include "light/layouts/WheelLayout.h"
#include "light/layouts/SingleRowLayout.h"
#include "light/layouts/SingleColumnLayout.h"
#include "light/layouts/PanelLayout.h"
#include "light/layouts/CubeLayout.h"
#include "light/layouts/TubesLayout.h"
#include "light/layouts/RingLayout.h"
#include "light/layouts/Rings241Layout.h"
#include "light/layouts/SpiralLayout.h"
#include "light/layouts/RectangleLayout.h"
#include "light/layouts/PanelsLayout.h"
#include "light/layouts/HumanSizedCubeLayout.h"
#include "light/layouts/TorontoBarGourdsLayout.h"
#include "light/layouts/CarLightsLayout.h"
#include "light/effects/LinesEffect.h"
#include "light/effects/RainbowEffect.h"
#include "light/effects/WaveEffect.h"
#include "light/effects/FluidEffect.h"
#include "light/effects/NebulaEffect.h"
#include "light/effects/NoiseEffect.h"
#include "light/effects/FixedPointEffect.h"
#include "light/effects/MovingHeadEffect.h"
#include "light/effects/PacmanEffect.h"
#include "light/effects/PlasmaEffect.h"
#include "light/effects/PulseEffect.h"
#include "light/effects/MetaballsEffect.h"
#include "light/effects/FireEffect.h"
#include "light/effects/ParticlesEffect.h"
#include "light/moonlive/MoonLiveEffect.h"
#include "light/moonlive/MoonLiveModifier.h"
#include "light/moonlive/MoonLiveLayout.h"
#include "light/effects/SpiralEffect.h"
#include "light/effects/RingsEffect.h"
#include "light/effects/RipplesEffect.h"
#include "light/effects/LavaLampEffect.h"
#include "light/effects/NetworkReceiveEffect.h"
#include "light/effects/RadialSpectrumEffect.h"
#include "light/effects/VuMetersEffect.h"
#include "light/effects/BeatRipplesEffect.h"
#include "light/effects/AmbilightEffect.h"
#include "light/effects/AudioSpectrumEffect.h"
#include "light/effects/SineEffect.h"
#include "light/effects/DistortionWavesEffect.h"
#include "light/effects/GameOfLifeEffect.h"
#include "light/effects/GEQ3DEffect.h"
#include "light/effects/PaintBrushEffect.h"
#include "light/effects/SolidEffect.h"
#include "light/effects/StarSkyEffect.h"
#include "light/effects/SdfShapesEffect.h"
#include "light/effects/AuroraEffect.h"
#include "light/effects/PolarNoiseEffect.h"
#include "light/effects/WaterRippleEffect.h"
#include "light/effects/TrailsEffect.h"
#include "light/effects/ColorTrailsEffect.h"
#include "light/effects/TunnelEffect.h"
#include "light/effects/EchoEffect.h"
#include "light/effects/DissolveEffect.h"
#include "light/effects/SpectrumEffect.h"
#include "light/effects/FireworksEffect.h"
#include "light/effects/BallpitEffect.h"
#include "light/effects/FishTankEffect.h"
#include "light/effects/FlyingToastersEffect.h"
#include "light/effects/PongEffect.h"
#include "light/effects/SpaceInvadersEffect.h"
#include "light/effects/SpriteFountainEffect.h"
#include "light/effects/TruchetEffect.h"
#include "light/effects/VectorBallsEffect.h"
#include "light/effects/RaymarchEffect.h"
#include "light/effects/SphereMoveEffect.h"
#include "light/effects/StarFieldEffect.h"
#include "light/effects/PraxisEffect.h"
#include "light/effects/FixedRectangleEffect.h"
#include "light/effects/RandomEffect.h"
#include "light/effects/LissajousEffect.h"
#include "light/effects/RubiksCubeEffect.h"
#include "light/effects/BouncingBallsEffect.h"
#include "light/effects/TetrixEffect.h"
#include "light/effects/TextEffect.h"
#include "light/effects/FreqSawsEffect.h"
#include "light/effects/BlurzEffect.h"
#include "light/effects/FreqMatrixEffect.h"
#include "light/effects/GEQEffect.h"
#include "light/effects/NoiseMeterEffect.h"
#include "light/effects/DemoReelEffect.h"
#include "light/modifiers/MultiplyModifier.h"
#include "light/modifiers/CheckerboardModifier.h"
#include "light/modifiers/RandomMapModifier.h"
#include "light/modifiers/RotateModifier.h"
#include "light/modifiers/RegionModifier.h"
#include "light/modifiers/MirrorModifier.h"
#include "light/modifiers/TransposeModifier.h"
#include "light/modifiers/CircleModifier.h"
#include "light/modifiers/BlockModifier.h"
#include "light/modifiers/PinwheelModifier.h"
#include "light/modifiers/RippleXZModifier.h"
#include "light/drivers/Drivers.h"   // the Drivers container (registered + wired below); driver subclasses include DriverBase.h directly
#include "light/drivers/LightPresetsModule.h"  // the reusable light-preset library (Drivers submodule)
#include "light/drivers/HueDriver.h"
#include "light/drivers/NetworkSendDriver.h"
#include "light/drivers/NdiDriver.h"
#include "light/drivers/HlsDriver.h"
#include "light/drivers/RtspDriver.h"
#include "light/drivers/PreviewDriver.h"
/// LED drivers are compiled in per chip, gated on the peripheral each one needs, so a board carries only the drivers its silicon can run: @xref{why-led-drivers-are-gated-by-the-preprocessor}.
#if defined(CONFIG_SOC_RMT_SUPPORTED) || MM_LINKS_ALL_LED_DRIVERS
#include "light/drivers/RmtLedDriver.h"
#endif
// The parallel-WS2812 driver + its peripheral backends: @xref{why-led-drivers-are-gated-by-the-preprocessor}.
#if defined(CONFIG_SOC_LCD_I80_SUPPORTED) || MM_LINKS_ALL_LED_DRIVERS
#include "light/drivers/I80Peripheral.h"      // esp_lcd i80 backend (I80Peripheral)
#endif
#if defined(CONFIG_SOC_LCDCAM_I80_LCD_SUPPORTED) || MM_LINKS_ALL_LED_DRIVERS
#include "light/drivers/MoonI80Peripheral.h"          // MoonI80 own-GDMA backend (MoonI80Peripheral)
#endif
#if defined(CONFIG_SOC_PARLIO_SUPPORTED) || MM_LINKS_ALL_LED_DRIVERS
#include "light/drivers/ParlioPeripheral.h"        // Parlio backend (ParlioPeripheral)
#endif
// Panel receiver cards are opt-in per firmware rather than per chip: @xref{why-panel-cards-are-per-firmware-and-hub75-is-per-chip}.
#if defined(MM_PANEL_CARDS) || MM_LINKS_ALL_LED_DRIVERS
#include "light/drivers/PanelCardDriver.h"
#endif
// HUB75 is a GPIO panel, not a receiver card, so it gates on the chip: @xref{why-panel-cards-are-per-firmware-and-hub75-is-per-chip}.
#if defined(CONFIG_SOC_LCDCAM_I80_LCD_SUPPORTED) || defined(CONFIG_SOC_PARLIO_SUPPORTED) || \
    MM_LINKS_ALL_LED_DRIVERS
#include "light/drivers/Hub75Driver.h"
#endif
#include "core/system/HttpServerModule.h"
#include "core/system/SystemModule.h"
#include "core/system/ControlModule.h"
#include "core/services/Services.h"
#include "core/services/AudioService.h"
#include "core/services/VideoService.h"
#include "core/services/OscModule.h"
#include "core/system/I2cScanModule.h"
#include "core/system/TasksModule.h"
#include "core/system/PinsModule.h"
#include "core/services/AnalogService.h"
#include "core/services/ButtonService.h"
#include "core/services/InfraredService.h"
#include "core/services/MoonLiveService.h"
#include "core/system/FileManagerModule.h"
#include "core/system/FirmwareUpdateModule.h"
#include "core/system/MoonCloudModule.h"
#include "core/system/MoonStatsModule.h"
#include "core/system/MoonTalkModule.h"
#include "core/system/ImprovProvisioningModule.h"
#include "core/system/MqttModule.h"
#include "core/system/DevicesModule.h"
#include "core/system/FilesystemModule.h"
#include "core/util/ModuleFactory.h"
#include "platform/platform.h"

#include "core/system/NetworkModule.h"


void mm::registerModuleTypes() {
    // Containers first, the second argument being the module's spec page: @xref{what-registertype-captures}.
    mm::ModuleFactory::registerType<mm::Layouts>("Layouts", "light/supporting.md#layouts");
    mm::ModuleFactory::registerType<mm::Effects>("Effects", "light/supporting.md#effects");
    mm::ModuleFactory::registerType<mm::Layer>("Layer", "light/supporting.md#layer");
    mm::ModuleFactory::registerType<mm::Drivers>("Drivers", "light/supporting.md#drivers");
    mm::ModuleFactory::registerType<mm::LightPresetsModule>("LightPresetsModule", "light/supporting.md#lightpresets");

    // Wire the core quiesce-render hook to the light domain's encode worker: @xref{why-the-quiesce-render-hook-is-a-function-pointer}.
    mm::MoonModule::setQuiesceRenderHook([] { if (auto* d = mm::Drivers::active()) d->quiesceRenderSplit(); });
    // Concrete modules, layouts first, alphabetical by display name: @xref{what-registertype-captures}.
    mm::ModuleFactory::registerType<mm::CarLightsLayout>("CarLightsLayout", "light/layouts.md#carlights");
    mm::ModuleFactory::registerType<mm::CubeLayout>("CubeLayout", "light/layouts.md#cube");
    mm::ModuleFactory::registerType<mm::HumanSizedCubeLayout>("HumanSizedCubeLayout", "light/layouts.md#humansizedcube");
    mm::ModuleFactory::registerType<mm::MoonLiveLayout>("MoonLiveLayout",
                                                        "light/layouts.md#moonlive");
    mm::ModuleFactory::registerType<mm::PanelsLayout>("PanelsLayout", "light/layouts.md#panels");
    mm::ModuleFactory::registerType<mm::TorontoBarGourdsLayout>("TorontoBarGourdsLayout", "light/layouts.md#torontobargourds");
    mm::ModuleFactory::registerType<mm::GridLayout>("GridLayout", "light/layouts.md#grid");
    mm::ModuleFactory::registerType<mm::GridBlacksLayout>("GridBlacksLayout", "light/layouts.md#gridblacks");
    mm::ModuleFactory::registerType<mm::PanelLayout>("PanelLayout", "light/layouts.md#panel");
    mm::ModuleFactory::registerType<mm::RectangleLayout>("RectangleLayout", "light/layouts.md#rectangle");
    mm::ModuleFactory::registerType<mm::RingLayout>("RingLayout", "light/layouts.md#ring");
    mm::ModuleFactory::registerType<mm::Rings241Layout>("Rings241Layout", "light/layouts.md#rings241");
    mm::ModuleFactory::registerType<mm::SingleColumnLayout>("SingleColumnLayout", "light/layouts.md#singlecolumn");
    mm::ModuleFactory::registerType<mm::SingleRowLayout>("SingleRowLayout", "light/layouts.md#singlerow");
    mm::ModuleFactory::registerType<mm::SphereLayout>("SphereLayout", "light/layouts.md#sphere");
    mm::ModuleFactory::registerType<mm::SpiralLayout>("SpiralLayout", "light/layouts.md#spiral");
    mm::ModuleFactory::registerType<mm::TubesLayout>("TubesLayout", "light/layouts.md#tubes");
    mm::ModuleFactory::registerType<mm::WheelLayout>("WheelLayout", "light/layouts.md#wheel");
    // Effects, registered alphabetically by display name: @xref{what-registertype-captures}.
    mm::ModuleFactory::registerType<mm::AmbilightEffect>("AmbilightEffect", "light/effects.md#ambilight");
    mm::ModuleFactory::registerType<mm::AudioSpectrumEffect>("AudioSpectrumEffect", "light/effects.md#audiospectrum");
    mm::ModuleFactory::registerType<mm::RadialSpectrumEffect>("RadialSpectrumEffect", "light/effects.md#radialspectrum");
    mm::ModuleFactory::registerType<mm::VuMetersEffect>("VuMetersEffect", "light/effects.md#vumeters");
    mm::ModuleFactory::registerType<mm::BeatRipplesEffect>("BeatRipplesEffect", "light/effects.md#beatripples");
    mm::ModuleFactory::registerType<mm::BlurzEffect>("BlurzEffect", "light/effects.md#blurz");
    mm::ModuleFactory::registerType<mm::BouncingBallsEffect>("BouncingBallsEffect", "light/effects.md#bouncingballs");
    mm::ModuleFactory::registerType<mm::DemoReelEffect>("DemoReelEffect", "light/effects.md#demoreel");
    mm::ModuleFactory::registerType<mm::DistortionWavesEffect>("DistortionWavesEffect", "light/effects.md#distortionwaves");
    mm::ModuleFactory::registerType<mm::FireEffect>("FireEffect", "light/effects.md#fire");
    mm::ModuleFactory::registerType<mm::FixedRectangleEffect>("FixedRectangleEffect", "light/effects.md#fixedrectangle");
    mm::ModuleFactory::registerType<mm::FreqMatrixEffect>("FreqMatrixEffect", "light/effects.md#freqmatrix");
    mm::ModuleFactory::registerType<mm::FreqSawsEffect>("FreqSawsEffect", "light/effects.md#freqsaws");
    mm::ModuleFactory::registerType<mm::GameOfLifeEffect>("GameOfLifeEffect", "light/effects.md#gameoflife");
    mm::ModuleFactory::registerType<mm::GEQEffect>("GEQEffect", "light/effects.md#geq");
    mm::ModuleFactory::registerType<mm::GEQ3DEffect>("GEQ3DEffect", "light/effects.md#geq3d");
    mm::ModuleFactory::registerType<mm::LavaLampEffect>("LavaLampEffect", "light/effects.md#lavalamp");
    mm::ModuleFactory::registerType<mm::LinesEffect>("LinesEffect", "light/effects.md#lines");
    mm::ModuleFactory::registerType<mm::LissajousEffect>("LissajousEffect", "light/effects.md#lissajous");
    mm::ModuleFactory::registerType<mm::MetaballsEffect>("MetaballsEffect", "light/effects.md#metaballs");
    mm::ModuleFactory::registerType<mm::MoonLiveEffect>("MoonLiveEffect", "light/effects.md#moonlive");
    mm::ModuleFactory::registerType<mm::NetworkReceiveEffect>("NetworkReceiveEffect", "light/effects.md#networkreceive");
    mm::ModuleFactory::registerType<mm::NoiseEffect>("NoiseEffect", "light/effects.md#noise");
    mm::ModuleFactory::registerType<mm::NoiseMeterEffect>("NoiseMeterEffect", "light/effects.md#noisemeter");
    mm::ModuleFactory::registerType<mm::PaintBrushEffect>("PaintBrushEffect", "light/effects.md#paintbrush");
    mm::ModuleFactory::registerType<mm::ParticlesEffect>("ParticlesEffect", "light/effects.md#particles");
    mm::ModuleFactory::registerType<mm::PlasmaEffect>("PlasmaEffect", "light/effects.md#plasma");
    mm::ModuleFactory::registerType<mm::PraxisEffect>("PraxisEffect", "light/effects.md#praxis");
    mm::ModuleFactory::registerType<mm::PulseEffect>("PulseEffect", "light/effects.md#pulse");
    mm::ModuleFactory::registerType<mm::RainbowEffect>("RainbowEffect", "light/effects.md#rainbow");
    mm::ModuleFactory::registerType<mm::RandomEffect>("RandomEffect", "light/effects.md#random");
    mm::ModuleFactory::registerType<mm::RingsEffect>("RingsEffect", "light/effects.md#rings");
    mm::ModuleFactory::registerType<mm::RipplesEffect>("RipplesEffect", "light/effects.md#ripples");
    mm::ModuleFactory::registerType<mm::RubiksCubeEffect>("RubiksCubeEffect", "light/effects.md#rubikscube");
    mm::ModuleFactory::registerType<mm::SineEffect>("SineEffect", "light/effects.md#sine");
    mm::ModuleFactory::registerType<mm::SolidEffect>("SolidEffect", "light/effects.md#solid");
    mm::ModuleFactory::registerType<mm::SdfShapesEffect>("SdfShapesEffect", "light/effects.md#sdfshapes");
    mm::ModuleFactory::registerType<mm::AuroraEffect>("AuroraEffect", "light/effects.md#aurora");
    mm::ModuleFactory::registerType<mm::PolarNoiseEffect>("PolarNoiseEffect", "light/effects.md#polarnoise");
    mm::ModuleFactory::registerType<mm::WaterRippleEffect>("WaterRippleEffect", "light/effects.md#waterripple");
    mm::ModuleFactory::registerType<mm::FluidEffect>("FluidEffect", "light/effects.md#fluid");
    mm::ModuleFactory::registerType<mm::NebulaEffect>("NebulaEffect", "light/effects.md#nebula");
    mm::ModuleFactory::registerType<mm::TrailsEffect>("TrailsEffect", "light/effects.md#trails");
    mm::ModuleFactory::registerType<mm::ColorTrailsEffect>("ColorTrailsEffect", "light/effects.md#colortrails");
    mm::ModuleFactory::registerType<mm::TunnelEffect>("TunnelEffect", "light/effects.md#tunnel");
    mm::ModuleFactory::registerType<mm::EchoEffect>("EchoEffect", "light/effects.md#echo");
    mm::ModuleFactory::registerType<mm::DissolveEffect>("DissolveEffect", "light/effects.md#dissolve");
    mm::ModuleFactory::registerType<mm::SpectrumEffect>("SpectrumEffect", "light/effects.md#spectrum");
    mm::ModuleFactory::registerType<mm::FireworksEffect>("FireworksEffect", "light/effects.md#fireworks");
    mm::ModuleFactory::registerType<mm::FishTankEffect>("FishTankEffect", "light/effects.md#fishtank");
    mm::ModuleFactory::registerType<mm::PacmanEffect>("PacmanEffect", "light/effects.md#pacman");
    mm::ModuleFactory::registerType<mm::FixedPointEffect>("FixedPointEffect", "light/effects.md#fixedpoint");
    mm::ModuleFactory::registerType<mm::MovingHeadEffect>("MovingHeadEffect", "light/effects.md#movinghead");
    mm::ModuleFactory::registerType<mm::FlyingToastersEffect>("FlyingToastersEffect", "light/effects.md#flyingtoasters");
    mm::ModuleFactory::registerType<mm::SpaceInvadersEffect>("SpaceInvadersEffect", "light/effects.md#spaceinvaders");
    mm::ModuleFactory::registerType<mm::SpriteFountainEffect>("SpriteFountainEffect", "light/effects.md#spritefountain");
    mm::ModuleFactory::registerType<mm::PongEffect>("PongEffect", "light/effects.md#pong");
    mm::ModuleFactory::registerType<mm::BallpitEffect>("BallpitEffect", "light/effects.md#ballpit");
    mm::ModuleFactory::registerType<mm::TruchetEffect>("TruchetEffect", "light/effects.md#truchet");
    mm::ModuleFactory::registerType<mm::VectorBallsEffect>("VectorBallsEffect", "light/effects.md#vectorballs");
#if MM_HEAVY_COMPUTE
    // Only where the platform declares per-pixel float headroom; absent entirely elsewhere.
    mm::ModuleFactory::registerType<mm::RaymarchEffect>("RaymarchEffect", "light/effects.md#raymarch");
#endif
    mm::ModuleFactory::registerType<mm::SphereMoveEffect>("SphereMoveEffect", "light/effects.md#spheremove");
    mm::ModuleFactory::registerType<mm::SpiralEffect>("SpiralEffect", "light/effects.md#spiral");
    mm::ModuleFactory::registerType<mm::StarFieldEffect>("StarFieldEffect", "light/effects.md#starfield");
    mm::ModuleFactory::registerType<mm::StarSkyEffect>("StarSkyEffect", "light/effects.md#starsky");
    mm::ModuleFactory::registerType<mm::TetrixEffect>("TetrixEffect", "light/effects.md#tetrix");
    mm::ModuleFactory::registerType<mm::TextEffect>("TextEffect", "light/effects.md#text");
    mm::ModuleFactory::registerType<mm::WaveEffect>("WaveEffect", "light/effects.md#wave");
    // Modifiers, alphabetical by display name.
    mm::ModuleFactory::registerType<mm::BlockModifier>("BlockModifier", "light/modifiers.md#block");
    mm::ModuleFactory::registerType<mm::CheckerboardModifier>("CheckerboardModifier", "light/modifiers.md#checkerboard");
    mm::ModuleFactory::registerType<mm::MoonLiveModifier>("MoonLiveModifier",
                                                          "light/modifiers.md#moonlive");
    mm::ModuleFactory::registerType<mm::CircleModifier>("CircleModifier", "light/modifiers.md#circle");
    mm::ModuleFactory::registerType<mm::MirrorModifier>("MirrorModifier", "light/modifiers.md#mirror");
    mm::ModuleFactory::registerType<mm::MultiplyModifier>("MultiplyModifier", "light/modifiers.md#multiply");
    mm::ModuleFactory::registerType<mm::PinwheelModifier>("PinwheelModifier", "light/modifiers.md#pinwheel");
    mm::ModuleFactory::registerType<mm::RandomMapModifier>("RandomMapModifier", "light/modifiers.md#randommap");
    mm::ModuleFactory::registerType<mm::RegionModifier>("RegionModifier", "light/modifiers.md#region");
    mm::ModuleFactory::registerType<mm::RippleXZModifier>("RippleXZModifier", "light/modifiers.md#ripplexz");
    mm::ModuleFactory::registerType<mm::RotateModifier>("RotateModifier", "light/modifiers.md#rotate");
    mm::ModuleFactory::registerType<mm::TransposeModifier>("TransposeModifier", "light/modifiers.md#transpose");
    mm::ModuleFactory::registerType<mm::HueDriver>("HueDriver", "light/drivers.md#hue");
    mm::ModuleFactory::registerType<mm::NetworkSendDriver>("NetworkSendDriver", "light/drivers.md#networksend");
    mm::ModuleFactory::registerType<mm::PreviewDriver>("PreviewDriver", "light/drivers.md#preview");
    // NDI is gated by CAPABILITY, not by firmware, and `hasNdi` decides whether the picker offers it: @xref{why-led-drivers-are-gated-by-the-preprocessor}.
    if constexpr (mm::platform::hasNdi)
        mm::ModuleFactory::registerType<mm::NdiDriver>("NdiDriver", "light/drivers.md#ndi");
    if constexpr (mm::platform::hasHls)
        mm::ModuleFactory::registerType<mm::HlsDriver>("HlsDriver", "light/drivers.md#hls");
    if constexpr (mm::platform::hasRtsp)
        mm::ModuleFactory::registerType<mm::RtspDriver>("RtspDriver", "light/drivers.md#rtsp");
    // Same firmware gate as the include above.
#if defined(MM_PANEL_CARDS) || MM_LINKS_ALL_LED_DRIVERS
    mm::ModuleFactory::registerType<mm::PanelCardDriver>("PanelCardDriver", "light/drivers.md#panelcard");
#endif
    // Same silicon gate as the include above.
#if defined(CONFIG_SOC_LCDCAM_I80_LCD_SUPPORTED) || defined(CONFIG_SOC_PARLIO_SUPPORTED) || \
    MM_LINKS_ALL_LED_DRIVERS
    mm::ModuleFactory::registerType<mm::Hub75Driver>("Hub75Driver", "light/drivers.md#hub75");
#endif
    // Register only the LED drivers this chip's silicon can run, see the gated includes above: @xref{why-led-drivers-are-gated-by-the-preprocessor}.
#if defined(CONFIG_SOC_RMT_SUPPORTED) || MM_LINKS_ALL_LED_DRIVERS
    mm::ModuleFactory::registerType<mm::RmtLedDriver>("RmtLedDriver", "light/drivers.md#rmtled");
#endif
    // One driver for the parallel output whatever the DMA peripheral: each backend self-registers when its header is included, so the control offers exactly the ones this chip links.
#if defined(CONFIG_SOC_LCD_I80_SUPPORTED) || defined(CONFIG_SOC_LCDCAM_I80_LCD_SUPPORTED) || defined(CONFIG_SOC_PARLIO_SUPPORTED) || MM_LINKS_ALL_LED_DRIVERS
    mm::ModuleFactory::registerType<mm::ParallelLedDriver>("ParallelLedDriver", "light/drivers.md#parallelled");
#endif
    mm::ModuleFactory::registerType<mm::HttpServerModule>("HttpServerModule", "core/system.md");
    mm::ModuleFactory::registerType<mm::SystemModule>("SystemModule", "core/system.md#system");
    mm::ModuleFactory::registerType<mm::ControlModule>("ControlModule", "core/system.md#control");
    mm::ModuleFactory::registerType<mm::Services>("Services", "core/services.md#services");
    mm::ModuleFactory::registerType<mm::AudioService>("AudioService", "core/services.md#audio");
    mm::ModuleFactory::registerType<mm::VideoService>("VideoService", "core/services.md#video");
    mm::ModuleFactory::registerType<mm::OscModule>("OscModule", "core/services.md#osc");
    mm::ModuleFactory::registerType<mm::I2cScanModule>("I2cScanModule", "core/system.md#i2c-scan");
    mm::ModuleFactory::registerType<mm::TasksModule>("TasksModule", "core/system.md#tasks");
    mm::ModuleFactory::registerType<mm::PinsModule>("PinsModule", "core/system.md#pins");
    mm::ModuleFactory::registerType<mm::ButtonService>("ButtonService", "core/services.md#button");
    mm::ModuleFactory::registerType<mm::AnalogService>("AnalogService", "core/services.md#analog");
    mm::ModuleFactory::registerType<mm::InfraredService>("InfraredService", "core/services.md#infrared");
    mm::ModuleFactory::registerType<mm::MoonLiveService>("MoonLiveService", "core/services.md#moonliveservice");
    mm::ModuleFactory::registerType<mm::FileManagerModule>("FileManagerModule", "core/system.md#file-manager");
    mm::ModuleFactory::registerType<mm::FirmwareUpdateModule>("FirmwareUpdateModule", "core/system.md#firmware-update");
    mm::ModuleFactory::registerType<mm::MoonCloudModule>("MoonCloudModule", "core/system.md#mooncloud");
    mm::ModuleFactory::registerType<mm::MoonStatsModule>("MoonStatsModule", "core/system.md#stats");
    mm::ModuleFactory::registerType<mm::MoonTalkModule>("MoonTalkModule", "core/system.md#talk");
    mm::ModuleFactory::registerType<mm::ImprovProvisioningModule>("ImprovProvisioningModule", "core/system.md#improv-provisioning");
    mm::ModuleFactory::registerType<mm::MqttModule>("MqttModule", "core/system.md#mqtt");
    mm::ModuleFactory::registerType<mm::DevicesModule>("DevicesModule", "core/system.md#devices");
    mm::ModuleFactory::registerType<mm::NetworkModule>("NetworkModule", "core/system.md#network");
    mm::ModuleFactory::registerType<mm::FilesystemModule>("FilesystemModule", "core/system.md#filesystem");
}
