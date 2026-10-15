#pragma once

#include <cstddef>

namespace mm {

/// The one sort primitive for the small bounded collections the system holds.
///
/// @technical
///
/// A device list or a handful of rows is what this is for, and it lives in core so every module sorts the same way and supplies only its comparator.
///
/// ## Why insertion sort rather than std::sort
///
/// These collections are tiny and bounded, a few dozen at most, already nearly ordered between updates, and held in fixed arrays with no allocation.
/// Insertion sort is linear on nearly-sorted input, allocation-free, stable, and short enough that a contributor reads it at a glance.
/// The introsort machinery behind `std::sort` buys nothing at this scale and pulls in `<algorithm>`.
///
/// Reach for `std::sort` when a genuinely large or performance-critical sort turns up.
/// This one is off the hot path: call it when the collection changes, not per render tick.

/// Sort `arr` in place, `less(a, b)` being true when `a` sorts before `b`, keeping equals in order.
template <typename T, typename Less>
inline void insertionSort(T* arr, size_t n, Less less) {
    for (size_t i = 1; i < n; i++) {
        T key = arr[i];
        size_t j = i;
        while (j > 0 && less(key, arr[j - 1])) {
            arr[j] = arr[j - 1];
            j--;
        }
        arr[j] = key;
    }
}

}  // namespace mm
