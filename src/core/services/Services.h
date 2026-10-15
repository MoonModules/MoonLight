#pragma once

#include "core/module/MoonModule.h"

namespace mm {

/// The container for the user-added Service modules, the capability bridges a device provides or consumes.
/// @card Services.png
/// @video 09-services.webm Every service in turn: audio, button, analog, infrared, and a scripted one.
///
/// A service is added and removed at runtime, the core twin of the light domain's Effects and Drivers. Fixed device infrastructure, such as identity and network, lives under System.
class Services : public MoonModule {
public:
    /// Accept any service as a child, which is what makes this the mutable half; with no controls or state, the generic add, replace, delete and persistence machinery applies unchanged.
    const char* acceptsChildRoles() const override { return "service"; }
};

} // namespace mm
