#pragma once

/// The networks a WLED, Tasmota, ESPHome or any other ESP-IDF device uses, its saved WiFi and WLED's Ethernet board, read out of its config, which the migration image carries to MoonLight.
///
/// @technical
///
/// WLED keeps its networks in `nw.ins`, the names in `cfg.json` and the passwords in `wsec.json`, in one order: one in WLED 0.15, up to three in WLED 16.
///
/// A device the map sees is on one of them, so the migration carries every one, and MoonBase joins the first that answers.
///
/// The migration image has no room for a JSON parser, so this scrapes the one array it needs, skipping brackets inside strings.
///
/// WLED names its Ethernet board by an index into its board table (`eth.type` in `cfg.json`), which maps to MoonLight's Ethernet settings here.
///
/// It lives here once so the migration image (`moonbase/migrate/`) and a desktop test run the same code, on the standard C headers alone.

#include "core/util/JsonEscape.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

/// The name WLED gives a network slot nobody filled (its `CLIENT_SSID`), which names no network.
inline constexpr const char* kWledPlaceholderSsid = "Your_Network";

/// The networks in WLED's `cfg` text and, when given, its `wsec` text, with every unnamed or placeholder slot dropped; how many there are. Both texts are restored as they were.
inline int readNetworks(char* cfg, char* wsec, Network* out, int max) {
    std::memset(out, 0, sizeof(Network) * static_cast<size_t>(max));
    const int n = cfg ? insValues(cfg, "ssid", out, max, false) : 0;
    if (n && wsec) insValues(wsec, "psk", out, n, true);
    int named = 0;
    for (int i = 0; i < n; i++)
        if (out[i].ssid[0] && std::strcmp(out[i].ssid, kWledPlaceholderSsid) != 0) out[named++] = out[i];
    return named;
}

/// Where Tasmota's settings blob keeps its text pool (`TSettings::text_pool`, offset 0x017), its `\0`-separated strings in `SettingsTextIndex` order.
inline constexpr size_t kTasmotaTextPool = 0x017;
/// The text pool's entries for the first network: `SET_STASSID1` and `SET_STAPWD1`.
inline constexpr int kTasmotaSsid = 4, kTasmotaPassword = 6;

/// The network a Tasmota saved, out of its settings blob (NVS `main`/`Settings` on an ESP32), into `out`; false when the blob is too short or names none.
inline bool readTasmotaNetwork(const uint8_t* blob, size_t len, Network& out) {
    std::memset(&out, 0, sizeof(out));
    if (!blob || len <= kTasmotaTextPool) return false;
    const char* at = reinterpret_cast<const char*>(blob) + kTasmotaTextPool;
    const char* end = reinterpret_cast<const char*>(blob) + len;
    for (int entry = 0; at < end && entry <= kTasmotaPassword; entry++) {
        const char* stop = static_cast<const char*>(std::memchr(at, 0, static_cast<size_t>(end - at)));
        if (!stop) return false;   // a pool cut off mid-string
        const size_t n = static_cast<size_t>(stop - at);
        if (entry == kTasmotaSsid) std::snprintf(out.ssid, sizeof(out.ssid), "%.*s", static_cast<int>(n), at);
        if (entry == kTasmotaPassword) std::snprintf(out.psk, sizeof(out.psk), "%.*s", static_cast<int>(n), at);
        at = stop + 1;
    }
    return out.ssid[0] != 0;
}

/// ESPHome's Improv network: NVS namespace `esphome`, the key of a build whose YAML names no WiFi, a `SavedWifiSettings` of a 33-byte SSID and a 65-byte password.
inline constexpr const char* kEsphomeNamespace = "esphome";
inline constexpr const char* kEsphomeWifiKey = "88491487";
inline constexpr size_t kEsphomeSsidLen = 33, kEsphomeWifiLen = 33 + 65;

/// The network an ESPHome device saved, out of its WiFi preference blob, into `out`; false when the blob is short or names none.
inline bool readEsphomeNetwork(const uint8_t* blob, size_t len, Network& out) {
    std::memset(&out, 0, sizeof(out));
    if (!blob || len < kEsphomeWifiLen) return false;
    std::snprintf(out.ssid, sizeof(out.ssid), "%.*s", static_cast<int>(kEsphomeSsidLen - 1), reinterpret_cast<const char*>(blob));
    std::snprintf(out.psk, sizeof(out.psk), "%.*s", static_cast<int>(kEsphomeWifiLen - kEsphomeSsidLen - 1), reinterpret_cast<const char*>(blob + kEsphomeSsidLen));
    return out.ssid[0] != 0;
}

