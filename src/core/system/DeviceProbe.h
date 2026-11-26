#pragma once

#include "core/util/HttpHead.h"   // findHeader: the probe reads a page's Server header as the server reads a request's
#include "core/util/format.h"     // formatTo: nonblocking formatting into a fixed buffer
#include "platform/platform_net.h"   // TcpConnection

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>

/// The request for an added address's page, whose Server header names the firmware and whose page names the device; Discovery decides its timing and the row.

namespace mm {

/// The request in flight and what the page said so far.
struct PageProbe {
    platform::TcpConnection conn;   ///< the request's connection
    uint8_t receiver = 0;  ///< which added address
    uint8_t state = 0;     ///< 0 idle, 1 connecting, 2 sending, 3 reading
    uint8_t sent = 0;      ///< how much of the request the socket took
    uint8_t ticks = 0;     ///< ticks in this state, which bounds a silent address
    const char* tag = "";  ///< the tag holding the name, picked once the headers name the firmware
    uint8_t tagAt = 0;     ///< how much of that tag the page has matched across reads
    bool    inTag = false; ///< inside that tag, whose text is the name
    char    server[40] = {};   ///< the Server header's value
    char    name[24] = {};     ///< the name's text
    uint8_t nameLen = 0;   ///< its length so far
    char    line[48] = {}; ///< the header line being read, for the Server one
    uint8_t lineLen = 0;   ///< its length so far
    bool    body = false;  ///< past the headers

    /// Read a piece of the page, the Server header then the name, true once the name is complete: the page's <title>, or a Tasmota's first <h2>.
    bool scan(const uint8_t* b, size_t n) {
        for (size_t i = 0; i < n; i++) {
            const char c = static_cast<char>(b[i]);
            if (!body) readHeader(c);
            else if (readBody(c)) return true;
        }
        return false;
    }

    /// One character of the headers: a line ends at '\n', the Server one is kept, and the blank one ends the headers and picks the name's tag.
    void readHeader(char c) {
        if (c == '\r') return;
        if (c != '\n') {
            if (lineLen + 1u < sizeof(line)) line[lineLen++] = c;
            return;
        }
        line[lineLen] = 0;
        if (lineLen == 0) {
            body = true;
            tag = std::strncmp(server, "Tasmota/", 8) == 0 ? "<h2>" : "<title>";
        } else if (const char* h = http::findHeader(line, "Server:")) {
            mm::formatTo(server, sizeof(server), "%s", http::skipSpace(h + 7));
        }
        lineLen = 0;
    }

    /// One character of the body: the tag matched across reads, then its text up to the next '<'; true once that text is complete.
    bool readBody(char c) {
        if (!inTag) {
            matchTag(c);
            return false;
        }
        if (c == '<') {   // the name ends; a line break before the closing tag is the page's layout, not the name
            while (nameLen && std::isspace(static_cast<unsigned char>(name[nameLen - 1]))) name[--nameLen] = 0;
            return true;
        }
        const bool leadingSpace = nameLen == 0 && std::isspace(static_cast<unsigned char>(c));   // a title's leading line break
        if (!leadingSpace && nameLen + 1u < sizeof(name)) { name[nameLen++] = c; name[nameLen] = 0; }
        return false;
    }

    /// One more character of the name's tag matched, or the match restarted; the tag is case-insensitive, as HTML's are.
    void matchTag(char c) {
        const char lc = static_cast<char>(c | 0x20);   // a letter lowercased; '<', '>' and '2' stay as they are
        tagAt = (lc == tag[tagAt]) ? tagAt + 1 : (c == '<' ? 1 : 0);
        inTag = tag[tagAt] == '\0';
    }
};

}  // namespace mm
