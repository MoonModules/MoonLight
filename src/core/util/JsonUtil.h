#pragma once

/// @defgroup JsonUtil Reading JSON
/// @{
/// Two layers, both off the hot path, so bounded stack use is fine; the integer reader is out of line for its flash, as its number conversion is (parse.h).
///
/// The flat helpers scan for a key over the subset we emit, and never descend into a nested object or an array.
/// The recursive reader walks nested structure, which the persisted device and preset lists need.
///
/// @moreinfo
///
/// ## What the flat scan covers
///
/// Flat key and value pairs, with optional whitespace after the colon, and string, integer or boolean values.
/// Many callers rely on that being a cheap search rather than a parse.
///
/// ## The recursive reader allocates per parse
///
/// The text arena and the node pool are taken from the heap, sized to the input and grown as needed, then freed with the document.
/// Nodes are referenced by index, so growing the pool never dangles a pointer, and there is no node-count or length cap and no large standing buffer.
/// Only the recursion is bounded, for the task stack.
///
/// Malformed or truncated input fails cleanly: the parse reports false, the accessors return safe defaults, and nothing reads out of bounds.
///
/// ## An absent key is not a zero
///
/// The flat integer and boolean readers cannot tell one from the other, so applying their result for an absent key clobbers a control's non-zero default.
/// A load path asks whether the key is present first, or an older or partial save silently resets a control on every reboot.
///
/// ## The shared document is a function-local static
///
/// A list restore parses into one document rather than a stack local, since that document overflows a device task stack and boot-loops it.
/// It lives in its own non-template function so the heavy object is one copy however many callback types instantiate the iteration, or each would multiply it.
/// Sharing is safe because parsing is strictly serial: a boot-time load or a single control write, never concurrent.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core/util/hex.h"

namespace mm::json {

// The longest search pattern these readers build, sized from its parts rather than a round number so the bound is provable.
inline constexpr size_t kMaxKeyLen = 58;
inline constexpr size_t kSearchLen = kMaxKeyLen + 5;

/// Build the search pattern the readers look for, false when the key is too long: it reports rather than truncating, a truncated pattern still matching something.
inline bool buildKeyPattern(char (&buf)[kSearchLen], const char* key, const char* sep) {
    const int n = std::snprintf(buf, sizeof(buf), "\"%s\"%s", key, sep);
    return n > 0 && static_cast<size_t>(n) < sizeof(buf);
}

inline void parseString(const char* json, const char* key, char* out, size_t maxLen) {
    if (!json || !key || !out || maxLen == 0) return;
    char search[kSearchLen];
    if (!buildKeyPattern(search, key, ":\"")) return;      // key too long → treat as absent
    const char* start = std::strstr(json, search);
    if (!start) {
        if (!buildKeyPattern(search, key, ": \"")) return;
        start = std::strstr(json, search);
    }
    if (!start) return;
    start += std::strlen(search);
    // Copy to the real closing quote, decoding our own writer's escapes: a bare search stops at an escaped one inside the value.
    size_t oi = 0;
    for (const char* p = start; *p && oi + 1 < maxLen; p++) {
        if (*p == '\\' && p[1]) {
            p++;                 // consume the backslash; map the escape
            switch (*p) {
                case 'n': out[oi++] = '\n'; break;
                case 'r': out[oi++] = '\r'; break;
                case 't': out[oi++] = '\t'; break;
                case 'b': out[oi++] = '\b'; break;
                case 'f': out[oi++] = '\f'; break;
                case 'u': {     // \uXXXX: decode the low byte (the writer only emits \u00XX)
                    int h1 = p[1] ? hexDigit(p[1]) : -1, h2 = (p[1] && p[2]) ? hexDigit(p[2]) : -1;
                    int h3 = (p[1] && p[2] && p[3]) ? hexDigit(p[3]) : -1;
                    int h4 = (p[1] && p[2] && p[3] && p[4]) ? hexDigit(p[4]) : -1;
                    if (h1 >= 0 && h2 >= 0 && h3 >= 0 && h4 >= 0) {
                        out[oi++] = static_cast<char>((h3 << 4) | h4);   // low byte (high byte is 0x00)
                        p += 4;
                    } else { out[oi++] = 'u'; }   // malformed \u: copy literally, don't run off
                    break;
                }
                default:  out[oi++] = *p;   break;   // \" \\ / and anything else: copy literally
            }
        } else if (*p == '"') {
            break;               // unescaped quote: end of string
        } else {
            out[oi++] = *p;
        }
    }
    out[oi] = 0;
}

/// Whether the key is present at all: @xref{an-absent-key-is-not-a-zero|why a reader cannot tell one from a zero}.
inline bool hasKey(const char* json, const char* key) {
    if (!json || !key) return false;
    char search[kSearchLen];
    if (!buildKeyPattern(search, key, ":")) return false;   // key too long → treat as absent
    return std::strstr(json, search) != nullptr;
}

/// A key's integer value, quoted or not, decimal or 0x-prefixed hex; 0 when absent or not a number (parse.h).
int parseInt(const char* json, const char* key);

inline bool parseBool(const char* json, const char* key) {
    if (!json || !key) return false;
    char search[kSearchLen];
    if (!buildKeyPattern(search, key, ":")) return false;   // key too long → treat as absent
    const char* start = std::strstr(json, search);
    if (!start) {
        if (!buildKeyPattern(search, key, ": ")) return false;
        start = std::strstr(json, search);
    }
    if (!start) return false;
    const char* val = start + std::strlen(search);
    while (*val == ' ') val++;
    // Both the literal and a numeric one, since device models wrote numbers for flags that are now boolean controls, and some clients still send them.
    return std::strncmp(val, "true", 4) == 0 || *val == '1';
}

// The recursive reader: a standard recursive-descent parser that copies the input into the document's own buffer and links nodes by index.

// The recursion bound for a device task stack, far deeper than anything we emit yet a hard guard against a pathological input.
inline constexpr int kMaxDepth = 64;

enum class JsonType : uint8_t { Null, Bool, Int, String, Object, Array };

/// One value in the arena, its children linked by index so every node stays fixed-size with no per-node child array.
struct JsonNode {
    JsonType type = JsonType::Null;   ///< which kind of value this node holds
    const char* key = nullptr;    ///< the member name when this node is an object member
    const char* str = nullptr;    // string value (points into doc buffer) when type == String
    long intValue = 0;            ///< the numeric value, and a mirror for a boolean
    int firstChild = -1;          ///< the first child's index, or none
    int next = -1;                ///< the next sibling's index, or none
};

/// The parsed document, owning the text buffer and the node arena: @xref{the-recursive-reader-allocates-per-parse|how both are sized and freed}.
/// Non-copyable, owning two blocks, so a caller keeps it alive while walking.
struct JsonDoc {
    char*     buf = nullptr;     ///< the mutable copy of the input, which un-escaping rewrites in place
    JsonNode* nodes = nullptr;   ///< the node pool, grown as the parser allocates
    int       cap = 0;           ///< allocated node slots
    int       count = 0;         ///< used node slots
    int       root = -1;         ///< the top value's index, or none until a parse succeeds
    bool      outOfMemory = false;   ///< why a parse failed, when it was memory rather than the text

