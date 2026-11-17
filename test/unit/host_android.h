#pragma once

namespace mm {

/// Whether the tests run on an Android device: an app there is updated by its next APK and an adb-shell process has no audio identity, so the tests that assume a host skip.
#ifdef __ANDROID__
inline constexpr bool kHostIsAndroid = true;
#else
inline constexpr bool kHostIsAndroid = false;
#endif

}  // namespace mm
