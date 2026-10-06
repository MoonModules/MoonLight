/// Out-of-line half of parse.h, for the reason its header gives.

#include "core/util/parse.h"

#include <cerrno>
#include <climits>
#include <cstdlib>

namespace mm {

int parseIntStr(const char* s, int fallback) {
    if (!s) return fallback;
    char* end = nullptr;
    errno = 0;                                              // strtol only ever SETS it on error
    // A 0x prefix reads hex, the form a datasheet gives an I2C address in; base 0 would also read a leading zero as octal.
    const bool hex = s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
    const long v = std::strtol(s, &end, hex ? 16 : 10);
    if (end == s) return fallback;                          // no digits: not a number at all
    if (errno == ERANGE) return fallback;                   // saturated: outside `long`
    if (v < INT_MIN || v > INT_MAX) return fallback;        // fits `long`, would not survive `int`
    return static_cast<int>(v);
}

}  // namespace mm
