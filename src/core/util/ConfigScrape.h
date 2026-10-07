#pragma once

/// @defgroup ConfigScrape Reading a few keys out of a saved config without a parser
/// @{
/// How MoonBase, the recovery image, reads the WiFi credentials, the Ethernet wiring and the access point out of the application's saved config.
///
/// @moreinfo
///
/// ## Why a scraper, and why it is shared
///
/// MoonBase has no room for a JSON parser, so it scrapes a handful of keys it knows the application writes.
/// The file's shape is a contract between two images built from one tree, so the scraper lives here once.
/// MoonBase includes it, and `unit_MoonBaseContract` runs it against the file the application writes.
/// A key matches at the top level (`"key":`), under a child module (`"0.key":`), and inside a list row.
///
/// Depends on nothing but the standard C headers and `hex.h`, which is what lets the small image include it.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "core/util/hex.h"
#include "core/util/Ipv4.h"   // the saved addresses, and the rule a static setting is checked by

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

/// Decode the escape after a backslash at `p`, advancing it; false for a code point past ASCII or a malformed `\u`.
inline bool decodeEscape(const char*& p, char& c) {
    // The app's escapes, as JsonUtil reads them; `\u00XX` carries a control byte, and a code point past ASCII is refused, visible as a failed join.
    const char e = *p++;
    if (e == 'n') { c = '\n'; return true; }
    if (e == 'r') { c = '\r'; return true; }
    if (e == 't') { c = '\t'; return true; }
    if (e != 'u') { c = e; return true; }   // \" \\ \/ decode to the char itself
    unsigned v = 0;
    for (int k = 0; k < 4; k++, p++) {
        const int d = hexDigit(*p);
        if (d < 0) return false;
        v = v * 16 + static_cast<unsigned>(d);
    }
    c = static_cast<char>(v);
    return v < 0x80;
}

