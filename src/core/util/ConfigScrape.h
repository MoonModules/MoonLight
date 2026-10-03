#pragma once

/// @defgroup ConfigScrape Reading a few keys out of a saved config without a parser
/// @{
/// How MoonBase, the recovery image, reads the WiFi credentials and the Ethernet wiring out of the application's `NetworkModule.json`.
///
/// @moreinfo
///
/// ## Why a scraper, and why it is shared
///
/// MoonBase has no room for a JSON parser and reads only a bounded prefix of the file, so it scrapes a handful of keys it knows the application writes.
/// The file's shape is a contract between two images built from one tree, so the scraper lives here once.
/// MoonBase includes it, and `unit_MoonBaseContract` runs it against the file the application writes.
/// A key matches at the top level (`"key":`), under a child module (`"0.key":`), and inside a list row.
///
/// Depends on nothing but `<cstring>` and `<cstdlib>`, which is what lets the small image include it.

#include <cstddef>
#include <cstdlib>
#include <cstring>

namespace mm::configscrape {

/// The value after `"key":`, top-level or child-prefixed, or null when the key is absent.
inline const char* findKey(const char* json, const char* key) {
    if (!json || !key || !key[0]) return nullptr;
    const size_t k = std::strlen(key);
    for (const char* p = std::strstr(json, key); p; p = std::strstr(p + 1, key)) {
        if (p == json) continue;
        const char before = p[-1];
        if ((before == '"' || before == '.') && p[k] == '"' && p[k + 1] == ':') return p + k + 2;
    }
    return nullptr;
}

/// Decode the JSON string starting at `value` (its opening quote) into `out`, false when empty or not decodable here.
inline bool readString(const char* value, char* out, size_t outLen) {
    if (!value || *value != '"' || outLen == 0) return false;
    const char* p = value + 1;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < outLen) {
        char c = *p++;
        if (c == '\\' && *p) {
            // All the app's escapes but \u decode here: a credential holding a raw control byte fails the join and lands on the access point, visible and recoverable.
            const char e = *p++;
            switch (e) {
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case 'u': out[0] = '\0'; return false;
                default:  c = e;      // \" \\ \/ decode to the char itself
            }
        }
        out[i++] = c;
    }
    out[i] = '\0';
    return i > 0;
}

/// A string key's value into `out`, false when absent or empty.
inline bool findString(const char* json, const char* key, char* out, size_t outLen) {
    return readString(findKey(json, key), out, outLen);
}

/// A numeric key's value into `out`; absent leaves it untouched, so callers pre-load their defaults.
inline void findInt(const char* json, const char* key, int* out) {
    const char* v = findKey(json, key);
    if (v && (*v == '-' || (*v >= '0' && *v <= '9'))) *out = static_cast<int>(std::strtol(v, nullptr, 10));
}

/// A boolean key's value into `out`; absent leaves it untouched.
inline void findBool(const char* json, const char* key, bool* out) {
    const char* v = findKey(json, key);
    if (!v) return;
    if (std::strncmp(v, "true", 4) == 0)  *out = true;
    if (std::strncmp(v, "false", 5) == 0) *out = false;
}

/// The first known network's name and password: the first `ssid`, and the `password` that follows it in the same row.
inline bool findFirstNetwork(const char* json, char* ssid, size_t ssidLen, char* password, size_t passwordLen) {
    if (passwordLen) password[0] = '\0';
    const char* row = findKey(json, "ssid");
    if (!readString(row, ssid, ssidLen)) return false;
    readString(findKey(row, "password"), password, passwordLen);
    return true;
}

}  // namespace mm::configscrape

/// @}