    /// An empty document, owning nothing until a parse fills it.
    JsonDoc() = default;
    /// Frees both blocks.
    ~JsonDoc() { std::free(buf); std::free(nodes); }
    /// Non-copyable: it owns two heap blocks.
    JsonDoc(const JsonDoc&) = delete;
    JsonDoc& operator=(const JsonDoc&) = delete;

    /// Whether a parse succeeded.
    bool valid() const { return root >= 0; }
    /// The node at an index, or nothing when it is out of range.
    const JsonNode* node(int i) const { return (i >= 0 && i < count) ? &nodes[i] : nullptr; }
    /// The top value, or nothing until a parse succeeds.
    const JsonNode* rootNode() const { return node(root); }

    /// Grow the pool when full, doubling so reallocations stay logarithmic; indices survive the move.
    bool ensureNode() {
        if (count < cap) return true;
        int newCap = cap ? cap * 2 : 32;
        auto* grown = static_cast<JsonNode*>(std::realloc(nodes, static_cast<size_t>(newCap) * sizeof(JsonNode)));
        if (!grown) return false;
        nodes = grown;
        cap = newCap;
        return true;
    }
};

namespace detail {

/// The cursor over the document's own buffer, which doubles as scratch: un-escaping rewrites string bytes in place.
struct JsonParser {
    JsonDoc& doc;   ///< the document being filled
    char* p;        ///< the read cursor into its buffer
    bool ok = true; ///< cleared once the input is known to be malformed

    /// A cursor at the start of the document's buffer.
    explicit JsonParser(JsonDoc& d) : doc(d), p(d.buf) {}

    /// Advance past any whitespace.
    void skipWs() { while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++; }

    /// Take the next node slot, or report failure when the pool cannot grow.
    int alloc() {
        if (!doc.ensureNode()) { ok = false; doc.outOfMemory = true; return -1; }   // grows the heap pool
        const int i = doc.count++;
        doc.nodes[i] = JsonNode{};   // realloc doesn't construct: reset the freshly-used slot
        return i;
    }

