#pragma once

// Board name constants — these strings are the stable persistent identifiers stored in JSON.
// They must match the names passed to addBoardValue() in setupDefinition().
// Never rename these; adding new boards anywhere in the list is safe.
// The array order matches the old numeric IDs for legacy migration (index 0 = none/empty).
//
// This header is pure C++ (no ESP32/Arduino dependencies) so it can be included in native tests.

#include <stddef.h>

namespace BoardName {
  // Order must match old IO_BoardsEnum for legacy numeric ID migration.
  // New boards must be appended at the end.
  static constexpr const char* names[] = {
    "",                          //  0 — none (board_none)
    "QuinLED Dig-2-Go",         //  1 (board_QuinLEDDig2Go)
    "QuinLED Dig-Next-2",       //  2 (board_QuinLEDDigNext2)
    "QuinLED Dig-Uno v3",       //  3 (board_QuinLEDDigUnoV3)
    "QuinLED Dig-Quad v3",      //  4 (board_QuinLEDDigQuadV3)
    "QuinLED Dig-Octa v2",      //  5 (board_QuinLEDDigOctaV2)
    "Serg Universal Shield",    //  6 (board_SergUniShieldV5)
    "Serg Mini Shield",         //  7 (board_SergMiniShield)
    "SE16 v1",                  //  8 (board_SE16V1)
    "LightCrafter16",           //  9 (board_LightCrafter16)
    "MHC V43 controller",       // 10 (board_MHCV43)
    "MHC V57 PRO controller",   // 11 (board_MHCV57PRO)
    "MHC P4 Nano Shield V1.0",  // 12 (board_MHCP4NanoV1)
    "MHC P4 Nano Shield V2.0",  // 13 (board_MHCP4NanoV2)
    "Yves V48",                 // 14 (board_YvesV48)
    "Troy P4 Nano",             // 15 (board_TroyP4Nano)
    "Atom S3R",                 // 16 (board_AtomS3)
    "Luxceo Mood1 Xiao Mod",   // 17 (board_LuxceoMood1XiaoMod)
    "Cube202010",               // 18 (board_Cube202010)
    "Olimex ESP32-POE",         // 19 — new (not in old enum)
  };
  static constexpr size_t count = sizeof(names) / sizeof(names[0]);

  // Named accessors — same values as the array, for readable code
  static constexpr const char* none               = names[0];
  static constexpr const char* QuinLEDDig2Go      = names[1];
  static constexpr const char* QuinLEDDigNext2    = names[2];
  static constexpr const char* QuinLEDDigUnoV3    = names[3];
  static constexpr const char* QuinLEDDigQuadV3   = names[4];
  static constexpr const char* QuinLEDDigOctaV2   = names[5];
  static constexpr const char* SergUniShieldV5    = names[6];
  static constexpr const char* SergMiniShield     = names[7];
  static constexpr const char* SE16V1             = names[8];
  static constexpr const char* LightCrafter16     = names[9];
  static constexpr const char* MHCV43             = names[10];
  static constexpr const char* MHCV57PRO          = names[11];
  static constexpr const char* MHCP4NanoV1        = names[12];
  static constexpr const char* MHCP4NanoV2        = names[13];
  static constexpr const char* YvesV48            = names[14];
  static constexpr const char* TroyP4Nano         = names[15];
  static constexpr const char* AtomS3             = names[16];
  static constexpr const char* LuxceoMood1XiaoMod = names[17];
  static constexpr const char* Cube202010         = names[18];
  static constexpr const char* OlimexESP32POE     = names[19];

  /// Convert a legacy numeric board preset ID to the corresponding name string.
  /// Returns names[0] ("") if the ID is out of range.
  static const char* fromLegacyId(int id) {
    if (id >= 0 && id < (int)count) return names[id];
    return names[0];
  }
}
