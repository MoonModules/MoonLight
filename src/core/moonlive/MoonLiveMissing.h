#pragma once

#include <cstddef>
#include <cstring>

/// The shipped scripts a module names and the device does not hold, which an open page downloads.
///
/// The state says what is wanted and this list what is missing. Any open page closes the gap with the picker's own download, so a script a document names arrives as a picked one does.
namespace mm::moonlive {

/// Room for the missing names: a show naming five or six at once fits, and one more waits for the next pass.
inline constexpr size_t kMissingText = 160;

/// The missing names, `|`-separated, as System's hidden read-only control shows them to every page.
inline char* missingScripts() {
    static char text[kMissingText] = "";
    return text;
}

/// Where `name` is in the list, or null.
inline char* findMissing(const char* name) {
    char* text = missingScripts();
    const size_t n = std::strlen(name);
    for (char* at = text; *at;) {
        char* end = std::strchr(at, '|');
        const size_t len = end ? static_cast<size_t>(end - at) : std::strlen(at);
        if (len == n && std::strncmp(at, name, n) == 0) return at;
        if (!end) break;
        at = end + 1;
    }
    return nullptr;
}

/// Add `name`, once, from a compile on the loop's task, the list's one writer; a name past the room waits until one before it arrives.
inline void noteScriptMissing(const char* name) {
    if (!name || !*name || findMissing(name)) return;
    char* text = missingScripts();
    const size_t used = std::strlen(text), n = std::strlen(name);
    if (used + (used ? 1 : 0) + n + 1 > kMissingText) return;
    if (used) text[used] = '|';
    std::memcpy(text + used + (used ? 1 : 0), name, n + 1);
}

/// Drop `name`, as its file has arrived.
inline void noteScriptPresent(const char* name) {
    if (!name || !*name) return;
    char* at = findMissing(name);
    if (!at) return;
    char* text = missingScripts();
    char* end = at + std::strlen(name);
    if (*end == '|') end++;                       // the separator after it goes with it
    else if (at > text) at--;                     // the last one takes the separator before it
    std::memmove(at, end, std::strlen(end) + 1);
}

/// Drop the script a written file holds, wherever it landed, so a name whose module went before its download arrived leaves the list too.
inline void noteScriptLanded(const char* path) {
    if (!path) return;
    const char* slash = std::strrchr(path, '/');
    noteScriptPresent(slash ? slash + 1 : path);
}

}  // namespace mm::moonlive