    /// Un-escape a string literal in place and terminate it, returning its first byte, or nothing when unterminated.
    char* parseStringLiteral() {
        p++;                       // opening quote
        char* out = p;             // write cursor (<= read cursor, so in-place is safe)
        char* start = out;
        while (*p) {
            char c = *p++;
            if (c == '"') { *out = 0; return start; }
            if (c == '\\') {
                char e = *p++;
                switch (e) {
                    case '"':  c = '"';  break;
                    case '\\': c = '\\'; break;
                    case '/':  c = '/';  break;
                    case 'n':  c = '\n'; break;
                    case 'r':  c = '\r'; break;
                    case 't':  c = '\t'; break;
                    case 'b':  c = '\b'; break;
                    case 'f':  c = '\f'; break;
                    case 0:    ok = false; return nullptr;  // trailing backslash, truncated
                    default:   c = e;    break;             // unknown escape (incl. \u): keep byte
                }
            }
            *out++ = c;
        }
        ok = false;                // ran off the end without a closing quote
        return nullptr;
    }

    /// Parse any value at the cursor, recursing into an object or an array.
    int parseValue(int depth) {
        if (!ok) return -1;
        if (depth >= kMaxDepth) { ok = false; return -1; }
        skipWs();
        switch (*p) {
            case '{': return parseObject(depth);
            case '[': return parseArray(depth);
            case '"': {
                int idx = alloc();
                char* s = parseStringLiteral();
                if (!ok || idx < 0) return -1;
                doc.nodes[idx].type = JsonType::String;
                doc.nodes[idx].str = s;
                return idx;
            }
            case 't':
            case 'f': {
                bool isTrue = (*p == 't');
                const char* lit = isTrue ? "true" : "false";
                size_t n = std::strlen(lit);
                if (std::strncmp(p, lit, n) != 0) { ok = false; return -1; }
                p += n;
                int idx = alloc();
                if (idx < 0) return -1;
                doc.nodes[idx].type = JsonType::Bool;
                doc.nodes[idx].intValue = isTrue ? 1 : 0;
                return idx;
            }
            case 'n': {
                if (std::strncmp(p, "null", 4) != 0) { ok = false; return -1; }
                p += 4;
                int idx = alloc();
                if (idx < 0) return -1;
                doc.nodes[idx].type = JsonType::Null;
                return idx;
            }
            default: {
                // An integer-only model: the sign and digits, then any fractional tail discarded, since we never persist a float.
                if (*p != '-' && (*p < '0' || *p > '9')) { ok = false; return -1; }
                char* endp = nullptr;
                long v = std::strtol(p, &endp, 10);
                if (endp == p) { ok = false; return -1; }
                p = endp;
                // Skipped precisely, so the scan stops at the number's real end rather than swallowing a following token.
                auto digits = [&] { while (*p >= '0' && *p <= '9') p++; };
                if (*p == '.') { p++; digits(); }
                if (*p == 'e' || *p == 'E') {
                    char* e = p++;
                    if (*p == '+' || *p == '-') p++;
                    if (*p >= '0' && *p <= '9') digits();
                    else p = e;     // bare 'e' with no exponent digits: not part of the number
                }
                int idx = alloc();
                if (idx < 0) return -1;
                doc.nodes[idx].type = JsonType::Int;
                doc.nodes[idx].intValue = v;
                return idx;
            }
        }
    }

    int parseObject(int depth) {
        int self = alloc();
        if (self < 0) return -1;
        doc.nodes[self].type = JsonType::Object;
        p++;                       // '{'
        skipWs();
        int prev = -1;
        if (*p == '}') { p++; return self; }
        while (ok) {
            skipWs();
            if (*p != '"') { ok = false; return -1; }
            char* key = parseStringLiteral();
            if (!ok) return -1;
            skipWs();
            if (*p != ':') { ok = false; return -1; }
            p++;
            int child = parseValue(depth + 1);
            if (!ok || child < 0) return -1;
            doc.nodes[child].key = key;
            if (prev < 0) doc.nodes[self].firstChild = child;
            else          doc.nodes[prev].next = child;
            prev = child;
            skipWs();
            if (*p == ',') { p++; continue; }
            if (*p == '}') { p++; return self; }
            ok = false; return -1;
        }
        return -1;
    }

