#pragma once

/// Reading an HTTP/1.1 request head (RFC 9112), as pure functions the app's server and MoonBase share and the desktop tests.
///
/// @technical
///
/// The reads take a `recv(buf, cap)` callable returning the bytes read, 0 or less when the peer is gone, so a test feeds the bytes in whatever pieces it likes: TCP keeps no message boundaries, and a head or a body arrives split anywhere.

#include "core/util/Ipv4.h"   // isPrivateIPv4: whether an Origin's address is this network's

#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace mm::http {

/// The interim response a client that sent `Expect: 100-continue` waits for before its body (RFC 9110 § 10.1.1).
inline constexpr char kContinue[] = "HTTP/1.1 100 Continue\r\n\r\n";

/// The header line starting with `name`, colon included, case-insensitively (RFC 9112), or null; a line start only, up to the blank line, so no lookalike or body byte matches.
inline const char* findHeader(const char* head, const char* name) {
    const size_t n = std::strlen(name);
    const char* line = head;
    while (line && *line) {
        if (line[0] == '\r' && line[1] == '\n') break;   // the blank line: the end of the header section
        size_t i = 0;
        while (i < n && std::tolower(static_cast<unsigned char>(line[i])) ==
                        std::tolower(static_cast<unsigned char>(name[i]))) i++;
        if (i == n) return line;
        const char* nl = std::strchr(line, '\n');
        line = nl ? nl + 1 : nullptr;
    }
    return nullptr;
}

/// Whether `c` ends a header field's value: the line's end, whitespace, or the text's end.
inline bool isFieldEnd(char c) { return c == '\r' || c == '\n' || c == ' ' || c == '\t' || c == '\0'; }

