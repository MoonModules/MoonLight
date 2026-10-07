#pragma once

#include "doctest.h"
#include "core/moonlive/MoonLiveScriptFile.h"
#include "platform/platform.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

/// Put a script on the filesystem and return its name, removed when the process exits.
inline std::vector<std::string>& mmScriptRegistry() {
    static std::vector<std::string> paths;
    static const bool once = [] {
        std::atexit([] {
            for (const auto& p : mmScriptRegistry()) mm::platform::fsRemove(p.c_str());
        });
        return true;
    }();
    (void)once;
    return paths;
}

inline const char* mmWriteScript(const char* text) {
    static std::atomic<int> counter{0};
    thread_local char name[32];
    std::snprintf(name, sizeof(name), "t%d.mle", ++counter);

    char path[96];
    std::snprintf(path, sizeof(path), "%s/%s", mm::moonlive::kScriptDir, name);
    mm::platform::fsMkdir(mm::moonlive::kScriptDir);
    REQUIRE(mm::platform::fsWriteAtomic(path, text, std::strlen(text)));
    {
        // The registry is shared across threads even though each thread's name is its own.
        static std::mutex m;
        std::lock_guard<std::mutex> lock(m);
        mmScriptRegistry().emplace_back(path);
    }
    return name;
}
