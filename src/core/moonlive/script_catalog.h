#pragma once

/// The names of every script a device knows about.
///
/// Generated from `moonlive/` by `catalog_scripts.cmake`: do not edit, rebuild to update.
///
/// @technical
///
/// A device carries names only and the UI fetches a script's text on first pick, so flash scales with the script count and the filesystem holds only what is used.
///
/// There is one array per role: a script's role follows from its extension and its folder from its role, so neither is stored per entry. A subfolder is an index into one shared table.
#include <cstddef>

namespace mm::moonlive {

/// The subfolders a script may sit in under its role's folder upstream, "" first for the folder itself.
constexpr const char* kCatalogSubfolders[] = {
    "",
    "audio",
    "basics",
    "hpwit",
    "moving-heads",
    "particles",
    "shaders",
    "stadbeest",
    "waves",
};

/// Every factory effect by file name, from `moonlive/effects/` upstream.
constexpr const char* kEffectCatalog[] = {
    "glow.mle",
    "pulse.mle",
    "spectrum.mle",
    "breathe.mle",
    "chase.mle",
    "crosshair.mle",
    "dot.mle",
    "gradient.mle",
    "lines.mle",
    "random-pixel.mle",
    "sparkle.mle",
    "edge-comets.mle",
    "fire.mle",
    "mandala.mle",
    "particle-fountain.mle",
    "plasma-ring.mle",
    "plasma-wander.mle",
    "ripple-rings.mle",
    "spinning-squares.mle",
    "twinkle-stars.mle",
    "voronoi.mle",
    "mh-aim.mle",
    "mh-ambient.mle",
    "mh-sweep.mle",
    "mh-troy.mle",
    "mh-wowi.mle",
    "ballpit.mle",
    "balls.mle",
    "comet-trail.mle",
    "ember.mle",
    "fountain.mle",
    "rain.mle",
    "fluid.mle",
    "fractal.mle",
    "metal.mle",
    "nebula.mle",
    "trails.mle",
    "stadbeest-eye-orb.mle",
    "stadbeest-frog-eye.mle",
    "stadbeest-mouth.mle",
    "stadbeest-orb.mle",
    "stadbeest-sauron.mle",
    "stadbeest-thrusters.mle",
    "stadbeest-walk.mle",
    "aurora.mle",
    "noise.mle",
    "octopus.mle",
    "plasma.mle",
    "ripples.mle",
};
constexpr size_t kEffectCatalogCount = 49;
/// What each effect above declares about itself, in the same order, 0 meaning it says nothing and the device decides.
constexpr unsigned char kEffectCatalogDim[] = {
    1,
    1,
    2,
    1,
    3,
    2,
    2,
    3,
    2,
    1,
    3,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    1,
    1,
    1,
    1,
    1,
    2,
    2,
    2,
    1,
    2,
    2,
    3,
    2,
    2,
    3,
    3,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    3,
    2,
    2,
    2,
    2,
};
/// The emoji each declares, "" when it declares none.
constexpr const char* kEffectCatalogTags[] = {
    "💫🎶",
    "💫🎵",
    "💫🎶",
    "💫",
    "💫",
    "💫",
    "💫",
    "💫",
    "💫",
    "💫",
    "💫",
    "💫✨",
    "💫",
    "💫",
    "💫✨",
    "💫",
    "💫",
    "💫",
    "💫",
    "💫",
    "💫",
    "💫🎯",
    "🗼🎶🎯",
    "💫🎯",
    "🗼🎶🎯",
    "🗼🎯",
    "💫✨",
    "💫",
    "💫✨",
    "💫",
    "💫✨",
    "💫✨",
    "💫🖌️",
    "💫🖌️",
    "💫🖌️",
    "💫🖌️",
    "💫🖌️",
    "💫",
    "💫🎵🎶",
    "💫🎵",
    "💫",
    "💫🎵🎶",
    "💫🎵",
    "💫🎵",
    "💫",
    "💫",
    "💫🖌️",
    "💫",
    "💫",
};
/// Where each effect above sits under `moonlive/effects/`, as an index into kCatalogSubfolders.
constexpr unsigned char kEffectCatalogSub[] = {
    1,
    1,
    1,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    3,
    3,
    3,
    3,
    3,
    3,
    3,
    3,
    3,
    3,
    4,
    4,
    4,
    4,
    4,
    5,
    5,
    5,
    5,
    5,
    5,
    6,
    6,
    6,
    6,
    6,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    8,
    8,
    8,
    8,
    8,
};
constexpr const char* kEffectFolder = "effects";   ///< its directory upstream

/// Every factory layout by file name, from `moonlive/layouts/` upstream.
constexpr const char* kLayoutCatalog[] = {
    "backlight.mll",
    "diagonal.mll",
    "grid.mll",
    "lattice.mll",
    "lightcrafter16.mll",
    "reversed-row.mll",
    "ring.mll",
    "rose.mll",
    "se16.mll",
    "sixteen-rings.mll",
    "two-rows.mll",
};
constexpr size_t kLayoutCatalogCount = 11;
/// What each layout above declares about itself, in the same order, 0 meaning it says nothing and the device decides.
constexpr unsigned char kLayoutCatalogDim[] = {
    2,
    2,
    2,
    3,
    2,
    1,
    2,
    2,
    2,
    2,
    2,
};
/// The emoji each declares, "" when it declares none.
constexpr const char* kLayoutCatalogTags[] = {
    "💫",
    "💫",
    "💫",
    "💫",
    "🚥",
    "💫",
    "💫",
    "💫",
    "🚥",
    "🚥",
    "💫",
};
/// Where each layout above sits under `moonlive/layouts/`, as an index into kCatalogSubfolders.
constexpr unsigned char kLayoutCatalogSub[] = {
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
};
constexpr const char* kLayoutFolder = "layouts";   ///< its directory upstream

/// Every factory modifier by file name, from `moonlive/modifiers/` upstream.
constexpr const char* kModifierCatalog[] = {
    "mirror.mlm",
    "shift.mlm",
    "transpose.mlm",
};
constexpr size_t kModifierCatalogCount = 3;
/// What each modifier above declares about itself, in the same order, 0 meaning it says nothing and the device decides.
constexpr unsigned char kModifierCatalogDim[] = {
    2,
    2,
    2,
};
/// The emoji each declares, "" when it declares none.
constexpr const char* kModifierCatalogTags[] = {
    "💫",
    "💫",
    "💫",
};
/// Where each modifier above sits under `moonlive/modifiers/`, as an index into kCatalogSubfolders.
constexpr unsigned char kModifierCatalogSub[] = {
    0,
    0,
    0,
};
constexpr const char* kModifierFolder = "modifiers";   ///< its directory upstream

/// Every factory service by file name, from `moonlive/services/` upstream.
constexpr const char* kServiceCatalog[] = {
    "button.mls",
    "chase.mls",
    "power.mls",
    "stadbeest-autopilot.mls",
    "sweep.mls",
};
constexpr size_t kServiceCatalogCount = 5;
/// What each service above declares about itself, in the same order, 0 meaning it says nothing and the device decides.
constexpr unsigned char kServiceCatalogDim[] = {
    0,
    0,
    0,
    0,
    0,
};
/// The emoji each declares, "" when it declares none.
constexpr const char* kServiceCatalogTags[] = {
    "",
    "",
    "",
    "",
    "",
};
/// Where each service above sits under `moonlive/services/`, as an index into kCatalogSubfolders.
constexpr unsigned char kServiceCatalogSub[] = {
    0,
    0,
    0,
    0,
    0,
};
constexpr const char* kServiceFolder = "services";   ///< its directory upstream

/// Every factory palette by file name, from `moonlive/palettes/` upstream.
constexpr const char* kPaletteCatalog[] = {
    "beat-flash.mlp",
    "drift.mlp",
    "fire.mlp",
    "spectrum.mlp",
    "stadbeest-fire.mlp",
    "temperature.mlp",
};
constexpr size_t kPaletteCatalogCount = 6;
/// What each palette above declares about itself, in the same order, 0 meaning it says nothing and the device decides.
constexpr unsigned char kPaletteCatalogDim[] = {
    0,
    0,
    0,
    0,
    0,
    0,
};
/// The emoji each declares, "" when it declares none.
constexpr const char* kPaletteCatalogTags[] = {
    "🎨🎶",
    "🎨",
    "🎨",
    "🎨🎶",
    "🎨🎶",
    "🎨",
};
/// Where each palette above sits under `moonlive/palettes/`, as an index into kCatalogSubfolders.
constexpr unsigned char kPaletteCatalogSub[] = {
    0,
    0,
    0,
    0,
    0,
    0,
};
constexpr const char* kPaletteFolder = "palettes";   ///< its directory upstream

constexpr size_t kCatalogCount = 74;   ///< every factory script, all roles

} // namespace mm::moonlive
