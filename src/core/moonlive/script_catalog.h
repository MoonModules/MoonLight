#pragma once

/// The names of every script a device knows about.
///
/// Generated from `moonlive/` by `catalog_scripts.cmake`: do not edit, rebuild to update.
///
/// @technical
///
/// A device carries names only and the UI fetches a script's text on first pick, so flash scales with the script count and the filesystem holds only what is used.
///
/// There is one array per role, since a script's role follows from its extension. Its folder under `moonlive/` is an index into one shared table, so a project folder can hold every role.
#include <cstddef>

namespace mm::moonlive {

/// The folders a script sits in under `moonlive/` upstream.
constexpr const char* kCatalogFolders[] = {
    "effects/audio",
    "effects/basics",
    "effects/moving-heads",
    "effects/particles",
    "effects/shaders",
    "effects/waves",
    "layouts",
    "modifiers",
    "palettes",
    "projects/hpwit",
    "projects/stadbeest",
    "services",
};

/// Every factory effect by file name, from `moonlive/` upstream.
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
    "aurora.mle",
    "noise.mle",
    "octopus.mle",
    "plasma.mle",
    "ripples.mle",
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
    "stadbeest-eye-orb.mle",
    "stadbeest-frog-eye.mle",
    "stadbeest-mouth.mle",
    "stadbeest-orb.mle",
    "stadbeest-sauron.mle",
    "stadbeest-thrusters.mle",
    "stadbeest-walk.mle",
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
    "💫",
    "💫🖌️",
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
    "💫",
    "💫🎵🎶",
    "💫🎵",
    "💫",
    "💫🎵🎶",
    "💫🎵",
    "💫🎵",
};
/// Where each effect above sits under `moonlive/`, as an index into kCatalogFolders.
constexpr unsigned char kEffectCatalogFolder[] = {
    0,
    0,
    0,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
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
    9,
    9,
    9,
    9,
    9,
    9,
    9,
    9,
    9,
    9,
    10,
    10,
    10,
    10,
    10,
    10,
    10,
};

/// Every factory layout by file name, from `moonlive/` upstream.
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
/// Where each layout above sits under `moonlive/`, as an index into kCatalogFolders.
constexpr unsigned char kLayoutCatalogFolder[] = {
    6,
    6,
    6,
    6,
    6,
    6,
    6,
    6,
    6,
    6,
    6,
};

/// Every factory modifier by file name, from `moonlive/` upstream.
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
/// Where each modifier above sits under `moonlive/`, as an index into kCatalogFolders.
constexpr unsigned char kModifierCatalogFolder[] = {
    7,
    7,
    7,
};

/// Every factory service by file name, from `moonlive/` upstream.
constexpr const char* kServiceCatalog[] = {
    "hpwit-show.mls",
    "stadbeest-autopilot.mls",
    "button.mls",
    "chase.mls",
    "power.mls",
    "sweep.mls",
};
constexpr size_t kServiceCatalogCount = 6;
/// What each service above declares about itself, in the same order, 0 meaning it says nothing and the device decides.
constexpr unsigned char kServiceCatalogDim[] = {
    0,
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
    "",
};
/// Where each service above sits under `moonlive/`, as an index into kCatalogFolders.
constexpr unsigned char kServiceCatalogFolder[] = {
    9,
    10,
    11,
    11,
    11,
    11,
};

/// Every factory palette by file name, from `moonlive/` upstream.
constexpr const char* kPaletteCatalog[] = {
    "beat-flash.mlp",
    "drift.mlp",
    "fire.mlp",
    "spectrum.mlp",
    "temperature.mlp",
    "stadbeest-fire.mlp",
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
    "🎨",
    "🎨🎶",
};
/// Where each palette above sits under `moonlive/`, as an index into kCatalogFolders.
constexpr unsigned char kPaletteCatalogFolder[] = {
    8,
    8,
    8,
    8,
    8,
    10,
};

constexpr size_t kCatalogCount = 75;   ///< every factory script, all roles

} // namespace mm::moonlive
