/// Out-of-line half of JsonUtil.h.
/// Only what must not be inlined lives here.
///
/// The rest of the header is deliberately header-only: small first-match readers whose callers benefit from inlining.
/// `parseInt` is the exception: every integer setting is read through it, and out of line it measured 1.3 KB less flash on the classic ESP32.
/// Every caller is off the hot path, so the call is free in practice.
///
/// The number reader, the JSON Pointer walk, the edits and the writer live here for that reason: each has a few callers, none on a hot path.

#include "core/util/JsonUtil.h"
#include "core/util/JsonSink.h"
#include "core/util/parse.h"

#include <climits>
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

namespace {

/// Whether `c` is a decimal digit, as JSON's grammar spells one.
bool isDigit(char c) { return c >= '0' && c <= '9'; }

/// Powers of ten a uint64_t holds, the scale every step of the number reader multiplies or divides by.
constexpr uint64_t kPow10[20] = {
    1ull, 10ull, 100ull, 1000ull, 10000ull, 100000ull, 1000000ull, 10000000ull, 100000000ull,
    1000000000ull, 10000000000ull, 100000000000ull, 1000000000000ull, 10000000000000ull,
    100000000000000ull, 1000000000000000ull, 10000000000000000ull, 100000000000000000ull,
    1000000000000000000ull, 10000000000000000000ull};

/// The mantissa's ceiling: 17 significant digits, past what a double carries, so a digit beyond it only moves the decimal point.
constexpr uint64_t kMantissaCap = 10000000000000000ull;

/// `m` times ten to `e`, truncated toward zero and saturated at the uint64_t range.
uint64_t scaleWhole(uint64_t m, int e) {
    if (e >= 0) {
        for (; e > 0; e--) {
            if (m > UINT64_MAX / 10) return UINT64_MAX;
            m *= 10;
        }
        return m;
    }
    return -e > 19 ? 0 : m / kPow10[-e];
}

/// `m` times ten to `e` in Q16.16 magnitude, rounded to the nearest 1/65536 and saturated at INT32_MAX.
uint32_t scaleFixed(uint64_t m, int e) {
    if (e >= 0) {
        const uint64_t whole = scaleWhole(m, e);
        return whole > 0x7FFF ? INT32_MAX : static_cast<uint32_t>(whole << 16);
    }
    int k = -e;
    if (k > 19) { m /= kPow10[k > 38 ? 19 : k - 19]; k = 19; }   // past 19 digits the fraction is far below 1/65536
    const uint64_t whole = m / kPow10[k];
    if (whole > 0x7FFF) return INT32_MAX;
    uint64_t rem = m % kPow10[k], den = kPow10[k];
    // Fourteen digits of fraction keep rem * 65536 inside 64 bits and still resolve 1/65536.
    if (k > 14) { rem /= kPow10[k - 14]; den = kPow10[14]; }
    const uint64_t frac = (rem * 65536u + den / 2) / den;
    const uint64_t mag = (whole << 16) + frac;
    return mag > INT32_MAX ? INT32_MAX : static_cast<uint32_t>(mag);
}

/// The digits at `s` into the mantissa, `e` counting how far the decimal point moves; the end of the digits.
const char* readDigits(const char* s, uint64_t& m, int& e, bool fraction) {
    for (; isDigit(*s); s++) {
        if (m < kMantissaCap) {
            m = m * 10 + static_cast<uint64_t>(*s - '0');
            if (fraction) e--;
        } else if (!fraction) {
            e++;
        }
    }
    return s;
}

/// An exponent at `s` added to `e`, and its end, or `s` itself where a bare 'e' has no digits and so is no part of the number.
const char* readExponent(const char* s, int& e) {
    if (*s != 'e' && *s != 'E') return s;
    const char* d = s + 1;
    const bool neg = *d == '-';
    if (*d == '+' || *d == '-') d++;
    if (!isDigit(*d)) return s;
    int x = 0;
    for (; isDigit(*d); d++)
        if (x < 400) x = x * 10 + (*d - '0');
    e += neg ? -x : x;
    return d;
}

}  // namespace