/// Past the optional whitespace after a field's colon.
inline const char* skipSpace(const char* p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/// What a request says about its body's length.
enum class Length : uint8_t { Absent, Valid, Invalid };

/// Read the Content-Length header into `out`: Absent without one, Invalid when a value is not one whole decimal number or two lines disagree, which leaves the body's framing ambiguous (RFC 9112 § 6.3).
inline Length contentLength(const char* head, uint64_t& out) {
    static constexpr char kName[] = "Content-Length:";
    Length result = Length::Absent;
    for (const char* line = findHeader(head, kName); line;) {
        const char* value = skipSpace(line + sizeof(kName) - 1);
        if (!std::isdigit(static_cast<unsigned char>(*value))) return Length::Invalid;   // strtoull would take a sign
        char* end = nullptr;
        errno = 0;
        const unsigned long long parsed = std::strtoull(value, &end, 10);
        if (!isFieldEnd(*end) || errno == ERANGE) return Length::Invalid;
        if (result == Length::Valid && parsed != out) return Length::Invalid;
        out = parsed;
        result = Length::Valid;
        const char* nl = std::strchr(line, '\n');
        line = nl ? findHeader(nl + 1, kName) : nullptr;
    }
    return result;
}

/// Whether the client holds its body until it hears kContinue.
inline bool expectsContinue(const char* head) {
    static constexpr char kName[] = "Expect:";
    static constexpr char kValue[] = "100-continue";
    const char* line = findHeader(head, kName);
    if (!line) return false;
    const char* value = skipSpace(line + sizeof(kName) - 1);
    for (size_t i = 0; i < sizeof(kValue) - 1; i++)
        if (std::tolower(static_cast<unsigned char>(value[i])) != kValue[i]) return false;
    return isFieldEnd(value[sizeof(kValue) - 1]);
}

/// Read into `buf` until the blank line, the peer leaving or `cap - 1` bytes; answers the bytes held, `headLen` the head's with its blank line, or all held without one.
template <typename Recv>
size_t readHead(char* buf, size_t cap, Recv&& recv, size_t& headLen) {
    size_t got = 0;
    const char* end = nullptr;
    buf[0] = '\0';
    while (got + 1 < cap) {
        const int n = recv(buf + got, cap - 1 - got);
        if (n <= 0) break;
        got += static_cast<size_t>(n);
        buf[got] = '\0';
        end = std::strstr(buf, "\r\n\r\n");
        if (end) break;
    }
    headLen = end ? static_cast<size_t>(end + 4 - buf) : got;
    return got;
}

/// Read into `buf` until it holds `want` bytes or the peer goes, starting from the `have` already there; answers the bytes held.
template <typename Recv>
size_t fillTo(char* buf, size_t have, size_t want, Recv&& recv) {
    while (have < want) {
        const int n = recv(buf + have, want - have);
        if (n <= 0) break;
        have += static_cast<size_t>(n);
    }
    return have;
}

/// The value of query parameter `name`, up to the next `&` or the end, or null; a parameter matches whole, so `xlive=` is not `live=`.
inline const char* queryValue(const char* query, const char* name) {
    const size_t n = std::strlen(name);
    for (const char* p = query; p && *p;) {
        if (std::strncmp(p, name, n) == 0 && p[n] == '=') return p + n + 1;
        p = std::strchr(p, '&');
        if (p) p++;
    }
    return nullptr;
}

/// Whether the query holds the parameter `name=value` whole: `live=10` is not `live=1`.
inline bool queryHas(const char* query, const char* name, const char* value) {
    const char* at = queryValue(query, name);
    const size_t v = std::strlen(value);
    return at && std::strncmp(at, value, v) == 0 && (at[v] == '&' || at[v] == '\0');
}

/// Whether a bracketed IPv6 host is loopback, unique local (fc00::/7) or link-local (fe80::/10).
inline bool isLocalIPv6(const char* h) {
    return std::strcmp(h, "[::1]") == 0 || std::strncmp(h, "[fc", 3) == 0 || std::strncmp(h, "[fd", 3) == 0
        || (std::strncmp(h, "[fe", 3) == 0 && h[3] >= '8' && h[3] <= 'b');
}

/// Whether a lowercase name of `n` bytes is local: `localhost` (RFC 6761), `.local` (RFC 6762), `.home.arpa` (RFC 8375), `.internal`, or one label.
inline bool isLocalName(const char* h, size_t n) {
    constexpr const char* kTails[] = {".localhost", ".local", ".home.arpa", ".internal"};
    if (std::strcmp(h, "localhost") == 0 || std::strchr(h, '.') == nullptr) return true;
    for (const char* t : kTails) {
        const size_t l = std::strlen(t);
        if (n > l && std::strcmp(h + n - l, t) == 0) return true;
    }
    return false;
}

/// `host` (`n` bytes) lowercased into `h`, a fully qualified name's root dot dropped; its length, 0 when empty or past the buffer.
inline size_t lowerHost(const char* host, size_t n, char (&h)[64]) {
    if (n == 0 || n >= sizeof(h)) return 0;
    for (size_t i = 0; i < n; i++) h[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(host[i])));
    h[n] = '\0';
    if (h[n - 1] == '.') h[--n] = '\0';
    return n;
}

/// Whether a URL's host (`n` bytes, no port) names this network: a private or loopback address, or a local name.
inline bool isLocalHost(const char* host, size_t n) {
    char h[64];
    if ((n = lowerHost(host, n, h)) == 0) return false;
    if (h[0] == '[') return isLocalIPv6(h);
    uint8_t ip[4];
    return parseDottedQuad(h, ip) ? isPrivateIPv4(ip) : isLocalName(h, n);
}

/// The length of the host at `p`, up to its port, path or the field's end; an IPv6 address runs to its closing bracket, past its colons.
inline size_t hostLength(const char* p) {
    const char* stop = *p == '[' ? "]" : ":/";
    const char* end = p;
    while (!isFieldEnd(*end) && !std::strchr(stop, *end)) end++;
    if (*end == ']') end++;
    return static_cast<size_t>(end - p);
}