/// ESP-IDF's own saved station network, which its WiFi library keeps unless the firmware turns that off: NVS namespace `nvs.net80211`, `sta.ssid` a 4-byte little-endian length and 32 bytes, `sta.pswd` 65 bytes ending in a NUL.
inline constexpr const char* kIdfWifiNamespace = "nvs.net80211";
inline constexpr const char* kIdfSsidKey = "sta.ssid";
inline constexpr const char* kIdfPasswordKey = "sta.pswd";
inline constexpr size_t kIdfSsidLen = 4 + 32;

/// The network ESP-IDF saved, out of its SSID and password blobs, into `out`; false when the SSID blob is short or names none. An open network has no password blob.
inline bool readIdfNetwork(const uint8_t* ssid, size_t ssidLen, const uint8_t* psk, size_t pskLen, Network& out) {
    std::memset(&out, 0, sizeof(out));
    if (!ssid || ssidLen < kIdfSsidLen) return false;
    const uint32_t n = ssid[0] | ssid[1] << 8 | ssid[2] << 16 | static_cast<uint32_t>(ssid[3]) << 24;
    if (n == 0 || n > kIdfSsidLen - 4) return false;
    std::snprintf(out.ssid, sizeof(out.ssid), "%.*s", static_cast<int>(n), reinterpret_cast<const char*>(ssid + 4));
    if (psk) std::snprintf(out.psk, sizeof(out.psk), "%.*s", static_cast<int>(pskLen < sizeof(out.psk) ? pskLen : sizeof(out.psk) - 1), reinterpret_cast<const char*>(psk));
    return out.ssid[0] != 0;
}

/// One Ethernet board's wiring, in MoonLight's Ethernet settings: the PHY type, its address, the reset, MDC and MDIO lines, and the RMII clock.
struct Ethernet {
    int  type;         ///< an EthPhyType: 1 a LAN8720
    int  phyAddr;      ///< the PHY's address on the MDIO bus
    int  rstGpio;      ///< WLED's power line, which a reset pulse leaves high
    int  mdcGpio;      ///< the MDIO clock line
    int  mdioGpio;     ///< the MDIO data line
    int  clockGpio;    ///< the RMII clock's pin
    bool clockExtIn;   ///< the clock fed in on GPIO 0, else sent out on clockGpio
};

/// WLED's board table (`network.cpp`) by its `eth.type`, the entries WLED and WLED-MM share, the Ethernet Kit's IP101 (7) out as MoonLight drives one on the P4 only.
inline constexpr Ethernet kWledEthernet[] = {
    {0, 0, -1, -1, -1, 0, true},     // 0 none
    {1, 1, 16, 23, 18, 0, true},     // 1 WT32-ETH01
    {1, 0, 12, 23, 18, 17, false},   // 2 ESP32-POE
    {1, 0, -1, 16, 17, 0, true},     // 3 WESP32
    {1, 0, 5, 23, 18, 17, false},    // 4 QuinLED-ESP32, the Olimex Gateway's wiring too
    {1, 0, 5, 23, 18, 17, false},    // 5 TwilightLord-ESP32
    {1, 1, -1, 23, 18, 17, false},   // 6 ESP32Deux
    {0, 0, -1, -1, -1, 0, true},     // 7 ESP32-Ethernet-Kit: an IP101
    {1, 0, -1, 23, 18, 17, false},   // 8 QuinLED-Dig-Octa, LilyGO T-ETH-POE
    {1, 1, 5, 23, 33, 17, false},    // 9 ABC! WLED V43
    {1, 1, 5, 23, 18, 17, false},    // 10 Serg74
};

/// WLED's highest TX power, 19.5 dBm in ESP-IDF's quarter-dBm steps (`WIFI_POWER_19_5dBm`).
inline constexpr long kWledMaxTxPower = 78;

/// WLED's TX power limit from its `cfg` text in whole dBm, as MoonLight's `txPowerSetting` takes it; 0, no limit, when WLED runs at its maximum or names none.
inline int readTxPower(const char* cfg) {
    const char* wifi = cfg ? std::strstr(cfg, "\"wifi\":{") : nullptr;
    const char* p = wifi ? std::strstr(wifi, "\"txpwr\":") : nullptr;
    if (!p) return 0;
    const long quarters = std::strtol(p + 8, nullptr, 10);
    return quarters > 0 && quarters < kWledMaxTxPower ? static_cast<int>(quarters / 4) : 0;
}