const char* parseNumber(const char* p, long& whole, int32_t& fixed, bool& decimal) {
    const char* s = p;
    const bool neg = *s == '-';
    if (neg) s++;
    if (!isDigit(*s)) return nullptr;
    // The significant digits in one integer and the decimal point as a power of ten, so no step rounds.
    uint64_t m = 0;
    int e = 0;
    s = readDigits(s, m, e, false);
    decimal = *s == '.';
    if (decimal) s = readDigits(s + 1, m, e, true);
    const char* end = readExponent(s, e);
    decimal = decimal || end != s;
    s = end;
    const uint64_t w = scaleWhole(m, e);
    if (neg) whole = w > static_cast<uint64_t>(LONG_MAX) ? LONG_MIN : -static_cast<long>(w);
    else     whole = w > static_cast<uint64_t>(LONG_MAX) ? LONG_MAX : static_cast<long>(w);
    const uint32_t f = scaleFixed(m, e);
    fixed = neg ? -static_cast<int32_t>(f) : static_cast<int32_t>(f);
    return s;
}

namespace {

/// The character a pointer token holds at `i`, "~1" being '/' and "~0" '~', with `i` moved past an escape.
char tokenChar(const char* tok, size_t len, size_t& i) {
    if (tok[i] == '~' && i + 1 < len && (tok[i + 1] == '0' || tok[i + 1] == '1')) return tok[++i] == '0' ? '~' : '/';
    return tok[i];
}

/// Whether an object key equals a pointer token, decoding as it compares, so no token is copied.
bool tokenIs(const char* key, const char* tok, size_t len) {
    for (size_t i = 0; i < len; i++, key++)
        if (*key != tokenChar(tok, len, i)) return false;
    return *key == '\0';
}

/// The array index a token spells, digits with no leading zero as RFC 6901 writes it, or -1.
long tokenIndex(const char* tok, size_t len) {
    if (len == 0 || len > 9 || (len > 1 && tok[0] == '0')) return -1;
    long v = 0;
    for (size_t i = 0; i < len; i++) {
        if (!isDigit(tok[i])) return -1;
        v = v * 10 + (tok[i] - '0');
    }
    return v;
}

/// The member of object `n` a token names, or -1.
int memberIndex(const JsonDoc& doc, const JsonNode& n, const char* tok, size_t len) {
    for (int i = n.firstChild; i >= 0; i = doc.nodes[i].next)
        if (doc.nodes[i].key && tokenIs(doc.nodes[i].key, tok, len)) return i;
    return -1;
}

/// Element `want` of array `n`, or -1.
int elementIndex(const JsonDoc& doc, const JsonNode& n, long want) {
    for (int i = n.firstChild; i >= 0 && want >= 0; i = doc.nodes[i].next, want--)
        if (want == 0) return i;
    return -1;
}

/// The child of `parent` a token names, or -1.
int childIndex(const JsonDoc& doc, int parent, const char* tok, size_t len) {
    const JsonNode* n = doc.node(parent);
    if (n && n->type == JsonType::Object) return memberIndex(doc, *n, tok, len);
    if (n && n->type == JsonType::Array) return elementIndex(doc, *n, tokenIndex(tok, len));
    return -1;
}

/// The token after the '/' at `ptr`, its length in `len`, with `ptr` moved to its end.
const char* nextToken(const char*& ptr, size_t& len) {
    const char* tok = ++ptr;
    while (*ptr && *ptr != '/') ptr++;
    len = static_cast<size_t>(ptr - tok);
    return tok;
}

/// The index a pointer names, or -1; the walk is by index, so an edit growing the pool between two walks never dangles.
int indexAt(const JsonDoc& doc, const char* ptr) {
    if (!ptr || !doc.valid()) return -1;
    int cur = doc.root;
    if (*ptr && *ptr != '/') return -1;
    while (*ptr == '/' && cur >= 0) {
        size_t len = 0;
        const char* tok = nextToken(ptr, len);
        cur = childIndex(doc, cur, tok, len);
    }
    return cur;
}

/// A fresh node, or -1 with the document marked out of memory.
int newNode(JsonDoc& doc, JsonType type) {
    if (!doc.ensureNode()) { doc.outOfMemory = true; return -1; }
    const int i = doc.count++;
    doc.nodes[i] = JsonNode{};
    doc.nodes[i].type = type;
    return i;
}

/// Link `child` as the last child of `parent`.
void appendChild(JsonDoc& doc, int parent, int child) {
    int* link = &doc.nodes[parent].firstChild;
    while (*link >= 0) link = &doc.nodes[*link].next;
    *link = child;
}

/// The decoded text of a pointer token, owned by the document, for a member an edit creates.
const char* ownToken(JsonDoc& doc, const char* tok, size_t len) {
    char key[64];
    size_t n = 0;
    for (size_t i = 0; i < len && n + 1 < sizeof(key); i++) key[n++] = tokenChar(tok, len, i);
    return doc.own(key, n);
}

/// A new child of `parent` for a token it lacks: an object's member by the token's name, or an array's next element at "-"; -1 where none can be.
int addChild(JsonDoc& doc, int parent, const char* tok, size_t len) {
    const JsonType type = doc.nodes[parent].type;
    const bool member = type == JsonType::Object;
    if (!member && !(type == JsonType::Array && len == 1 && tok[0] == '-')) return -1;
    const char* key = member ? ownToken(doc, tok, len) : nullptr;
    if (member && !key) return -1;
    const int child = newNode(doc, JsonType::Null);
    if (child < 0) return -1;
    doc.nodes[child].key = key;
    appendChild(doc, parent, child);
    return child;
}

/// The child of `parent` a token names, added where it has none; -1 where none can be.
int childOrNew(JsonDoc& doc, int parent, const char* tok, size_t len) {
    const int child = childIndex(doc, parent, tok, len);
    return child >= 0 ? child : addChild(doc, parent, tok, len);
}

/// The index a pointer names, creating a missing object member and any object on the way; "-" appends to an array; -1 where the path cannot exist.
int ensureIndex(JsonDoc& doc, const char* ptr) {
    if (!ptr || (*ptr && *ptr != '/')) return -1;
    if (!doc.valid()) doc.root = newNode(doc, JsonType::Object);
    int cur = doc.root;
    while (*ptr == '/' && cur >= 0) {
        size_t len = 0;
        const char* tok = nextToken(ptr, len);
        const int child = childOrNew(doc, cur, tok, len);
        if (child < 0) return -1;
        // A step with more path after it is an object, so a set on a fresh path builds the folders it names.
        if (*ptr == '/' && doc.nodes[child].type == JsonType::Null) doc.nodes[child].type = JsonType::Object;
        cur = child;
    }
    return cur;
}

/// Give a node a new type, dropping what it held; the children it had stay in the pool until the document goes.
JsonNode* resetNode(JsonDoc& doc, int i, JsonType type) {
    JsonNode& n = doc.nodes[i];
    n.type = type;
    n.firstChild = -1;
    n.decimal = false;
    n.numLen = 0;
    n.str = nullptr;
    return &n;
}

/// The text a string or a number carries, copied so it outlives the document it came from.
bool copyText(JsonDoc& dst, int di, const JsonNode& s) {
    const bool number = s.type == JsonType::Int && s.numLen;
    if (s.type != JsonType::String && !number) return true;
    const char* text = dst.own(s.str, number ? s.numLen : std::strlen(s.str));
    if (!text) return false;
    dst.nodes[di].str = text;
    dst.nodes[di].numLen = number ? s.numLen : 0;
    return true;
}

bool copyInto(JsonDoc& dst, int di, const JsonDoc& src, int si, int depth);

/// A new last child of `di` holding a copy of `src`'s node `sc`, its member name included.
bool copyChild(JsonDoc& dst, int di, const JsonDoc& src, int sc, int depth) {
    const int dc = newNode(dst, JsonType::Null);
    if (dc < 0) return false;
    const char* key = src.nodes[sc].key;
    if (key && !(key = dst.own(key, std::strlen(key)))) return false;
    dst.nodes[dc].key = key;
    appendChild(dst, di, dc);
    return copyInto(dst, dc, src, sc, depth + 1);
}

/// Copy `src`'s node `si` and everything under it into `dst`'s node `di`, by index on both sides.
bool copyInto(JsonDoc& dst, int di, const JsonDoc& src, int si, int depth) {
    if (depth >= kMaxDepth) return false;
    const JsonNode s = src.nodes[si];   // by value: `dst` growing never moves it
    JsonNode* d = resetNode(dst, di, s.type);
    d->decimal = s.decimal;
    d->intValue = s.intValue;
    if (!copyText(dst, di, s)) return false;
    for (int sc = s.firstChild; sc >= 0; sc = src.nodes[sc].next)
        if (!copyChild(dst, di, src, sc, depth)) return false;
    return true;
}

}  // namespace

