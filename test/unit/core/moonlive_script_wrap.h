#pragma once

#include <cstdio>
#include <cstring>

/// Wrap a bare statement body in the class and entry point a script needs.
///
/// @technical
///
/// ## Why the buffers are a thread-local ring
///
/// Thread-local because the concurrency test compiles from two threads and a shared buffer would hand each the other's source.
/// A ring because tests build tables of scripts that hold the pointers and use them later, where a single buffer would alias every row to the last written.
///
/// Both bounds are measured rather than guessed, at roughly four times the longest script and deepest table seen, and a caller past either owns its own string.
///
/// ## Which entry point a script declares
///
/// A layout, a modifier and an effect each look up a different name.
/// A test for one binding has to emit that name, or the script compiles and the binding finds nothing.
///
/// Wrap a bare statement body in the class and entry point a MoonLive script needs.
///
/// A script is a class: `class T { void tick() { … } }`. Nearly every test here is about ONE behavior inside that body (a loop counter surviving a call, a control keeping its value, a golden byte sequence).
/// Spelling the enclosing class out at every call site would bury the assertion under four lines of identical ceremony. This puts the ceremony in one place so a test reads as what it is about.
///
/// A test that is about the class shape itself passes its own complete source and skips this.

/// The same, naming the entry point the binding under test asks for.

inline const char* mmScriptAs(const char* entry, const char* body) {
    // A thread-local ring, both bounds measured: see the appendix.
    static constexpr size_t kSlots = 12;
    static constexpr size_t kSlotBytes = 640;
    thread_local char ring[kSlots][kSlotBytes];
    thread_local unsigned next = 0;
    char* wrapped = ring[next++ % kSlots];

    // Leading declarations go to class scope, since a member is not a function-body local.
    const char* p = body;
    const char* declEnd = body;
    while (true) {
        while (*p == ' ' || *p == '\t' || *p == '\n') p++;
        // Every member type, and the keyword must end at a non-identifier to avoid a false match.
        auto atType = [](const char* q) {
            static const struct { const char* kw; size_t len; } kTypes[] = {
                {"int", 3}, {"byte", 4}, {"bool", 4}, {"fixed", 5}, {"string", 6}};
            for (const auto& t : kTypes) {
                if (std::strncmp(q, t.kw, t.len) == 0) {
                    const char c = q[t.len];
                    const bool identChar = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                           (c >= '0' && c <= '9') || c == '_';
                    if (!identChar) return true;
                }
            }
            return false;
        };
        if (!atType(p)) break;
        const char* semi = std::strchr(p, ';');
        if (!semi) break;
        const char* eol = std::strchr(semi, '\n');
        declEnd = eol ? eol : semi + 1;
        p = declEnd;
    }

    // Refused rather than clipped, which would have the test check a script nobody wrote.
    if (std::strlen(body) + 64 >= kSlotBytes) return "";

    if (declEnd != body) {
        const int declLen = static_cast<int>(declEnd - body);
        std::snprintf(wrapped, kSlotBytes, "class T {\n%.*s\n  void %s() {\n%s\n  }\n}\n",
                      declLen, body, entry, declEnd);
    } else {
        std::snprintf(wrapped, kSlotBytes, "class T {\n  void %s() {\n%s\n  }\n}\n", entry, body);
    }
    return wrapped;
}

inline const char* mmScript(const char* body) { return mmScriptAs("tick", body); }