/// The Ethernet board WLED's `cfg` text names, mapped; false when it names none, or one MoonLight's migration does not map.
inline bool readEthernet(const char* cfg, Ethernet& out) {
    const char* eth = cfg ? std::strstr(cfg, "\"eth\":{") : nullptr;
    const char* type = eth ? std::strstr(eth, "\"type\":") : nullptr;
    if (!type) return false;
    const int index = static_cast<int>(std::strtol(type + 7, nullptr, 10));
    if (index <= 0 || index >= static_cast<int>(sizeof(kWledEthernet) / sizeof(kWledEthernet[0]))) return false;
    out = kWledEthernet[index];
    return out.type != 0;
}

/// `s` as a JSON string literal at `doc + n`, control characters escaped so a name holding a tab or a newline still makes a valid document; the new length, `cap` when it does not fit.
inline int appendQuoted(char* doc, size_t cap, int n, const char* s) {
    size_t at = static_cast<size_t>(n);
    if (at + 2 >= cap) return static_cast<int>(cap);
    doc[at++] = '"';
    for (char esc[7]; *s; s++) {
        const size_t k = jsonEscapeByte(static_cast<unsigned char>(*s), esc);
        if (at + k + 2 >= cap) return static_cast<int>(cap);
        std::memcpy(doc + at, esc, k);
        at += k;
    }
    doc[at++] = '"';
    doc[at] = 0;
    return static_cast<int>(at);
}

/// One known network, the `index`th, at `doc + n`; the new length, past `cap` when it does not fit.
inline int appendNetwork(char* doc, size_t cap, int n, const Network& net, int index) {
    n += std::snprintf(doc + n, cap - n, "%s{\"id\":%d,\"ssid\":", index ? "," : "", index + 1);
    if (static_cast<size_t>(n) < cap) n = appendQuoted(doc, cap, n, net.ssid);
    if (static_cast<size_t>(n) < cap) n += std::snprintf(doc + n, cap - n, ",\"password\":");
    if (static_cast<size_t>(n) < cap) n = appendQuoted(doc, cap, n, net.psk);
    if (static_cast<size_t>(n) < cap) n += std::snprintf(doc + n, cap - n, ",\"ipSettings\":0}");
    return n;
}

/// The Ethernet card for `eth` at `doc + n`, a member of Network; the new length.
inline int appendEthernet(char* doc, size_t cap, int n, const Ethernet& eth) {
    return n + std::snprintf(doc + n, cap - n, "\"Ethernet\":{\"type\":\"EthernetModule\",\"ethBoard\":\"Custom\",\"ethType\":%d,\"ethPhyAddr\":%d,"
        "\"ethRstGpio\":%d,\"ethMdcGpio\":%d,\"ethMdioGpio\":%d,\"ethClockGpio\":%d,\"ethClockExtIn\":%s},", eth.type, eth.phyAddr,
        eth.rstGpio, eth.mdcGpio, eth.mdioGpio, eth.clockGpio, eth.clockExtIn ? "true" : "false");
}

/// The WiFi card, its TX power limit when one is set and its known networks, at `doc + n`; the new length, past `cap` when it does not fit.
inline int appendNetworks(char* doc, size_t cap, int n, const Network* nets, int count, int txPower) {
    n += std::snprintf(doc + n, cap - n, "\"WiFi\":{\"type\":\"WiFiModule\",");
    if (txPower > 0 && static_cast<size_t>(n) < cap) n += std::snprintf(doc + n, cap - n, "\"txPowerSetting\":%d,", txPower);
    if (static_cast<size_t>(n) < cap) n += std::snprintf(doc + n, cap - n, "\"known\":[");
    for (int i = 0; i < count && static_cast<size_t>(n) < cap; i++) n = appendNetwork(doc, cap, n, nets[i], i);
    return n;
}

/// The networks as the state document MoonLight saves them in, which MoonBase also reads: the Ethernet card when WLED's board maps, then the WiFi card with WLED's TX power limit and its networks; its length, or 0 when it does not fit.
inline int networkDocument(char* doc, size_t cap, const Network* nets, int count, const Ethernet* eth, int txPower = 0) {
    int n = std::snprintf(doc, cap, "{\"Network\":{");
    if (eth) n = appendEthernet(doc, cap, n, *eth);
    if (static_cast<size_t>(n) < cap) n = appendNetworks(doc, cap, n, nets, count, txPower);
    if (static_cast<size_t>(n) < cap) n += std::snprintf(doc + n, cap - n, "]}}}");
    return static_cast<size_t>(n) < cap ? n : 0;
}

}  // namespace mm::wlednet
