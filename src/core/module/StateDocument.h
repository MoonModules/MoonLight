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

/// What applying a document did: the first failure and where it happened, or how much it changed.
struct StateDocumentResult {
    bool ok = true;            ///< false at the first failure, which stops the walk
    const char* error = "";    ///< what failed, a constant
    char where[64] = {};       ///< the dotted path of the failing key, such as `Effects.Layer.Noise.speed`
    uint16_t changes = 0;      ///< controls set and modules created, replaced or removed
    uint16_t deferred = 0;     ///< controls a script declares, set once the next prepare has compiled it
};

/// Apply a state document to the tree, checking what creation needs before anything changes; a root `$` key, such as a preset's `$slot`, is the file's own.
StateDocumentResult applyStateDocument(Scheduler& scheduler, const char* text);

/// Write what applying a document did as the API answers it: `ok` and `changes`, or the `error` and where it is, and how many controls wait for the rebuild.
void writeStateResult(JsonSink& sink, const StateDocumentResult& r);

/// Write `m` as one root member, its path from the top level around it, which applied gives back exactly this subtree.
void writeStateMember(JsonSink& sink, MoonModule& m);

/// Set the controls a document named before the script declaring them had compiled; the tree's prepare calls it.
void applyDeferredControls(Scheduler& scheduler);

}  // namespace mm

/// @}
