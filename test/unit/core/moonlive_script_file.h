#pragma once

#include "doctest.h"
#include "core/moonlive/MoonLiveScriptFile.h"
#include "platform/platform.h"

#include <cstdio>
#include <cstring>

/// Write a script into the device's script directory, the same place the UI saves one.
///
/// Through the PLATFORM fs, not std::ofstream: the desktop platform roots its filesystem somewhere of its own choosing, so writing to a host-relative "/moonlive" put the file where the loader would never look and every script silently failed to compile.
inline void writeScript(const char* name, const char* body) {
    mm::platform::fsMkdir(mm::moonlive::kScriptDir);
    char path[96];
    std::snprintf(path, sizeof(path), "%s/%s", mm::moonlive::kScriptDir, name);
    REQUIRE(mm::platform::fsWriteAtomic(path, body, std::strlen(body)));
}