const JsonNode* at(const JsonDoc& doc, const char* pointer) {
    return doc.node(indexAt(doc, pointer));
}

int32_t readFixed(const JsonNode* n, int32_t fallback) {
    if (!n) return fallback;
    if (n->type == JsonType::Bool) return n->intValue ? 65536 : 0;
    if (n->type != JsonType::Int) return fallback;
    // A decimal is its text, read again here rather than kept per node, since only a script asks.
    if (n->decimal && n->numLen) {
        long whole = 0;
        int32_t fixed = 0;
        bool decimal = false;
        if (parseNumber(n->str, whole, fixed, decimal)) return fixed;
    }
    const long v = n->intValue;
    return v > 0x7FFF ? INT32_MAX : v < -0x8000 ? INT32_MIN : static_cast<int32_t>(v * 65536);
}

bool readNumber(const JsonNode* n, long& whole, int32_t& fixed) {
    if (!n) return false;
    if (n->type == JsonType::Int || n->type == JsonType::Bool) {
        whole = n->intValue;
        fixed = readFixed(n);
        return true;
    }
    if (n->type != JsonType::String || !n->str) return false;
    const char* s = n->str;
    while (*s == ' ') s++;
    if (*s == '+') s++;
    bool decimal = false;
    return parseNumber(s, whole, fixed, decimal) != nullptr;
}

