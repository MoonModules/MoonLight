#pragma once

/// Reading an HTTP/1.1 request head (RFC 9112), as pure functions the app's server and MoonBase share and the desktop tests.
///
/// @technical
///
/// The reads take a `recv(buf, cap)` callable returning the bytes read, 0 or less when the peer is gone, so a test feeds the bytes in whatever pieces it likes: TCP keeps no message boundaries, and a head or a body arrives split anywhere.

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

/// Read the Content-Length header into `out`: Absent without one, Invalid when its value is not one whole decimal number.
inline Length contentLength(const char* head, uint64_t& out) {
    static constexpr char kName[] = "Content-Length:";
    const char* line = findHeader(head, kName);
    if (!line) return Length::Absent;
    const char* value = skipSpace(line + sizeof(kName) - 1);
    if (!std::isdigit(static_cast<unsigned char>(*value))) return Length::Invalid;   // strtoull would take a sign
    char* end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    if (!isFieldEnd(*end) || errno == ERANGE) return Length::Invalid;
    out = parsed;
    return Length::Valid;
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

}  // namespace mm::http
