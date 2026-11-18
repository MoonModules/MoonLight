#pragma once

#include "core/module/MoonModule.h"
#include "core/system/DeviceGroup.h"   // kIdLen: the width of a group id
#include "core/util/Hlc.h"
#include "core/util/ScratchBuffer.h"
#include "platform/platform.h"

#include <cstddef>
#include <cstdint>

/// The device groups' definitions between devices: one well-known multicast address every device listens on, so a device hears a group it was added to before it knows the group.

namespace mm {

class DeviceGroup;
namespace osc { class Reader; }

/// Carries every device group's definition over OSC, creates a group on a member that hears of it, and keeps the groups deleted anywhere deleted everywhere.
///
/// @technical
///
/// **One address for every definition.** 239.255.77.79, beside discovery's 239.255.77.77 and OSC feedback's .78, port 65510: a device listens before it knows a group, and a definition is small and repeated rarely, so one address costs a swarm little. A group's own address is for the state its members share (the next step).
///
/// **Who keeps a group.** Its members and the device that made it. A member that hears of a group it lacks creates it, as the API adds a module, so it saves and shows like any other; a device the group's members leave out drops it.
///
/// **Deleted is deleted everywhere.** A group the user deletes leaves a marker, its id and a stamp, saved in Discovery's hidden `deletedGroups`: sent three times, answered to any late copy, and newer than the group's every edit, so it holds against a member that was off. The newest eight markers are kept, the oldest giving way.
///
/// **A marker only grows newer.** An equal or older deletion leaves it as it is, and a group held under a marker goes, retried each second until it has.
///
/// **What it accepts.** A message whose id is not twenty lowercase hex digits, or whose stamp leaves the clock no room to grow, is ignored, and a definition is merged whole or not at all.
class DeviceGroupSync {
public:
    /// Its buffers belong to `owner`, Discovery, which frees them on release.
    explicit DeviceGroupSync(MoonModule& owner) : owner_(owner), buf_(owner), seen_(owner), tombs_(owner) {}

    /// Once a second: read what arrived, notice a group deleted here, send what Trickle says is due, and save the markers when they changed.
    void tick(MoonModule& devices, char* saved, size_t savedCap);

    /// Close the socket and free the buffers.
    void close();

    /// Handle one received datagram against the groups under `devices`, as tick does: what a test drives directly.
    void handle(MoonModule& devices, const uint8_t* pkt, size_t len);

    /// Whether a group with this id is marked deleted.
    bool deleted(const char* id) const;

    /// The multicast address every definition travels on.
    static constexpr uint8_t kGroup[4] = {239, 255, 77, 79};
    /// Its port.
    static constexpr uint16_t kPort = 65510;
    /// The OSC address of a deletion marker.
    static constexpr const char* kDeletedAddress = "/moonlight/devicegroup/deleted";

private:
    struct GroupId { char id[DeviceGroup::kIdLen + 1]; };
    /// A deleted group's marker, and how many more times it goes out.
    struct Tomb { char id[DeviceGroup::kIdLen + 1]; Stamp stamp; uint8_t sendsLeft; };

    bool ensureOpen();
    /// True when the datagram is a definition, handled or not.
    bool handleDefinition(MoonModule& devices, const uint8_t* pkt, size_t len);
    bool answerIfDeleted(const char* id);
    DeviceGroup* adopt(MoonModule& devices, const char* id, osc::Reader& r);
    /// After a merge: drop a group this device left, take a newer name.
    void settle(MoonModule& devices, DeviceGroup& g, const char* wantName);
    void rename(MoonModule& devices, DeviceGroup& g, const char* to);
    static bool parseTomb(char* entry, Tomb& t);
    void handleDeletion(MoonModule& devices, const uint8_t* pkt, size_t len);
    void noticeDeleted(MoonModule& devices);
    static Stamp stampNow(MoonModule& devices);
    void sendDue(MoonModule& devices);
    void markDeleted(const char* id, const Stamp& stamp, uint8_t sends);
    /// The index of the group's marker, or -1 when it has none.
    int tombIndex(const char* id) const;
    /// A slot for a new marker: one more while room and memory allow, else the oldest; null when memory refuses the first.
    Tomb* tombSlot();
    void sendDeletion(const Tomb& t);
    void send(const uint8_t* data, size_t len);
    void loadTombs(const char* saved);
    void saveTombs(char* saved, size_t cap) const;
    static DeviceGroup* groupById(MoonModule& devices, const char* id);
    static void apply(const char* doc);
    void create(MoonModule& devices, const char* id, const char* name);
    void removeLocal(MoonModule& devices, DeviceGroup& g);

    MoonModule& owner_;            ///< Discovery, which saves the markers
    platform::UdpSocket sock_;
    bool open_ = false, loaded_ = false, tombsChanged_ = false;
    ScratchBuffer<uint8_t> buf_;   ///< one datagram, taken when the socket opens
    ScratchBuffer<GroupId> seen_;  ///< the group ids held last second, so a group deleted here is noticed
    uint16_t seenCount_ = 0;
    ScratchBuffer<Tomb> tombs_;    ///< the deletion markers
    uint8_t tombCount_ = 0;
    static constexpr size_t kDatagram = 1472;   ///< an Ethernet frame's UDP payload
    static constexpr uint16_t kPerChunk = 16;   ///< members a message carries, about 1 KB
    static constexpr uint8_t kMaxTombs = 8;
};

}  // namespace mm
