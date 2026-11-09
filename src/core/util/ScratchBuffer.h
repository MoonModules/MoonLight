#pragma once

#include <cstddef>
#include <cstdint>

#include "core/module/ModuleResource.h"   // the tie to its module, which frees it on release

/// One owned allocation with a typed view, so a module sizes its working memory once rather than per frame.
///
/// A module declares one as a member and calls resize; the destructor, the accounting and the null guard come with it.
///
/// @technical
///
/// ## One copy of the logic, however many element types
///
/// The type-erased base owns the allocation, the tie to its module and the one heavy body, and is compiled once.
/// The typed face above it is inline sugar that folds into its call sites, so a device using several element types pays for the logic once rather than per type.
///
/// That is the type-erased base with a thin typed face, the same split a standard container uses to share code across element types.
/// Applied here so a memory-holding effect writes one resize call and nothing else.
namespace mm {

/// The type-erased base, which owns the allocation and the one heavy body, a ModuleResource its module frees on release.
class ScratchBufferBase : public ModuleResource {
protected:
    /// Registers with the owning module, which frees it on release.
    explicit ScratchBufferBase(MoonModule& owner) : ModuleResource(owner) {}
    ~ScratchBufferBase() override;                   // frees (.cpp); the base deregisters
    /// The module's release: free the allocation.
    void releaseResource() override { resizeBytes(0); }

    /// Size to exactly that many bytes, zero meaning free, reallocating only when the count changes; false on an allocation failure.
    bool resizeBytes(std::size_t bytes);

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

    T*          data()        { return static_cast<T*>(raw_); }         ///< the one cast in the design
    const T*    data()  const { return static_cast<const T*>(raw_); }   ///< the same, read-only
    std::size_t count() const { return bytes_ / sizeof(T); }            ///< how many elements fit
    std::size_t bytes() const { return bytes_; }                        ///< how many bytes are held
    explicit operator bool() const { return raw_ != nullptr; }          ///< whether anything is allocated

    T&       operator[](std::size_t i)       { return data()[i]; }
    const T& operator[](std::size_t i) const { return data()[i]; }
};

} // namespace mm