/// Decode the JSON string starting at `value` (its opening quote) into `out`, false when empty or not decodable here.
inline bool readString(const char* value, char* out, size_t outLen) {
    if (!value || *value != '"' || outLen == 0) return false;
    const char* p = value + 1;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < outLen) {
        char c = *p++;
        if (c == '\\' && *p && !decodeEscape(p, c)) { out[0] = '\0'; return false; }
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

/// The `"N.key":` beside a `"N.type":` found at `typeKey`, into `want`; false when it does not fit or the type is top-level.
inline bool childKey(const char* json, const char* typeKey, const char* key, char* want, size_t wantLen) {
    // Back from `type` to the key's opening quote: `"2.type"` gives the prefix `2.`, and a top-level `type` none.
    const char* q = typeKey;
    while (q > json && q[-1] != '"') q--;
    const size_t pl = static_cast<size_t>(typeKey - q), kl = std::strlen(key);
    if (pl == 0 || pl + kl + 4 > wantLen) return false;
    want[0] = '"';
    std::memcpy(want + 1, q, pl);
    std::memcpy(want + 1 + pl, key, kl);
    std::memcpy(want + 1 + pl + kl, "\":", 3);   // with the terminator
    return true;
}

/// The value of a key of the child whose type is `childType`, the `"N.key":` beside its `"N.type":`, or null when either is absent.
inline const char* findChildValue(const char* json, const char* childType, const char* key) {
    if (!json || !childType || !key) return nullptr;
    const size_t tl = std::strlen(childType);
    for (const char* p = std::strstr(json, "type\":\""); p; p = std::strstr(p + 1, "type\":\"")) {
        char want[64];
        if (std::strncmp(p + 7, childType, tl) != 0 || p[7 + tl] != '"' || !childKey(json, p, key, want, sizeof(want))) continue;
        const char* at = std::strstr(json, want);
        return at ? at + std::strlen(want) : nullptr;
    }
    return nullptr;
}

/// A string key of the child whose type is `childType`, false when either is absent or the value empty.
inline bool findChildString(const char* json, const char* childType, const char* key, char* out, size_t outLen) {
    return readString(findChildValue(json, childType, key), out, outLen);
}

/// The known network at `index` in the app's priority order: its `ssid`, and the `password` that follows it in the same row.
inline bool findNetwork(const char* json, uint8_t index, char* ssid, size_t ssidLen, char* password, size_t passwordLen) {
    if (passwordLen) password[0] = '\0';
    const char* row = findKey(json, "ssid");
    for (uint8_t k = 0; row && k < index; k++) row = findKey(row, "ssid");
    if (!readString(row, ssid, ssidLen)) return false;
    readString(findKey(row, "password"), password, passwordLen);
    return true;
}

/// One interface's addressing as the app saved it: DHCP or Static, and the four addresses Static pins.
struct SavedIp {
    int mode = 0;                       ///< 0 DHCP, 1 Static, as the app's `ipSettings` select
    uint8_t ip[4] = {};                 ///< the address
    uint8_t gateway[4] = {};            ///< the router
    uint8_t subnet[4] = {255, 255, 255, 0};   ///< the netmask
    uint8_t dns[4] = {};                ///< the name server

    /// Whether it pins a static address the app would also use, by the app's own rule.
    bool usable() const { return mode == 1 && ipv4::staticFault(ip, gateway, subnet, dns) == ipv4::Fault::None; }
};

/// Where one key's value is, for the reader below: `ctx` carries what the lookup needs.
using ValueAt = const char* (*)(const void* ctx, const char* key);

/// The five IP keys, each value found by `at`; a key absent or malformed keeps its default.
inline SavedIp readIp(ValueAt at, const void* ctx) {
    SavedIp s;
    if (const char* v = at(ctx, "ipSettings"); v && *v >= '0' && *v <= '9') s.mode = static_cast<int>(std::strtol(v, nullptr, 10));
    uint8_t* const dst[4] = {s.ip, s.gateway, s.subnet, s.dns};
    const char* const names[4] = {"ip", "gateway", "subnet", "dns"};
    for (int k = 0; k < 4; k++) {
        char text[16];
        if (readString(at(ctx, names[k]), text, sizeof(text))) parseDottedQuad(text, dst[k]);
    }
    return s;
}

/// The IP settings of the child whose type is `childType`, the Ethernet card's.
inline SavedIp findChildIp(const char* json, const char* childType) {
    struct Ctx { const char* json; const char* type; } ctx{json, childType};
    return readIp([](const void* c, const char* key) {
        const auto* x = static_cast<const Ctx*>(c);
        return findChildValue(x->json, x->type, key);
    }, &ctx);
}

/// The closing quote of the string opening at `p`, past its escapes, or null when it never closes.
inline const char* stringEnd(const char* p) {
    for (p++; *p; p++) {
        if (*p == '\\' && p[1]) p++;
        else if (*p == '"') return p;
    }
    return nullptr;
}

// String-aware, since a password may hold a brace; an unclosed object has no end.
/// The closing brace of the object `p` sits inside, or null when there is none.
inline const char* objectEnd(const char* p) {
    int depth = 0;
    for (; p && *p; p++) {
        if (*p == '"' && !(p = stringEnd(p))) return nullptr;
        if (*p == '{' || *p == '[') depth++;
        else if ((*p == '}' || *p == ']') && depth-- == 0) return p;
    }
    return nullptr;
}

/// The IP settings of the known network at `index`, read inside its own row.
inline SavedIp findNetworkIp(const char* json, uint8_t index) {
    const char* row = findKey(json, "ssid");
    for (uint8_t k = 0; row && k < index; k++) row = findKey(row, "ssid");
    if (!row) return {};
    // The row's own closing brace ends it, so a row saved without a key reads neither its neighbor's nor a later module's.
    const char* end = objectEnd(row);
    struct Ctx { const char* row; const char* end; } ctx{row, end};
    return readIp([](const void* c, const char* key) -> const char* {
        const auto* x = static_cast<const Ctx*>(c);
        const char* v = findKey(x->row, key);
        return (v && (!x->end || v < x->end)) ? v : nullptr;
    }, &ctx);
}

}  // namespace mm::configscrape

/// @}
