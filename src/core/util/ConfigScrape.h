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
/// The file's shape, the module's state document, is a contract between two images built from one tree, so the scraper lives here once.
/// MoonBase includes it, and `unit_MoonBaseContract` runs it against the file the application writes.
/// `findKey` matches a whole key anywhere, the first one in the file; a child's key is read among that child's own members, the child found by its `type`.
/// Until the release after 2026-10-08 it also reads the flat format older builds wrote (`"0.key":` beside `"0.type":`), since MoonBase can boot before the app has converted the file.
///
/// Depends on nothing but the standard C headers and `hex.h`, which is what lets the small image include it.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "core/util/hex.h"
#include "core/util/Ipv4.h"   // the saved addresses, and the rule a static setting is checked by

namespace mm::configscrape {

/// The value after the first `"key":` in the file, or the flat format's `"0.key":`, or null when the key is absent.
inline const char* findKey(const char* json, const char* key) {
    if (!json || !key || !key[0]) return nullptr;
    const size_t k = std::strlen(key);
    for (const char* p = std::strstr(json, key); p; p = std::strstr(p + 1, key)) {
        if (p != json && (p[-1] == '"' || p[-1] == '.') && p[k] == '"' && p[k + 1] == ':') return p + k + 2;
    }
    return nullptr;
}

/// The closing quote of the string opening at `p`, past its escapes, or null when it never closes.
inline const char* stringEnd(const char* p) {
    for (p++; *p; p++) {
        if (*p == '\\' && p[1]) p++;
        else if (*p == '"') return p;
    }
    return nullptr;
}

/// Whether the string from `p` to its closing quote `e` is `key` used as a member name.
inline bool isKey(const char* p, const char* e, const char* key, size_t kl) {
    return static_cast<size_t>(e - p - 1) == kl && std::strncmp(p + 1, key, kl) == 0 && e[1] == ':';
}

/// Track the nesting `c` opens or closes; false when it closes the object the walk started in.
inline bool stillInside(char c, int& depth) {
    if (c == '{' || c == '[') depth++;
    else if ((c == '}' || c == ']') && depth-- == 0) return false;
    return true;
}

/// The value of `key` among the members of the object `from` sits in, at that object's own depth so a nested child's is not taken, or null.
inline const char* memberValue(const char* from, const char* key) {
    const size_t kl = std::strlen(key);
    int depth = 0;
    for (const char* p = from; p && *p; p++) {
        if (*p != '"') {
            if (!stillInside(*p, depth)) return nullptr;   // the object ended without it
            continue;
        }
        const char* e = stringEnd(p);
        if (!e) return nullptr;
        if (depth == 0 && isKey(p, e, key, kl)) return e + 2;
        p = e;
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

/// The flat format's `"N.key":` beside a `"N.type":` found at `typeKey`, into `want`; false when it does not fit or the type has no index prefix.
inline bool flatChildKey(const char* json, const char* typeKey, const char* key, char* want, size_t wantLen) {
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

/// The flat format's value of `key` beside the `"N.type":` found at `typeKey`, or null.
inline const char* flatChildValue(const char* json, const char* typeKey, const char* key) {
    char want[64];
    if (!flatChildKey(json, typeKey, key, want, sizeof(want))) return nullptr;
    const char* at = std::strstr(json, want);
    return at ? at + std::strlen(want) : nullptr;
}

/// The value of a key of the child whose type is `childType`, read among that child's own members, or null when either is absent.
inline const char* findChildValue(const char* json, const char* childType, const char* key) {
    if (!json || !childType || !key) return nullptr;
    const size_t tl = std::strlen(childType);
    for (const char* p = std::strstr(json, "type\":\""); p; p = std::strstr(p + 1, "type\":\"")) {
        const char* name = p + 7;
        if (std::strncmp(name, childType, tl) != 0 || name[tl] != '"') continue;
        // A quoted `"type"` is a document member; a list row's `type` names no child, holds no such key, and the search goes on.
        const char* v = (p > json && p[-1] == '"') ? memberValue(name + tl + 1, key) : flatChildValue(json, p, key);
        if (v) return v;
    }
    return nullptr;
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
