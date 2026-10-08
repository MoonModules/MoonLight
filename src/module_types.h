#pragma once

namespace mm {

class MoonModule;

/// Register every module type this build can construct, and wire the core quiesce-render hook to the light domain.
void registerModuleTypes();

/// Create a top-level module as the device boots it: named as main.cpp names it, with the children the device wires into it, each marked wired; null for an unregistered type.
MoonModule* createTopLevel(const char* typeName);

}  // namespace mm