/// The host of a URL in a header value, `n` bytes up to its port, or null when its line holds no `scheme://`.
inline const char* urlHost(const char* value, size_t& n) {
    const char* scheme = skipSpace(value);
    while (!isFieldEnd(*scheme) && std::strncmp(scheme, "://", 3) != 0) scheme++;
    if (std::strncmp(scheme, "://", 3) != 0) return nullptr;
    n = hostLength(scheme + 3);
    return scheme + 3;
}

/// Whether `n` bytes of `a` and `b` match, case-insensitively, as names compare.
inline bool sameName(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

/// Whether a host's first label is `own`, case-insensitively: the device under whatever domain its router or mDNS gives the name it sends as its DHCP hostname.
inline bool isOwnName(const char* host, size_t n, const char* own) {
    const size_t k = own ? std::strlen(own) : 0;
    return k > 0 && n >= k && (n == k || host[k] == '.') && sameName(host, own, k);
}

/// Whether a `Host` value names this device as only the network can: an IP literal, a local name that public DNS leaves unserved, or its own name under any domain.
inline bool isOwnHost(const char* host, size_t n, const char* own) {
    char h[64];
    if ((n = lowerHost(host, n, h)) == 0) return false;
    uint8_t ip[4];
    return h[0] == '[' || parseDottedQuad(h, ip) || isLocalName(h, n) || isOwnName(h, n, own);
}

/// Whether an `Origin` value, the page a browser request comes from, names this network by an address or a local name, judged by itself.
inline bool isTrustedOrigin(const char* value) {
    size_t n = 0;
    const char* host = urlHost(value, n);
    return host && isLocalHost(host, n);
}

/// Whether an `Origin` names the host the request names: a page the device served, under whichever name or address it was opened by, which the `Host` check vets.
inline bool isSameHost(const char* value, const char* host, size_t hostLen) {
    size_t n = 0;
    const char* origin = urlHost(value, n);
    return origin && host && n == hostLen && sameName(origin, host, n);
}

/// Whether an `Origin` names another device under the domain this request names the device by (`mm-aaaa.fritz.box` to `mm-bbbb.fritz.box`): the map on one device driving another, both under their router's names.
inline bool isSiblingOrigin(const char* value, const char* host, size_t hostLen, const char* own) {
    size_t n = 0;
    const char* origin = urlHost(value, n);
    const size_t k = own ? std::strlen(own) : 0;
    if (!origin || !host || !isOwnName(host, hostLen, own) || hostLen <= k) return false;   // the device's own name, under a domain
    const char* dot = static_cast<const char*>(std::memchr(origin, '.', n));
    const size_t domainLen = hostLen - k;
    return dot && static_cast<size_t>(origin + n - dot) == domainLen && sameName(dot, host + k, domainLen);
}

/// Whether a request comes from beyond this network, by a public `Origin` or, for a rebinding page's read that carries none, a `Host` naming another than device `own`; the access point exempt.
inline bool fromBeyondNetwork(const char* head, bool viaAccessPoint, const char* own = nullptr) {
    if (viaAccessPoint) return false;
    const char* host = findHeader(head, "Host:");
    if (host) host = skipSpace(host + 5);
    const size_t hostLen = host ? hostLength(host) : 0;
    const char* origin = findHeader(head, "Origin:");
    if (origin && !isTrustedOrigin(origin + 7) && !isSameHost(origin + 7, host, hostLen) && !isSiblingOrigin(origin + 7, host, hostLen, own)) return true;
    return host && !isOwnHost(host, hostLen, own);   // HTTP/1.0 names none
}

/// Whether `url` is one a device fetches: http or https only.
inline bool isHttpUrl(const char* url) { return std::strncmp(url, "http://", 7) == 0 || std::strncmp(url, "https://", 8) == 0; }

}  // namespace mm::http