bool appendText(JsonDoc& doc, const char* pointer, const char* text) {
    if (!text) return false;
    const int i = ensureIndex(doc, pointer);
    if (i < 0) return false;
    const JsonNode& n = doc.nodes[i];
    const char* before = n.type == JsonType::String && n.str ? n.str : "";
    const size_t a = std::strlen(before), b = std::strlen(text);
    char* joined = static_cast<char*>(std::malloc(a + b + 1));
    if (!joined) return false;
    std::memcpy(joined, before, a);
    std::memcpy(joined + a, text, b + 1);
    const char* kept = doc.own(joined, a + b);
    std::free(joined);
    if (!kept) return false;
    resetNode(doc, i, JsonType::String)->str = kept;
    return true;
}

bool appendValue(JsonDoc& dst, const char* dstPointer, const JsonDoc& src, const char* srcPointer) {
    const JsonNode* n = at(src, srcPointer);
    if (!n) return false;
    if (n->type == JsonType::String) return appendText(dst, dstPointer, n->str ? n->str : "");
    JsonSink sink;
    write(sink, src, n);
    return !sink.overflowed() && appendText(dst, dstPointer, sink.data());
}

bool setInt(JsonDoc& doc, const char* pointer, long value) {
    const int i = ensureIndex(doc, pointer);
    if (i < 0) return false;
    JsonNode* n = resetNode(doc, i, JsonType::Int);
    n->intValue = value;
    return true;
}

