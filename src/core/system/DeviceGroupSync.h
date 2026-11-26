#pragma once

#include "core/module/MoonModule.h"
#include "core/system/DeviceGroup.h"   // kIdLen: the width of a group id
#include "core/system/SharedControls.h"   // what the members share: the values their slots drive, and the look
#include "core/util/ControlSurface.h"     // the Control module tells this sync what changed here, as it tells a desk
#include "core/util/Hlc.h"
#include "core/util/ScratchBuffer.h"
#include "core/util/WallPlace.h"   // the members' places in a wall
#include "platform/platform_net.h"   // UdpSocket

#include <cstddef>
#include <cstdint>

/// The device groups between devices: their definitions and shared controls on one well-known multicast address every device listens on, so a device hears a group it was added to before it knows the group.

namespace mm {

class DeviceGroup;
namespace osc { class Reader; }

/// Carries every device group's definition and its shared controls over OSC, creates a group on a member that hears of it, and keeps the groups deleted anywhere deleted everywhere.
///
/// @technical
///
/// **One address for everything a group carries.** 239.255.77.79, beside discovery's 239.255.77.77 and OSC feedback's .78, port 65510: a device listens before it knows a group, and its messages are small, so one address costs a swarm little. Each message names its group, and a device takes only those of a group it holds.
///
/// **Membership is the one setting.** Membership alone makes a device follow the group's shared controls: this sync is a surface of the Control module, so a switch, encoder or fader changed here, from the UI, the API or a desk, goes out as what it drives (`Drivers.brightness`), its value and its stamp, to every group held.
///
/// **Applied by name** ([SharedControls.h](SharedControls.h)). A member applies the parameter whatever its own slots drive. A value a group set is not sent on, so no loop forms, and only a parameter a slot drives leaves the board.
///
/// **A look travels whole.** An effects preset applied here goes out as its document, in chunks every member takes from the multicast and repairs on request ([SharedControls.h](SharedControls.h)), applying it at the change's time. A driver or layout preset carries the board's pins and stays.
///
/// **Catch-up is anti-entropy.** The shared values and the look's advertisement ride the definition's Trickle repeats with their stamps, and a member whose values differ resets its interval (RFC 6206), so a member back from a restart takes the newer values within seconds and its older saved ones cannot win.
///
/// **Read every 20 ms.** Shared values and look chunks apply on the 20 ms read, so a change reaches the members within a frame or two; a definition or deletion, which may create or remove a module, waits for the once-a-second tick.
///
/// **Who keeps a group.** Its members and the device that made it. A member that hears of a group it lacks creates it, as the API adds a module, so it saves and shows like any other; a device the group's members leave out drops it.
///
/// **Deleted is deleted everywhere.** A group the user deletes leaves a marker, its id and a stamp, saved in Discovery's hidden `deletedGroups`: sent three times, answered to any late copy, and winning over every edit, so it holds against a member that was off or edited at once. The newest eight markers are kept, an older one giving way to a newer.
///
/// **A marker only grows newer.** An equal or older deletion leaves it as it is, and a group held under a marker goes, retried each second until it has.
///
/// **What it accepts.** A message whose id is not twenty lowercase hex digits, or whose stamp is past the largest valid reading (Stamp::kMaxHlc), is ignored, as is a malformed definition.
///
/// ## Applied at a time
///
/// A change made here goes out as an OSC bundle whose timetag is the fleet time kApplyLeadUs ahead; a member on the fleet clock holds it until then, so members that hear it at different moments switch together.
///
/// A member off the fleet clock applies it on arrival, and so does one whose change is stamped further ahead than kMaxHoldUs. The device where the change is made applies it at once, so it leads the members by the lead. The Trickle repeats go out plain, as state to converge on.
class DeviceGroupSync : public ControlSurface {
public:
    /// Its buffers belong to `owner`, Discovery, which frees them on release.
    explicit DeviceGroupSync(MoonModule& owner) : owner_(owner), shared_(owner), buf_(owner), out_(owner), pending_(owner), held_(owner), seen_(owner), tombs_(owner), places_(owner) {}