    /// Parse an array at the cursor, linking each element as a child.
    int parseArray(int depth) {
        int self = alloc();
        if (self < 0) return -1;
        doc.nodes[self].type = JsonType::Array;
        p++;                       // '['
        skipWs();
        int prev = -1;
        if (*p == ']') { p++; return self; }
        while (ok) {
            int child = parseValue(depth + 1);
            if (!ok || child < 0) return -1;
            if (prev < 0) doc.nodes[self].firstChild = child;
            else          doc.nodes[prev].next = child;
            prev = child;
            skipWs();
            if (*p == ',') { p++; continue; }
            if (*p == ']') { p++; return self; }
            ok = false; return -1;
        }
        return -1;
    }
};

}  // namespace detail

/// Parse into a document, false on malformed, truncated or too-deep input, and safe on a null pointer or the empty string.
inline bool parse(const char* json, JsonDoc& out) {
    out.count = 0;
    out.root = -1;
    out.outOfMemory = false;
    if (!json) return false;
    size_t len = std::strlen(json);
    if (len == 0) return false;
    // A copy sized exactly to the input, which doubles as the string arena since un-escaping rewrites in place.
    std::free(out.buf);
    out.buf = static_cast<char*>(std::malloc(len + 1));
    if (!out.buf) { out.outOfMemory = true; return false; }
    std::memcpy(out.buf, json, len + 1);

    detail::JsonParser parser(out);
    int root = parser.parseValue(0);
    if (!parser.ok || root < 0) return false;
    parser.skipWs();
    if (*parser.p != 0) return false;   // trailing garbage after the top-level value
    out.root = root;
    return true;
}

// Navigation. Every accessor is null-safe and bounds-safe, returning a child or a safe default rather than crashing on a wrong-typed or missing node.

// Member of an object by key, or nullptr if `obj` is not an object / has no such member.
inline const JsonNode* member(const JsonDoc& doc, const JsonNode* obj, const char* key) {
    if (!obj || obj->type != JsonType::Object || !key) return nullptr;
    for (int i = obj->firstChild; i >= 0;) {
        const JsonNode* n = doc.node(i);
        if (!n) break;
        if (n->key && std::strcmp(n->key, key) == 0) return n;
        i = n->next;
    }
    return nullptr;
}

// Number of elements in an array (0 if `arr` is not an array).
inline int arraySize(const JsonDoc& doc, const JsonNode* arr) {
    if (!arr || arr->type != JsonType::Array) return 0;
    int count = 0;
    for (int i = arr->firstChild; i >= 0;) {
        const JsonNode* n = doc.node(i);
        if (!n) break;
        count++;
        i = n->next;
    }
    return count;
}

// Element `index` of an array, or nullptr if out of range / not an array.
inline const JsonNode* element(const JsonDoc& doc, const JsonNode* arr, int index) {
    if (!arr || arr->type != JsonType::Array || index < 0) return nullptr;
    int at = 0;
    for (int i = arr->firstChild; i >= 0;) {
        const JsonNode* n = doc.node(i);
        if (!n) break;
        if (at == index) return n;
        at++;
        i = n->next;
    }
    return nullptr;
}

/// Read a node as a string, always terminated and empty for a non-string or absent node; true when one was copied.
inline bool readString(const JsonNode* n, char* out, size_t maxLen) {
    if (!out || maxLen == 0) return false;
    out[0] = 0;
    if (!n || n->type != JsonType::String || !n->str) return false;
    std::strncpy(out, n->str, maxLen - 1);
    out[maxLen - 1] = 0;
    return true;
}

// Read a node as an int. A Bool reads as 0/1; anything else returns `fallback`.
inline long readInt(const JsonNode* n, long fallback = 0) {
    if (!n) return fallback;
    if (n->type == JsonType::Int || n->type == JsonType::Bool) return n->intValue;
    return fallback;
}

// Read a node as a bool. A non-zero Int reads as true; anything non-bool/non-int is `fallback`.
inline bool readBool(const JsonNode* n, bool fallback = false) {
    if (!n) return fallback;
    if (n->type == JsonType::Bool || n->type == JsonType::Int) return n->intValue != 0;
    return fallback;
}

/// Navigate to the array under a key, or nothing when the input names none: @xref{the-shared-document-is-a-function-local-static|why its own function}.
inline const JsonNode* parseListArray(const char* json, const char* key, JsonDoc*& docOut) {
    static JsonDoc doc;
    docOut = &doc;
    if (!parse(json, doc)) return nullptr;
    const JsonNode* arr = member(doc, doc.rootNode(), key);
    if (!arr || arr->type != JsonType::Array) return nullptr;
    return arr;
}

template <typename Fn>
inline bool forEachListElement(const char* json, const char* key, Fn&& fn) {
    JsonDoc* doc = nullptr;
    const JsonNode* arr = parseListArray(json, key, doc);
    if (!arr) return false;
    const int n = arraySize(*doc, arr);
    for (int i = 0; i < n; i++) {
        const JsonNode* el = element(*doc, arr, i);
        if (el && el->type == JsonType::Object) fn(*doc, el);
    }
    return true;
}

/// @}
} // namespace mm::json
