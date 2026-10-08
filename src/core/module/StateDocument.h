#pragma once

#include <cstdint>

/// @defgroup StateDocument Applying part of the device's state as one JSON document
/// @{
/// A state document mirrors the module tree, keyed by module name, and applies as a JSON Merge Patch (RFC 7386); its rules are in integrating.md § Setting everything at once.

#include <cstddef>

namespace mm {

/// The largest document applied or saved, far past a real one: a bigger one is a mistake, refused before any allocation.
inline constexpr size_t kStateDocumentMax = 32 * 1024;

class JsonSink;
class MoonModule;
class Scheduler;

/// Where a document comes from, which decides how strictly it applies.
enum class StateSource : uint8_t {
    Request,   ///< a client's document, refused whole on the first problem so nothing changes by half
    Stored,    ///< a file the device wrote, onto the running tree: what this build cannot place is skipped, values clamp, code-wired children stay
    Boot,      ///< the same file during boot, before any setup: no lifecycle and no reactions, which the boot phases run after it
};

/// What applying a document did: the first failure and where it happened, or how much it changed.
struct StateDocumentResult {
    bool ok = true;            ///< false at the first failure, which stops the walk
    const char* error = "";    ///< what failed, a constant
    char where[64] = {};       ///< the dotted path of the failing key, such as `Effects.Layer.Noise.speed`
    uint16_t changes = 0;      ///< controls set and modules created, replaced or removed
    uint16_t deferred = 0;     ///< controls a script declares, set once the next prepare has compiled it
    uint16_t skipped = 0;      ///< members a stored file names that this build cannot place, each logged
};

/// Apply a state document to the tree; a request is checked whole before anything changes, a stored file member by member. A root `$` key, such as a preset's `$slot`, is the file's own.
StateDocumentResult applyStateDocument(Scheduler& scheduler, const char* text, StateSource source = StateSource::Request);

/// Write what applying a document did as the API answers it: `ok` and `changes`, or the `error` and where it is, and how many controls wait for the rebuild.
void writeStateResult(JsonSink& sink, const StateDocumentResult& r);

/// Apply `body`, the members of `m`'s own object, as a document reaching `m` from the top level, for a caller holding the module rather than its path.
StateDocumentResult applyStateAt(Scheduler& scheduler, MoonModule& m, const char* body, StateSource source = StateSource::Request);

/// Write `m` as one root member, its path from the top level around it, which applied gives back exactly this subtree; `withSecrets` for the device's own config file.
void writeStateMember(JsonSink& sink, MoonModule& m, bool withSecrets = false);

/// Temporary until the release after 2026-10-08 (see MIGRATING): write an older build's flat config file as the state document of `top`, false when the text is not one.
bool flatToStateDocument(const char* text, MoonModule& top, JsonSink& out);

/// Temporary with flatToStateDocument: whether `text` is a config file in the flat format, which records its module's `enabled` at the root.
bool isFlatConfig(const char* text);

/// Set the controls a document named before the script declaring them had compiled; the tree's prepare calls it.
void applyDeferredControls(Scheduler& scheduler);

}  // namespace mm

/// @}