    /// Once a second: notice a group deleted here, read what arrived, definitions and deletions included, send what Trickle says is due, and save the markers when they changed.
    void tick(MoonModule& devices, char* saved, size_t savedCap);

    /// Every 20 ms: apply the shared values, look chunks and places that arrived, setting aside a definition or deletion for the tick, apply a whole look at its time, and send the chunks due.
    void receive(MoonModule& devices);
    /// Whether the 20 ms read applies datagram `pkt` now: a shared value, a look chunk or a place, none of which creates a module.
    bool takeNow(MoonModule& devices, const uint8_t* pkt, size_t len);

    /// A switch, encoder or fader changed here: what it drives goes to every group held, unless a group set it.
    void sendValue(SurfaceControl kind, uint8_t index, uint8_t value) override;

    /// A look was applied here: its document goes to every group held, in chunks.
    void lookApplied(const char* name) override;

    /// What a group's status adds about the shared controls, or null.
    const char* sharedNote() const MM_NONBLOCKING { return shared_.note(); }

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
    /// The address a device's place in a wall travels on: its MAC, then its corner and its size in three axes.
    static constexpr const char* kPlaceAddress = "/moonlight/devicegroup/place";
    /// How often a placed device repeats its place, in seconds; a change goes out at once.
    static constexpr uint8_t kPlaceEverySec = 5;
    /// How long a member's place is kept unheard, so a board restarting keeps its part of the wall and one switched off leaves it.
    static constexpr uint32_t kPlaceHoldMs = 60000;
    /// How far ahead a change made here applies on the members, in microseconds of fleet time: @xref{applied-at-a-time}.
    static constexpr int64_t kApplyLeadUs = 100000;
    /// The furthest ahead a change is held, ten leads; one stamped later, from a sender on another time or a crafted datagram, applies on arrival.
    static constexpr int64_t kMaxHoldUs = 10 * kApplyLeadUs;
    /// How many timed changes a member holds at once; one more applies on arrival.
    static constexpr uint8_t kHeld = 8;

protected:
    /// Send one datagram to the groups' address, the seam a test captures.
    virtual void transmit(const uint8_t* data, size_t len);

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
    /// Mark every group held last second and gone now, unless this sync removed it: a group the user deleted here.
    void noticeDeleted(MoonModule& devices);
    /// Remove a group held under a marker, retried each second, and remember the groups held for the next noticeDeleted.
    void holdGroups(MoonModule& devices);
    static Stamp stampNow(MoonModule& devices);
    void sendDue(MoonModule& devices);
    void markDeleted(const char* id, const Stamp& stamp, uint8_t sends);
    /// The index of the group's marker, or -1 when it has none.
    int tombIndex(const char* id) const;
    /// A slot for a marker stamped `stamp`: one more while room and memory allow, else the oldest when this one is newer; null otherwise.
    Tomb* tombSlot(const Stamp& stamp);
    void sendDeletion(const Tomb& t);
    void send(const uint8_t* data, size_t len);
    /// Send a change made here as a bundle applying kApplyLeadUs ahead on the fleet clock, or plain while this device has no fleet time.
    void sendTimed(const uint8_t* data, size_t len);
    /// Handle a received datagram that may be a timed change: hold it until its time, else hand it on.
    bool receiveShared(MoonModule& devices, const uint8_t* pkt, size_t len);
    /// Apply the held changes whose time has come.
    void applyDue(MoonModule& devices);
    /// The markers read from the saved text; false, none held, when memory refuses them.
    bool loadTombs(const char* saved);
    void saveTombs(char* saved, size_t cap) const;
    static DeviceGroup* groupById(MoonModule& devices, const char* id);
    static void apply(const char* doc);
    void create(MoonModule& devices, const char* id, const char* name);
    void removeLocal(MoonModule& devices, DeviceGroup& g);

