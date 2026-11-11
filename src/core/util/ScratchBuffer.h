#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "core/module/ModuleResource.h"   // the tie to its module, which frees it on release
#include "platform/nonblocking.h"         // MM_NONBLOCKING: reading a held element neither allocates nor blocks

/// One owned allocation with a typed view, so a module sizes its working memory once rather than per frame.
///
/// A module declares one as a member and calls resize; the destructor, the accounting and the null guard come with it.
///
/// @technical
///
/// ## One copy of the logic, however many element types
///
/// The type-erased base owns the allocation, the tie to its module and the one heavy body, compiled once; the typed face is inline sugar, so several element types cost the logic once.
///
/// That is the split a standard container uses to share code across element types, applied so a memory-holding effect writes one resize call.
namespace mm {

/// The type-erased base, which owns the allocation and the one heavy body, a ModuleResource its module frees on release: @xref{one-copy-of-the-logic-however-many-element-types}.
class ScratchBufferBase : public ModuleResource {
protected:
    /// Registers with the owning module, which frees it on release.
    explicit ScratchBufferBase(MoonModule& owner) : ModuleResource(owner) {}
    ~ScratchBufferBase() override;                   // frees (.cpp); the base deregisters
    /// The module's release: free the allocation.
    void releaseResource() override { resizeBytes(0); }

    /// Size to exactly that many bytes, zero meaning free, reallocating only when the count changes; false on an allocation failure.
    bool resizeBytes(std::size_t bytes);
    /// Hold at least `bytes`, keeping what is held, doubling from the current size (at least `minBytes`); the exact size when the doubled one misses the reserve, false when neither fits.
    bool ensureBytes(std::size_t bytes, std::size_t minBytes);
    /// Move what is held into a new block of `want` bytes, zeroing the rest; false, the old block kept, when the reserve or the heap refuses.
    bool growTo(std::size_t want);

    void*       raw_   = nullptr;
    std::size_t bytes_ = 0;
};

/// The typed face, pure inline sugar: every method folds into its call site, so a new element type adds no flash.
template <class T>
class ScratchBuffer : private ScratchBufferBase {
public:
    /// Declared as a member of the module that owns it.
    explicit ScratchBuffer(MoonModule& owner) : ScratchBufferBase(owner) {}

    /// Size to hold `count` elements (0 frees). Returns true on success.
    bool resize(std::size_t count) { return resizeBytes(count * sizeof(T)); }

    /// Hold at least `count` elements, keeping those held: a list that grows at runtime, the allocation rule's doubling in one place. False when memory refuses, the list as it was.
    bool ensure(std::size_t count) {
        static_assert(std::is_trivially_copyable<T>::value, "ensure moves elements bytewise");
        return ensureBytes(count * sizeof(T), 4 * sizeof(T));
    }

    T*          data()        MM_NONBLOCKING { return static_cast<T*>(raw_); }         ///< the one cast in the design
    const T*    data()  const MM_NONBLOCKING { return static_cast<const T*>(raw_); }   ///< the same, read-only
    std::size_t count() const MM_NONBLOCKING { return bytes_ / sizeof(T); }            ///< how many elements fit
    std::size_t bytes() const MM_NONBLOCKING { return bytes_; }                        ///< how many bytes are held
    explicit operator bool() const MM_NONBLOCKING { return raw_ != nullptr; }          ///< whether anything is allocated

    T&       operator[](std::size_t i)       MM_NONBLOCKING { return data()[i]; }
    const T& operator[](std::size_t i) const MM_NONBLOCKING { return data()[i]; }
};

} // namespace mm