bool setFixed(JsonDoc& doc, const char* pointer, int32_t value) {
    const int i = ensureIndex(doc, pointer);
    if (i < 0) return false;
    // Written as its text, four decimals resolving 1/65536 to within a rounding step, so a decimal is its text however it came.
    const int64_t v = value;
    const uint64_t mag = static_cast<uint64_t>(v < 0 ? -v : v);
    uint64_t whole = mag >> 16, frac = ((mag & 0xFFFF) * 10000 + 32768) >> 16;
    if (frac >= 10000) { whole++; frac -= 10000; }
    char text[24];
    const int len = frac ? std::snprintf(text, sizeof(text), "%s%lu.%04lu", v < 0 ? "-" : "",
                                         static_cast<unsigned long>(whole), static_cast<unsigned long>(frac))
                         : std::snprintf(text, sizeof(text), "%s%lu", v < 0 && whole ? "-" : "", static_cast<unsigned long>(whole));
    const char* kept = doc.own(text, static_cast<size_t>(len));
    if (!kept) return false;
    JsonNode* n = resetNode(doc, i, JsonType::Int);
    n->intValue = value / 65536;   // truncated toward zero, as a parsed decimal is
    n->decimal = frac != 0;
    n->str = kept;
    n->numLen = static_cast<uint16_t>(len);
    return true;
}

bool setString(JsonDoc& doc, const char* pointer, const char* value) {
    if (!value) return false;
    const int i = ensureIndex(doc, pointer);
    if (i < 0) return false;
    const char* text = doc.own(value, std::strlen(value));
    if (!text) return false;
    resetNode(doc, i, JsonType::String)->str = text;
    return true;
}

bool copyValue(JsonDoc& dst, const char* dstPointer, const JsonDoc& src, const char* srcPointer) {
    const int si = indexAt(src, srcPointer);
    if (si < 0) return false;
    // Within one document the source could be the destination's own parent, so the value goes through a copy first.
    if (&dst == &src) {
        JsonDoc tmp;
        tmp.root = newNode(tmp, JsonType::Null);
        if (tmp.root < 0 || !copyInto(tmp, tmp.root, src, si, 0)) return false;
        return copyValue(dst, dstPointer, tmp, "");
    }
    const int di = ensureIndex(dst, dstPointer);
    if (di < 0) return false;
    return copyInto(dst, di, src, si, 0);
}

namespace {

void writeNode(JsonSink& out, const JsonDoc& doc, const JsonNode* n, int depth);

/// Write a value that holds no other, a parsed number repeated as written.
void writeScalar(JsonSink& out, const JsonNode& n) {
    switch (n.type) {
        case JsonType::Bool:   out.writeBool(n.intValue != 0); return;
        case JsonType::String: out.writeJsonString(n.str); return;
        case JsonType::Int:
            if (n.numLen) out.append(n.str, n.numLen);
            else out.appendf("%ld", n.intValue);
            return;
        default: out.append("null");
    }
}

/// Write an object or an array and everything under it.
void writeContainer(JsonSink& out, const JsonDoc& doc, const JsonNode& n, int depth) {
    const bool obj = n.type == JsonType::Object;
    out.append(obj ? "{" : "[");
    for (int i = n.firstChild; i >= 0; i = doc.nodes[i].next) {
        if (i != n.firstChild) out.append(",");
        if (obj) {
            out.writeJsonString(doc.nodes[i].key);
            out.append(":");
        }
        writeNode(out, doc, &doc.nodes[i], depth + 1);
    }
    out.append(obj ? "}" : "]");
}

/// Write one node at a depth, the bound an edit-built path also respects.
void writeNode(JsonSink& out, const JsonDoc& doc, const JsonNode* n, int depth) {
    if (!n || depth >= kMaxDepth) out.append("null");
    else if (n->type == JsonType::Object || n->type == JsonType::Array) writeContainer(out, doc, *n, depth);
    else writeScalar(out, *n);
}

}  // namespace

void write(JsonSink& out, const JsonDoc& doc, const JsonNode* n) {
    writeNode(out, doc, n, 0);
}

}  // namespace mm::json
