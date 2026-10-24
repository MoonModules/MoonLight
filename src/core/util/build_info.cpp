/// Which binary this is: the one unit holding the build identity and the clock, so every other unit stays one a compiler cache can reuse.

#include "core/util/build_info.h"
#include "core/util/build_id.h"   // generated on every build: the git hash and library.json's version

// A release build passes these as flags on this unit alone; a local build takes the defaults.
#ifndef MM_VERSION
#define MM_VERSION MM_LIBRARY_VERSION
#endif
#ifndef MM_FIRMWARE_NAME
#define MM_FIRMWARE_NAME "unknown"
#endif
#ifndef MM_RELEASE
#define MM_RELEASE ""
#endif

namespace mm {

const char* const kVersion      = MM_VERSION;
const char* const kBuildId      = MM_BUILD_ID;
const char* const kFirmwareName = MM_FIRMWARE_NAME;
const char* const kRelease      = MM_RELEASE;
// Recompiled on every build: generate_build_info.py touches the stamp this file depends on.
const char* const kBuildDate    = __DATE__ " " __TIME__;

}  // namespace mm
