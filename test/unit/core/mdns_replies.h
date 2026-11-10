#pragma once

/// A real mDNS reply for the tests that read one: an ESPHome Web 26.9.2 board on the bench answering the legacy unicast browse for _esphomelib._tcp.local, captured 2026-11-10.

#include <cstdint>
#include <string>
#include <vector>

namespace mdns_replies {

/// A real reply to the legacy unicast browse for _esphomelib._tcp.local, captured 2026-11-10 from a bench board.
inline const char* kEsphomeReply =
    "4d4d840000000001000000030b5f657370686f6d656c6962045f746370056c6f63616c00000c00010000119400151265"
    "7370686f6d652d7765622d373338386338c00cc02e0021000100000078001b0000000017a512657370686f6d652d7765"
    "622d373338386338c01dc02e00100001000011940105547061636b6167655f696d706f72745f75726c3d676974687562"
    "3a2f2f657370686f6d652f6669726d776172652f657370686f6d652d7765622f65737033322e79616d6c406d61696e3f"
    "66756c6c5f636f6e6669671670726f6a6563745f76657273696f6e3d32362e392e321870726f6a6563745f6e616d653d"
    "657370686f6d652e7765620c6e6574776f726b3d776966690b626f6172643d65737033320e706c6174666f726d3d4553"
    "503332106d61633d63386630396537333838633814636f6e6669675f686173683d32626666353436641076657273696f"
    "6e3d323032362e392e3120667269656e646c795f6e616d653d455350486f6d652057656220373338386338c055000100"
    "01000000780004c0a801e3";

/// The bytes a hex string spells.
inline std::vector<uint8_t> bytes(const char* hex) {
    std::vector<uint8_t> out;
    for (const char* p = hex; p[0] && p[1]; p += 2) out.push_back(static_cast<uint8_t>(std::stoi(std::string(p, 2), nullptr, 16)));
    return out;
}


}  // namespace mdns_replies
