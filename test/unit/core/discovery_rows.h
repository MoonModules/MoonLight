#pragma once

/// Helpers the Discovery tests share: a presence packet heard from an address, and a row read back through the list's serialized rows, its only public window.

#include "core/system/DevicesModule.h"
#include "core/system/WledPacket.h"
#include "core/util/JsonSink.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace discovery_rows {

/// A presence packet from `ip` naming `name`, marked a MoonLight peer when `mm`.
inline void hearPresence(mm::DevicesModule& dev, const uint8_t ip[4], const char* name, bool mm) {
    uint8_t pkt[mm::WledPacket::kSize];
    mm::WledPacket::build(pkt, ip, name, /*boardType=*/34, /*lightsOn=*/true);
    if (mm) mm::WledPacket::stampMmMarker(pkt);
    dev.injectPacketForTest(pkt, sizeof(pkt), ip);
}

/// The row at `ip` as JSON, its summary or its detail, or "" when no row holds that address.
inline std::string rowAt(const mm::DevicesModule& dev, const char* ip, bool detail = false) {
    char quoted[24];
    std::snprintf(quoted, sizeof(quoted), "\"ip\":\"%s\"", ip);
    for (mm::ListRow i = 0; i < dev.listRowCount(); i++) {
        mm::JsonSink sink;
        if (detail) dev.writeListRowDetail(sink, i);
        else dev.writeListRow(sink, i);
        if (std::strstr(sink.data(), quoted)) return sink.data();
    }
    return "";
}

}  // namespace discovery_rows
