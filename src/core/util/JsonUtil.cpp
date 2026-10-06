/// Out-of-line half of JsonUtil.h.
/// Only what must not be inlined lives here.
///
/// The rest of the header is deliberately header-only: small first-match readers whose callers benefit from inlining.
/// `parseInt` is the exception: every integer setting is read through it, and out of line it measured 1.3 KB less flash on the classic ESP32.
/// Every caller is off the hot path, so the call is free in practice.

#include "core/util/JsonUtil.h"
#include "core/util/parse.h"

#include <cstring>

namespace mm::json {

int parseInt(const char* json, const char* key) {
    if (!json || !key) return 0;
    char search[kSearchLen];
    if (!buildKeyPattern(search, key, ":")) return 0;       // key too long → treat as absent
    const char* start = std::strstr(json, search);
    if (!start) {
        if (!buildKeyPattern(search, key, ": ")) return 0;
        start = std::strstr(json, search);
    }
    if (!start) return 0;
    // A quoted number reads as the number, so "24" and "0x18" mean what they say rather than 0.
    const char* v = start + std::strlen(search);
    while (*v == ' ') v++;
    if (*v == '"') v++;
    return parseIntStr(v);
}

}  // namespace mm::json
