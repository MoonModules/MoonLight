#pragma once

/// @defgroup script_catalog The factory script catalog
/// @{
/// The names of every script a device knows about.
///
/// Generated from `moonlive/` by `catalog_scripts.cmake`: do not edit, rebuild to update.
///
/// @moreinfo
///
/// This is the catalog rather than the library: names only.
/// A device carries the list, and the UI fetches a script's text from GitHub the first time someone picks it.
/// Flash therefore scales with how many scripts exist rather than how large they are, and the filesystem holds only what is used.
///
/// There is one array per role, because the folder a script lives in is implied by its role and the role by its extension, so neither is stored per entry.
#include <cstddef>

namespace mm::moonlive {

/// Every factory effect by file name, from `moonlive/effects/` upstream.
constexpr const char* kEffectCatalog[] = {
    "aurora.mle",
    "ballpit.mle",
    "balls.mle",
    "breathe.mle",
    "chase.mle",
    "comet-trail.mle",
    "crosshair.mle",
    "dot.mle",
    "ember.mle",
    "fluid.mle",
    "fountain.mle",
    "fractal.mle",
    "gradient.mle",
    "lines.mle",
    "metal.mle",
    "mh-aim.mle",
    "mh-ambient.mle",
    "mh-sweep.mle",
    "mh-troy.mle",
    "mh-wowi.mle",
    "nebula.mle",
    "noise.mle",
    "octopus.mle",
    "plasma.mle",
    "pulse.mle",
    "rain.mle",
    "random-pixel.mle",
    "ripples.mle",
    "sparkle.mle",
    "spectrum.mle",
    "stadbeest-eyes.mle",
    "stadbeest-legs.mle",
    "trails.mle",
};
constexpr size_t kEffectCatalogCount = 33;
/// What each effect above declares about itself, in the same order, 0 meaning it says nothing and the device decides.
constexpr unsigned char kEffectCatalogDim[] = {
    3,
    2,
    2,
    1,
    3,
    2,
    2,
    2,
    1,
    3,
    2,
    2,
    3,
    2,
    2,
    1,
    1,
    1,
    1,
    1,
    3,
    2,
    2,
    2,
    1,
    2,
    1,
    2,
    3,
    2,
    2,
    2,
    3,
};
/// The emoji each declares, "" when it declares none.
constexpr const char* kEffectCatalogTags[] = {
    "💫",
    "💫✨",
    "💫",
    "💫",
    "💫",
    "💫✨",
    "💫",
    "💫",
    "💫",
    "💫🖌️",
    "💫✨",
    "💫🖌️",
    "💫",
    "💫",
    "💫🖌️",
    "💫🎯",
    "🗼🎶🎯",
    "💫🎯",
    "🗼🎶🎯",
    "🗼🎯",
    "💫🖌️",
    "💫",
    "💫🖌️",
    "💫",
    "💫🎵",
    "💫✨",
    "💫",
    "💫",
    "💫",
    "💫🎶",
    "💫🎶",
    "💫🎶",
    "💫🖌️",
};
constexpr const char* kEffectFolder = "effects";   ///< its directory upstream

/// Every factory layout by file name, from `moonlive/layouts/` upstream.
constexpr const char* kLayoutCatalog[] = {
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
constexpr size_t kLayoutCatalogCount = 10;
/// What each layout above declares about itself, in the same order, 0 meaning it says nothing and the device decides.
constexpr unsigned char kLayoutCatalogDim[] = {
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
    "🚥",
    "💫",
    "💫",
    "💫",
    "🚥",
    "🚥",
    "💫",
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
constexpr const char* kModifierFolder = "modifiers";   ///< its directory upstream

/// Every factory service by file name, from `moonlive/services/` upstream.
constexpr const char* kServiceCatalog[] = {
    "button.mls",
    "chase.mls",
    "power.mls",
    "sweep.mls",
};
constexpr size_t kServiceCatalogCount = 4;
/// What each service above declares about itself, in the same order, 0 meaning it says nothing and the device decides.
constexpr unsigned char kServiceCatalogDim[] = {
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
};
constexpr const char* kServiceFolder = "services";   ///< its directory upstream

/// Every factory palette by file name, from `moonlive/palettes/` upstream.
constexpr const char* kPaletteCatalog[] = {
    "beat-flash.mlp",
    "drift.mlp",
    "fire.mlp",
    "spectrum.mlp",
    "temperature.mlp",
};
constexpr size_t kPaletteCatalogCount = 5;
/// What each palette above declares about itself, in the same order, 0 meaning it says nothing and the device decides.
constexpr unsigned char kPaletteCatalogDim[] = {
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
};
constexpr const char* kPaletteFolder = "palettes";   ///< its directory upstream

constexpr size_t kCatalogCount = 55;   ///< every factory script, all roles

/// @}
} // namespace mm::moonlive
