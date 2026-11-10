#pragma once

/// The WiFi networks a WLED device saved, read out of its config files, which the migration image carries to MoonLight.
///
/// @technical
///
/// WLED keeps its networks in `nw.ins`, the names in `cfg.json` and the passwords in `wsec.json`, in one order: one in WLED 0.15, up to three in WLED 16.
///
/// A device the map sees is on one of them, so the migration carries every one, and MoonBase joins the first that answers.
///
/// The migration image has no room for a JSON parser, so this scrapes the one array it needs, skipping brackets inside strings.
///
/// It lives here once so the migration image (`moonbase/migrate/`) and a desktop test run the same code, on the standard C headers alone.

#include <cstddef>
#include <cstdio>
#include <cstring>

namespace mm::wlednet {

/// One saved network: its name and its password, empty for an open network.
struct Network {
    char ssid[33];
    char psk[65];
};

/// The string value after `"key":"` at or after `text`, into `out`, unescaped; false when absent or empty.
inline bool jsonString(const char* text, const char* key, char* out, size_t outLen) {
    char pattern[24];
    std::snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char* p = std::strstr(text, pattern);
    if (!p || outLen == 0) return false;
    p += std::strlen(pattern);
    size_t i = 0;
    for (; *p && *p != '"' && i + 1 < outLen; p++) {
        if (*p == '\\' && p[1]) p++;
        out[i++] = *p;
    }
    out[i] = 0;
    return i > 0;
}

/// The quote that closes the string `open` starts at, past its escapes; the text's end when it never closes.
inline const char* stringEnd(const char* open) {
    const char* q = open + 1;
    for (; *q && *q != '"'; q++)
        if (*q == '\\' && q[1]) q++;
    return q;
}

/// The bracket that closes the one `open` points at, skipping brackets inside strings; null when it never closes.
inline const char* closingBracket(const char* open) {
    int depth = 0;
    for (const char* q = open; *q; q++) {
        if (*q == '"') {
            q = stringEnd(q);
            if (!*q) return nullptr;   // a string that never closes
            continue;
        }
        if (*q == '[' || *q == '{') depth++;
        if ((*q == ']' || *q == '}') && --depth == 0) return q;
    }
    return nullptr;
}

/// The `"ins":[...]` array in `text`: its `[`, with `end` at its matching `]`; null when absent or unclosed.
inline const char* insArray(const char* text, const char*& end) {
    const char* p = std::strstr(text, "\"ins\":[");
    if (!p) return nullptr;
    p += 6;   // at the '['
    end = closingBracket(p);
    return end ? p : nullptr;
}

/// Each `key` value in the `ins` array of `text`, in order, into the networks' `ssid` or `psk`; how many were found. `text` is bounded in place while searched, then restored.
inline int insValues(char* text, const char* key, Network* out, int max, bool psk) {
    const char* end = nullptr;
    const char* p = insArray(text, end);
    if (!p) return 0;
    char* stop = const_cast<char*>(end);
    const char saved = *stop;
    *stop = 0;   // the search stays inside the array
    char pattern[24];
    std::snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    int n = 0;
    for (const char* at = std::strstr(p, pattern); at && n < max; at = std::strstr(at + 1, pattern), n++) {
        if (psk) jsonString(at, key, out[n].psk, sizeof(out[n].psk));
        else jsonString(at, key, out[n].ssid, sizeof(out[n].ssid));
    }
    *stop = saved;
    return n;
}

/// The networks in WLED's `cfg` text and, when given, its `wsec` text, with every unnamed slot dropped; how many there are. Both texts are restored as they were.
inline int readNetworks(char* cfg, char* wsec, Network* out, int max) {
    std::memset(out, 0, sizeof(Network) * static_cast<size_t>(max));
    const int n = cfg ? insValues(cfg, "ssid", out, max, false) : 0;
    if (n && wsec) insValues(wsec, "psk", out, n, true);
    int named = 0;
    for (int i = 0; i < n; i++)
        if (out[i].ssid[0]) out[named++] = out[i];
    return named;
}

}  // namespace mm::wlednet
