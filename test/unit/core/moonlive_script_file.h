#pragma once

#include "doctest.h"
#include "core/moonlive/MoonLiveScriptFile.h"
#include "platform/platform.h"

#include <cstdio>
#include <cstring>

/// Write a script where the UI saves one, through the platform fs: the desktop roots its filesystem where it chooses, so a host path is never where the loader looks.
inline void writeScript(const char* name, const char* body) {
    mm::platform::fsMkdir(mm::moonlive::kScriptDir);
    char path[96];
    std::snprintf(path, sizeof(path), "%s/%s", mm::moonlive::kScriptDir, name);
    REQUIRE(mm::platform::fsWriteAtomic(path, body, std::strlen(body)));
}
