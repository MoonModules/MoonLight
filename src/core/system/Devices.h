#pragma once

#include "core/module/MoonModule.h"

namespace mm {

/// The container for what reaches past this device to the others on the network: the Discovery list, and the groups that join it.
///
/// Fleet-scope rather than this-device, so neither System nor Network: a device's own interfaces live under Network, the devices it finds live here.
///
/// @technical
///
/// It holds no settings of its own, and Discovery is wired in by code, which every boot puts back.
class Devices : public MoonModule {};

} // namespace mm