    /// What a slot drove and its value when last seen here, so only a change of the same parameter is shared.
    struct SlotSeen { uint32_t target = 0; int32_t value = 0; };
    /// Read everything that arrived, the set-aside datagram first, between noticing a group deleted here and holding the groups.
    void readAll(MoonModule& devices);
    /// Attach to the Control module once it is up, each slot's parameter read first.
    void attach();
    /// Whether the parameter slot `slot` drives changed here, its value read into `value`.
    bool changedHere(uint8_t slot, const char* target, int32_t& value);
    /// Send one change of `param` to every group held.
    void shareValue(const char* param, int32_t value);
    /// True when the datagram is a shared value or part of a look, handled or not.
    bool handleShared(MoonModule& devices, const uint8_t* pkt, size_t len);
    /// Send group `g`'s values and look advertisement, as its definition goes.
    void sendShared(const DeviceGroup& g);
    /// Send the chunks and requests each group's look has due, a few per read.
    void sendLookChunks(MoonModule& devices);

    MoonModule& owner_;            ///< Discovery, which saves the markers
    MoonModule* devices_ = nullptr;   ///< the container the groups live in, met on the first tick
    SharedControls shared_;
    static constexpr uint8_t kSlots = 24;   ///< the Control module's switches, encoders and faders
    SlotSeen slotSeen_[kSlots] = {};
    bool attached_ = false;
    platform::UdpSocket sock_;
    bool open_ = false, loaded_ = false, tombsChanged_ = false;
    ScratchBuffer<uint8_t> buf_;   ///< one datagram, taken when the socket opens
    ScratchBuffer<uint8_t> out_;   ///< one datagram to send, a look's chunk or request, taken on the first
    ScratchBuffer<uint8_t> pending_;   ///< a definition or deletion the 20 ms read set aside for the tick
    /// A change that arrived ahead of its time.
    struct Held { int64_t atUs; uint16_t len; uint8_t msg[160]; };
    ScratchBuffer<Held> held_;     ///< the changes waiting for their time, taken when the socket opens
    uint8_t heldCount_ = 0;
    size_t pendingLen_ = 0;
    ScratchBuffer<GroupId> seen_;  ///< the group ids held last second, so a group deleted here is noticed
    uint16_t seenCount_ = 0;
    ScratchBuffer<Tomb> tombs_;    ///< the deletion markers
    uint8_t tombCount_ = 0;
    /// A member's place as it last sent it.
    struct Heard { uint8_t mac[6]; WallPlace place; uint32_t atMs; };
    ScratchBuffer<Heard> places_;  ///< the members' places, kept only for members of a group held here
    uint8_t placeCount_ = 0;
    WallPlace sentPlace_;          ///< this device's place as it last went out
    uint8_t placeTicks_ = 0;       ///< seconds since it last went out
    static constexpr uint8_t kMaxPlaces = 64;   ///< places held at most, the members of every group a wall spans

    /// Take a member's place, or forget it when it sent none; false for another message.
    bool handlePlace(MoonModule& devices, const uint8_t* pkt, size_t len);
    /// Keep a member's place, or forget it when it is unplaced.
    void storePlace(const uint8_t mac[6], const WallPlace& p);
    /// Send this device's place on a change, and every kPlaceEverySec while it holds one.
    void sendPlace(MoonModule& devices);
    /// The wall this device's groups make, from its own place and its members' heard ones, set on Devices.
    void deriveWall(MoonModule& devices);
    /// Whether a group held here has this MAC as a member.
    static bool isMember(MoonModule& devices, const uint8_t mac[6]);
    static constexpr size_t kDatagram = 1472;   ///< an Ethernet frame's UDP payload
    static constexpr uint16_t kPerChunk = 16;   ///< members a message carries, about 1 KB
    static constexpr uint8_t kMaxTombs = 8;
};

}  // namespace mm
